/*
 * test_imageloader_iwi.cpp — ABI conformance tests for the IWI loader plugin.
 *
 * Two claims, the same two every per-format test makes: the file path decodes
 * and describes correctly, and LoadImageFromMemory on the same bytes produces
 * an identical descriptor and identical pixels.
 *
 * On top of that, IWI is the only format here whose parser is *in* the loader
 * rather than in a library, so its refusals are the loader's own decisions and
 * are tested directly: planar storage, a non-8-bit component layout, a
 * component parked off its i*8 slot, a header whose declared payload runs past
 * the buffer.  Each of those must come back as a decode error with no data,
 * because the alternative — accepting them — renders a plausible but wrong
 * image.
 *
 * The fixtures are rgba_4x4.iwi and rgb_4x4.iwi, emitted by
 * fixtures/generate_format_fixtures.c with the same sample values as every
 * other format fixture:
 *
 *   R = 16 + 16*x,  G = 32 + 16*y,  B = 200 - 16*(x + y),  A = 240 + x
 *
 * The RGB fixture has no alpha component at all, so the two files exercise the
 * loader's two accepted shapes rather than one shape twice.
 */

#include <doctest/doctest.h>

#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

#include "plugin_abi.hpp"

#ifndef IW_IWI_PLUGIN_PATH
#  error "IW_IWI_PLUGIN_PATH must be defined by the build system"
#endif
#ifndef IW_ABI_FIXTURE_DIR
#  error "IW_ABI_FIXTURE_DIR must be defined by the build system"
#endif

namespace {

constexpr std::size_t kHeaderBytes = 74;

const std::filesystem::path kPluginPath =
    iwabi::DeployedPluginPath(std::filesystem::path{ IW_IWI_PLUGIN_PATH });

std::filesystem::path Fixture(const char* name)
{
    return std::filesystem::path{ IW_ABI_FIXTURE_DIR } / name;
}

iwabi::ImageLoaderPlugin& Plugin()
{
    static iwabi::ImageLoaderPlugin plugin{ kPluginPath };
    return plugin;
}

void PutU16(std::vector<uint8_t>& b, std::size_t at, uint16_t v)
{
    b[at]     = static_cast<uint8_t>(v & 0xFFu);
    b[at + 1] = static_cast<uint8_t>((v >> 8) & 0xFFu);
}

void PutU32(std::vector<uint8_t>& b, std::size_t at, uint32_t v)
{
    for (int i = 0; i < 4; ++i)
        b[at + static_cast<std::size_t>(i)] =
            static_cast<uint8_t>((v >> (8 * i)) & 0xFFu);
}

void PutU64(std::vector<uint8_t>& b, std::size_t at, uint64_t v)
{
    for (int i = 0; i < 8; ++i)
        b[at + static_cast<std::size_t>(i)] =
            static_cast<uint8_t>((v >> (8 * i)) & 0xFFu);
}

/*
 * A minimal valid container, built here rather than read from disk, so each
 * refusal case can mutate exactly one field and leave the rest correct.  The
 * pixel payload is `components` bytes of 0x5A per pixel — the tests that use
 * this only assert the load is refused, so the values do not matter, but they
 * are present so the only defect is the one the case introduces.
 */
std::vector<uint8_t> MakeIwi(uint32_t width, uint32_t height, uint16_t components)
{
    const std::size_t payload = static_cast<std::size_t>(width) *
                                static_cast<std::size_t>(height) * components;
    std::vector<uint8_t> b(kHeaderBytes + payload, 0);

    b[0] = 'I'; b[1] = 'W'; b[2] = 'I'; b[3] = '1';
    PutU32(b, 4, width);
    PutU32(b, 8, height);
    b[12] = 0;                                   /* sRGB */
    PutU16(b, 13, components);
    PutU16(b, 15, static_cast<uint16_t>(components * 8));
    b[17] = 0;                                   /* interleaved */
    for (uint16_t i = 0; i < components; ++i) {
        const std::size_t rec = 18u + static_cast<std::size_t>(i) * 6u;
        b[rec + 0] = static_cast<uint8_t>(i + 1); /* R, G, B[, A] */
        b[rec + 1] = 2;                           /* UNORM */
        PutU16(b, rec + 2, static_cast<uint16_t>(i * 8));
        PutU16(b, rec + 4, 8);
    }
    PutU64(b, 66, payload);
    for (std::size_t i = 0; i < payload; ++i)
        b[kHeaderBytes + i] = 0x5A;

    return b;
}

/* Decode a span, assert it was refused, and return the reported reason so a
 * case can check the loader explained *why*.  Frees anything returned, so a
 * failing expectation cannot leak. */
std::string RefusedReason(iwabi::ImageLoaderPlugin& plugin,
                          const std::vector<uint8_t>& bytes)
{
    ImagePluginResult r = plugin.loadMemory(bytes.data(), bytes.size());
    if (r.data) plugin.free(r.data);
    CHECK(r.code == IMAGE_PLUGIN_DECODE_ERROR);
    CHECK(r.data == nullptr);
    return plugin.lastError();
}

int R(int x)        { return 16 + 16 * x; }
int G(int y)        { return 32 + 16 * y; }
int B(int x, int y) { return 200 - 16 * (x + y); }
int A(int x)        { return 240 + x; }

} // namespace

TEST_SUITE("IWI loader ABI: file path")
{
    TEST_CASE("rgba_4x4.iwi decodes as 4-component RGBA8 sRGB")
    {
        iwabi::ImageLoaderPlugin& plugin = Plugin();
        ImagePluginResult r = plugin.load(Fixture("rgba_4x4.iwi"));
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

        /* Every component sits at the slot the header declared. */
        CHECK(d.format.components[0].semantic       == IW_COMPONENT_SEMANTIC_R);
        CHECK(d.format.components[1].semantic       == IW_COMPONENT_SEMANTIC_G);
        CHECK(d.format.components[2].semantic       == IW_COMPONENT_SEMANTIC_B);
        CHECK(d.format.components[3].semantic       == IW_COMPONENT_SEMANTIC_A);
        CHECK(d.format.components[3].bitOffset      == 24);
        CHECK(d.format.components[0].componentClass == IW_COMPONENT_CLASS_UNORM);

        CHECK(iwabi::PixelComponent(d, 0, 0, 0) == static_cast<uint32_t>(R(0)));
        CHECK(iwabi::PixelComponent(d, 1, 0, 0) == static_cast<uint32_t>(G(0)));
        CHECK(iwabi::PixelComponent(d, 2, 0, 0) == static_cast<uint32_t>(B(0, 0)));
        CHECK(iwabi::PixelComponent(d, 3, 0, 0) == static_cast<uint32_t>(A(0)));
        CHECK(iwabi::PixelComponent(d, 0, 3, 2) == static_cast<uint32_t>(R(3)));
        CHECK(iwabi::PixelComponent(d, 1, 3, 2) == static_cast<uint32_t>(G(2)));
        CHECK(iwabi::PixelComponent(d, 3, 3, 3) == static_cast<uint32_t>(A(3)));

        plugin.free(r.data);
    }

    TEST_CASE("rgb_4x4.iwi decodes as 3-component RGB8 sRGB")
    {
        iwabi::ImageLoaderPlugin& plugin = Plugin();
        ImagePluginResult r = plugin.load(Fixture("rgb_4x4.iwi"));
        REQUIRE(r.code == IMAGE_PLUGIN_OK);
        REQUIRE(r.data != nullptr);

        const ImagePluginData& d = *r.data;
        CHECK(d.width  == 4);
        CHECK(d.height == 4);
        CHECK(d.colorSpace == IMAGE_COLOR_SPACE_SRGB);
        CHECK(d.format.componentCount == 3);
        CHECK(d.format.bitsPerPixel   == 24);
        CHECK(d.format.storageLayout  == IW_STORAGE_INTERLEAVED);
        CHECK(d.stride == 4 * 3);
        CHECK(d.size   == static_cast<size_t>(d.stride) * 4u);

        const std::string problem = iwabi::DescriptorProblem(d);
        INFO("descriptor problem: " << problem);
        CHECK(problem.empty());

        /* No fourth component is invented: the container is opaque RGB and the
         * descriptor says so. */
        CHECK(d.format.components[3].semantic == IW_COMPONENT_SEMANTIC_NONE);

        CHECK(iwabi::PixelComponent(d, 0, 0, 0) == static_cast<uint32_t>(R(0)));
        CHECK(iwabi::PixelComponent(d, 1, 0, 0) == static_cast<uint32_t>(G(0)));
        CHECK(iwabi::PixelComponent(d, 2, 0, 0) == static_cast<uint32_t>(B(0, 0)));
        CHECK(iwabi::PixelComponent(d, 2, 3, 3) == static_cast<uint32_t>(B(3, 3)));

        plugin.free(r.data);
    }
}

TEST_SUITE("IWI loader ABI: memory path")
{
    TEST_CASE("The optional memory export is present")
    {
        CHECK(Plugin().hasMemory());
    }

    TEST_CASE("Memory decode matches the file decode byte for byte")
    {
        for (const char* name : { "rgba_4x4.iwi", "rgb_4x4.iwi" }) {
            iwabi::ImageLoaderPlugin& plugin = Plugin();
            const std::vector<uint8_t> bytes = iwabi::ReadFileBytes(Fixture(name));
            REQUIRE(!bytes.empty());

            ImagePluginResult fromFile   = plugin.load(Fixture(name));
            ImagePluginResult fromMemory = plugin.loadMemory(bytes.data(), bytes.size());
            REQUIRE(fromFile.code   == IMAGE_PLUGIN_OK);
            REQUIRE(fromMemory.code == IMAGE_PLUGIN_OK);
            REQUIRE(fromFile.data   != nullptr);
            REQUIRE(fromMemory.data != nullptr);

            const std::string difference =
                iwabi::DecodeDifference(*fromFile.data, *fromMemory.data);
            INFO(name << " difference: " << difference);
            CHECK(difference.empty());

            plugin.free(fromFile.data);
            plugin.free(fromMemory.data);
        }
    }

    TEST_CASE("Empty and null input are refused, not dereferenced")
    {
        iwabi::ImageLoaderPlugin& plugin = Plugin();

        ImagePluginResult empty = plugin.loadMemory(nullptr, 0);
        CHECK(empty.code == IMAGE_PLUGIN_FILE_ERROR);
        CHECK(empty.data == nullptr);

        ImagePluginResult nullData = plugin.loadMemory(nullptr, 4096);
        CHECK(nullData.code == IMAGE_PLUGIN_FILE_ERROR);
        CHECK(nullData.data == nullptr);
    }
}

TEST_SUITE("IWI loader ABI: header refusals")
{
    TEST_CASE("A buffer shorter than the header is malformed")
    {
        iwabi::ImageLoaderPlugin& plugin = Plugin();
        const std::string reason = RefusedReason(plugin, std::vector<uint8_t>(10, 0));
        CHECK(reason.find("header") != std::string::npos);
    }

    TEST_CASE("A wrong magic is refused")
    {
        iwabi::ImageLoaderPlugin& plugin = Plugin();
        std::vector<uint8_t> bytes = MakeIwi(2, 2, 4);
        bytes[0] = 'P'; bytes[1] = 'N'; bytes[2] = 'G'; bytes[3] = '\0';

        const std::string reason = RefusedReason(plugin, bytes);
        CHECK(reason.find("magic") != std::string::npos);
    }

    TEST_CASE("Planar storage is refused, not reinterpreted")
    {
        iwabi::ImageLoaderPlugin& plugin = Plugin();
        std::vector<uint8_t> bytes = MakeIwi(2, 2, 4);
        bytes[17] = 1;

        const std::string reason = RefusedReason(plugin, bytes);
        CHECK(reason.find("planar") != std::string::npos);
    }

    TEST_CASE("Two components are refused: there is no colour space for them")
    {
        iwabi::ImageLoaderPlugin& plugin = Plugin();
        std::vector<uint8_t> bytes = MakeIwi(2, 2, 2);
        PutU16(bytes, 15, 16);

        RefusedReason(plugin, bytes);
    }

    TEST_CASE("A sub-8-bit layout is refused")
    {
        iwabi::ImageLoaderPlugin& plugin = Plugin();
        std::vector<uint8_t> bytes = MakeIwi(2, 2, 4);
        /* Claim 16 bits per pixel (4 x 4-bit) without moving the components;
         * the bitsPerPixel check fires before the per-component one. */
        PutU16(bytes, 15, 16);

        const std::string reason = RefusedReason(plugin, bytes);
        CHECK(reason.find("8-bit") != std::string::npos);
    }

    TEST_CASE("A component parked off its i*8 slot is refused")
    {
        iwabi::ImageLoaderPlugin& plugin = Plugin();
        std::vector<uint8_t> bytes = MakeIwi(2, 2, 3);
        /* Move the green component from offset 8 to offset 16, where it would
         * overwrite blue.  A decoder that trusted the offsets would render
         * green and blue swapped; this one refuses. */
        PutU16(bytes, 18u + 6u + 2u, 16);

        const std::string reason = RefusedReason(plugin, bytes);
        CHECK(reason.find("offset") != std::string::npos);
    }

    TEST_CASE("A wrong component semantic is refused")
    {
        iwabi::ImageLoaderPlugin& plugin = Plugin();
        std::vector<uint8_t> bytes = MakeIwi(2, 2, 3);
        bytes[18] = IW_COMPONENT_SEMANTIC_B;   /* first component says blue */

        const std::string reason = RefusedReason(plugin, bytes);
        CHECK(reason.find("semantic") != std::string::npos);
    }

    TEST_CASE("A floating-point component class is refused")
    {
        iwabi::ImageLoaderPlugin& plugin = Plugin();
        std::vector<uint8_t> bytes = MakeIwi(2, 2, 3);
        bytes[18 + 1] = IW_COMPONENT_CLASS_FLOAT;

        const std::string reason = RefusedReason(plugin, bytes);
        CHECK(reason.find("class") != std::string::npos);
    }

    TEST_CASE("A width beyond the addressable range is refused")
    {
        iwabi::ImageLoaderPlugin& plugin = Plugin();
        /* 0xFFFFFFFF wide is a legal u32 and a lie: the buffer cannot hold it,
         * and the descriptor's stride is an int.  The geometry check must fire
         * before anything is sized from the header. */
        std::vector<uint8_t> bytes = MakeIwi(1, 1, 4);
        PutU32(bytes, 4, 0xFFFFFFFFu);

        const std::string reason = RefusedReason(plugin, bytes);
        CHECK(reason.find("addressable") != std::string::npos);
    }

    TEST_CASE("A declared payload of UINT64_MAX is refused without wrapping")
    {
        iwabi::ImageLoaderPlugin& plugin = Plugin();
        std::vector<uint8_t> bytes = MakeIwi(2, 2, 4);
        PutU64(bytes, 66, 0xFFFFFFFFFFFFFFFFull);

        RefusedReason(plugin, bytes);
    }

    TEST_CASE("A declared payload smaller than the geometry is refused")
    {
        iwabi::ImageLoaderPlugin& plugin = Plugin();
        std::vector<uint8_t> bytes = MakeIwi(2, 2, 4);
        PutU64(bytes, 66, 8);   /* needs 16 */

        const std::string reason = RefusedReason(plugin, bytes);
        CHECK(reason.find("declares") != std::string::npos);
    }

    TEST_CASE("A payload cut short of the declared size is refused")
    {
        iwabi::ImageLoaderPlugin& plugin = Plugin();
        std::vector<uint8_t> bytes = MakeIwi(2, 2, 4);
        bytes.resize(bytes.size() - 1);

        RefusedReason(plugin, bytes);
    }

    TEST_CASE("Zero width is refused")
    {
        iwabi::ImageLoaderPlugin& plugin = Plugin();
        std::vector<uint8_t> bytes = MakeIwi(1, 2, 4);
        PutU32(bytes, 4, 0);

        const std::string reason = RefusedReason(plugin, bytes);
        CHECK(reason.find("dimensions") != std::string::npos);
    }

    TEST_CASE("A truncated real file is refused")
    {
        iwabi::ImageLoaderPlugin& plugin = Plugin();
        const std::vector<uint8_t> bytes = iwabi::ReadFileBytes(Fixture("rgba_4x4.iwi"));
        REQUIRE(bytes.size() > kHeaderBytes);

        /* One byte short of the declared payload, and short of the header. */
        RefusedReason(plugin, std::vector<uint8_t>(bytes.begin(), bytes.end() - 1));
        RefusedReason(plugin, std::vector<uint8_t>(bytes.begin(),
                                                   bytes.begin() + 40));
    }
}
