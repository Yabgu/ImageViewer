/*
 * test_filterpixman.cpp — ABI conformance tests for the filter/dither plugin.
 *
 * These drive the built libFilterPixman.so / FilterPixman.dll through its
 * exported C ABI and encode the matrix that was previously checked by hand:
 *
 *     mode   ∈ {none, ordered, floyd, pwm}
 *   × screen ∈ {8-bit, 10-bit}
 *   × class  ∈ {UNORM, UINT}
 *
 * with an RGB8 4x4 flat mid-grey (128) source.  Every row must come back
 * mid-grey: this is the regression that the old additive Bayer tile broke, where
 * mid-grey reached a 4x4 mean of 0.8608 because a 0..239 tile was added to an
 * already-quantised destination.
 *
 * The UINT rows matter independently of the UNORM ones: FilterCore used to clamp
 * raw 0..255 samples to 1.0, so a UINT-labelled input rendered white.  Both the
 * loader tagging fix and the FilterCore normalisation are observable here.
 */

#include <doctest/doctest.h>

#include <cmath>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <string>
#include <vector>

#include "plugin_abi.hpp"
#include "Args.hpp"   /* host-side IWFilterMode: the values the CLI sends */

#ifndef IW_FILTERPIXMAN_PLUGIN_PATH
#  error "IW_FILTERPIXMAN_PLUGIN_PATH must be defined by the build system"
#endif

namespace {

const std::filesystem::path kPluginPath =
    iwabi::DeployedPluginPath(std::filesystem::path{ IW_FILTERPIXMAN_PLUGIN_PATH });

/* Loaded once per binary; see the note in test_imageloader_jpeg.cpp about the
 * DSO's static-initialisation allocations that dlclose cannot reclaim. */
iwabi::FilterPlugin& Plugin()
{
    static iwabi::FilterPlugin plugin{ kPluginPath };
    return plugin;
}

/*
 * The modes are named with the host's own enumerators (Args.hpp) rather than
 * private copies, so this suite verifies the mapping the CLI actually sends.
 * If the plugin's internal mode numbering ever changed, these cases would fail:
 * the assertions below all check behaviour, not just that a call succeeded.
 */
const char* ModeName(IWFilterMode mode)
{
    switch (mode) {
    case kModeNone:           return "none";
    case kModeOrdered:        return "ordered";
    case kModeFloydSteinberg: return "floyd";
    case kModePWM:            return "pwm";
    default:                  return "?";
    }
}

/* Mid-grey as an 8-bit code, and the fraction it represents. */
constexpr uint8_t kMidCode = 128;
constexpr double  kMidFrac = kMidCode / 255.0;   /* ≈ 0.50196 */
constexpr double  kTol     = 1.0 / 255.0;        /* ±1 code */

/* Owns a 4x4 RGB8 source buffer and the descriptor that describes it. */
struct Rgb8Source
{
    std::vector<uint8_t> pixels;
    ImagePluginData      data{};

    Rgb8Source(int w, int h, uint8_t value, IWComponentClass cls)
        : pixels(static_cast<size_t>(w) * static_cast<size_t>(h) * 3u, value)
    {
        data.width      = w;
        data.height     = h;
        data.stride     = w * 3;
        data.colorSpace = IMAGE_COLOR_SPACE_SRGB;
        data.size       = pixels.size();
        data.data       = pixels.data();

        data.format = IWImageFormat{};
        data.format.componentCount = 3;
        data.format.bitsPerPixel   = 24;
        data.format.storageLayout  = IW_STORAGE_INTERLEAVED;
        data.format.components[0]  = { IW_COMPONENT_SEMANTIC_R, cls,  0, 8 };
        data.format.components[1]  = { IW_COMPONENT_SEMANTIC_G, cls,  8, 8 };
        data.format.components[2]  = { IW_COMPONENT_SEMANTIC_B, cls, 16, 8 };
    }
};

/* Build the options structs for a mode.  pwm_bits=1 keeps PWM at 2 frames. */
struct FilterArgs
{
    IWFilterArg     args[2]{};
    IWFilterOptions options{};

    explicit FilterArgs(uint32_t mode)
    {
        std::strcpy(args[0].name, "mode");
        args[0].type      = IW_FILTER_ARG_UINT32;
        args[0].value.u32 = mode;

        std::strcpy(args[1].name, "pwm_bits");
        args[1].type      = IW_FILTER_ARG_UINT32;
        args[1].value.u32 = 1u;

        options.version = IW_FILTER_OPTIONS_VERSION;
        options.argc    = 2u;
        options.argv    = args;
    }
};

/* Min / max / mean of one channel across every pixel of every frame. */
struct ChannelStats
{
    double min = 1e9;
    double max = -1e9;
    double mean = 0.0;
    size_t count = 0;

    void add(double v)
    {
        if (v < min) min = v;
        if (v > max) max = v;
        mean += v;
        ++count;
    }
    double average() const { return count ? mean / static_cast<double>(count) : 0.0; }
};

} // namespace

TEST_SUITE("FilterPixman ABI: dithering matrix keeps mid-grey mid-grey")
{
    TEST_CASE("every mode x screen depth x input class keeps mid-grey mid-grey")
    {
    /* mode × screen depth × input component class, RGB8 4x4 flat mid-grey. */
    for (IWFilterMode mode : { kModeNone, kModeOrdered, kModeFloydSteinberg, kModePWM }) {
        for (int depth : { 8, 10 }) {
            for (int clsIdx : { 0, 1 }) {
                const IWComponentClass cls = clsIdx == 0 ? IW_COMPONENT_CLASS_UNORM
                                                         : IW_COMPONENT_CLASS_UINT;

                const std::string label = std::string("mode=") + ModeName(mode) +
                                          " screen=" + std::to_string(depth) +
                                          "bit input=" + (clsIdx == 0 ? "UNORM" : "UINT");

                {
                    CAPTURE(label);
                    Rgb8Source src(4, 4, kMidCode, cls);
                    IWScreenInfo screen{ static_cast<uint8_t>(depth), 3 };
                    FilterArgs   opts(static_cast<uint32_t>(mode));

                    iwabi::FilterPlugin& plugin = Plugin();
                    IWFilterImageSet* set = plugin.Filter(&src.data, &screen, &opts.options);

                    INFO("plugin error: " << plugin.lastError());
                    REQUIRE(set != nullptr);
                    REQUIRE(set->frameCount >= 1u);
                    REQUIRE(set->frames != nullptr);

                    /* PWM is the only multi-frame mode at pwm_bits=1. */
                    CHECK(set->frameCount == (mode == kModePWM ? 2u : 1u));

                    for (uint32_t f = 0; f < set->frameCount; ++f) {
                        REQUIRE(set->frames[f] != nullptr);
                        const ImagePluginData& out = *set->frames[f];

                        REQUIRE(out.format.componentCount == 3);

                        ChannelStats stats;
                        for (int y = 0; y < out.height; ++y)
                            for (int x = 0; x < out.width; ++x)
                                for (int c = 0; c < 3; ++c)
                                    stats.add(iwabi::ComponentFraction(out, c, x, y));

                        INFO("frame " << f << " avg=" << stats.average()
                                      << " min=" << stats.min << " max=" << stats.max);

                        /* The regression: the 4x4 mean must be mid-grey, not 0.86. */
                        CHECK(std::fabs(stats.average() - kMidFrac) <= kTol);
                        /* Per-pixel spread stays within one code of the mean. */
                        CHECK(stats.min >= kMidFrac - kTol);
                        CHECK(stats.max <= kMidFrac + kTol);
                        /* And nothing saturated to full scale. */
                        CHECK(stats.max < 1.0);
                    }

                    plugin.Free(set);
                }
            }
        }
    }
    }
}

TEST_SUITE("FilterPixman ABI: exactly representable values get no added noise")
{
    TEST_CASE("flat black and flat white survive every mode byte-identically")
    {
    /* 8-bit in → 8-bit out has no fractional bits to dither, so a flat field
     * must survive every mode untouched.  The old additive tile failed this:
     * white stayed white but mid-grey and every other code were lifted. */
    for (IWFilterMode mode : { kModeNone, kModeOrdered, kModeFloydSteinberg, kModePWM }) {
        for (uint8_t value : { uint8_t{0}, uint8_t{255} }) {
            const std::string label = std::string("mode=") + ModeName(mode) +
                                      " value=" + std::to_string(value);

            {
                CAPTURE(label);
                Rgb8Source src(4, 4, value, IW_COMPONENT_CLASS_UNORM);
                IWScreenInfo screen{ 8, 3 };
                FilterArgs   opts(static_cast<uint32_t>(mode));

                iwabi::FilterPlugin& plugin = Plugin();
                IWFilterImageSet* set = plugin.Filter(&src.data, &screen, &opts.options);
                REQUIRE(set != nullptr);
                REQUIRE(set->frames != nullptr);

                const uint8_t expected = value;
                for (uint32_t f = 0; f < set->frameCount; ++f) {
                    REQUIRE(set->frames[f] != nullptr);
                    const ImagePluginData& out = *set->frames[f];
                    const int bpp = static_cast<int>((out.format.bitsPerPixel + 7u) / 8u);

                    for (int y = 0; y < out.height; ++y) {
                        for (int x = 0; x < out.width; ++x) {
                            const uint8_t* px = out.data +
                                static_cast<std::ptrdiff_t>(y) * out.stride +
                                static_cast<std::ptrdiff_t>(x) * bpp;
                            /* Byte-identical: R, G and B each equal the input code. */
                            CHECK(px[0] == expected);
                            CHECK(px[1] == expected);
                            CHECK(px[2] == expected);
                        }
                    }
                }

                plugin.Free(set);
            }
        }
    }
    }
}

TEST_SUITE("FilterPixman ABI: unknown mode is reported, not silently ignored")
{
    TEST_CASE("out-of-range mode returns nullptr and names the mode")
    {
    for (uint32_t mode : { 7u, 4294967295u }) {
        const std::string label = "mode=" + std::to_string(mode);

        {
            CAPTURE(label);
            Rgb8Source src(4, 4, kMidCode, IW_COMPONENT_CLASS_UNORM);
            IWScreenInfo screen{ 8, 3 };
            FilterArgs   opts(mode);

            iwabi::FilterPlugin& plugin = Plugin();
            IWFilterImageSet* set = plugin.Filter(&src.data, &screen, &opts.options);

            /* Rejected, not degraded to a no-op pass-through. */
            CHECK(set == nullptr);

            const std::string err = plugin.lastError();
            INFO("plugin error: " << err);
            CHECK(!err.empty());
            const std::string expected = "FilterImage: unknown mode " + std::to_string(mode);
            CHECK(err == expected);

            plugin.Free(set);   /* must tolerate nullptr */
        }
    }
    }
}
