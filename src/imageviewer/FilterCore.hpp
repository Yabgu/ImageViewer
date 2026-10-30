#pragma once
/*
 * FilterCore.hpp — self-contained dithering math
 *
 * Dependencies: ImagePluginDef.h, FilterPluginDef.h, Utils.hpp
 * No external library (pixman, GL, …) required.
 *
 * All output pixel data is allocated with malloc(); callers must free with
 * FilterCore::FreeFrame() or FilterCore::FreeImageSet().
 */

#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <cmath>
#include <algorithm>
#include <vector>

#include "ImagePluginDef.h"
#include "FilterPluginDef.h"
#include "Utils.hpp"
#include "BlueNoiseTile.hpp"

namespace FilterCore {

/* ── 4×4 Bayer threshold matrix (values in [0, 1)) ──────────────────────── */
static constexpr float kBayer4x4[4][4] = {
    {  0.f/16,  8.f/16,  2.f/16, 10.f/16 },
    { 12.f/16,  4.f/16, 14.f/16,  6.f/16 },
    {  3.f/16, 11.f/16,  1.f/16,  9.f/16 },
    { 15.f/16,  7.f/16, 13.f/16,  5.f/16 }
};

/* ── 8×8 Bayer threshold matrix, raw ranks 0..63 ────────────────────────── */
/* Standard recursive construction.  Threshold = (rank + 0.5) / 64, i.e. never
 * 0 and never 1, so a constant input averages exactly over the tile. */
static constexpr int kBayer8x8Raw[8][8] = {
    {  0, 32,  8, 40,  2, 34, 10, 42 },
    { 48, 16, 56, 24, 50, 18, 58, 26 },
    { 12, 44,  4, 36, 14, 46,  6, 38 },
    { 60, 28, 52, 20, 62, 30, 54, 22 },
    {  3, 35, 11, 43,  1, 33,  9, 41 },
    { 51, 19, 59, 27, 49, 17, 57, 25 },
    { 15, 47,  7, 39, 13, 45,  5, 37 },
    { 63, 31, 55, 23, 61, 29, 53, 21 }
};

/* Which threshold field a spatial mode draws from. */
enum class SpatialKind : uint8_t { Bayer4, Bayer8, BlueNoise };

/* Spatial resolution of each field, in bits (the tile holds 2^bits ranks).
 * Bayer 4x4 = 16 ranks (4 bits), Bayer 8x8 = 64 ranks (6 bits),
 * blue noise = 64x64 = 4096 ranks (12 bits). */
static inline uint32_t SpatialKindBits(SpatialKind kind) noexcept
{
    switch (kind) {
    case SpatialKind::Bayer4:    return 4u;
    case SpatialKind::Bayer8:    return 6u;
    case SpatialKind::BlueNoise: return 12u;
    }
    return 0u;
}

/* Threshold for pixel (x, y) in (0, 1).  `seed` shifts the blue-noise tile so
 * repeated loads can use different dither patterns without changing the
 * statistics; Bayer matrices are position-locked by definition. */
static inline float SpatialThreshold(SpatialKind kind, int x, int y, uint64_t seed) noexcept
{
    switch (kind) {
    case SpatialKind::Bayer4:
        return kBayer4x4[y & 3][x & 3] + 0.5f / 16.f;
    case SpatialKind::Bayer8:
        return (static_cast<float>(kBayer8x8Raw[y & 7][x & 7]) + 0.5f) / 64.f;
    case SpatialKind::BlueNoise: {
        const int sx = (x + static_cast<int>(seed & 63u)) & 63;
        const int sy = (y + static_cast<int>((seed >> 6) & 63u)) & 63;
        return (static_cast<float>(kBlueNoise64[sy][sx]) + 0.5f) / 4096.f;
    }
    }
    return 0.5f;
}

/* ── Target-format construction ──────────────────────────────────────────── */

/*
 * Build a packed interleaved UNORM IWImageFormat that matches the screen's
 * bit depth and uses RGBA (or fewer channels) semantics.
 */
inline IWImageFormat MakeTargetFormat(const IWScreenInfo& screen) noexcept
{
    IWImageFormat fmt = {};
    const uint16_t bw = screen.bitsPerChannel ? screen.bitsPerChannel : 8u;
    const uint16_t nc = (screen.channelCount >= 4u) ? 4u
                      : (screen.channelCount  > 0u) ? screen.channelCount
                      : 4u;
    fmt.componentCount = nc;
    fmt.bitsPerPixel   = static_cast<uint16_t>(nc * bw);
    fmt.storageLayout  = IW_STORAGE_INTERLEAVED;

    static const IWComponentSemantic kSemantics[4] = {
        IW_COMPONENT_SEMANTIC_R, IW_COMPONENT_SEMANTIC_G,
        IW_COMPONENT_SEMANTIC_B, IW_COMPONENT_SEMANTIC_A
    };
    for (uint16_t i = 0; i < nc; ++i) {
        fmt.components[i] = {
            kSemantics[i],
            IW_COMPONENT_CLASS_UNORM,
            static_cast<uint16_t>(i * bw),
            bw
        };
    }
    return fmt;
}

/* ── Internal helpers ────────────────────────────────────────────────────── */

/* Return bytes per pixel for a packed interleaved format. */
static inline int BytesPerPixel(const IWImageFormat& fmt) noexcept
{
    return static_cast<int>((fmt.bitsPerPixel + 7u) / 8u);
}

/*
 * Read one component already scaled to its class's normalised range.
 *
 * ExtractComponent() deliberately returns *raw integers* for
 * IW_COMPONENT_CLASS_UINT, so that unpacked integers (RGB565 fields, bit
 * masks, …) keep their numeric value.  A pipeline that consumes normalised
 * floats has to divide by the component's own maximum; otherwise every UINT
 * sample >= 1 collapses to 1.0 as soon as ExtractPixelF() clamps to [0, 1],
 * which turns a whole image white.
 *
 * A no-op for UNORM / SNORM / FLOAT components: only UINT is rescaled.
 */
static inline float ExtractComponentNormalised(const uint8_t* pixel,
                                                const IWComponentDef& comp) noexcept
{
    const float v = ExtractComponent(pixel, comp);
    if (comp.componentClass != IW_COMPONENT_CLASS_UINT || comp.bitWidth == 0u)
        return v;

    const uint64_t maxVal = (comp.bitWidth < 64u)
                            ? ((uint64_t{1} << comp.bitWidth) - 1u)
                            : ~uint64_t{0};
    return v / static_cast<float>(maxVal);
}

/*
 * Extract one pixel at (x, y) from src as normalised RGBA floats.
 * Missing channels default to 0 (RGB) or 1 (A).
 * UINT components are scaled by their declared bit width (see
 * ExtractComponentNormalised) so mislabelled input cannot saturate to 1.0.
 */
static inline void ExtractPixelF(const ImagePluginData& src,
                                  int x, int y, float rgba[4]) noexcept
{
    rgba[0] = 0.f; rgba[1] = 0.f; rgba[2] = 0.f; rgba[3] = 1.f;
    const IWImageFormat& fmt = src.format;
    const int bpp = BytesPerPixel(fmt);
    const uint8_t* pixel = src.data
                         + static_cast<ptrdiff_t>(y) * src.stride
                         + x * bpp;
    for (int c = 0; c < static_cast<int>(fmt.componentCount); ++c) {
        const float v = ExtractComponentNormalised(pixel, fmt.components[c]);
        switch (fmt.components[c].semantic) {
        case IW_COMPONENT_SEMANTIC_R:    rgba[0] = v;                    break;
        case IW_COMPONENT_SEMANTIC_G:    rgba[1] = v;                    break;
        case IW_COMPONENT_SEMANTIC_B:    rgba[2] = v;                    break;
        case IW_COMPONENT_SEMANTIC_A:    rgba[3] = v;                    break;
        case IW_COMPONENT_SEMANTIC_GRAY: rgba[0] = rgba[1] = rgba[2] = v; break;
        default:                                                           break;
        }
    }
    /* Normalise all channels to [0.0, 1.0] so downstream quantisers always
     * receive values in range (e.g. HDR floats > 1.0 are clipped to display). */
    for (int i = 0; i < 4; ++i)
        rgba[i] = std::max(0.f, std::min(1.f, rgba[i]));
}

/*
 * Map a channel index (0=R,1=G,2=B,3=A) in the target format's component
 * list to the corresponding value in rgba[4].
 */
static inline float GetComponentValue(const float rgba[4],
                                       IWComponentSemantic sem) noexcept
{
    switch (sem) {
    case IW_COMPONENT_SEMANTIC_R: return rgba[0];
    case IW_COMPONENT_SEMANTIC_G: return rgba[1];
    case IW_COMPONENT_SEMANTIC_B: return rgba[2];
    case IW_COMPONENT_SEMANTIC_A: return rgba[3];
    default:                      return 0.f;
    }
}

/*
 * Write a pre-computed integer quantised value into one component slot of
 * a packed-interleaved pixel. Supports arbitrary bit offsets/widths up to
 * 32 bits and preserves surrounding bits in packed formats.
 */
static inline void WriteBitsLE(uint8_t* pixelBase,
                               uint16_t bitOffset,
                               uint16_t bitWidth,
                               uint32_t value) noexcept
{
    if (bitWidth == 0) {
        return;
    }

    if (bitWidth < 32u) {
        value &= ((1u << bitWidth) - 1u);
    }

    uint32_t remaining = bitWidth;
    uint32_t valueShift = 0;
    uint32_t currentBit = bitOffset;

    while (remaining > 0u) {
        const uint32_t byteIndex = currentBit / 8u;
        const uint32_t bitInByte = currentBit % 8u;
        const uint32_t bitsThisByte = std::min(remaining, 8u - bitInByte);
        const uint32_t fieldMask = ((1u << bitsThisByte) - 1u) << bitInByte;
        const uint32_t fieldValue =
            ((value >> valueShift) & ((1u << bitsThisByte) - 1u)) << bitInByte;

        pixelBase[byteIndex] = static_cast<uint8_t>(
            (pixelBase[byteIndex] & ~fieldMask) | fieldValue);

        currentBit += bitsThisByte;
        valueShift += bitsThisByte;
        remaining -= bitsThisByte;
    }
}

static inline void WriteQuantizedComponent(uint8_t* pixelBase,
                                            const IWComponentDef& cd,
                                            uint32_t quantized) noexcept
{
    WriteBitsLE(pixelBase, cd.bitOffset, cd.bitWidth, quantized);
}

/* Round a normalised float to the nearest integer in [0, maxVal]. */
static inline uint32_t QuantizeRound(float v, uint32_t maxVal) noexcept
{
    v = std::max(0.f, std::min(1.f, v));
    const uint32_t q = static_cast<uint32_t>(v * static_cast<float>(maxVal) + 0.5f);
    return std::min(q, maxVal);
}

/*
 * Ordered (Bayer) quantization: floor(v * maxVal + bayer_threshold).
 * bayer_threshold is in [0, 1).
 */
static inline uint32_t QuantizeBayer(float v, uint32_t maxVal,
                                      float bayer) noexcept
{
    v = std::max(0.f, std::min(1.f, v));
    const float scaled = v * static_cast<float>(maxVal) + bayer;
    const uint32_t q = static_cast<uint32_t>(std::floor(scaled));
    return std::min(q, maxVal);
}

/* Allocate and initialise an output ImagePluginData frame (no pixel data). */
static inline ImagePluginData* AllocFrame(const ImagePluginData& src,
                                           const IWImageFormat& targetFmt)
{
    const int bpp    = BytesPerPixel(targetFmt);
    const int stride = src.width * bpp;
    const size_t sz  = static_cast<size_t>(src.height) * stride;

    uint8_t* buf = static_cast<uint8_t*>(std::malloc(sz));
    if (!buf) return nullptr;

    ImagePluginData* out = static_cast<ImagePluginData*>(
        std::malloc(sizeof(ImagePluginData)));
    if (!out) { std::free(buf); return nullptr; }

    out->width      = src.width;
    out->height     = src.height;
    out->stride     = stride;
    out->colorSpace = src.colorSpace;
    out->size       = sz;
    out->data       = buf;
    out->format     = targetFmt;
    return out;
}

/* ── Public API ──────────────────────────────────────────────────────────── */

/* Free a single output frame returned by any Apply* function. */
inline void FreeFrame(ImagePluginData* frame) noexcept
{
    if (frame) {
        std::free(frame->data);
        std::free(frame);
    }
}

/* Free an IWFilterImageSet and all its frames. */
inline void FreeImageSet(IWFilterImageSet* set) noexcept
{
    if (!set) return;
    if (set->frames) {
        for (uint32_t i = 0; i < set->frameCount; ++i)
            FreeFrame(set->frames[i]);
        std::free(set->frames);
    }
    std::free(set);
}

/*
 * ApplyNone — truncate / round to target bit depth, no dithering.
 * Returns a heap-allocated ImagePluginData; free with FreeFrame().
 * Returns nullptr on allocation failure.
 */
inline ImagePluginData* ApplyNone(const ImagePluginData& src,
                                   const IWImageFormat& targetFmt)
{
    ImagePluginData* out = AllocFrame(src, targetFmt);
    if (!out) return nullptr;

    const int bpp      = BytesPerPixel(targetFmt);
    const int nc       = static_cast<int>(targetFmt.componentCount);
    const uint32_t maxVal =
        (targetFmt.components[0].bitWidth < 32u)
        ? ((1u << targetFmt.components[0].bitWidth) - 1u)
        : 0xFFFFFFFFu;

    for (int y = 0; y < src.height; ++y) {
        uint8_t* row = out->data + static_cast<ptrdiff_t>(y) * out->stride;
        for (int x = 0; x < src.width; ++x) {
            float rgba[4];
            ExtractPixelF(src, x, y, rgba);
            uint8_t* pix = row + x * bpp;
            for (int c = 0; c < nc; ++c) {
                const float v = GetComponentValue(rgba, targetFmt.components[c].semantic);
                WriteQuantizedComponent(pix, targetFmt.components[c],
                                        QuantizeRound(v, maxVal));
            }
        }
    }
    return out;
}

/*
 * ApplyOrdered — Bayer 4×4 ordered dithering.
 * Alpha channel is not dithered (rounded instead).
 * Returns a heap-allocated ImagePluginData; free with FreeFrame().
 */
inline ImagePluginData* ApplyOrdered(const ImagePluginData& src,
                                      const IWImageFormat& targetFmt)
{
    ImagePluginData* out = AllocFrame(src, targetFmt);
    if (!out) return nullptr;

    const int bpp = BytesPerPixel(targetFmt);
    const int nc  = static_cast<int>(targetFmt.componentCount);
    const uint32_t maxVal =
        (targetFmt.components[0].bitWidth < 32u)
        ? ((1u << targetFmt.components[0].bitWidth) - 1u)
        : 0xFFFFFFFFu;

    for (int y = 0; y < src.height; ++y) {
        uint8_t* row = out->data + static_cast<ptrdiff_t>(y) * out->stride;
        for (int x = 0; x < src.width; ++x) {
            float rgba[4];
            ExtractPixelF(src, x, y, rgba);
            const float bayer = kBayer4x4[y & 3][x & 3];
            uint8_t* pix = row + x * bpp;
            for (int c = 0; c < nc; ++c) {
                const IWComponentDef& cd = targetFmt.components[c];
                const float v = GetComponentValue(rgba, cd.semantic);
                const uint32_t q =
                    (cd.semantic == IW_COMPONENT_SEMANTIC_A)
                    ? QuantizeRound(v, maxVal)
                    : QuantizeBayer(v, maxVal, bayer);
                WriteQuantizedComponent(pix, cd, q);
            }
        }
    }
    return out;
}

/*
 * ApplyFloydSteinberg — Floyd-Steinberg error diffusion.
 * Alpha channel is not dithered.
 * Returns a heap-allocated ImagePluginData; free with FreeFrame().
 */
inline ImagePluginData* ApplyFloydSteinberg(const ImagePluginData& src,
                                             const IWImageFormat& targetFmt)
{
    ImagePluginData* out = AllocFrame(src, targetFmt);
    if (!out) return nullptr;

    const int bpp = BytesPerPixel(targetFmt);
    const int nc  = static_cast<int>(targetFmt.componentCount);
    const uint32_t maxVal =
        (targetFmt.components[0].bitWidth < 32u)
        ? ((1u << targetFmt.components[0].bitWidth) - 1u)
        : 0xFFFFFFFFu;
    const float fMaxVal = static_cast<float>(maxVal);

    /* Per-row error buffers for R, G, B (4 slots: idx 0=R, 1=G, 2=B, 3=unused). */
    const int W = src.width;
    std::vector<float> errCurr(static_cast<size_t>(W) * 4, 0.f);
    std::vector<float> errNext(static_cast<size_t>(W) * 4, 0.f);

    /* Map semantic → error buffer slot (only R/G/B diffused; A rounded). */
    auto SlotOf = [](IWComponentSemantic sem) -> int {
        switch (sem) {
        case IW_COMPONENT_SEMANTIC_R: return 0;
        case IW_COMPONENT_SEMANTIC_G: return 1;
        case IW_COMPONENT_SEMANTIC_B: return 2;
        default:                      return -1;
        }
    };

    for (int y = 0; y < src.height; ++y) {
        std::fill(errNext.begin(), errNext.end(), 0.f);
        uint8_t* row = out->data + static_cast<ptrdiff_t>(y) * out->stride;

        for (int x = 0; x < W; ++x) {
            float rgba[4];
            ExtractPixelF(src, x, y, rgba);

            /* Add accumulated error. */
            rgba[0] += errCurr[x * 4 + 0];
            rgba[1] += errCurr[x * 4 + 1];
            rgba[2] += errCurr[x * 4 + 2];

            uint8_t* pix = row + x * bpp;
            for (int c = 0; c < nc; ++c) {
                const IWComponentDef& cd = targetFmt.components[c];
                float v = GetComponentValue(rgba, cd.semantic);
                const uint32_t q = QuantizeRound(v, maxVal);
                WriteQuantizedComponent(pix, cd, q);

                /* Propagate quantization error for colour channels. */
                const int slot = SlotOf(cd.semantic);
                if (slot < 0) continue; /* alpha: skip */
                v = std::max(0.f, std::min(1.f, v));
                const float err = v - static_cast<float>(q) / fMaxVal;

                if (x + 1 < W)
                    errCurr[(x + 1) * 4 + slot] += err * (7.f / 16.f);
                if (y + 1 < src.height) {
                    if (x > 0)
                        errNext[(x - 1) * 4 + slot] += err * (3.f / 16.f);
                    errNext[x       * 4 + slot] += err * (5.f / 16.f);
                    if (x + 1 < W)
                        errNext[(x + 1) * 4 + slot] += err * (1.f / 16.f);
                }
            }
        }
        std::swap(errCurr, errNext);
    }
    return out;
}

/*
 * ApplySpatial — single-frame spatial dither (Bayer 4×4, Bayer 8×8 or the
 * vendored 64×64 blue-noise tile).
 *
 * Same quantiser as ApplyOrdered but with the (rank + 0.5) / N² threshold
 * convention and a selectable field.  Over a tile the average equals the input
 * exactly (the thresholds are uniform in (0, 1)), so the extra source bits are
 * carried spatially rather than in time.
 *
 * Alpha is not dithered (rounded instead).
 */
inline ImagePluginData* ApplySpatial(const ImagePluginData& src,
                                     const IWImageFormat& targetFmt,
                                     SpatialKind kind, uint64_t seed)
{
    ImagePluginData* out = AllocFrame(src, targetFmt);
    if (!out) return nullptr;

    const int bpp = BytesPerPixel(targetFmt);
    const int nc  = static_cast<int>(targetFmt.componentCount);
    const uint32_t maxVal =
        (targetFmt.components[0].bitWidth < 32u)
        ? ((1u << targetFmt.components[0].bitWidth) - 1u)
        : 0xFFFFFFFFu;

    for (int y = 0; y < src.height; ++y) {
        uint8_t* row = out->data + static_cast<ptrdiff_t>(y) * out->stride;
        for (int x = 0; x < src.width; ++x) {
            float rgba[4];
            ExtractPixelF(src, x, y, rgba);
            const float s = SpatialThreshold(kind, x, y, seed);
            uint8_t* pix = row + x * bpp;
            for (int c = 0; c < nc; ++c) {
                const IWComponentDef& cd = targetFmt.components[c];
                const float v = GetComponentValue(rgba, cd.semantic);
                const uint32_t q =
                    (cd.semantic == IW_COMPONENT_SEMANTIC_A)
                    ? QuantizeRound(v, maxVal)
                    : QuantizeBayer(v, maxVal, s);
                WriteQuantizedComponent(pix, cd, q);
            }
        }
    }
    return out;
}

/*
 * ApplySpatialTemporal — two complementary phase frames.
 *
 * Each frame carries 8 bits per channel and the pair carries one more bit in
 * time, so the temporal average alone is a 9-bit (511-level) value.  The
 * caller's spatial budget is clamped to the resolution of the threshold field
 * (SpatialKindBits) and shapes the pattern, not the mean — see the note at the
 * end.
 *
 * Construction, per pixel and per colour channel:
 *
 *     q9   = round(v * 2*maxVal)     // 9-bit value, 0 .. 2*maxVal
 *     base = q9 >> 1                 // code both frames start from
 *     odd  = q9 & 1                  // the half-code the two frames trade
 *     p    = (s(x,y) < 0.5)          // complementary phase selector, 50 %
 *     A    = base + (odd ? p : 0)
 *     B    = base + (odd ? 1-p : 0)
 *
 * A + B = 2*base + odd = q9 at every pixel, so the temporal average is exactly
 * the smooth 9-bit value: the matrix pattern cancels between the two frames and
 * the average contains no residual lattice.  Each frame on its own carries one
 * half of the pattern, so the dither is present in every frame and inverts
 * between them.  Where q9 is even there is no half-code to trade and the two
 * frames are identical.
 *
 * Deliberate consequence: because the average must stay lattice-free, the
 * temporal pair cannot also carry sub-9-bit DC information — the spatial field
 * decides *which phase* shows the half-code, not the mean.  Modes that want the
 * spatial average to carry the extra source bits use ApplySpatial.
 *
 * Alpha is not dithered (rounded in both frames).
 */
inline IWFilterImageSet* ApplySpatialTemporal(const ImagePluginData& src,
                                              const IWImageFormat& targetFmt,
                                              SpatialKind kind, uint64_t seed)
{
    const int bpp    = BytesPerPixel(targetFmt);
    const int stride = src.width * bpp;
    const size_t fsz = static_cast<size_t>(src.height) * stride;
    const int nc     = static_cast<int>(targetFmt.componentCount);
    const uint32_t maxVal =
        (targetFmt.components[0].bitWidth < 32u)
        ? ((1u << targetFmt.components[0].bitWidth) - 1u)
        : 0xFFFFFFFFu;
    const uint32_t level9 = 2u * maxVal;

    IWFilterImageSet* set = static_cast<IWFilterImageSet*>(
        std::malloc(sizeof(IWFilterImageSet)));
    if (!set) return nullptr;
    set->frameCount = 2;
    set->frames = static_cast<ImagePluginData**>(
        std::malloc(sizeof(ImagePluginData*) * 2u));
    if (!set->frames) { std::free(set); return nullptr; }
    set->frames[0] = nullptr;
    set->frames[1] = nullptr;

    for (uint32_t f = 0; f < 2u; ++f) {
        uint8_t* buf = static_cast<uint8_t*>(std::malloc(fsz));
        if (!buf) { FreeImageSet(set); return nullptr; }
        ImagePluginData* pd = static_cast<ImagePluginData*>(
            std::malloc(sizeof(ImagePluginData)));
        if (!pd) { std::free(buf); FreeImageSet(set); return nullptr; }
        pd->width      = src.width;
        pd->height     = src.height;
        pd->stride     = stride;
        pd->colorSpace = src.colorSpace;
        pd->size       = fsz;
        pd->data       = buf;
        pd->format     = targetFmt;
        set->frames[f] = pd;
    }

    uint8_t* const out0 = set->frames[0]->data;
    uint8_t* const out1 = set->frames[1]->data;

    for (int y = 0; y < src.height; ++y) {
        for (int x = 0; x < src.width; ++x) {
            float rgba[4];
            ExtractPixelF(src, x, y, rgba);
            const bool pHigh = (SpatialThreshold(kind, x, y, seed) < 0.5f);

            uint8_t* pix0 = out0 + static_cast<ptrdiff_t>(y) * stride + x * bpp;
            uint8_t* pix1 = out1 + static_cast<ptrdiff_t>(y) * stride + x * bpp;

            for (int c = 0; c < nc; ++c) {
                const IWComponentDef& cd = targetFmt.components[c];
                const float v = GetComponentValue(rgba, cd.semantic);

                uint32_t a = 0, b = 0;
                if (cd.semantic == IW_COMPONENT_SEMANTIC_A) {
                    a = b = QuantizeRound(v, maxVal);
                } else {
                    const uint32_t q9   = QuantizeRound(v, level9);
                    const uint32_t base = q9 >> 1;
                    const uint32_t odd  = q9 & 1u;
                    a = base + ((odd != 0u &&  pHigh) ? 1u : 0u);
                    b = base + ((odd != 0u && !pHigh) ? 1u : 0u);
                }
                WriteQuantizedComponent(pix0, cd, a);
                WriteQuantizedComponent(pix1, cd, b);
            }
        }
    }

    return set;
}

/*
 * ApplyPWM — temporal / PWM dither.
 *
 * frameCount = 2^pwmBits frames are generated.  For each pixel, frame f
 * shows `hi` (= floor(v*maxVal) + 1) when f < round(frac * frameCount),
 * otherwise `lo` (= floor(v*maxVal)).  This produces the correct average
 * brightness across all frames.
 *
 * Alpha is not dithered.
 * Returns a heap-allocated IWFilterImageSet; free with FreeImageSet().
 */
inline IWFilterImageSet* ApplyPWM(const ImagePluginData& src,
                                   const IWImageFormat& targetFmt,
                                   uint32_t pwmBits)
{
    if (pwmBits == 0u) pwmBits = 1u;
    if (pwmBits > 8u)  pwmBits = 8u; /* cap at 256 frames */

    const uint32_t frameCount = 1u << pwmBits;
    const int bpp     = BytesPerPixel(targetFmt);
    const int stride  = src.width * bpp;
    const size_t fsz  = static_cast<size_t>(src.height) * stride;
    const int nc      = static_cast<int>(targetFmt.componentCount);
    const uint32_t maxVal =
        (targetFmt.components[0].bitWidth < 32u)
        ? ((1u << targetFmt.components[0].bitWidth) - 1u)
        : 0xFFFFFFFFu;

    /* Allocate set. */
    IWFilterImageSet* set = static_cast<IWFilterImageSet*>(
        std::malloc(sizeof(IWFilterImageSet)));
    if (!set) return nullptr;

    set->frameCount = frameCount;
    set->frames = static_cast<ImagePluginData**>(
        std::malloc(sizeof(ImagePluginData*) * frameCount));
    if (!set->frames) { std::free(set); return nullptr; }

    for (uint32_t f = 0; f < frameCount; ++f)
        set->frames[f] = nullptr;

    /* Allocate per-frame buffers. */
    for (uint32_t f = 0; f < frameCount; ++f) {
        uint8_t* buf = static_cast<uint8_t*>(std::malloc(fsz));
        if (!buf) { FreeImageSet(set); return nullptr; }

        ImagePluginData* pd = static_cast<ImagePluginData*>(
            std::malloc(sizeof(ImagePluginData)));
        if (!pd) { std::free(buf); FreeImageSet(set); return nullptr; }

        pd->width      = src.width;
        pd->height     = src.height;
        pd->stride     = stride;
        pd->colorSpace = src.colorSpace;
        pd->size       = fsz;
        pd->data       = buf;
        pd->format     = targetFmt;
        set->frames[f] = pd;
    }

    /* Fill per-pixel values across frames. */
    const float fFrameCount = static_cast<float>(frameCount);
    for (int y = 0; y < src.height; ++y) {
        for (int x = 0; x < src.width; ++x) {
            float rgba[4];
            ExtractPixelF(src, x, y, rgba);

            for (int c = 0; c < nc; ++c) {
                const IWComponentDef& cd = targetFmt.components[c];
                float v = GetComponentValue(rgba, cd.semantic);
                v = std::max(0.f, std::min(1.f, v));

                /* Alpha: just round, no temporal dither. */
                if (cd.semantic == IW_COMPONENT_SEMANTIC_A) {
                    const uint32_t q = QuantizeRound(v, maxVal);
                    for (uint32_t f = 0; f < frameCount; ++f) {
                        uint8_t* pix = set->frames[f]->data
                                     + static_cast<ptrdiff_t>(y) * stride
                                     + x * bpp;
                        WriteQuantizedComponent(pix, cd, q);
                    }
                    continue;
                }

                const float scaled  = v * static_cast<float>(maxVal);
                const uint32_t lo   = static_cast<uint32_t>(std::floor(scaled));
                const uint32_t hi   = std::min(lo + 1u, maxVal);
                const float frac    = scaled - static_cast<float>(lo);
                /* Number of frames that should show `hi`. */
                const uint32_t hiCount = static_cast<uint32_t>(
                    frac * fFrameCount + 0.5f);

                for (uint32_t f = 0; f < frameCount; ++f) {
                    const uint32_t q = (f < hiCount) ? hi : lo;
                    uint8_t* pix = set->frames[f]->data
                                 + static_cast<ptrdiff_t>(y) * stride
                                 + x * bpp;
                    WriteQuantizedComponent(pix, cd, q);
                }
            }
        }
    }
    return set;
}

} /* namespace FilterCore */
