module;

#include <cstdio>
#include <cstring>
#include <cstdint>
#include <filesystem>
#include "ImagePluginDef.h"

#define STB_IMAGE_IMPLEMENTATION
#define STBI_NO_JPEG
#define STBI_NO_PNG
#include <stb_image.h>

export module ImageLoaderStb;

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

    int width = 0;
    int height = 0;
    int componentsPerPixel = 0;
    stbi_uc* pixels = stbi_load_from_file(file, &width, &height, &componentsPerPixel, 0);
    std::fclose(file);

    if (!pixels)
    {
        const char* stbiErr = stbi_failure_reason();
        snprintf(lastError, sizeof(lastError), "stb_image decode failed: %s", stbiErr ? stbiErr : "unknown");
        result.code = IMAGE_PLUGIN_DECODE_ERROR;
        return result;
    }

    size_t dataSize = static_cast<size_t>(width) * height * componentsPerPixel;
    ImagePluginData* data = new ImagePluginData{
        .width = width,
        .height = height,
        .componentsPerPixel = componentsPerPixel,
        .stride = width * componentsPerPixel,
        .size = dataSize,
        .data = new uint8_t[dataSize]
    };
    if (!data->data)
    {
        stbi_image_free(pixels);
        delete data;
        snprintf(lastError, sizeof(lastError), "Failed to allocate image buffer");
        result.code = IMAGE_PLUGIN_ALLOC_ERROR;
        return result;
    }

    std::memcpy(data->data, pixels, dataSize);
    stbi_image_free(pixels);

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
