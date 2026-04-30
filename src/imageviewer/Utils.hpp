#pragma once

#include <cstdint>
#include <cstring>
#include <cmath>
#include "ImagePluginDef.h"

// ─── Format validation ────────────────────────────────────────────────────────

/*
 * Validate an IWImageFormat for internal consistency.
 *
 * Returns false (and optionally sets *outError to a static description string)
 * when any of the following holds:
 *   - componentCount > IW_MAX_COMPONENTS
 *   - any active component has bitWidth == 0
 *   - any active component's bit range (bitOffset+bitWidth) exceeds bitsPerPixel
 *     (interleaved only)
 *   - any two active components have overlapping bit ranges (interleaved only)
 *
 * Returns true and leaves *outError unchanged when the descriptor is valid.
 */
inline bool ValidateImageFormat(const IWImageFormat& fmt,
                                 const char** outError = nullptr) noexcept
{
    auto fail = [&](const char* msg) -> bool {
        if (outError) *outError = msg;
        return false;
    };

    if (fmt.componentCount > IW_MAX_COMPONENTS)
        return fail("componentCount exceeds IW_MAX_COMPONENTS");

    for (uint16_t i = 0u; i < fmt.componentCount; ++i) {
        if (fmt.components[i].bitWidth == 0u)
            return fail("component bitWidth is 0");
    }

    if (fmt.storageLayout == IW_STORAGE_INTERLEAVED) {
        for (uint16_t i = 0u; i < fmt.componentCount; ++i) {
            const uint32_t end = static_cast<uint32_t>(fmt.components[i].bitOffset)
                               + fmt.components[i].bitWidth;
            if (end > fmt.bitsPerPixel)
                return fail("bitsPerPixel too small for component layout");
        }
        for (uint16_t i = 0u; i < fmt.componentCount; ++i) {
            const uint32_t startI = fmt.components[i].bitOffset;
            const uint32_t endI   = startI + fmt.components[i].bitWidth;
            for (uint16_t j = static_cast<uint16_t>(i + 1u); j < fmt.componentCount; ++j) {
                const uint32_t startJ = fmt.components[j].bitOffset;
                const uint32_t endJ   = startJ + fmt.components[j].bitWidth;
                if (startI < endJ && startJ < endI)
                    return fail("interleaved component bit ranges overlap");
            }
        }
    }

    if (outError) *outError = nullptr;
    return true;
}

// ─── GL-upload classification (GL-type-free) ─────────────────────────────────

/*
 * Lightweight classification of a format for direct OpenGL upload.
 *
 * This struct carries no GL-specific types so it can be used in contexts that
 * do not include glad/GL headers.  TexturePool maps it to actual GLenum values.
 */
struct IWGLUploadHint {
    bool    valid;    /* false → slow path (ConvertToRGBAF32) */
    uint8_t channels; /* 1, 2, 3, or 4  (meaningful only when valid) */
    uint8_t bitWidth; /* 8, 16, or 32   (meaningful only when valid) */
    bool    isFloat;  /* true = float32 (meaningful only when valid) */
    bool    isBGR;    /* B/G/R channel order (meaningful only when valid) */
};

/*
 * Returns a valid IWGLUploadHint only for interleaved, uniform-width,
 * byte-aligned, standard-semantic formats whose GL equivalents are
 * GL_UNSIGNED_BYTE / GL_UNSIGNED_SHORT / GL_FLOAT:
 *
 *   GRAY8, GRAY16, GRAY32F  (1 channel)
 *   GRAY+A 8/16/32F         (2 channels)
 *   RGB/BGR 8/16/32F        (3 channels)
 *   RGBA/BGRA 8/16/32F      (4 channels)
 *
 * Everything else (mixed-precision, float16, exotic semantics, planar) returns
 * valid=false.
 */
inline IWGLUploadHint ClassifyGLUpload(const IWImageFormat& fmt) noexcept
{
    const IWGLUploadHint INVALID = {};   /* zero-init → valid=false */

    if (fmt.storageLayout != IW_STORAGE_INTERLEAVED) return INVALID;

    const uint16_t n = fmt.componentCount;
    if (n == 0u || n > 4u) return INVALID;

    const IWComponentClass cls = fmt.components[0].componentClass;
    const uint16_t         bw  = fmt.components[0].bitWidth;
    for (uint16_t i = 1u; i < n; ++i)
        if (fmt.components[i].componentClass != cls || fmt.components[i].bitWidth != bw)
            return INVALID;   /* mixed-precision */

    const bool isFloat = (cls == IW_COMPONENT_CLASS_FLOAT);
    if (isFloat && bw != 32u) return INVALID;           /* float16 / float64 */
    if (!isFloat && bw != 8u && bw != 16u) return INVALID; /* packed / exotic width */

    auto s = [&](int i) { return fmt.components[i].semantic; };

    bool isBGR = false;
    bool ok    = false;
    if (n == 1u && s(0) == IW_COMPONENT_SEMANTIC_GRAY) {
        ok = true;
    } else if (n == 2u && s(0) == IW_COMPONENT_SEMANTIC_GRAY &&
                           s(1) == IW_COMPONENT_SEMANTIC_A) {
        ok = true;
    } else if (n == 3u && s(0) == IW_COMPONENT_SEMANTIC_R &&
                           s(1) == IW_COMPONENT_SEMANTIC_G &&
                           s(2) == IW_COMPONENT_SEMANTIC_B) {
        ok = true;
    } else if (n == 3u && s(0) == IW_COMPONENT_SEMANTIC_B &&
                           s(1) == IW_COMPONENT_SEMANTIC_G &&
                           s(2) == IW_COMPONENT_SEMANTIC_R) {
        ok = true; isBGR = true;
    } else if (n == 4u && s(0) == IW_COMPONENT_SEMANTIC_R &&
                           s(1) == IW_COMPONENT_SEMANTIC_G &&
                           s(2) == IW_COMPONENT_SEMANTIC_B &&
                           s(3) == IW_COMPONENT_SEMANTIC_A) {
        ok = true;
    } else if (n == 4u && s(0) == IW_COMPONENT_SEMANTIC_B &&
                           s(1) == IW_COMPONENT_SEMANTIC_G &&
                           s(2) == IW_COMPONENT_SEMANTIC_R &&
                           s(3) == IW_COMPONENT_SEMANTIC_A) {
        ok = true; isBGR = true;
    }

    if (!ok) return INVALID;   /* exotic semantics (H, S, L, UV, …) */

    return { true,
             static_cast<uint8_t>(n),
             static_cast<uint8_t>(bw),
             isFloat,
             isBGR };
}

// ─── IEEE 754 half-precision (float16) conversion ────────────────────────────

/* Convert a 16-bit IEEE 754 half-precision value to a 32-bit float.
 * Handles subnormals, infinities and NaN correctly. */
inline float HalfToFloat(uint16_t h) noexcept
{
    const uint32_t sign     = static_cast<uint32_t>(h & 0x8000u) << 16u;
    const uint32_t exponent = static_cast<uint32_t>(h & 0x7C00u);
    const uint32_t mantissa = static_cast<uint32_t>(h & 0x03FFu);
    uint32_t f;
    if (exponent == 0u) {
        if (mantissa == 0u) {
            f = sign;
        } else {
            uint32_t m = mantissa, e = 0x38800000u;
            while (!(m & 0x400u)) { m <<= 1u; e -= 0x800000u; }
            f = sign | e | ((m & 0x3FFu) << 13u);
        }
    } else if (exponent == 0x7C00u) {
        f = sign | 0x7F800000u | (mantissa << 13u);
    } else {
        f = sign | (((exponent >> 10u) + 112u) << 23u) | (mantissa << 13u);
    }
    float result; std::memcpy(&result, &f, sizeof(result)); return result;
}

// ─── Per-component value extraction ──────────────────────────────────────────

/* Extract one component value from a raw interleaved pixel, normalising it to
 * a float according to the component class declared in comp.
 *
 * pixelBytes must point to the start of the pixel word; comp.bitOffset and
 * comp.bitWidth describe where in that word the component lives. */
inline float ExtractComponent(const uint8_t* pixelBytes,
                               const IWComponentDef& comp) noexcept
{
    const int byteStart   = comp.bitOffset / 8;
    const int bitStart    = comp.bitOffset % 8;
    const int bytesNeeded = (bitStart + comp.bitWidth + 7) / 8;
    // A component can be at most 32 bits wide; starting up to 7 bits into a byte
    // means we read at most ceil((7+32)/8) = 5 bytes.  Cap at 5 to avoid
    // reading beyond a valid pixel word.
    uint64_t word = 0;
    for (int i = 0; i < bytesNeeded && i < 5; ++i)
        word |= static_cast<uint64_t>(pixelBytes[byteStart + i]) << (i * 8);
    word >>= bitStart;
    const uint64_t mask = (comp.bitWidth < 64u)
                          ? ((uint64_t{1} << comp.bitWidth) - 1u) : ~uint64_t{0};
    const uint64_t raw = word & mask;

    switch (comp.componentClass) {
    case IW_COMPONENT_CLASS_FLOAT:
        if (comp.bitWidth == 16u) return HalfToFloat(static_cast<uint16_t>(raw));
        if (comp.bitWidth == 32u) {
            float f; uint32_t u = static_cast<uint32_t>(raw);
            std::memcpy(&f, &u, sizeof(f)); return f;
        }
        return static_cast<float>(raw);
    case IW_COMPONENT_CLASS_UNORM: {
        const uint64_t mx = (comp.bitWidth < 64u)
                            ? ((uint64_t{1} << comp.bitWidth) - 1u) : ~uint64_t{0};
        return static_cast<float>(raw) / static_cast<float>(mx);
    }
    case IW_COMPONENT_CLASS_SNORM: {
        const uint64_t hr = uint64_t{1} << (comp.bitWidth - 1u);
        const int64_t  s  = (raw >= hr)
                            ? (static_cast<int64_t>(raw) - static_cast<int64_t>(uint64_t{1} << comp.bitWidth))
                            : static_cast<int64_t>(raw);
        return std::max(-1.0f, static_cast<float>(s) / static_cast<float>(hr - 1u));
    }
    case IW_COMPONENT_CLASS_SINT: {
        const uint64_t hr = uint64_t{1} << (comp.bitWidth - 1u);
        const int64_t  s  = (raw >= hr)
                            ? (static_cast<int64_t>(raw) - static_cast<int64_t>(uint64_t{1} << comp.bitWidth))
                            : static_cast<int64_t>(raw);
        return static_cast<float>(s);
    }
    default: /* IW_COMPONENT_CLASS_UINT */
        return static_cast<float>(raw);
    }
}
