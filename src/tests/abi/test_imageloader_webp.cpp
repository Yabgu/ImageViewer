/*
 * test_imageloader_webp.cpp — ABI conformance tests for the WebP loader plugin.
 *
 * Same two claims as the PNG test: the file path decodes and describes
 * correctly, and LoadImageFromMemory on the same bytes produces an identical
 * descriptor and identical pixels.
 *
 * The fixture is lossless_4x4.webp (VP8L, emitted by
 * fixtures/generate_format_fixtures.c).  Lossless on purpose: a lossy VP8
 * encode would still be self-consistent between the two decode paths, but the
 * exact sample values would depend on the encoder build, and the assertions
 * below are exact.
 *
 *   R = 16 + 16*x,  G = 32 + 16*y,  B = 200 - 16*(x + y),  A = 240 + x
 */

#include <doctest/doctest.h>

#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

#include "plugin_abi.hpp"

#ifndef IW_WEBP_PLUGIN_PATH
#  error "IW_WEBP_PLUGIN_PATH must be defined by the build system"
#endif
#ifndef IW_ABI_FIXTURE_DIR
#  error "IW_ABI_FIXTURE_DIR must be defined by the build system"
#endif

namespace {

const std::filesystem::path kPluginPath =
    iwabi::DeployedPluginPath(std::filesystem::path{ IW_WEBP_PLUGIN_PATH });

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
int A(int x)        { return 240 + x; }

} // namespace

TEST_SUITE("WebP loader ABI: file path")
{
    TEST_CASE("lossless_4x4.webp decodes as 4-component RGBA8 sRGB")
    {
        iwabi::ImageLoaderPlugin& plugin = Plugin();
        ImagePluginResult r = plugin.load(Fixture("lossless_4x4.webp"));
        REQUIRE(r.code == IMAGE_PLUGIN_OK);
        REQUIRE(r.data != nullptr);

        const ImagePluginData& d = *r.data;
        CHECK(d.width  == 4);
        CHECK(d.height == 4);
        CHECK(d.colorSpace == IMAGE_COLOR_SPACE_SRGB);
        /* The fixture carries an alpha ramp, so the loader must take its RGBA
         * branch rather than the opaque RGB one. */
        CHECK(d.format.componentCount == 4);
        CHECK(d.format.bitsPerPixel   == 32);
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

TEST_SUITE("WebP loader ABI: memory path")
{
    TEST_CASE("The optional memory export is present")
    {
        CHECK(Plugin().hasMemory());
    }

    TEST_CASE("Memory decode matches the file decode byte for byte")
    {
        iwabi::ImageLoaderPlugin& plugin = Plugin();
        const std::vector<uint8_t> bytes = iwabi::ReadFileBytes(Fixture("lossless_4x4.webp"));
        REQUIRE(!bytes.empty());

        ImagePluginResult fromFile   = plugin.load(Fixture("lossless_4x4.webp"));
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

    TEST_CASE("Truncated input is refused")
    {
        iwabi::ImageLoaderPlugin& plugin = Plugin();
        const std::vector<uint8_t> bytes = iwabi::ReadFileBytes(Fixture("lossless_4x4.webp"));
        REQUIRE(bytes.size() > 16);

        ImagePluginResult r = plugin.loadMemory(bytes.data(), 16);
        CHECK(r.code != IMAGE_PLUGIN_OK);
        CHECK(r.data == nullptr);
    }
}
