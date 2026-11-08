/*
 * test_imageloader_stb.cpp — ABI conformance tests for the stb loader plugin.
 *
 * The stb plugin covers the formats libjpeg/libpng/libwebp do not: BMP, TGA,
 * GIF, PSD, HDR and PNM.  The fixture is a 24-bit BMP
 * (rgb_4x4.bmp, emitted by fixtures/generate_format_fixtures.c) because it is
 * the format a mod or an editor asset is most likely to be:
 *
 *   R = 16 + 16*x,  G = 32 + 16*y,  B = 200 - 16*(x + y)
 *
 * Truncation is handled differently here than in the other four loaders, and
 * the test says so rather than asserting something stb does not do: stb_image
 * decodes a BMP whose pixel data is cut short without reporting an error, and
 * it does so identically through the file path.  The property this plugin must
 * hold is therefore "the memory path agrees with the file path", not "stb
 * rejects truncation" -- the latter is stb's behaviour to change, not this
 * plugin's.
 */

#include <doctest/doctest.h>

#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

#include "plugin_abi.hpp"

#ifndef IW_STB_PLUGIN_PATH
#  error "IW_STB_PLUGIN_PATH must be defined by the build system"
#endif
#ifndef IW_ABI_FIXTURE_DIR
#  error "IW_ABI_FIXTURE_DIR must be defined by the build system"
#endif

namespace {

const std::filesystem::path kPluginPath =
    iwabi::DeployedPluginPath(std::filesystem::path{ IW_STB_PLUGIN_PATH });

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

TEST_SUITE("stb loader ABI: file path")
{
    TEST_CASE("rgb_4x4.bmp decodes as 3-component RGB8 sRGB")
    {
        iwabi::ImageLoaderPlugin& plugin = Plugin();
        ImagePluginResult r = plugin.load(Fixture("rgb_4x4.bmp"));
        REQUIRE(r.code == IMAGE_PLUGIN_OK);
        REQUIRE(r.data != nullptr);

        const ImagePluginData& d = *r.data;
        CHECK(d.width  == 4);
        CHECK(d.height == 4);
        CHECK(d.colorSpace == IMAGE_COLOR_SPACE_SRGB);
        CHECK(d.format.componentCount == 3);
        CHECK(d.format.bitsPerPixel   == 24);
        CHECK(d.stride == 4 * 3);
        CHECK(d.size   == static_cast<size_t>(d.stride) * 4u);

        const std::string problem = iwabi::DescriptorProblem(d);
        INFO("descriptor problem: " << problem);
        CHECK(problem.empty());

        /* The BMP is stored bottom-up; the loader must return it top-down, so
         * row 0 of the raster is the fixture's y = 0. */
        CHECK(iwabi::PixelComponent(d, 0, 0, 0) == static_cast<uint32_t>(R(0)));
        CHECK(iwabi::PixelComponent(d, 1, 0, 0) == static_cast<uint32_t>(G(0)));
        CHECK(iwabi::PixelComponent(d, 2, 0, 0) == static_cast<uint32_t>(B(0, 0)));
        CHECK(iwabi::PixelComponent(d, 1, 0, 3) == static_cast<uint32_t>(G(3)));

        plugin.free(r.data);
    }
}

TEST_SUITE("stb loader ABI: memory path")
{
    TEST_CASE("The optional memory export is present")
    {
        CHECK(Plugin().hasMemory());
    }

    TEST_CASE("Memory decode matches the file decode byte for byte")
    {
        iwabi::ImageLoaderPlugin& plugin = Plugin();
        const std::vector<uint8_t> bytes = iwabi::ReadFileBytes(Fixture("rgb_4x4.bmp"));
        REQUIRE(!bytes.empty());

        ImagePluginResult fromFile   = plugin.load(Fixture("rgb_4x4.bmp"));
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

    TEST_CASE("Empty and null input are refused, not dereferenced")
    {
        iwabi::ImageLoaderPlugin& plugin = Plugin();

        ImagePluginResult empty = plugin.loadMemory(nullptr, 0);
        CHECK(empty.code == IMAGE_PLUGIN_FILE_ERROR);
        CHECK(empty.data == nullptr);

        ImagePluginResult nullData = plugin.loadMemory(nullptr, 102);
        CHECK(nullData.code == IMAGE_PLUGIN_FILE_ERROR);
        CHECK(nullData.data == nullptr);
    }

    TEST_CASE("Truncated input never crashes and never lies about its buffer")
    {
        iwabi::ImageLoaderPlugin& plugin = Plugin();
        const std::vector<uint8_t> bytes = iwabi::ReadFileBytes(Fixture("rgb_4x4.bmp"));
        REQUIRE(bytes.size() > 16);

        for (size_t cut : { size_t{16}, bytes.size() / 2, bytes.size() - 1 })
        {
            ImagePluginResult r = plugin.loadMemory(bytes.data(), cut);
            if (r.code == IMAGE_PLUGIN_OK)
            {
                /* stb accepts a short BMP.  It must still describe what it
                 * returned: a descriptor that does not match the buffer is the
                 * failure mode that renders as a misread red channel. */
                REQUIRE(r.data != nullptr);
                const ImagePluginData& d = *r.data;
                CHECK(d.size == static_cast<size_t>(d.stride) * static_cast<size_t>(d.height));
                const std::string problem = iwabi::DescriptorProblem(d);
                INFO("cut " << cut << " descriptor problem: " << problem);
                CHECK(problem.empty());
                plugin.free(r.data);
            }
            else
            {
                CHECK(r.data == nullptr);
            }
        }
    }
}
