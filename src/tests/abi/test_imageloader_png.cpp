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
 * The fixture is rgba_4x4.png, emitted by fixtures/generate_format_fixtures.cpp
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


/*
 * ── A 16-bit gray+alpha PNG, synthesised here ──────────────────────────────
 *
 * The loader used to hand PNG_COLOR_TYPE_GRAY_ALPHA the 8-bit component table
 * whatever the bit depth was, so a 16-bit file was described as two 8-bit
 * samples at offsets 0 and 8 - a descriptor that disagrees with the pixels it
 * accompanies. Nothing catches that except a 16-bit fixture, and there is no
 * such fixture in the directory, so this builds one.
 *
 * Written with stored (uncompressed) deflate blocks and a hand-rolled CRC32, so
 * the test needs no zlib and no libpng of its own: the plugin does the real
 * decoding, which is the point. Every byte here is a valid PNG stream.
 */

namespace {

uint32_t Crc32(const uint8_t* data, std::size_t len)
{
    uint32_t crc = 0xFFFFFFFFu;
    for (std::size_t i = 0; i < len; ++i) {
        crc ^= data[i];
        for (int bit = 0; bit < 8; ++bit) {
            const uint32_t mask = 0u - (crc & 1u);
            crc = (crc >> 1) ^ (0xEDB88320u & mask);
        }
    }
    return ~crc;
}

uint32_t Adler32(const uint8_t* data, std::size_t len)
{
    uint32_t a = 1u, b = 0u;
    for (std::size_t i = 0; i < len; ++i) {
        a = (a + data[i]) % 65521u;
        b = (b + a) % 65521u;
    }
    return (b << 16) | a;
}

void PutBe32(std::vector<uint8_t>& v, uint32_t x)
{
    v.push_back(static_cast<uint8_t>(x >> 24));
    v.push_back(static_cast<uint8_t>(x >> 16));
    v.push_back(static_cast<uint8_t>(x >> 8));
    v.push_back(static_cast<uint8_t>(x));
}

void PushChunk(std::vector<uint8_t>& out, const char type[4],
               const std::vector<uint8_t>& data)
{
    PutBe32(out, static_cast<uint32_t>(data.size()));

    std::vector<uint8_t> body;
    body.insert(body.end(), type, type + 4);
    body.insert(body.end(), data.begin(), data.end());

    out.insert(out.end(), body.begin(), body.end());
    PutBe32(out, Crc32(body.data(), body.size()));
}

/* A zlib stream whose deflate payload is stored blocks: valid, if bulky. */
std::vector<uint8_t> ZlibStored(const std::vector<uint8_t>& raw)
{
    std::vector<uint8_t> z;
    z.push_back(0x78);
    z.push_back(0x01);

    std::size_t off = 0;
    for (;;) {
        std::size_t n = raw.size() - off;
        if (n > 65535u) {
            n = 65535u;
        }
        const bool last = (off + n >= raw.size());

        z.push_back(last ? 1u : 0u);
        z.push_back(static_cast<uint8_t>(n & 0xFFu));
        z.push_back(static_cast<uint8_t>((n >> 8) & 0xFFu));
        z.push_back(static_cast<uint8_t>(~n & 0xFFu));
        z.push_back(static_cast<uint8_t>((~n >> 8) & 0xFFu));
        z.insert(z.end(), raw.begin() + static_cast<std::ptrdiff_t>(off),
                 raw.begin() + static_cast<std::ptrdiff_t>(off + n));

        off += n;
        if (last) {
            break;
        }
    }

    PutBe32(z, Adler32(raw.data(), raw.size()));
    return z;
}

/* 4x4, 16 bits per sample, colour type 4 (gray + alpha), no interlace. */
std::vector<uint8_t> MakeGrayAlpha16Png(uint32_t width, uint32_t height)
{
    std::vector<uint8_t> raw;
    for (uint32_t y = 0; y < height; ++y) {
        raw.push_back(0);                       /* filter: none */
        for (uint32_t x = 0; x < width; ++x) {
            const uint16_t gray  = static_cast<uint16_t>((y * width + x) * 0x1111u);
            const uint16_t alpha = static_cast<uint16_t>(0xFFFFu - gray);
            /* PNG stores 16-bit samples big-endian. */
            raw.push_back(static_cast<uint8_t>(gray >> 8));
            raw.push_back(static_cast<uint8_t>(gray));
            raw.push_back(static_cast<uint8_t>(alpha >> 8));
            raw.push_back(static_cast<uint8_t>(alpha));
        }
    }

    std::vector<uint8_t> png = { 0x89, 'P', 'N', 'G', 0x0D, 0x0A, 0x1A, 0x0A };

    std::vector<uint8_t> ihdr;
    PutBe32(ihdr, width);
    PutBe32(ihdr, height);
    ihdr.push_back(16);   /* bit depth            */
    ihdr.push_back(4);    /* colour type: gray+alpha */
    ihdr.push_back(0);    /* compression          */
    ihdr.push_back(0);    /* filter               */
    ihdr.push_back(0);    /* interlace            */
    PushChunk(png, "IHDR", ihdr);

    PushChunk(png, "IDAT", ZlibStored(raw));
    PushChunk(png, "IEND", {});
    return png;
}

} // namespace

TEST_SUITE("PNG loader ABI: 16-bit gray+alpha")
{
    TEST_CASE("A 16-bit gray+alpha PNG is described as 2 x 16 bits at 0/16")
    {
        const std::vector<uint8_t> png = MakeGrayAlpha16Png(4, 4);

        iwabi::ImageLoaderPlugin& plugin = Plugin();
        REQUIRE(plugin.hasMemory());

        ImagePluginResult r = plugin.loadMemory(png.data(), png.size());
        INFO("plugin error: " << plugin.lastError());
        REQUIRE(r.code == IMAGE_PLUGIN_OK);
        REQUIRE(r.data != nullptr);

        const ImagePluginData& d = *r.data;
        CHECK(d.width  == 4);
        CHECK(d.height == 4);
        CHECK(d.format.componentCount == 2);
        CHECK(d.format.bitsPerPixel   == 32);

        /* The regression: the 8-bit table would say bitWidth 8 at offsets 0/8. */
        REQUIRE(d.format.components != nullptr);
        CHECK(d.format.components[0].semantic       == IW_COMPONENT_SEMANTIC_GRAY);
        CHECK(d.format.components[0].componentClass == IW_COMPONENT_CLASS_UNORM);
        CHECK(d.format.components[0].bitOffset      == 0);
        CHECK(d.format.components[0].bitWidth       == 16);
        CHECK(d.format.components[1].semantic       == IW_COMPONENT_SEMANTIC_A);
        CHECK(d.format.components[1].bitOffset      == 16);
        CHECK(d.format.components[1].bitWidth       == 16);

        plugin.free(r.data);
    }

    TEST_CASE("The 16-bit descriptor and the pixel buffer agree on size")
    {
        const std::vector<uint8_t> png = MakeGrayAlpha16Png(4, 4);

        iwabi::ImageLoaderPlugin& plugin = Plugin();
        ImagePluginResult r = plugin.loadMemory(png.data(), png.size());
        REQUIRE(r.code == IMAGE_PLUGIN_OK);
        REQUIRE(r.data != nullptr);

        const ImagePluginData& d = *r.data;

        /* 4 x 4 pixels x 2 components x 2 bytes.  A descriptor that claimed
         * 8-bit samples would describe half the buffer it was given. */
        uint32_t declaredBits = 0;
        for (uint16_t i = 0; i < d.format.componentCount; ++i) {
            declaredBits += d.format.components[i].bitWidth;
        }
        CHECK(declaredBits == d.format.bitsPerPixel);
        CHECK(d.size == static_cast<size_t>(d.width) * d.height * (declaredBits / 8u));

        plugin.free(r.data);
    }
}
