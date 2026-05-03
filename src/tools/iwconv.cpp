#include <algorithm>
#include <cassert>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <stdexcept>
#include <string>
#include <vector>

#include <png.h>

#include "ImagePluginDef.h"
#include "IWImageIO.hpp"

// ── PNG helpers ───────────────────────────────────────────────────────────────

struct PNGImage {
    int width    = 0;
    int height   = 0;
    int channels = 0;
    int bitDepth = 0;
    std::vector<uint8_t> pixels;
    ImageColorSpace colorSpace = IMAGE_COLOR_SPACE_UNKNOWN;
};

static PNGImage ReadPNG(const std::string& path)
{
    PNGImage out;

    FILE* f = fopen(path.c_str(), "rb");
    if (!f)
        throw std::runtime_error("Cannot open: " + path);

    png_structp png = png_create_read_struct(PNG_LIBPNG_VER_STRING,
                                             nullptr, nullptr, nullptr);
    png_infop info  = png_create_info_struct(png);
    if (!png || !info) {
        fclose(f);
        throw std::runtime_error("PNG struct alloc failed");
    }

    if (setjmp(png_jmpbuf(png))) {
        png_destroy_read_struct(&png, &info, nullptr);
        fclose(f);
        throw std::runtime_error("PNG decode error reading: " + path);
    }

    png_init_io(png, f);
    png_read_info(png, info);

    int ct = png_get_color_type(png, info);
    int bd = png_get_bit_depth(png, info);

    // Apply standard transforms (same logic as ImageLoaderPng.ixx)
    if (ct == PNG_COLOR_TYPE_PALETTE)
        png_set_palette_to_rgb(png);
    if (ct == PNG_COLOR_TYPE_GRAY && bd < 8)
        png_set_expand_gray_1_2_4_to_8(png);
    if (png_get_valid(png, info, PNG_INFO_tRNS))
        png_set_tRNS_to_alpha(png);
    if (bd == 16)
        png_set_swap(png);  // PNG is big-endian; swap to host byte order

    png_read_update_info(png, info);

    out.width    = static_cast<int>(png_get_image_width(png, info));
    out.height   = static_cast<int>(png_get_image_height(png, info));
    out.bitDepth = static_cast<int>(png_get_bit_depth(png, info));
    out.channels = static_cast<int>(png_get_channels(png, info));

    // Detect colour space from PNG metadata
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

static void WritePNG(const std::string& path, const iw::Image& proto_img)
{
    const IWImageFormat fmt = iw::FromProtoFormat(proto_img.format());
    assert(fmt.componentCount > 0 && fmt.components[0].bitWidth > 0);

    const int w   = static_cast<int>(proto_img.width());
    const int h   = static_cast<int>(proto_img.height());
    const int n   = static_cast<int>(fmt.componentCount);
    const int bw  = static_cast<int>(fmt.components[0].bitWidth);
    const int bpc = (bw + 7) / 8;
    const int stride = w * n * bpc;

    FILE* f = fopen(path.c_str(), "wb");
    if (!f)
        throw std::runtime_error("Cannot create: " + path);

    png_structp png = png_create_write_struct(PNG_LIBPNG_VER_STRING,
                                              nullptr, nullptr, nullptr);
    png_infop info  = png_create_info_struct(png);
    if (!png || !info) {
        fclose(f);
        throw std::runtime_error("PNG write struct alloc failed");
    }

    if (setjmp(png_jmpbuf(png))) {
        png_destroy_write_struct(&png, &info);
        fclose(f);
        throw std::runtime_error("PNG write error: " + path);
    }

    png_init_io(png, f);

    // Determine PNG colour type from component semantics
    bool hasAlpha = false;
    bool isGray   = false;
    for (int i = 0; i < n; ++i) {
        if (fmt.components[i].semantic == IW_COMPONENT_SEMANTIC_A)    hasAlpha = true;
        if (fmt.components[i].semantic == IW_COMPONENT_SEMANTIC_GRAY) isGray   = true;
    }
    const int color_type = isGray
        ? (hasAlpha ? PNG_COLOR_TYPE_GRAY_ALPHA : PNG_COLOR_TYPE_GRAY)
        : (hasAlpha ? PNG_COLOR_TYPE_RGBA        : PNG_COLOR_TYPE_RGB);

    png_set_IHDR(png, info, static_cast<png_uint_32>(w), static_cast<png_uint_32>(h),
                 bw, color_type,
                 PNG_INTERLACE_NONE, PNG_COMPRESSION_TYPE_DEFAULT, PNG_FILTER_TYPE_DEFAULT);

    // Preserve colour space metadata
    const ImageColorSpace cs = iw::FromProtoColorSpace(proto_img.color_space());
    if (cs == IMAGE_COLOR_SPACE_SRGB)
        png_set_sRGB(png, info, PNG_sRGB_INTENT_PERCEPTUAL);
    else if (cs == IMAGE_COLOR_SPACE_LINEAR)
        png_set_gAMA(png, info, 1.0);

    png_write_info(png, info);

    if (bw == 16)
        png_set_swap(png);  // host → PNG big-endian

    const auto* pixels =
        reinterpret_cast<const uint8_t*>(proto_img.pixel_data().data());
    for (int y = 0; y < h; ++y) {
        // png_write_rows takes a non-const pointer; data is not modified
        png_bytep row =
            const_cast<uint8_t*>(pixels + static_cast<ptrdiff_t>(y) * stride);
        png_write_rows(png, &row, 1);
    }

    png_write_end(png, nullptr);
    png_destroy_write_struct(&png, &info);
    fclose(f);
}

static IWImageFormat BuildFormatFromPNG(const PNGImage& img)
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

static std::string ToLower(std::string s)
{
    std::transform(s.begin(), s.end(), s.begin(),
                   [](unsigned char c){ return static_cast<char>(std::tolower(c)); });
    return s;
}

// ── main ─────────────────────────────────────────────────────────────────────

int main(int argc, char* argv[])
{
    if (argc != 3) {
        fprintf(stderr, "Usage: %s <input> <output>\n", argv[0]);
        fprintf(stderr, "  .png  -> .iwi   Convert PNG to IWI image definition\n");
        fprintf(stderr, "  .iwi  -> .png   Convert IWI image definition to PNG\n");
        return 1;
    }

    const std::string input  = argv[1];
    const std::string output = argv[2];

    namespace fs = std::filesystem;
    const std::string inExt  = ToLower(fs::path(input).extension().string());
    const std::string outExt = ToLower(fs::path(output).extension().string());

    try {
        if (inExt == ".png" && outExt == ".iwi") {
            const PNGImage      img      = ReadPNG(input);
            const IWImageFormat fmt      = BuildFormatFromPNG(img);
            const iw::Image     protoImg = iw::BuildProtoImage(
                img.width, img.height, img.colorSpace,
                fmt, img.pixels.data(), img.pixels.size());
            iw::SaveIWI(output, protoImg);
            fprintf(stdout, "Converted %s -> %s\n", input.c_str(), output.c_str());

        } else if (inExt == ".iwi" && outExt == ".png") {
            const iw::Image protoImg = iw::LoadIWI(input);
            WritePNG(output, protoImg);
            fprintf(stdout, "Converted %s -> %s\n", input.c_str(), output.c_str());

        } else {
            fprintf(stderr, "Unsupported conversion: %s -> %s\n",
                    inExt.c_str(), outExt.c_str());
            fprintf(stderr, "Supported: .png -> .iwi   or   .iwi -> .png\n");
            return 1;
        }
    } catch (const std::exception& e) {
        fprintf(stderr, "Error: %s\n", e.what());
        return 1;
    }

    return 0;
}
