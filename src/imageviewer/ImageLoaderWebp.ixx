module;

#include <cstdio>
#include <cstring>
#include <cstdint>
#include <filesystem>
#include "ImagePluginDef.h"
#include <webp/decode.h>

export module ImageLoaderWebp;

static thread_local char lastError[256] = "";

export extern "C" IMAGEPLUGIN_API ImagePluginResult LoadImageFromFile(const ImagePluginPath filePath)
{
    ImagePluginResult result = { .code = IMAGE_PLUGIN_UNKNOWN_ERROR, .data = nullptr };
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

    std::fseek(file, 0, SEEK_END);
    long fileSize = std::ftell(file);
    std::fseek(file, 0, SEEK_SET);

    if (fileSize <= 0)
    {
        std::fclose(file);
        snprintf(lastError, sizeof(lastError), "Invalid file size");
        result.code = IMAGE_PLUGIN_FILE_ERROR;
        return result;
    }

    uint8_t* fileData = new uint8_t[fileSize];
    if (std::fread(fileData, 1, fileSize, file) != static_cast<size_t>(fileSize))
    {
        std::fclose(file);
        delete[] fileData;
        snprintf(lastError, sizeof(lastError), "Failed to read file");
        result.code = IMAGE_PLUGIN_FILE_ERROR;
        return result;
    }
    std::fclose(file);

    int width = 0;
    int height = 0;
    WebPBitstreamFeatures features{};
    if (WebPGetFeatures(fileData, fileSize, &features) == VP8_STATUS_OK && features.has_alpha)
    {
        uint8_t* decoded = WebPDecodeRGBA(fileData, fileSize, &width, &height);
        delete[] fileData;
        if (!decoded)
        {
            snprintf(lastError, sizeof(lastError), "WebP RGBA decode failed");
            result.code = IMAGE_PLUGIN_DECODE_ERROR;
            return result;
        }
        constexpr int components = 4;
        ImagePluginData* data = new ImagePluginData{
            .width = width,
            .height = height,
            .componentsPerPixel = components,
            .stride = width * components,
            .size = static_cast<size_t>(width * height * components),
            .data = new uint8_t[width * height * components],
            .pixelFormat  = IMAGE_PIXEL_FORMAT_U8,
            .colorSpace   = IMAGE_COLOR_SPACE_SRGB,
            .channelOrder = IMAGE_CHANNEL_ORDER_RGBA
        };
        std::memcpy(data->data, decoded, data->size);
        WebPFree(decoded);
        result.code = IMAGE_PLUGIN_OK;
        result.data = data;
    }
    else
    {
        uint8_t* decoded = WebPDecodeRGB(fileData, fileSize, &width, &height);
        delete[] fileData;
        if (!decoded)
        {
            snprintf(lastError, sizeof(lastError), "WebP RGB decode failed");
            result.code = IMAGE_PLUGIN_DECODE_ERROR;
            return result;
        }
        constexpr int components = 3;
        ImagePluginData* data = new ImagePluginData{
            .width = width,
            .height = height,
            .componentsPerPixel = components,
            .stride = width * components,
            .size = static_cast<size_t>(width * height * components),
            .data = new uint8_t[width * height * components],
            .pixelFormat  = IMAGE_PIXEL_FORMAT_U8,
            .colorSpace   = IMAGE_COLOR_SPACE_SRGB,
            .channelOrder = IMAGE_CHANNEL_ORDER_RGB
        };
        std::memcpy(data->data, decoded, data->size);
        WebPFree(decoded);
        result.code = IMAGE_PLUGIN_OK;
        result.data = data;
    }

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
