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
 * fixtures/generate_format_fixtures.cpp):
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


/*
 * ── Two TIFFs, one thread ──────────────────────────────────────────────────
 *
 * The native-scanline path used to fill a `static thread_local IWComponentDef
 * nativeComps[4]` and hand its address to the caller.  Every image loaded on a
 * thread therefore shared one component table, so the second load silently
 * rewrote the first image's descriptor: the pixels stayed correct while the
 * thing describing them changed underneath.
 *
 * Catching that needs two files whose descriptors differ - the fixture set has
 * only the RGB one, so the second is built here.  A baseline little-endian
 * grayscale TIFF is small enough to write out by hand, and writing it means the
 * test depends on nothing but the plugin under test.
 */

namespace {

std::vector<uint8_t> MakeGray8Tiff(uint32_t width, uint32_t height)
{
    const uint32_t ifdOffset  = 8;
    const uint32_t entryCount = 9;
    const uint32_t ifdSize    = 2 + entryCount * 12 + 4;
    const uint32_t stripAt    = ifdOffset + ifdSize;
    const uint32_t stripBytes = width * height;

    std::vector<uint8_t> t;

    auto u16 = [&t](uint16_t v) {
        t.push_back(static_cast<uint8_t>(v & 0xFFu));
        t.push_back(static_cast<uint8_t>(v >> 8));
    };
    auto u32 = [&t](uint32_t v) {
        for (int i = 0; i < 4; ++i) {
            t.push_back(static_cast<uint8_t>((v >> (8 * i)) & 0xFFu));
        }
    };

    /* Header: little-endian, classic TIFF, first IFD at offset 8. */
    t.push_back('I');
    t.push_back('I');
    u16(42);
    u32(ifdOffset);

    struct Entry { uint16_t tag, type; uint32_t count, value; };
    const Entry entries[] = {
        { 256, 3, 1, width },        /* ImageWidth        */
        { 257, 3, 1, height },       /* ImageLength       */
        { 258, 3, 1, 8 },            /* BitsPerSample     */
        { 259, 3, 1, 1 },            /* Compression: none */
        { 262, 3, 1, 1 },            /* Photometric: BlackIsZero */
        { 273, 4, 1, stripAt },      /* StripOffsets      */
        { 277, 3, 1, 1 },            /* SamplesPerPixel   */
        { 278, 3, 1, height },       /* RowsPerStrip      */
        { 279, 4, 1, stripBytes },   /* StripByteCounts   */
    };

    u16(static_cast<uint16_t>(entryCount));
    for (const Entry& e : entries) {
        u16(e.tag);
        u16(e.type);
        u32(e.count);
        u32(e.value);   /* SHORT values sit in the low two bytes, little-endian */
    }
    u32(0);             /* no next IFD */

    for (uint32_t i = 0; i < stripBytes; ++i) {
        t.push_back(static_cast<uint8_t>(i * 5u + 7u));
    }
    return t;
}

} // namespace

TEST_SUITE("TIFF loader ABI: component-table lifetime")
{
    TEST_CASE("A second TIFF load does not rewrite the first image's descriptor")
    {
        iwabi::ImageLoaderPlugin& plugin = Plugin();
        REQUIRE(plugin.hasMemory());

        /* First: the RGB fixture, whose descriptor is three components. */
        ImagePluginResult first = plugin.load(Fixture("rgb_4x4.tif"));
        INFO("plugin error: " << plugin.lastError());
        REQUIRE(first.code == IMAGE_PLUGIN_OK);
        REQUIRE(first.data != nullptr);
        CHECK(first.data->format.componentCount == 3);
        REQUIRE(first.data->format.components != nullptr);
        CHECK(first.data->format.components[0].semantic == IW_COMPONENT_SEMANTIC_R);

        /* Second: a grayscale image, loaded on the same thread while the first
         * result is still alive.  With a shared table this overwrites it. */
        const std::vector<uint8_t> gray = MakeGray8Tiff(4, 4);
        ImagePluginResult second = plugin.loadMemory(gray.data(), gray.size());
        INFO("plugin error: " << plugin.lastError());
        REQUIRE(second.code == IMAGE_PLUGIN_OK);
        REQUIRE(second.data != nullptr);
        CHECK(second.data->format.componentCount == 1);
        REQUIRE(second.data->format.components != nullptr);
        CHECK(second.data->format.components[0].semantic == IW_COMPONENT_SEMANTIC_GRAY);

        /* The assertion the old code failed: the first image's descriptor is
         * still its own. */
        CHECK(first.data->format.componentCount == 3);
        CHECK(first.data->format.components[0].semantic == IW_COMPONENT_SEMANTIC_R);
        CHECK(first.data->format.components[1].semantic == IW_COMPONENT_SEMANTIC_G);
        CHECK(first.data->format.components[2].semantic == IW_COMPONENT_SEMANTIC_B);
        CHECK(first.data->format.components[0].bitWidth == 8);

        /* And the two descriptors are genuinely separate storage. */
        CHECK(first.data->format.components != second.data->format.components);

        plugin.free(first.data);
        plugin.free(second.data);
    }
}
