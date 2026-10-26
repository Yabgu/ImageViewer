module;

#include <cstdint>
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

/*
 * FilterPluginPixman.ixx — filter plugin backed by pixman
 *
 * Compiled as a SHARED library.  Exports the three C symbols expected by
 * FilterPlugin:
 *   FilterImage            — convert / dither an image to screen precision
 *   FreeFilterImageSet     — release memory
 *   FilterPluginGetLastError — last error string
 *
 * Named arguments recognised by this plugin (all optional):
 *
 *   "mode"     uint32  0=none (default), 1=ordered, 2=floyd_steinberg, 3=pwm
 *   "pwm_bits" uint32  extra virtual bits for PWM mode (default 1, max 8)
 *
 * Strategy
 * ────────
 * For common packed-integer source formats (RGBA8, BGRA8, RGB8, …) that
 * pixman understands natively:
 *   • mode=0 (none)    – use pixman_image_composite32(PIXMAN_OP_SRC) for fast
 *                        format conversion.
 *   • mode=1 (ordered) – add a tiled 4×4 Bayer noise image via PIXMAN_OP_ADD
 *                        on the 8-bit output, then clamp.
 *
 * For all other source formats (16-bit, float, planar, exotic semantics)
 * and for mode=2 (floyd_steinberg) / mode=3 (pwm), the implementation falls
 * back to FilterCore, which operates in normalised float space.
 *
 * Internal mode constants (NOT part of the public ABI):
 */
static constexpr uint32_t kModeNone            = 0u;
static constexpr uint32_t kModeOrdered         = 1u;
static constexpr uint32_t kModeFloydSteinberg  = 2u;
static constexpr uint32_t kModePWM             = 3u;
/*
 * Hosts pass named IWFilterArg entries to select behaviour; these integer
 * values are internal to FilterPluginPixman and are not exposed in any header.
 */

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
 * TODO(stride-guard-follow-up): mode=none reaches this function without the
 * 4-byte stride check that IsExactCodeMapping() applies to the ordered path;
 * move the guard in here so both callers inherit it.
 */
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
 * DELIBERATE DESIGN DECISION — DO NOT RESTORE A DITHER TILE HERE.
 *
 * At equal source/destination precision (the only case this fast path ever
 * sees) the correct ordered-dither result is an exact copy, so
 * PixmanOrderedDither() intentionally applies no tile and performs a plain
 * conversion.  This is not an unfinished optimisation and not a TODO.
 *
 * The measurement that settled it: the previous implementation added a 4x4
 * tile of n = (kBayerIdx * 255 + 8) / 16, i.e. the 16 values
 *   0, 16, 32, 48, 64, 80, 96, 112, 128, 143, 159, 175, 191, 207, 223, 239
 * whose mean is 1913/16 = +119.6 codes.  For mid-grey (128) that lifted the
 * eight cells with n >= 128 up to the 255 clamp and gave a 4x4 mean of 219.5
 * (0.8608 normalised) instead of 128 (0.5020).  That is a brightness lift, not
 * dithering.  Any tile that is not identically zero at this precision either
 * repeats that bias (>= 1 LSB) or rounds away in integer destination
 * arithmetic (< 1 LSB), so zero is the only correct choice.
 *
 * Sub-LSB dithering requires the source to carry more precision than the
 * display.  That belongs to FilterCore::ApplyOrdered(), which is exactly what
 * this function defers to by returning false.
 */

/*
 * True when pixman can reproduce `src` in `dst` as an exact 1:1 code mapping.
 *
 * Two conditions are required:
 *   • every active source component carries an already-normalised display code
 *     (IW_COMPONENT_CLASS_UNORM);
 *   • each of those components uses the same bit width in source and
 *     destination, so no rescaling between quantisation steps is involved; and
 *   • both row strides are 4-byte aligned, which pixman_image_create_bits()
 *     requires (it asserts rowstride_bytes % sizeof(uint32_t) == 0).  RGB8
 *     widths not divisible by 4 produce unaligned strides; those images are
 *     routed to FilterCore rather than handed to pixman on an undefined
 *     contract.
 *
 * Raw integers (IW_COMPONENT_CLASS_UINT) are deliberately excluded: their
 * numeric scale is 2^bitWidth - 1, so mapping them onto display codes needs a
 * division that pixman's fixed-function compositing cannot express here.  Such
 * inputs are deferred to FilterCore, which normalises each component by its
 * declared width (ExtractComponentNormalised) before quantising.
 */
static bool IsExactCodeMapping(const ImagePluginData& src,
                               const ImagePluginData& dst) noexcept
{
    if (src.format.storageLayout != IW_STORAGE_INTERLEAVED ||
        dst.format.storageLayout != IW_STORAGE_INTERLEAVED)
        return false;

    /* pixman_image_create_bits() requires 4-byte-aligned row strides.  Both
     * sides are checked: a 4-component source can have an aligned stride while
     * a 3-component destination of the same width does not (and vice versa). */
    if ((src.stride % 4) != 0 || (dst.stride % 4) != 0)
        return false;

    const uint16_t n  = src.format.componentCount;
    const uint16_t dn = dst.format.componentCount;
    if (n == 0u || dn == 0u || n > IW_MAX_COMPONENTS)
        return false;

    for (uint16_t i = 0; i < n; ++i) {
        if (src.format.components[i].componentClass != IW_COMPONENT_CLASS_UNORM)
            return false;
        /* A component the destination does not carry (e.g. alpha dropped for a
         * 3-channel screen) is handled by pixman's conversion; skip it. */
        if (i < dn &&
            src.format.components[i].bitWidth != dst.format.components[i].bitWidth)
            return false;
    }
    return true;
}

/*
 * pixman-accelerated ordered (Bayer) path.  Applies no tile — see the
 * DELIBERATE DESIGN DECISION note above IsExactCodeMapping() for why exact
 * conversion at equal precision is correct and must not be "fixed" back.
 *
 * This path is only reachable when the source and destination quantisation
 * steps are identical (GetPixmanFormat() accepts 8-bit packed formats only,
 * and anything wider than the screen has already gone to FilterCore via
 * NeedsDownsample()), so the mathematically correct ordered-dither result is
 * the input itself: there are no fractional bits below the destination's LSB
 * for a Bayer threshold to round.
 *
 * Returns true when `dst` now holds the converted image.
 */
static bool PixmanOrderedDither(const ImagePluginData& src,
                                 ImagePluginData*       dst) noexcept
{
    /* Raw integers and mixed quantisation steps need numeric interpretation;
     * hand those to FilterCore instead of compositing them as display codes. */
    if (!IsExactCodeMapping(src, *dst))
        return false;

    /* Exact 1:1 conversion — no tile, no added noise. */
    return PixmanConvert(src, dst);
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

/*
 * Look up a named uint32 argument from the options argv list.
 * Returns defaultVal when the argument is not present or has the wrong type.
 */
static uint32_t GetArgU32(const IWFilterOptions* opts,
                           const char*            name,
                           uint32_t               defaultVal) noexcept
{
    if (!opts || !opts->argv) return defaultVal;
    for (uint32_t i = 0; i < opts->argc; ++i) {
        const IWFilterArg& a = opts->argv[i];
        if (std::strcmp(a.name, name) == 0) {
            if (a.type == IW_FILTER_ARG_UINT32) return a.value.u32;
            break;
        }
    }
    return defaultVal;
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

    if (options->version != IW_FILTER_OPTIONS_VERSION) {
        SetError("FilterImage: unsupported options version");
        return nullptr;
    }

    const IWImageFormat targetFmt = FilterCore::MakeTargetFormat(*screen);

    /* Read named arguments (all optional; defaults shown in comments). */
    const uint32_t mode    = GetArgU32(options, "mode",     kModeNone);
    const uint32_t pwmBits = GetArgU32(options, "pwm_bits", 1u);

    /* ── PWM path (always uses FilterCore) ────────────────────────────── */
    if (mode == kModePWM) {
        IWFilterImageSet* set =
            FilterCore::ApplyPWM(*src, targetFmt, pwmBits);
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

    if (mode == kModeNone) {
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

    } else if (mode == kModeOrdered) {
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

    } else if (mode == kModeFloydSteinberg) {
        frame = FilterCore::ApplyFloydSteinberg(*src, targetFmt);

    } else {
        /* Unknown mode: report through the plugin error contract instead of
         * silently degrading to no dither (which made a typo'd mode look like
         * a successful pass-through to the caller). */
        std::free(set->frames);
        std::free(set);
        char msg[96];
        std::snprintf(msg, sizeof(msg), "FilterImage: unknown mode %u", mode);
        SetError(msg);
        return nullptr;
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
