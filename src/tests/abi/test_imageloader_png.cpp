/*
 * test_imageloader_png.cpp — ABI conformance tests for the PNG loader plugin.
 *
 * Two things are checked, and they are different claims:
 *
 *   1. The plugin decodes the fixture through its exported file entry point,
 *      and describes it correctly (RGBA8, sRGB, four components).
 *   2. The plugin decodes the *same bytes* through LoadImageFromMemory and
 *      produces an identical descriptor and identical pixels.
 *
 * The second is why this file exists.  A memory path that decodes "something"
 * is easy; a memory path that is byte-for-byte the file path is the property
 * hosts that read assets out of a container actually depend on.
 *
 * The fixture is rgba_4x4.png, emitted by fixtures/generate_format_fixtures.c
 * and checked in.  Its 4x4 pattern is
 *
 *   R = 16 + 16*x,  G = 32 + 16*y,  B = 200 - 16*(x + y),  A = 240 + x
 *
 * so the numbers below are the encoder's exact samples, not a tolerance.
 */

#include <doctest/doctest.h>

#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

#include "plugin_abi.hpp"

#ifndef IW_PNG_PLUGIN_PATH
#  error "IW_PNG_PLUGIN_PATH must be defined by the build system"
#endif
#ifndef IW_ABI_FIXTURE_DIR
#  error "IW_ABI_FIXTURE_DIR must be defined by the build system"
#endif

namespace {

const std::filesystem::path kPluginPath =
    iwabi::DeployedPluginPath(std::filesystem::path{ IW_PNG_PLUGIN_PATH });

std::filesystem::path Fixture(const char* name)
{
    return std::filesystem::path{ IW_ABI_FIXTURE_DIR } / name;
}

/* One handle for the binary: loading a DSO per test case would repeat the
 * dlopen and its uncollectable static initialisation. */
iwabi::ImageLoaderPlugin& Plugin()
{
    static iwabi::ImageLoaderPlugin plugin{ kPluginPath };
    return plugin;
}

int R(int x)      { return 16 + 16 * x; }
int G(int y)      { return 32 + 16 * y; }
int B(int x, int y) { return 200 - 16 * (x + y); }
int A(int x)      { return 240 + x; }

} // namespace

TEST_SUITE("PNG loader ABI: file path")
{
    TEST_CASE("rgba_4x4.png decodes as 4-component RGBA8 sRGB")
    {
        iwabi::ImageLoaderPlugin& plugin = Plugin();
        ImagePluginResult r = plugin.load(Fixture("rgba_4x4.png"));
        REQUIRE(r.code == IMAGE_PLUGIN_OK);
        REQUIRE(r.data != nullptr);

        const ImagePluginData& d = *r.data;
        CHECK(d.width  == 4);
        CHECK(d.height == 4);
        CHECK(d.colorSpace == IMAGE_COLOR_SPACE_SRGB);
        CHECK(d.format.componentCount == 4);
        CHECK(d.format.bitsPerPixel   == 32);
        CHECK(d.format.storageLayout  == IW_STORAGE_INTERLEAVED);
        CHECK(d.stride == 4 * 4);
        CHECK(d.size   == static_cast<size_t>(d.stride) * 4u);

        const std::string problem = iwabi::DescriptorProblem(d);
        INFO("descriptor problem: " << problem);
        CHECK(problem.empty());

        CHECK(iwabi::PixelComponent(d, 0, 0, 0) == static_cast<uint32_t>(R(0)));
        CHECK(iwabi::PixelComponent(d, 1, 0, 0) == static_cast<uint32_t>(G(0)));
        CHECK(iwabi::PixelComponent(d, 2, 0, 0) == static_cast<uint32_t>(B(0, 0)));
        CHECK(iwabi::PixelComponent(d, 3, 0, 0) == static_cast<uint32_t>(A(0)));
        CHECK(iwabi::PixelComponent(d, 3, 3, 3) == static_cast<uint32_t>(A(3)));

        plugin.free(r.data);
    }
}

TEST_SUITE("PNG loader ABI: memory path")
{
    TEST_CASE("The optional memory export is present")
    {
        CHECK(Plugin().hasMemory());
    }

    TEST_CASE("Memory decode matches the file decode byte for byte")
    {
        iwabi::ImageLoaderPlugin& plugin = Plugin();
        const std::vector<uint8_t> bytes = iwabi::ReadFileBytes(Fixture("rgba_4x4.png"));
        REQUIRE(!bytes.empty());

        ImagePluginResult fromFile   = plugin.load(Fixture("rgba_4x4.png"));
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

    TEST_CASE("Memory decode reports the same geometry and colour space")
    {
        iwabi::ImageLoaderPlugin& plugin = Plugin();
        const std::vector<uint8_t> bytes = iwabi::ReadFileBytes(Fixture("rgba_4x4.png"));

        ImagePluginResult r = plugin.loadMemory(bytes.data(), bytes.size());
        REQUIRE(r.code == IMAGE_PLUGIN_OK);
        REQUIRE(r.data != nullptr);
        CHECK(r.data->width  == 4);
        CHECK(r.data->height == 4);
        CHECK(r.data->colorSpace == IMAGE_COLOR_SPACE_SRGB);
        CHECK(r.data->format.componentCount == 4);
        plugin.free(r.data);
    }

    TEST_CASE("Empty and null input are refused, not dereferenced")
    {
        iwabi::ImageLoaderPlugin& plugin = Plugin();

        ImagePluginResult empty = plugin.loadMemory(nullptr, 0);
        CHECK(empty.code == IMAGE_PLUGIN_FILE_ERROR);
        CHECK(empty.data == nullptr);

        ImagePluginResult nullData = plugin.loadMemory(nullptr, 64);
        CHECK(nullData.code == IMAGE_PLUGIN_FILE_ERROR);
        CHECK(nullData.data == nullptr);
    }

    TEST_CASE("Truncated input is refused through the PNG error path")
    {
        iwabi::ImageLoaderPlugin& plugin = Plugin();
        const std::vector<uint8_t> bytes = iwabi::ReadFileBytes(Fixture("rgba_4x4.png"));
        REQUIRE(bytes.size() > 16);

        /* A PNG header without its image data: the read callback must report a
         * short read through png_error, not return fewer bytes and let libpng
         * walk off the end. */
        ImagePluginResult r = plugin.loadMemory(bytes.data(), 16);
        CHECK(r.code != IMAGE_PLUGIN_OK);
        CHECK(r.data == nullptr);
    }
}
