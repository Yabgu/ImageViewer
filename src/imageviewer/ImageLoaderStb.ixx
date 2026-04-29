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

        // Detect whether the file carries HDR (32-bit float) data.
        // stbi_is_hdr_from_file reads only the header, so rewind afterwards.
        bool isHdr = (stbi_is_hdr_from_file(file) != 0);
        std::fseek(file, 0, SEEK_SET);

        int width = 0;
        int height = 0;
        int componentsPerPixel = 0;

        ImagePixelFormat  pixelFormat;
        ImageColorSpace   colorSpace;

        void* pixels = nullptr;
        size_t bytesPerChannel;

        if (isHdr)
        {
            pixels          = stbi_loadf_from_file(file, &width, &height, &componentsPerPixel, 0);
            pixelFormat     = IMAGE_PIXEL_FORMAT_F32;
            colorSpace      = IMAGE_COLOR_SPACE_LINEAR;
            bytesPerChannel = 4;
        }
        else
        {
            pixels          = stbi_load_from_file(file, &width, &height, &componentsPerPixel, 0);
            pixelFormat     = IMAGE_PIXEL_FORMAT_U8;
            colorSpace      = IMAGE_COLOR_SPACE_SRGB;
            bytesPerChannel = 1;
        }
        std::fclose(file);

        if (!pixels)
        {
            const char* stbiErr = stbi_failure_reason();
            throw std::runtime_error(std::string("stb_image decode failed: ") + (stbiErr ? stbiErr : "unknown"));
        }

        ImageChannelOrder channelOrder;
        switch (componentsPerPixel) {
        case 1:  channelOrder = IMAGE_CHANNEL_ORDER_GRAY;       break;
        case 2:  channelOrder = IMAGE_CHANNEL_ORDER_GRAY_ALPHA; break;
        case 3:  channelOrder = IMAGE_CHANNEL_ORDER_RGB;        break;
        case 4:  channelOrder = IMAGE_CHANNEL_ORDER_RGBA;       break;
        default:
            stbi_image_free(pixels);
            throw std::runtime_error("Unsupported number of channels: " + std::to_string(componentsPerPixel));
        }

        /* Build the per-component format descriptor. */
        IWImageFormat fmt{};
        fmt.componentCount = (uint16_t)componentsPerPixel;
        const uint16_t bitsPerComp = isHdr ? 32u : 8u;
        fmt.bitsPerPixel   = (uint16_t)(componentsPerPixel * bitsPerComp);
        const IWComponentClass cls = isHdr ? IW_COMPONENT_CLASS_FLOAT : IW_COMPONENT_CLASS_UNORM;
        switch (channelOrder) {
        case IMAGE_CHANNEL_ORDER_GRAY:
            fmt.components[0] = { IW_COMPONENT_SEMANTIC_GRAY, cls, 0, bitsPerComp };
            break;
        case IMAGE_CHANNEL_ORDER_GRAY_ALPHA:
            fmt.components[0] = { IW_COMPONENT_SEMANTIC_GRAY, cls,            0, bitsPerComp };
            fmt.components[1] = { IW_COMPONENT_SEMANTIC_A,    cls, bitsPerComp, bitsPerComp };
            break;
        case IMAGE_CHANNEL_ORDER_RGB:
            fmt.components[0] = { IW_COMPONENT_SEMANTIC_R, cls,                0, bitsPerComp };
            fmt.components[1] = { IW_COMPONENT_SEMANTIC_G, cls,   bitsPerComp, bitsPerComp };
            fmt.components[2] = { IW_COMPONENT_SEMANTIC_B, cls, (uint16_t)(2*bitsPerComp), bitsPerComp };
            break;
        default: /* IMAGE_CHANNEL_ORDER_RGBA */
            fmt.components[0] = { IW_COMPONENT_SEMANTIC_R, cls,                0, bitsPerComp };
            fmt.components[1] = { IW_COMPONENT_SEMANTIC_G, cls,   bitsPerComp, bitsPerComp };
            fmt.components[2] = { IW_COMPONENT_SEMANTIC_B, cls, (uint16_t)(2*bitsPerComp), bitsPerComp };
            fmt.components[3] = { IW_COMPONENT_SEMANTIC_A, cls, (uint16_t)(3*bitsPerComp), bitsPerComp };
            break;
        }

        size_t dataSize = static_cast<size_t>(width) * height * componentsPerPixel * bytesPerChannel;
        auto block = static_cast<ImagePluginData*>(std::malloc(sizeof(ImagePluginData) + dataSize));
        if (!block)
        {
            stbi_image_free(pixels);
            throw std::runtime_error("Failed to allocate image buffer");
        }

        block->width              = width;
        block->height             = height;
        block->stride             = static_cast<int>(static_cast<size_t>(width) * componentsPerPixel * bytesPerChannel);
        block->size               = dataSize;
        block->data               = static_cast<uint8_t*>(static_cast<void*>(block)) + sizeof(ImagePluginData);
        block->colorSpace         = colorSpace;
        block->format             = fmt;

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
