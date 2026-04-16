module;

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cstdint>
#include <new>
#include <stdexcept>
#include <string>
#include "ImagePluginDef.h"

#define STB_IMAGE_IMPLEMENTATION
#define STBI_NO_JPEG
#define STBI_NO_PNG
#include <stb_image.h>

export module ImageLoaderStb;

static thread_local std::string lastError;

export extern "C" IMAGEPLUGIN_API ImagePluginResult LoadImageFromFile(const ImagePluginPath filePath)
{
    ImagePluginResult result = { .code = IMAGE_PLUGIN_UNKNOWN_ERROR, .data = nullptr };
    lastError.clear();

    try
    {
        FILE* file = nullptr;
#ifdef _WIN32
        _wfopen_s(&file, filePath, L"rb");
#else
        file = std::fopen(filePath, "rb");
#endif
        if (file == nullptr)
            throw std::runtime_error("Failed to open file");

        int width = 0;
        int height = 0;
        int componentsPerPixel = 0;
        stbi_uc* pixels = stbi_load_from_file(file, &width, &height, &componentsPerPixel, 0);
        std::fclose(file);

        if (!pixels)
        {
            const char* stbiErr = stbi_failure_reason();
            throw std::runtime_error(std::string("stb_image decode failed: ") + (stbiErr ? stbiErr : "unknown"));
        }

        size_t dataSize = static_cast<size_t>(width) * height * componentsPerPixel;
        auto block = (ImagePluginData*)std::malloc(sizeof(ImagePluginData) + dataSize);
        if (!block)
        {
            stbi_image_free(pixels);
            throw std::runtime_error("Failed to allocate image buffer");
        }

        block->width = width;
        block->height = height;
        block->componentsPerPixel = componentsPerPixel;
        block->stride = width * componentsPerPixel;
        block->size = dataSize;
        block->data = static_cast<uint8_t*>((void*)block) + sizeof(ImagePluginData);

        std::memcpy(block->data, pixels, dataSize);
        stbi_image_free(pixels);

        result.code = IMAGE_PLUGIN_OK;
        result.data = block;
    }
    catch (const std::runtime_error& e)
    {
        lastError = e.what();
    }

    return result;
}

export extern "C" IMAGEPLUGIN_API void FreeImageData(ImagePluginData* imageData)
{
    if (imageData)
        std::free(imageData);
}

export extern "C" IMAGEPLUGIN_API const char* ImagePluginGetLastError()
{
    return lastError.c_str();
}
