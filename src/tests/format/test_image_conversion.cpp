#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>

#include <algorithm>
#include <cassert>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <stdexcept>
#include <string>
#include <vector>

#include <png.h>

#include "ImagePluginDef.h"
#include "IWImageIO.hpp"

// ── PNG helpers (mirrors ImageLoaderPng.ixx and iwconv.cpp) ──────────────────

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
    png_infop info  = png_create_info_struct(png);
    REQUIRE(png != nullptr);
    REQUIRE(info != nullptr);

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

// Performs a lossless PNG → IWI (in-memory proto) → pixel comparison.
// The .iwi format stores raw pixel data, so the round-trip must be bit-exact.
static void RoundTripTest(const std::string& pngPath)
{
    const PNGImageData original = ReadPNG(pngPath);
    const IWImageFormat fmt     = BuildFormatFromPNG(original);

    // PNG -> proto
    const iw::Image protoImg = iw::BuildProtoImage(
        original.width, original.height, original.colorSpace,
        fmt, original.pixels.data(), original.pixels.size());

    // Serialize to string and deserialize back (tests full proto round-trip)
    std::string serialized;
    REQUIRE(protoImg.SerializeToString(&serialized));

    iw::Image restored;
    REQUIRE(restored.ParseFromString(serialized));

    // Dimensions must be preserved
    CHECK(static_cast<int>(restored.width())  == original.width);
    CHECK(static_cast<int>(restored.height()) == original.height);

    // Format must be preserved
    const IWImageFormat restoredFmt = iw::FromProtoFormat(restored.format());
    CHECK(restoredFmt.componentCount == fmt.componentCount);
    CHECK(restoredFmt.bitsPerPixel   == fmt.bitsPerPixel);
    CHECK(restoredFmt.storageLayout  == fmt.storageLayout);
    for (uint16_t i = 0; i < fmt.componentCount; ++i) {
        CHECK(restoredFmt.components[i].semantic       == fmt.components[i].semantic);
        CHECK(restoredFmt.components[i].componentClass == fmt.components[i].componentClass);
        CHECK(restoredFmt.components[i].bitOffset      == fmt.components[i].bitOffset);
        CHECK(restoredFmt.components[i].bitWidth       == fmt.components[i].bitWidth);
    }

    // Pixel data must be bit-exact (lossless format)
    const std::string& restoredPixels = restored.pixel_data();
    REQUIRE(restoredPixels.size() == original.pixels.size());
    CHECK(std::memcmp(restoredPixels.data(),
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
    TEST_CASE("RGB8 proto round-trip (synthetic)")
    {
        // Construct a tiny synthetic RGB8 image
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

        const iw::Image protoImg = iw::BuildProtoImage(
            W, H, IMAGE_COLOR_SPACE_SRGB, fmt,
            pixels.data(), pixels.size());

        std::string buf;
        REQUIRE(protoImg.SerializeToString(&buf));

        iw::Image restored;
        REQUIRE(restored.ParseFromString(buf));

        CHECK(restored.width()  == W);
        CHECK(restored.height() == H);
        CHECK(iw::FromProtoColorSpace(restored.color_space()) == IMAGE_COLOR_SPACE_SRGB);
        REQUIRE(restored.pixel_data().size() == pixels.size());
        CHECK(std::memcmp(restored.pixel_data().data(),
                          pixels.data(), pixels.size()) == 0);
    }

    TEST_CASE("GRAY16 planar proto round-trip (synthetic)")
    {
        constexpr int W = 3, H = 3;
        IWImageFormat fmt = {};
        fmt.componentCount = 1;
        fmt.bitsPerPixel   = 16;
        fmt.storageLayout  = IW_STORAGE_PLANAR;
        fmt.components[0]  = { IW_COMPONENT_SEMANTIC_GRAY, IW_COMPONENT_CLASS_UNORM, 0, 16 };

        std::vector<uint8_t> pixels(W * H * 2);  // 2 bytes per sample
        for (size_t i = 0; i < pixels.size(); ++i)
            pixels[i] = static_cast<uint8_t>(i * 7 % 256);

        const iw::Image protoImg = iw::BuildProtoImage(
            W, H, IMAGE_COLOR_SPACE_LINEAR, fmt,
            pixels.data(), pixels.size());

        std::string buf;
        REQUIRE(protoImg.SerializeToString(&buf));

        iw::Image restored;
        REQUIRE(restored.ParseFromString(buf));

        const IWImageFormat restoredFmt = iw::FromProtoFormat(restored.format());
        CHECK(restoredFmt.storageLayout  == IW_STORAGE_PLANAR);
        CHECK(restoredFmt.componentCount == 1);
        CHECK(restoredFmt.bitsPerPixel   == 16);
        CHECK(restoredFmt.components[0].semantic == IW_COMPONENT_SEMANTIC_GRAY);
        CHECK(iw::FromProtoColorSpace(restored.color_space()) == IMAGE_COLOR_SPACE_LINEAR);
        REQUIRE(restored.pixel_data().size() == pixels.size());
        CHECK(std::memcmp(restored.pixel_data().data(),
                          pixels.data(), pixels.size()) == 0);
    }
}
