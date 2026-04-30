#pragma once

#include <cstdint>
#include <cstring>
#include <cmath>
#include "ImagePluginDef.h"

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
