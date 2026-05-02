module;
/*
 * FilterPluginPixman.ixx — filter plugin backed by pixman
 *
 * Compiled as a SHARED library.  Exports the three C symbols expected by
 * FilterPlugin:
 *   FilterImage            — convert / dither an image to screen precision
 *   FreeFilterImageSet     — release memory
 *   FilterPluginGetLastError — last error string
 *
 * Strategy
 * ────────
 * For common packed-integer source formats (RGBA8, BGRA8, RGB8, …) that
 * pixman understands natively:
 *   • NONE dither  – use pixman_image_composite32(PIXMAN_OP_SRC) for fast
 *                    format conversion.
 *   • ORDERED      – add a tiled 4×4 Bayer noise image via PIXMAN_OP_ADD
 *                    on the 8-bit output, then clamp.  Meaningful for 8→8
 *                    passthrough; higher-precision source paths fall through
 *                    to FilterCore.
 *
 * For all other source formats (16-bit, float, planar, exotic semantics)
 * and for Floyd-Steinberg / PWM, the implementation falls back to
 * FilterCore, which operates in normalised float space.
 */

#include <cstdlib>
#include <cstring>
#include <cmath>
#include <cstdio>
#include <algorithm>

#include "ImagePluginDef.h"
#include "FilterPluginDef.h"
#include "Utils.hpp"
#include "FilterCore.hpp"
#include <pixman.h>

export module FilterPluginPixman;

/* ── Thread-local error string ───────────────────────────────────────────── */
static thread_local char g_lastError[512] = "";

static void SetError(const char* msg) noexcept
{
    std::snprintf(g_lastError, sizeof(g_lastError), "%s", msg);
}

/* ── pixman format mapping ───────────────────────────────────────────────── */
/*
 * Map a supported IWImageFormat to the corresponding pixman_format_code_t.
 *
 * Mapping rationale (little-endian memory layout):
 *
 *  IWImageFormat RGBA8 UNORM   R@0 G@8 B@16 A@24
 *    → uint32 LE value: R | (G<<8) | (B<<16) | (A<<24) = 0xAABBGGRR
 *    → pixman ABGR type (MSB=A, …, LSB=R) → PIXMAN_a8b8g8r8
 *
 *  IWImageFormat BGRA8 UNORM   B@0 G@8 R@16 A@24
 *    → uint32 LE: B|(G<<8)|(R<<16)|(A<<24) = 0xAARRGGBB → PIXMAN_a8r8g8b8
 *
 *  IWImageFormat RGB8  UNORM   R@0 G@8 B@16
 *    → uint24 LE: R|(G<<8)|(B<<16) — pixman type BGR (MSB=B) → PIXMAN_b8g8r8
 *
 *  IWImageFormat BGR8  UNORM   B@0 G@8 R@16
 *    → uint24 LE: B|(G<<8)|(R<<16) — pixman type RGB (MSB=R) → PIXMAN_r8g8b8
 *
 *  GRAY8 UNORM → PIXMAN_g8 (if available; defined in pixman 0.38+)
 *
 *  Returns PIXMAN_FORMAT(0,0,0,0,0,0) (== 0) when not mappable.
 */
static pixman_format_code_t GetPixmanFormat(const IWImageFormat& fmt) noexcept
{
    if (fmt.storageLayout != IW_STORAGE_INTERLEAVED) return (pixman_format_code_t)0;

    const uint16_t nc = fmt.componentCount;
    if (nc == 0 || nc > 4) return (pixman_format_code_t)0;

    /* All components must share the same bit width and class. */
    const uint16_t bw  = fmt.components[0].bitWidth;
    const uint32_t cls = fmt.components[0].componentClass;
    if (cls != IW_COMPONENT_CLASS_UNORM && cls != IW_COMPONENT_CLASS_UINT)
        return (pixman_format_code_t)0;
    for (uint16_t i = 1; i < nc; ++i)
        if (fmt.components[i].bitWidth != bw ||
            fmt.components[i].componentClass != cls)
            return (pixman_format_code_t)0;

    auto s = [&](int i) { return fmt.components[i].semantic; };

    if (bw == 8u) {
        if (nc == 4 &&
            s(0)==IW_COMPONENT_SEMANTIC_R && s(1)==IW_COMPONENT_SEMANTIC_G &&
            s(2)==IW_COMPONENT_SEMANTIC_B && s(3)==IW_COMPONENT_SEMANTIC_A)
            return PIXMAN_a8b8g8r8;

        if (nc == 4 &&
            s(0)==IW_COMPONENT_SEMANTIC_B && s(1)==IW_COMPONENT_SEMANTIC_G &&
            s(2)==IW_COMPONENT_SEMANTIC_R && s(3)==IW_COMPONENT_SEMANTIC_A)
            return PIXMAN_a8r8g8b8;

        if (nc == 3 &&
            s(0)==IW_COMPONENT_SEMANTIC_R && s(1)==IW_COMPONENT_SEMANTIC_G &&
            s(2)==IW_COMPONENT_SEMANTIC_B)
            return PIXMAN_b8g8r8;

        if (nc == 3 &&
            s(0)==IW_COMPONENT_SEMANTIC_B && s(1)==IW_COMPONENT_SEMANTIC_G &&
            s(2)==IW_COMPONENT_SEMANTIC_R)
            return PIXMAN_r8g8b8;

#ifdef PIXMAN_g8
        if (nc == 1 && s(0)==IW_COMPONENT_SEMANTIC_GRAY) return PIXMAN_g8;
#endif
    }

    return (pixman_format_code_t)0;
}

/* ── pixman-accelerated helpers ──────────────────────────────────────────── */

/*
 * Use pixman_image_composite32(PIXMAN_OP_SRC) to copy / convert between two
 * compatible 8-bit packed-integer formats.
 *
 * Returns true on success; false if either format is not pixman-mappable or
 * a pixman allocation fails (caller should fall back to FilterCore).
 */
static bool PixmanConvert(const ImagePluginData& src,
                           ImagePluginData*       dst) noexcept
{
    const pixman_format_code_t sf = GetPixmanFormat(src.format);
    const pixman_format_code_t df = GetPixmanFormat(dst->format);
    if (sf == (pixman_format_code_t)0 || df == (pixman_format_code_t)0)
        return false;

    pixman_image_t* si = pixman_image_create_bits(
        sf, src.width, src.height,
        reinterpret_cast<uint32_t*>(src.data), src.stride);
    if (!si) return false;

    pixman_image_t* di = pixman_image_create_bits(
        df, dst->width, dst->height,
        reinterpret_cast<uint32_t*>(dst->data), dst->stride);
    if (!di) { pixman_image_unref(si); return false; }

    pixman_image_composite32(PIXMAN_OP_SRC,
                              si, nullptr, di,
                              0, 0, 0, 0, 0, 0,
                              src.width, src.height);

    pixman_image_unref(si);
    pixman_image_unref(di);
    return true;
}

/*
 * pixman-accelerated Bayer ordered dither for 8-bit RGBA → 8-bit RGBA.
 *
 * Step 1: copy src → dst with PixmanConvert (PIXMAN_OP_SRC).
 * Step 2: create a tiling 4×4 Bayer noise image in PIXMAN_a8b8g8r8 format.
 * Step 3: pixman_image_composite32(PIXMAN_OP_ADD, noiseImg, _, dst, …) adds
 *         per-channel noise; saturating addition gives the dithered values.
 *
 * The Bayer noise is scaled so that each component gets max +1 extra count
 * relative to a uniform rounding threshold.  For 8-bit → 8-bit this is a
 * passthrough with sub-LSB noise.  For higher source precision the caller
 * should use FilterCore::ApplyOrdered instead.
 *
 * Returns true on success.
 */
static bool PixmanOrderedDither(const ImagePluginData& src,
                                 ImagePluginData*       dst) noexcept
{
    if (!PixmanConvert(src, dst)) return false;

    const pixman_format_code_t df = GetPixmanFormat(dst->format);
    if (df == (pixman_format_code_t)0) return false;

    /*
     * Build a 4×4 Bayer noise tile in RGBA8 (PIXMAN_a8b8g8r8) memory layout.
     * Noise value for each matrix entry n = round(Bayer[r][c] * 255 / 16) so
     * that it fits in one 8-bit channel, scaled to one LSB.
     * We use the same value for R/G/B; alpha noise is zero.
     */
    uint8_t bayerTile[4 * 4 * 4]; /* 4 rows × 4 pixels × 4 bytes (RGBA8) */
    static const uint8_t kBayerIdx[4][4] = {
        {  0,  8,  2, 10 },
        { 12,  4, 14,  6 },
        {  3, 11,  1,  9 },
        { 15,  7, 13,  5 }
    };
    for (int r = 0; r < 4; ++r) {
        for (int c = 0; c < 4; ++c) {
            const uint8_t n = static_cast<uint8_t>(
                (kBayerIdx[r][c] * 255u + 8u) / 16u);
            /* PIXMAN_a8b8g8r8: byte order [R, G, B, A] in memory. */
            uint8_t* p = bayerTile + (r * 4 + c) * 4;
            p[0] = n; /* R */
            p[1] = n; /* G */
            p[2] = n; /* B */
            p[3] = 0; /* A — no noise on alpha */
        }
    }

    pixman_image_t* noiseImg = pixman_image_create_bits(
        PIXMAN_a8b8g8r8, 4, 4,
        reinterpret_cast<uint32_t*>(bayerTile), 4 * 4 /* stride in bytes */);
    if (!noiseImg) return false;
    pixman_image_set_repeat(noiseImg, PIXMAN_REPEAT_NORMAL);

    pixman_image_t* di = pixman_image_create_bits(
        df, dst->width, dst->height,
        reinterpret_cast<uint32_t*>(dst->data), dst->stride);
    if (!di) { pixman_image_unref(noiseImg); return false; }

    pixman_image_composite32(PIXMAN_OP_ADD,
                              noiseImg, nullptr, di,
                              0, 0, 0, 0, 0, 0,
                              dst->width, dst->height);

    pixman_image_unref(noiseImg);
    pixman_image_unref(di);
    return true;
}

/* ── Helpers shared between FilterImage paths ────────────────────────────── */

/*
 * Decide whether the source image needs bit-depth reduction for the given
 * screen info.  Returns false when the source already fits (no downsampling
 * needed), true when FilterCore or pixman conversion is needed.
 */
static bool NeedsDownsample(const ImagePluginData& src,
                              const IWScreenInfo&   screen) noexcept
{
    /* If all source components are <= screen bits, no downsampling needed. */
    for (uint16_t i = 0; i < src.format.componentCount; ++i) {
        if (src.format.components[i].bitWidth > screen.bitsPerChannel)
            return true;
    }
    return false;
}

/* ── Exported plugin symbols ─────────────────────────────────────────────── */

export extern "C" FILTERPLUGIN_API
IWFilterImageSet* FilterImage(const ImagePluginData*  src,
                               const IWScreenInfo*     screen,
                               const IWFilterOptions*  options)
{
    g_lastError[0] = '\0';

    if (!src || !screen || !options) {
        SetError("FilterImage: null argument");
        return nullptr;
    }

    const IWImageFormat targetFmt = FilterCore::MakeTargetFormat(*screen);

    /* ── PWM path (always uses FilterCore) ────────────────────────────── */
    if (options->ditherMode == IW_DITHER_PWM) {
        IWFilterImageSet* set =
            FilterCore::ApplyPWM(*src, targetFmt, options->pwmBits);
        if (!set) SetError("FilterImage: PWM allocation failed");
        return set;
    }

    /* ── Single-frame paths ───────────────────────────────────────────── */
    IWFilterImageSet* set = static_cast<IWFilterImageSet*>(
        std::malloc(sizeof(IWFilterImageSet)));
    if (!set) { SetError("FilterImage: allocation failed"); return nullptr; }

    set->frames = static_cast<ImagePluginData**>(
        std::malloc(sizeof(ImagePluginData*)));
    if (!set->frames) {
        std::free(set);
        SetError("FilterImage: allocation failed");
        return nullptr;
    }
    set->frameCount  = 1u;
    set->frames[0]   = nullptr;

    ImagePluginData* frame = nullptr;

    switch (options->ditherMode) {

    case IW_DITHER_NONE: {
        /* Fast path: try pixman format conversion when source is
         * pixman-compatible and no bit depth reduction is needed. */
        if (!NeedsDownsample(*src, *screen) &&
            GetPixmanFormat(src->format) != (pixman_format_code_t)0)
        {
            frame = FilterCore::AllocFrame(*src, targetFmt);
            if (frame && !PixmanConvert(*src, frame)) {
                /* pixman failed; fall back */
                FilterCore::FreeFrame(frame);
                frame = nullptr;
            }
        }
        if (!frame)
            frame = FilterCore::ApplyNone(*src, targetFmt);
        break;
    }

    case IW_DITHER_ORDERED: {
        /* Try pixman Bayer on 8-bit → 8-bit paths; fall back to FilterCore. */
        if (!NeedsDownsample(*src, *screen) &&
            GetPixmanFormat(src->format) != (pixman_format_code_t)0)
        {
            frame = FilterCore::AllocFrame(*src, targetFmt);
            if (frame && !PixmanOrderedDither(*src, frame)) {
                FilterCore::FreeFrame(frame);
                frame = nullptr;
            }
        }
        if (!frame)
            frame = FilterCore::ApplyOrdered(*src, targetFmt);
        break;
    }

    case IW_DITHER_FLOYD_STEINBERG:
        frame = FilterCore::ApplyFloydSteinberg(*src, targetFmt);
        break;

    default:
        frame = FilterCore::ApplyNone(*src, targetFmt);
        break;
    }

    if (!frame) {
        std::free(set->frames);
        std::free(set);
        SetError("FilterImage: dither allocation failed");
        return nullptr;
    }

    set->frames[0] = frame;
    return set;
}

export extern "C" FILTERPLUGIN_API
void FreeFilterImageSet(IWFilterImageSet* set)
{
    FilterCore::FreeImageSet(set);
}

export extern "C" FILTERPLUGIN_API
const char* FilterPluginGetLastError(void)
{
    return g_lastError;
}
