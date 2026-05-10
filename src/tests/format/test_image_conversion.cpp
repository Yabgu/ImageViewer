#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>

#include <cassert>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <stdexcept>
#include <string>
#include <vector>

#include <png.h>

#include "ImagePluginDef.h"

import IWImageIO;

// ── PNG helpers ───────────────────────────────────────────────────────────────

struct PNGImageData {
    int width    = 0;
    int height   = 0;
    int channels = 0;
    int bitDepth = 0;
    std::vector<uint8_t> pixels;
    ImageColorSpace colorSpace = IMAGE_COLOR_SPACE_UNKNOWN;
};

static PNGImageData ReadPNG(const std::string& path)
{
    PNGImageData out;

    FILE* f = fopen(path.c_str(), "rb");
    REQUIRE_MESSAGE(f != nullptr, "Cannot open test image: " << path);

    png_structp png = png_create_read_struct(PNG_LIBPNG_VER_STRING,
                                             nullptr, nullptr, nullptr);
    REQUIRE_MESSAGE(png != nullptr, "png_create_read_struct failed");

    png_infop info = png_create_info_struct(png);
    if (!info) {
        png_destroy_read_struct(&png, nullptr, nullptr);
        fclose(f);
        FAIL("png_create_info_struct failed");
    }

    if (setjmp(png_jmpbuf(png))) {
        png_destroy_read_struct(&png, &info, nullptr);
        fclose(f);
        FAIL("PNG decode error: " << path);
    }

    png_init_io(png, f);
    png_read_info(png, info);

    int ct = png_get_color_type(png, info);
    int bd = png_get_bit_depth(png, info);

    if (ct == PNG_COLOR_TYPE_PALETTE)
        png_set_palette_to_rgb(png);
    if (ct == PNG_COLOR_TYPE_GRAY && bd < 8)
        png_set_expand_gray_1_2_4_to_8(png);
    if (png_get_valid(png, info, PNG_INFO_tRNS))
        png_set_tRNS_to_alpha(png);
    if (bd == 16)
        png_set_swap(png);

    png_read_update_info(png, info);

    out.width    = static_cast<int>(png_get_image_width(png, info));
    out.height   = static_cast<int>(png_get_image_height(png, info));
    out.bitDepth = static_cast<int>(png_get_bit_depth(png, info));
    out.channels = static_cast<int>(png_get_channels(png, info));

    int    srgb_intent = -1;
    double gamma_val   = 0.0;
    if (png_get_sRGB(png, info, &srgb_intent) == PNG_INFO_sRGB) {
        out.colorSpace = IMAGE_COLOR_SPACE_SRGB;
    } else if (png_get_gAMA(png, info, &gamma_val) == PNG_INFO_gAMA
               && gamma_val > 0.99 && gamma_val < 1.01) {
        out.colorSpace = IMAGE_COLOR_SPACE_LINEAR;
    } else {
        out.colorSpace = (out.bitDepth == 16)
                         ? IMAGE_COLOR_SPACE_LINEAR : IMAGE_COLOR_SPACE_SRGB;
    }

    const int stride = out.width * out.channels * (out.bitDepth / 8);
    out.pixels.resize(static_cast<size_t>(out.height) * stride);

    for (int y = 0; y < out.height; ++y) {
        png_bytep row = out.pixels.data() + static_cast<size_t>(y) * stride;
        png_read_rows(png, &row, nullptr, 1);
    }

    png_destroy_read_struct(&png, &info, nullptr);
    fclose(f);
    return out;
}

static IWImageFormat BuildFormatFromPNG(const PNGImageData& img)
{
    IWImageFormat fmt = {};
    fmt.componentCount = static_cast<uint16_t>(img.channels);
    fmt.bitsPerPixel   = static_cast<uint16_t>(img.channels * img.bitDepth);
    fmt.storageLayout  = IW_STORAGE_INTERLEAVED;

    const uint16_t bw = static_cast<uint16_t>(img.bitDepth);
    switch (img.channels) {
    case 1:
        fmt.components[0] = { IW_COMPONENT_SEMANTIC_GRAY, IW_COMPONENT_CLASS_UINT, 0, bw };
        break;
    case 2:
        fmt.components[0] = { IW_COMPONENT_SEMANTIC_GRAY, IW_COMPONENT_CLASS_UINT, 0,  bw };
        fmt.components[1] = { IW_COMPONENT_SEMANTIC_A,    IW_COMPONENT_CLASS_UINT, bw, bw };
        break;
    case 3:
        fmt.components[0] = { IW_COMPONENT_SEMANTIC_R, IW_COMPONENT_CLASS_UINT,
                               static_cast<uint16_t>(0 * bw), bw };
        fmt.components[1] = { IW_COMPONENT_SEMANTIC_G, IW_COMPONENT_CLASS_UINT,
                               static_cast<uint16_t>(1 * bw), bw };
        fmt.components[2] = { IW_COMPONENT_SEMANTIC_B, IW_COMPONENT_CLASS_UINT,
                               static_cast<uint16_t>(2 * bw), bw };
        break;
    case 4:
        fmt.components[0] = { IW_COMPONENT_SEMANTIC_R, IW_COMPONENT_CLASS_UINT,
                               static_cast<uint16_t>(0 * bw), bw };
        fmt.components[1] = { IW_COMPONENT_SEMANTIC_G, IW_COMPONENT_CLASS_UINT,
                               static_cast<uint16_t>(1 * bw), bw };
        fmt.components[2] = { IW_COMPONENT_SEMANTIC_B, IW_COMPONENT_CLASS_UINT,
                               static_cast<uint16_t>(2 * bw), bw };
        fmt.components[3] = { IW_COMPONENT_SEMANTIC_A, IW_COMPONENT_CLASS_UINT,
                               static_cast<uint16_t>(3 * bw), bw };
        break;
    default:
        throw std::runtime_error(
            "Unsupported channel count: " + std::to_string(img.channels));
    }
    return fmt;
}

// ── Round-trip helper ─────────────────────────────────────────────────────────

// Performs a lossless PNG → IWI (file) → IWI (memory) pixel comparison.
// The .iwi format stores raw pixel data, so the round-trip must be bit-exact.
static void RoundTripTest(const std::string& pngPath)
{
    const PNGImageData original = ReadPNG(pngPath);
    const IWImageFormat fmt     = BuildFormatFromPNG(original);

    // PNG -> IWI struct
    const iw::Image iwi = iw::BuildImage(
        original.width, original.height, original.colorSpace,
        fmt, original.pixels.data(), original.pixels.size());

    // Serialize to bytes and deserialize back (covers the full binary I/O path)
    const std::vector<uint8_t> buf = iw::SerializeImage(iwi);
    const iw::Image restored = iw::DeserializeImage(buf.data(), buf.size());

    // Dimensions must be preserved
    CHECK(static_cast<int>(restored.width)  == original.width);
    CHECK(static_cast<int>(restored.height) == original.height);

    // Format must be preserved
    CHECK(restored.format.componentCount == fmt.componentCount);
    CHECK(restored.format.bitsPerPixel   == fmt.bitsPerPixel);
    CHECK(restored.format.storageLayout  == fmt.storageLayout);
    for (uint16_t i = 0; i < fmt.componentCount; ++i) {
        CHECK(restored.format.components[i].semantic       == fmt.components[i].semantic);
        CHECK(restored.format.components[i].componentClass == fmt.components[i].componentClass);
        CHECK(restored.format.components[i].bitOffset      == fmt.components[i].bitOffset);
        CHECK(restored.format.components[i].bitWidth       == fmt.components[i].bitWidth);
    }

    // Pixel data must be bit-exact (lossless format)
    REQUIRE(restored.pixelData.size() == original.pixels.size());
    CHECK(std::memcmp(restored.pixelData.data(),
                      original.pixels.data(),
                      original.pixels.size()) == 0);
}

// Return the directory containing gradient test images.
// Prefer the GRADIENT_TEST_DIR environment variable; fall back to the path
// relative to the repository root that the CI working directory uses.
static std::string GradientDir()
{
    if (const char* e = std::getenv("GRADIENT_TEST_DIR"))
        return e;
#ifdef GRADIENT_TEST_DIR_DEFAULT
    return GRADIENT_TEST_DIR_DEFAULT;
#else
    return "third-party/gradient/test_sequences/1920x1080";
#endif
}

// ── Test suite ────────────────────────────────────────────────────────────────

TEST_SUITE("IWI format: PNG round-trip (gradient 1920x1080)")
{
    TEST_CASE("gradient 0-25")
    {
        RoundTripTest(GradientDir() + "/gradient_1920-1080_0-25.png");
    }

    TEST_CASE("gradient 25-50")
    {
        RoundTripTest(GradientDir() + "/gradient_1920-1080_25-50.png");
    }

    TEST_CASE("gradient 50-75")
    {
        RoundTripTest(GradientDir() + "/gradient_1920-1080_50-75.png");
    }

    TEST_CASE("gradient 75-100")
    {
        RoundTripTest(GradientDir() + "/gradient_1920-1080_75-100.png");
    }
}

TEST_SUITE("IWI format: serialization")
{
    TEST_CASE("RGB8 binary round-trip (synthetic)")
    {
        constexpr int W = 4, H = 2;
        IWImageFormat fmt = {};
        fmt.componentCount = 3;
        fmt.bitsPerPixel   = 24;
        fmt.storageLayout  = IW_STORAGE_INTERLEAVED;
        fmt.components[0]  = { IW_COMPONENT_SEMANTIC_R, IW_COMPONENT_CLASS_UINT,  0, 8 };
        fmt.components[1]  = { IW_COMPONENT_SEMANTIC_G, IW_COMPONENT_CLASS_UINT,  8, 8 };
        fmt.components[2]  = { IW_COMPONENT_SEMANTIC_B, IW_COMPONENT_CLASS_UINT, 16, 8 };

        std::vector<uint8_t> pixels(W * H * 3);
        for (int i = 0; i < W * H * 3; ++i)
            pixels[static_cast<size_t>(i)] = static_cast<uint8_t>(i * 13 % 256);

        const iw::Image iwi = iw::BuildImage(
            W, H, IMAGE_COLOR_SPACE_SRGB, fmt,
            pixels.data(), pixels.size());

        const std::vector<uint8_t> buf = iw::SerializeImage(iwi);
        const iw::Image restored = iw::DeserializeImage(buf.data(), buf.size());

        CHECK(restored.width  == W);
        CHECK(restored.height == H);
        CHECK(restored.colorSpace == IMAGE_COLOR_SPACE_SRGB);
        REQUIRE(restored.pixelData.size() == pixels.size());
        CHECK(std::memcmp(restored.pixelData.data(),
                          pixels.data(), pixels.size()) == 0);
    }

    TEST_CASE("GRAY16 planar binary round-trip (synthetic)")
    {
        constexpr int W = 3, H = 3;
        IWImageFormat fmt = {};
        fmt.componentCount = 1;
        fmt.bitsPerPixel   = 16;
        fmt.storageLayout  = IW_STORAGE_PLANAR;
        fmt.components[0]  = { IW_COMPONENT_SEMANTIC_GRAY, IW_COMPONENT_CLASS_UNORM, 0, 16 };

        std::vector<uint8_t> pixels(W * H * 2);
        for (size_t i = 0; i < pixels.size(); ++i)
            pixels[i] = static_cast<uint8_t>(i * 7 % 256);

        const iw::Image iwi = iw::BuildImage(
            W, H, IMAGE_COLOR_SPACE_LINEAR, fmt,
            pixels.data(), pixels.size());

        const std::vector<uint8_t> buf = iw::SerializeImage(iwi);
        const iw::Image restored = iw::DeserializeImage(buf.data(), buf.size());

        CHECK(restored.format.storageLayout  == IW_STORAGE_PLANAR);
        CHECK(restored.format.componentCount == 1);
        CHECK(restored.format.bitsPerPixel   == 16);
        CHECK(restored.format.components[0].semantic == IW_COMPONENT_SEMANTIC_GRAY);
        CHECK(restored.colorSpace == IMAGE_COLOR_SPACE_LINEAR);
        REQUIRE(restored.pixelData.size() == pixels.size());
        CHECK(std::memcmp(restored.pixelData.data(),
                          pixels.data(), pixels.size()) == 0);
    }

    TEST_CASE("SaveIWI / LoadIWI file round-trip (synthetic RGB8)")
    {
        constexpr int W = 8, H = 4;
        IWImageFormat fmt = {};
        fmt.componentCount = 3;
        fmt.bitsPerPixel   = 24;
        fmt.storageLayout  = IW_STORAGE_INTERLEAVED;
        fmt.components[0]  = { IW_COMPONENT_SEMANTIC_R, IW_COMPONENT_CLASS_UINT,  0, 8 };
        fmt.components[1]  = { IW_COMPONENT_SEMANTIC_G, IW_COMPONENT_CLASS_UINT,  8, 8 };
        fmt.components[2]  = { IW_COMPONENT_SEMANTIC_B, IW_COMPONENT_CLASS_UINT, 16, 8 };

        std::vector<uint8_t> pixels(W * H * 3);
        for (size_t i = 0; i < pixels.size(); ++i)
            pixels[i] = static_cast<uint8_t>(i * 31 % 256);

        const iw::Image iwi = iw::BuildImage(
            W, H, IMAGE_COLOR_SPACE_SRGB, fmt,
            pixels.data(), pixels.size());

        namespace fs = std::filesystem;
        const std::string tmp = (fs::temp_directory_path() / "iview_test_round_trip.iwi").string();

        iw::SaveIWI(tmp, iwi);
        const iw::Image loaded = iw::LoadIWI(tmp);
        fs::remove(tmp);

        CHECK(loaded.width  == W);
        CHECK(loaded.height == H);
        CHECK(loaded.colorSpace == IMAGE_COLOR_SPACE_SRGB);
        CHECK(loaded.format.componentCount == fmt.componentCount);
        CHECK(loaded.format.bitsPerPixel   == fmt.bitsPerPixel);
        REQUIRE(loaded.pixelData.size() == pixels.size());
        CHECK(std::memcmp(loaded.pixelData.data(),
                          pixels.data(), pixels.size()) == 0);
    }
}
