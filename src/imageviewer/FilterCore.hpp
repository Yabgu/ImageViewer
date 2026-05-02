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

namespace FilterCore {

/* ── 4×4 Bayer threshold matrix (values in [0, 1)) ──────────────────────── */
static constexpr float kBayer4x4[4][4] = {
    {  0.f/16,  8.f/16,  2.f/16, 10.f/16 },
    { 12.f/16,  4.f/16, 14.f/16,  6.f/16 },
    {  3.f/16, 11.f/16,  1.f/16,  9.f/16 },
    { 15.f/16,  7.f/16, 13.f/16,  5.f/16 }
};

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
 * Extract one pixel at (x, y) from src as normalised RGBA floats.
 * Missing channels default to 0 (RGB) or 1 (A).
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
        const float v = ExtractComponent(pixel, fmt.components[c]);
        switch (fmt.components[c].semantic) {
        case IW_COMPONENT_SEMANTIC_R:    rgba[0] = v;                    break;
        case IW_COMPONENT_SEMANTIC_G:    rgba[1] = v;                    break;
        case IW_COMPONENT_SEMANTIC_B:    rgba[2] = v;                    break;
        case IW_COMPONENT_SEMANTIC_A:    rgba[3] = v;                    break;
        case IW_COMPONENT_SEMANTIC_GRAY: rgba[0] = rgba[1] = rgba[2] = v; break;
        default:                                                           break;
        }
    }
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
