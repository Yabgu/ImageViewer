/*
 * test_imageloader_jpeg.cpp — ABI conformance tests for the JPEG loader plugin.
 *
 * These drive the built libImageLoaderJpeg.so / ImageLoaderJpeg.dll through its
 * exported C ABI.  They deliberately do not live in src/tests/test_iwimageformat.cpp:
 * that suite links only headers and structurally cannot reach code inside the
 * plugin, which is where the component-count policy lives.
 *
 * Fixtures are checked in under src/tests/abi/fixtures/ and are 16x16 flat
 * mid-grey images, generated once:
 *
 *   gray_1comp.jpg   1 component  — grayscale JPEG
 *   ycbcr_3comp.jpg  3 components — YCbCr JPEG (RGB after libjpeg conversion)
 *   cmyk_4comp.jpg   4 components — Adobe-style (inverted) CMYK JPEG
 *   two_comp.jpg     2 components — JCS_UNKNOWN, written via libjpeg with
 *                                   input_components = 2 (no standard 2-component
 *                                   colour space exists, so this is synthetic)
 *
 * The 1- and 3-component fixtures must load and describe themselves correctly.
 * The 2- and 4-component fixtures must be rejected with a specific message:
 * before the component-count policy was added, a 4-component file was given an
 * R/G/B descriptor at bitsPerPixel 24 while the scanline loop copied 4 bytes
 * per pixel, so the K sample was dropped and the next pixel's red was misread.
 */

#include <doctest/doctest.h>   /* main() lives in main.cpp */

#include <cstdint>
#include <filesystem>
#include <string>

#include "plugin_abi.hpp"

#ifndef IW_JPEG_PLUGIN_PATH
#  error "IW_JPEG_PLUGIN_PATH must be defined by the build system"
#endif
#ifndef IW_ABI_FIXTURE_DIR
#  error "IW_ABI_FIXTURE_DIR must be defined by the build system"
#endif

namespace {

const std::filesystem::path kPluginPath =
    iwabi::DeployedPluginPath(std::filesystem::path{ IW_JPEG_PLUGIN_PATH });

std::filesystem::path Fixture(const char* name)
{
    return std::filesystem::path{ IW_ABI_FIXTURE_DIR } / name;
}

/* One handle for the whole binary: load the plugin once, resolve its exports
 * once.  Loading it per test case would call dlopen several times, and each
 * dlopen leaves ~104 bytes of the DSO's own C++ static initialisation that
 * dlclose cannot reclaim (reproducible with a ten-line dlopen/dlclose program
 * and no ImageViewer code).  One load keeps that artefact minimal.
 *
 * Lazily created so a missing or broken plugin surfaces as a clear test
 * failure rather than a throw during static initialisation. */
iwabi::ImageLoaderPlugin& Plugin()
{
    static iwabi::ImageLoaderPlugin plugin{ kPluginPath };
    return plugin;
}

/* Sample channel `ch` (0-based component index) at (x, y), honouring the
 * descriptor's own bit offset so it works for GRAY-at-0 and R-at-0 alike. */
uint8_t SampleComponent(const ImagePluginData& d, int ch, int x, int y)
{
    const IWComponentDef& c = d.format.components[ch];
    const uint8_t* pixel = d.data + static_cast<std::ptrdiff_t>(y) * d.stride +
                           static_cast<std::ptrdiff_t>(x) *
                               ((d.format.bitsPerPixel + 7u) / 8u);
    const int byteIndex = c.bitOffset / 8;
    const int bitInByte = c.bitOffset % 8;
    return static_cast<uint8_t>((pixel[byteIndex] >> bitInByte) & 0xFFu);
}

/* Mid-grey as an 8-bit code, and the tolerance the fixtures are held to.
 * JPEG is lossy, but a flat field at quality 95 with 4:4:4 subsampling
 * reconstructs within one code. */
constexpr int kMidGrey = 128;
constexpr int kTolerance = 1;

} // namespace

TEST_SUITE("JPEG loader ABI: accepted component counts")
{
    TEST_CASE("1 component (grayscale) loads with a GRAY8 descriptor")
    {
        iwabi::ImageLoaderPlugin& plugin = Plugin();
        ImagePluginResult r = plugin.load(Fixture("gray_1comp.jpg"));

        REQUIRE(r.code == IMAGE_PLUGIN_OK);
        REQUIRE(r.data != nullptr);

        const ImagePluginData& d = *r.data;

        CHECK(d.format.componentCount == 1);
        CHECK(d.format.bitsPerPixel   == 8);
        CHECK(d.format.components[0].semantic == IW_COMPONENT_SEMANTIC_GRAY);
        CHECK(d.format.components[0].bitWidth == 8);
        CHECK(d.format.components[0].bitOffset == 0);

        /* Descriptor/buffer agreement. */
        const std::string problem = iwabi::DescriptorProblem(d);
        INFO("descriptor problem: " << problem);
        CHECK(problem.empty());

        CHECK(d.format.bitsPerPixel == d.format.componentCount * 8);
        CHECK(d.stride >= d.width * 1);
        CHECK(d.size   == static_cast<size_t>(d.stride) * static_cast<size_t>(d.height));

        /* Pixels: every sample of a flat mid-grey field is mid-grey. */
        for (int y = 0; y < d.height; ++y) {
            for (int x = 0; x < d.width; ++x) {
                const int v = SampleComponent(d, 0, x, y);
                CHECK(v >= kMidGrey - kTolerance);
                CHECK(v <= kMidGrey + kTolerance);
            }
        }

        plugin.free(r.data);
    }

    TEST_CASE("3 components (YCbCr) loads with an RGB8 descriptor, mid-grey intact")
    {
        iwabi::ImageLoaderPlugin& plugin = Plugin();
        ImagePluginResult r = plugin.load(Fixture("ycbcr_3comp.jpg"));

        REQUIRE(r.code == IMAGE_PLUGIN_OK);
        REQUIRE(r.data != nullptr);

        const ImagePluginData& d = *r.data;

        CHECK(d.format.componentCount == 3);
        CHECK(d.format.bitsPerPixel   == 24);
        CHECK(d.format.components[0].semantic == IW_COMPONENT_SEMANTIC_R);
        CHECK(d.format.components[1].semantic == IW_COMPONENT_SEMANTIC_G);
        CHECK(d.format.components[2].semantic == IW_COMPONENT_SEMANTIC_B);
        for (int c = 0; c < 3; ++c) {
            CHECK(d.format.components[c].bitWidth  == 8);
            CHECK(static_cast<int>(d.format.components[c].bitOffset) == c * 8);
        }

        const std::string problem = iwabi::DescriptorProblem(d);
        INFO("descriptor problem: " << problem);
        CHECK(problem.empty());

        CHECK(d.format.bitsPerPixel == d.format.componentCount * 8);
        CHECK(d.stride >= d.width * 3);
        CHECK(d.size   == static_cast<size_t>(d.stride) * static_cast<size_t>(d.height));

        /* YCbCr -> RGB of a flat neutral field must stay neutral. */
        for (int y = 0; y < d.height; ++y) {
            for (int x = 0; x < d.width; ++x) {
                for (int c = 0; c < 3; ++c) {
                    const int v = SampleComponent(d, c, x, y);
                    CHECK(v >= kMidGrey - kTolerance);
                    CHECK(v <= kMidGrey + kTolerance);
                }
            }
        }

        plugin.free(r.data);
    }
}

TEST_SUITE("JPEG loader ABI: rejected component counts")
{
    TEST_CASE("4 components (CMYK) is rejected, not described as RGB")
    {
        iwabi::ImageLoaderPlugin& plugin = Plugin();
        ImagePluginResult r = plugin.load(Fixture("cmyk_4comp.jpg"));

        CHECK(r.code != IMAGE_PLUGIN_OK);
        CHECK(r.code == IMAGE_PLUGIN_DECODE_ERROR);
        CHECK(r.data == nullptr);

        const std::string err = plugin.lastError();
        INFO("plugin error: " << err);
        CHECK(!err.empty());
        CHECK(err.find("JPEG has 4 components") != std::string::npos);
        CHECK(err.find("only grayscale and YCbCr are supported") != std::string::npos);

        plugin.free(r.data); /* must tolerate nullptr */
    }

    TEST_CASE("2 components is rejected with the same error shape")
    {
        iwabi::ImageLoaderPlugin& plugin = Plugin();
        ImagePluginResult r = plugin.load(Fixture("two_comp.jpg"));

        CHECK(r.code != IMAGE_PLUGIN_OK);
        CHECK(r.code == IMAGE_PLUGIN_DECODE_ERROR);
        CHECK(r.data == nullptr);

        const std::string err = plugin.lastError();
        INFO("plugin error: " << err);
        CHECK(!err.empty());
        CHECK(err.find("JPEG has 2 components") != std::string::npos);
        CHECK(err.find("only grayscale and YCbCr are supported") != std::string::npos);

        plugin.free(r.data);
    }
}
