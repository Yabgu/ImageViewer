/*
 * test_imageloader_gif.cpp — ABI conformance tests for the GIF loader plugin.
 *
 * Same two claims as the PNG test: the file path decodes and describes
 * correctly, and LoadImageFromMemory on the same bytes produces an identical
 * descriptor and identical pixels.
 *
 * The fixture is gif_4x4.gif, emitted by fixtures/generate_format_fixtures.c:
 * a GIF89a with a 16-entry global colour table in raster order, so palette
 * index i is pixel i and the decoded RGB is the same 4x4 pattern every other
 * fixture encodes:
 *
 *   R = 16 + 16*x,  G = 32 + 16*y,  B = 200 - 16*(x + y)
 *
 * GIF is palettized and this loader produces RGBA8, so the interesting
 * properties are that the palette is resolved (not the index), that a pixel
 * the frame covers is opaque, and that the canvas is the logical screen.
 * The fixture has no transparency and a frame that covers the whole canvas;
 * transparency and frame-offset compositing were checked against the previous
 * stb implementation out of band and agree, but they are not asserted here
 * because a second fixture would have to be checked in to make them stable.
 */

#include <doctest/doctest.h>

#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

#include "plugin_abi.hpp"

#ifndef IW_GIF_PLUGIN_PATH
#  error "IW_GIF_PLUGIN_PATH must be defined by the build system"
#endif
#ifndef IW_ABI_FIXTURE_DIR
#  error "IW_ABI_FIXTURE_DIR must be defined by the build system"
#endif

namespace {

const std::filesystem::path kPluginPath =
    iwabi::DeployedPluginPath(std::filesystem::path{ IW_GIF_PLUGIN_PATH });

std::filesystem::path Fixture(const char* name)
{
    return std::filesystem::path{ IW_ABI_FIXTURE_DIR } / name;
}

iwabi::ImageLoaderPlugin& Plugin()
{
    static iwabi::ImageLoaderPlugin plugin{ kPluginPath };
    return plugin;
}

int R(int x)        { return 16 + 16 * x; }
int G(int y)        { return 32 + 16 * y; }
int B(int x, int y) { return 200 - 16 * (x + y); }

} // namespace

TEST_SUITE("GIF loader ABI: file path")
{
    TEST_CASE("gif_4x4.gif decodes as 4-component RGBA8 sRGB")
    {
        iwabi::ImageLoaderPlugin& plugin = Plugin();
        ImagePluginResult r = plugin.load(Fixture("gif_4x4.gif"));
        REQUIRE(r.code == IMAGE_PLUGIN_OK);
        REQUIRE(r.data != nullptr);

        const ImagePluginData& d = *r.data;
        CHECK(d.width  == 4);
        CHECK(d.height == 4);
        CHECK(d.colorSpace == IMAGE_COLOR_SPACE_SRGB);
        /* Palettized on disk, RGBA8 in memory: the loader resolves the palette
         * rather than passing indices through. */
        CHECK(d.format.componentCount == 4);
        CHECK(d.format.bitsPerPixel   == 32);
        CHECK(d.stride == 4 * 4);
        CHECK(d.size   == static_cast<size_t>(d.stride) * 4u);

        const std::string problem = iwabi::DescriptorProblem(d);
        INFO("descriptor problem: " << problem);
        CHECK(problem.empty());

        /* Palette indices resolved to colour, and every covered pixel opaque. */
        CHECK(iwabi::PixelComponent(d, 0, 0, 0) == static_cast<uint32_t>(R(0)));
        CHECK(iwabi::PixelComponent(d, 1, 0, 0) == static_cast<uint32_t>(G(0)));
        CHECK(iwabi::PixelComponent(d, 2, 0, 0) == static_cast<uint32_t>(B(0, 0)));
        CHECK(iwabi::PixelComponent(d, 3, 0, 0) == 255u);
        CHECK(iwabi::PixelComponent(d, 0, 3, 0) == static_cast<uint32_t>(R(3)));
        CHECK(iwabi::PixelComponent(d, 1, 0, 3) == static_cast<uint32_t>(G(3)));
        CHECK(iwabi::PixelComponent(d, 2, 3, 3) == static_cast<uint32_t>(B(3, 3)));
        CHECK(iwabi::PixelComponent(d, 3, 3, 3) == 255u);

        plugin.free(r.data);
    }
}

TEST_SUITE("GIF loader ABI: memory path")
{
    TEST_CASE("The optional memory export is present")
    {
        CHECK(Plugin().hasMemory());
    }

    TEST_CASE("Memory decode matches the file decode byte for byte")
    {
        iwabi::ImageLoaderPlugin& plugin = Plugin();
        const std::vector<uint8_t> bytes = iwabi::ReadFileBytes(Fixture("gif_4x4.gif"));
        REQUIRE(!bytes.empty());

        ImagePluginResult fromFile   = plugin.load(Fixture("gif_4x4.gif"));
        ImagePluginResult fromMemory = plugin.loadMemory(bytes.data(), bytes.size());
        REQUIRE(fromFile.code   == IMAGE_PLUGIN_OK);
        REQUIRE(fromMemory.code == IMAGE_PLUGIN_OK);
        REQUIRE(fromFile.data   != nullptr);
        REQUIRE(fromMemory.data != nullptr);

        const std::string difference = iwabi::DecodeDifference(*fromFile.data, *fromMemory.data);
        INFO("difference: " << difference);
        CHECK(difference.empty());

        plugin.free(fromFile.data);
        plugin.free(fromMemory.data);
    }

    TEST_CASE("The GIF loader exports no preview, and that is not a load failure")
    {
        /* giflib has no cheap reduced-resolution decode, so the loader omits
         * the optional preview exports rather than faking one with a second
         * full decode.  The plugin must still load and resolve. */
        iwabi::ImageLoaderPlugin& plugin = Plugin();
        CHECK_FALSE(plugin.hasPreview());
        CHECK_FALSE(plugin.hasPreviewMemory());
    }

    TEST_CASE("Empty and null input are refused, not dereferenced")
    {
        iwabi::ImageLoaderPlugin& plugin = Plugin();

        ImagePluginResult empty = plugin.loadMemory(nullptr, 0);
        CHECK(empty.code == IMAGE_PLUGIN_FILE_ERROR);
        CHECK(empty.data == nullptr);

        ImagePluginResult nullData = plugin.loadMemory(nullptr, 96);
        CHECK(nullData.code == IMAGE_PLUGIN_FILE_ERROR);
        CHECK(nullData.data == nullptr);
    }

    TEST_CASE("Truncated input is refused through the giflib error path")
    {
        iwabi::ImageLoaderPlugin& plugin = Plugin();
        const std::vector<uint8_t> bytes = iwabi::ReadFileBytes(Fixture("gif_4x4.gif"));
        REQUIRE(bytes.size() > 16);

        /* Header and colour table but no image data: the read callback reports
         * a short read, and giflib surfaces it as an error rather than letting
         * the decoder run off the end of the span. */
        ImagePluginResult r = plugin.loadMemory(bytes.data(), 16);
        CHECK(r.code != IMAGE_PLUGIN_OK);
        CHECK(r.data == nullptr);
    }
}
