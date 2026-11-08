/*
 * test_imageloader_tiff.cpp — ABI conformance tests for the TIFF loader plugin.
 *
 * TIFF is the one loader whose memory path is not a pointer hand-off: libtiff
 * wants a seekable stream, so the plugin opens TIFFClientOpen over a
 * read/seek cursor.  That makes the interesting failures different from the
 * other four, and they are what the cases below target:
 *
 *   - the read callback must not read past the caller's span;
 *   - a seek past the end is refused rather than silently satisfied;
 *   - a truncated span must come back as a refusal, not a crash or a partial
 *     raster;
 *   - the successful decode must still be byte-identical to the file path.
 *
 * The fixture is rgb_4x4.tif (uncompressed RGB, emitted by
 * fixtures/generate_format_fixtures.c):
 *
 *   R = 16 + 16*x,  G = 32 + 16*y,  B = 200 - 16*(x + y)
 */

#include <doctest/doctest.h>

#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

#include "plugin_abi.hpp"

#ifndef IW_TIFF_PLUGIN_PATH
#  error "IW_TIFF_PLUGIN_PATH must be defined by the build system"
#endif
#ifndef IW_ABI_FIXTURE_DIR
#  error "IW_ABI_FIXTURE_DIR must be defined by the build system"
#endif

namespace {

const std::filesystem::path kPluginPath =
    iwabi::DeployedPluginPath(std::filesystem::path{ IW_TIFF_PLUGIN_PATH });

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

TEST_SUITE("TIFF loader ABI: file path")
{
    TEST_CASE("rgb_4x4.tif decodes as 3-component RGB8 sRGB")
    {
        iwabi::ImageLoaderPlugin& plugin = Plugin();
        ImagePluginResult r = plugin.load(Fixture("rgb_4x4.tif"));
        REQUIRE(r.code == IMAGE_PLUGIN_OK);
        REQUIRE(r.data != nullptr);

        const ImagePluginData& d = *r.data;
        CHECK(d.width  == 4);
        CHECK(d.height == 4);
        /* 8-bit RGB TIFF is display-encoded; the loader only reports LINEAR for
         * 16-bit and float samples. */
        CHECK(d.colorSpace == IMAGE_COLOR_SPACE_SRGB);
        CHECK(d.format.componentCount == 3);
        CHECK(d.format.bitsPerPixel   == 24);
        CHECK(d.stride == 4 * 3);
        CHECK(d.size   == static_cast<size_t>(d.stride) * 4u);

        const std::string problem = iwabi::DescriptorProblem(d);
        INFO("descriptor problem: " << problem);
        CHECK(problem.empty());

        CHECK(iwabi::PixelComponent(d, 0, 0, 0) == static_cast<uint32_t>(R(0)));
        CHECK(iwabi::PixelComponent(d, 1, 0, 0) == static_cast<uint32_t>(G(0)));
        CHECK(iwabi::PixelComponent(d, 2, 0, 0) == static_cast<uint32_t>(B(0, 0)));
        CHECK(iwabi::PixelComponent(d, 0, 3, 0) == static_cast<uint32_t>(R(3)));
        CHECK(iwabi::PixelComponent(d, 1, 0, 3) == static_cast<uint32_t>(G(3)));
        CHECK(iwabi::PixelComponent(d, 2, 3, 3) == static_cast<uint32_t>(B(3, 3)));

        plugin.free(r.data);
    }
}

TEST_SUITE("TIFF loader ABI: memory path")
{
    TEST_CASE("The optional memory export is present")
    {
        CHECK(Plugin().hasMemory());
    }

    TEST_CASE("Memory decode matches the file decode byte for byte")
    {
        iwabi::ImageLoaderPlugin& plugin = Plugin();
        const std::vector<uint8_t> bytes = iwabi::ReadFileBytes(Fixture("rgb_4x4.tif"));
        REQUIRE(!bytes.empty());

        ImagePluginResult fromFile   = plugin.load(Fixture("rgb_4x4.tif"));
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

        ImagePluginResult nullData = plugin.loadMemory(nullptr, 200);
        CHECK(nullData.code == IMAGE_PLUGIN_FILE_ERROR);
        CHECK(nullData.data == nullptr);
    }

    TEST_CASE("Truncated input is refused by the client-open path")
    {
        iwabi::ImageLoaderPlugin& plugin = Plugin();
        const std::vector<uint8_t> bytes = iwabi::ReadFileBytes(Fixture("rgb_4x4.tif"));
        REQUIRE(bytes.size() > 16);

        /* 16 bytes is inside the header: libtiff seeks for the IFD, which the
         * cursor refuses, so the open itself fails. */
        ImagePluginResult r = plugin.loadMemory(bytes.data(), 16);
        CHECK(r.code != IMAGE_PLUGIN_OK);
        CHECK(r.data == nullptr);
    }

    TEST_CASE("A span cut mid-strip fails instead of returning a short raster")
    {
        iwabi::ImageLoaderPlugin& plugin = Plugin();
        const std::vector<uint8_t> bytes = iwabi::ReadFileBytes(Fixture("rgb_4x4.tif"));
        REQUIRE(bytes.size() > 8);

        /* Keep the IFD but drop the pixel data: the directory parses, every
         * scanline read comes back short.  Whatever the loader reports, it must
         * not hand back a buffer with the unwritten rows. */
        ImagePluginResult r = plugin.loadMemory(bytes.data(), bytes.size() / 2);
        if (r.code == IMAGE_PLUGIN_OK)
            CHECK_MESSAGE(false, "a half-truncated TIFF decoded successfully");
        else
            CHECK(r.data == nullptr);
    }
}
