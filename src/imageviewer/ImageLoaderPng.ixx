module;

#include <cstdio>
#include <stdexcept>
#include <cstring>
#include <cstdint>
#include <png.h>
#include <filesystem>
#include <format>
#include "ImagePluginDef.h"

export module ImageLoaderPng;

static thread_local char lastError[256] = "";

export extern "C" IMAGEPLUGIN_API ImagePluginResult LoadImageFromFile(const ImagePluginPath filePath)
{
    ImagePluginResult result = { IMAGE_PLUGIN_UNKNOWN_ERROR, nullptr };
    lastError[0] = '\0';
    FILE* file = nullptr;
#ifdef _WIN32
    auto err = _wfopen_s(&file, filePath, L"rb");
#else
    file = std::fopen(filePath, "rb");
#endif
    if (file == nullptr)
    {
        snprintf(lastError, sizeof(lastError), "Failed to open file");
        result.code = IMAGE_PLUGIN_FILE_ERROR;
        return result;
    }

    png_structp png_ptr = png_create_read_struct(PNG_LIBPNG_VER_STRING, nullptr, nullptr, nullptr);
    if (!png_ptr)
    {
        fclose(file);
        snprintf(lastError, sizeof(lastError), "Failed to create PNG read struct");
        result.code = IMAGE_PLUGIN_ALLOC_ERROR;
        return result;
    }

    png_infop info_ptr = png_create_info_struct(png_ptr);
    if (!info_ptr)
    {
        png_destroy_read_struct(&png_ptr, nullptr, nullptr);
        fclose(file);
        snprintf(lastError, sizeof(lastError), "Failed to create PNG info struct");
        result.code = IMAGE_PLUGIN_ALLOC_ERROR;
        return result;
    }

    if (setjmp(png_jmpbuf(png_ptr)))
    {
        png_destroy_read_struct(&png_ptr, &info_ptr, nullptr);
        fclose(file);
        snprintf(lastError, sizeof(lastError), "PNG decode failed");
        result.code = IMAGE_PLUGIN_DECODE_ERROR;
        return result;
    }

    png_init_io(png_ptr, file);
    png_read_info(png_ptr, info_ptr);

    int width      = png_get_image_width(png_ptr, info_ptr);
    int height     = png_get_image_height(png_ptr, info_ptr);
    int color_type = png_get_color_type(png_ptr, info_ptr);
    int bit_depth  = png_get_bit_depth(png_ptr, info_ptr);

    // Expand palette to RGB
    if (color_type == PNG_COLOR_TYPE_PALETTE)
        png_set_palette_to_rgb(png_ptr);
    // Expand low-bit-depth gray to 8-bit
    if (color_type == PNG_COLOR_TYPE_GRAY && bit_depth < 8)
        png_set_expand_gray_1_2_4_to_8(png_ptr);
    // Expand tRNS transparency chunk to a full alpha channel
    if (png_get_valid(png_ptr, info_ptr, PNG_INFO_tRNS))
        png_set_tRNS_to_alpha(png_ptr);
    // For 16-bit data swap from big-endian (PNG native) to host byte order
    if (bit_depth == 16)
        png_set_swap(png_ptr);

    png_read_update_info(png_ptr, info_ptr);

    // Re-read metadata after transforms
    bit_depth  = png_get_bit_depth(png_ptr, info_ptr);
    color_type = png_get_color_type(png_ptr, info_ptr);
    int componentsPerPixel = png_get_channels(png_ptr, info_ptr);

    // --- Pixel format ---
    ImagePixelFormat pixelFormat = (bit_depth == 16) ? IMAGE_PIXEL_FORMAT_U16 : IMAGE_PIXEL_FORMAT_U8;
    int bpc = ImagePixelFormatBytesPerChannel(pixelFormat);

    // --- Colour space: prefer explicit PNG metadata over heuristics ---
    ImageColorSpace colorSpace;
    {
        // Gamma value for linear light (1.0) expressed in the same scale as
        // the value returned by png_get_gAMA (which already gives a double).
        constexpr double LINEAR_GAMMA_LOWER = 0.99;
        constexpr double LINEAR_GAMMA_UPPER = 1.01;
        int srgb_intent = -1;
        double gamma_val = 0.0;
        if (png_get_sRGB(png_ptr, info_ptr, &srgb_intent) == PNG_INFO_sRGB) {
            colorSpace = IMAGE_COLOR_SPACE_SRGB;
        } else if (png_get_gAMA(png_ptr, info_ptr, &gamma_val) == PNG_INFO_gAMA
                   && gamma_val > LINEAR_GAMMA_LOWER && gamma_val < LINEAR_GAMMA_UPPER) {
            colorSpace = IMAGE_COLOR_SPACE_LINEAR;
        } else {
            // Convention: 16-bit PNG is most commonly linear-light encoded
            colorSpace = (bit_depth == 16) ? IMAGE_COLOR_SPACE_LINEAR : IMAGE_COLOR_SPACE_SRGB;
        }
    }

    // --- Channel order ---
    ImageChannelOrder channelOrder;
    switch (color_type) {
    case PNG_COLOR_TYPE_GRAY:
        channelOrder = IMAGE_CHANNEL_ORDER_GRAY;
        break;
    case PNG_COLOR_TYPE_GRAY_ALPHA:
        channelOrder = IMAGE_CHANNEL_ORDER_GRAY_ALPHA;
        break;
    case PNG_COLOR_TYPE_RGBA:
        channelOrder = IMAGE_CHANNEL_ORDER_RGBA;
        break;
    default: /* PNG_COLOR_TYPE_RGB */
        channelOrder = IMAGE_CHANNEL_ORDER_RGB;
        break;
    }

    int    strideBytes = width * componentsPerPixel * bpc;
    size_t dataSize    = static_cast<size_t>(height) * strideBytes;

    ImagePluginData* data = new ImagePluginData{
        .width             = width,
        .height            = height,
        .componentsPerPixel = componentsPerPixel,
        .stride            = strideBytes,
        .size              = dataSize,
        .data              = new uint8_t[dataSize],
        .pixelFormat       = pixelFormat,
        .colorSpace        = colorSpace,
        .channelOrder      = channelOrder
    };
    if (!data->data) {
        png_destroy_read_struct(&png_ptr, &info_ptr, nullptr);
        fclose(file);
        delete data;
        snprintf(lastError, sizeof(lastError), "Failed to allocate image buffer");
        result.code = IMAGE_PLUGIN_ALLOC_ERROR;
        return result;
    }

    for (int y = 0; y < height; ++y)
    {
        png_bytep row = data->data + static_cast<size_t>(y) * strideBytes;
        png_read_rows(png_ptr, &row, nullptr, 1);
    }

    png_destroy_read_struct(&png_ptr, &info_ptr, nullptr);
    fclose(file);
    result.code = IMAGE_PLUGIN_OK;
    result.data = data;
    return result;
}

export extern "C" IMAGEPLUGIN_API void FreeImageData(ImagePluginData* imageData)
{
    if (imageData)
    {
        delete[] imageData->data;
        delete imageData;
    }
}

export extern "C" IMAGEPLUGIN_API const char* ImagePluginGetLastError()
{
    return lastError;
}
