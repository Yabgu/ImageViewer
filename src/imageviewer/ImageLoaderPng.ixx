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

export extern "C" __declspec(dllexport) ImagePluginResult LoadImageFromFile(const wchar_t* filePath)
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

    int width = png_get_image_width(png_ptr, info_ptr);
    int height = png_get_image_height(png_ptr, info_ptr);
    int color_type = png_get_color_type(png_ptr, info_ptr);
    int bit_depth = png_get_bit_depth(png_ptr, info_ptr);

    if (bit_depth == 16)
        png_set_strip_16(png_ptr);
    if (color_type == PNG_COLOR_TYPE_PALETTE)
        png_set_palette_to_rgb(png_ptr);
    if (color_type == PNG_COLOR_TYPE_GRAY && bit_depth < 8)
        png_set_expand_gray_1_2_4_to_8(png_ptr);
    if (png_get_valid(png_ptr, info_ptr, PNG_INFO_tRNS))
        png_set_tRNS_to_alpha(png_ptr);

    png_read_update_info(png_ptr, info_ptr);
    int componentsPerPixel = png_get_channels(png_ptr, info_ptr);

    ImagePluginData* data = new ImagePluginData{
        .width = width,
        .height = height,
        .componentsPerPixel = componentsPerPixel,
        .stride = width * componentsPerPixel,
        .size = static_cast<size_t>(width * height * componentsPerPixel),
        .data = new uint8_t[width * height * componentsPerPixel]
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
        png_bytep row = data->data + y * data->stride;
        png_read_rows(png_ptr, &row, nullptr, 1);
    }

    png_destroy_read_struct(&png_ptr, &info_ptr, nullptr);
    fclose(file);
    result.code = IMAGE_PLUGIN_OK;
    result.data = data;
    return result;
}

export extern "C" __declspec(dllexport) void FreeImageData(ImagePluginData* imageData)
{
    if (imageData)
    {
        delete[] imageData->data;
        delete imageData;
    }
}

export extern "C" __declspec(dllexport) const char* GetLastError()
{
    return lastError;
}
