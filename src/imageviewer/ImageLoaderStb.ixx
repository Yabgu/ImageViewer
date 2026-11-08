module;

#include <climits>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cstdint>
#include <new>
#include <stdexcept>
#include <string>
#include <vector>
#include "ImagePluginDef.h"

/*
 * One format, one decoder: this plugin keeps only the formats nothing else in
 * the tree serves.  JPEG and PNG have loaders of their own, so their decoders
 * are compiled out; whatever remains here is served by stb and by nothing
 * else in the tree.
 */
#define STB_IMAGE_IMPLEMENTATION
#define STBI_NO_JPEG
#define STBI_NO_PNG
#include <stb_image.h>

export module ImageLoaderStb;

static thread_local std::string lastError;

namespace {

/*
 * Where the compressed bytes come from.  Exactly one is set.
 *
 * stb has a first-class memory entry point and no streaming one worth using
 * here (the *_from_file variants call the same seek/read layer), so both paths
 * converge on a span; the file path only has to materialise it.
 */
struct Source
{
    const uint8_t*  span = nullptr;
    size_t          size = 0;
    ImagePluginPath path = nullptr;
};

void ReadWholeFile(ImagePluginPath path, std::vector<uint8_t>& out)
{
    FILE* file = nullptr;
#ifdef _WIN32
    _wfopen_s(&file, path, L"rb");
#else
    file = std::fopen(path, "rb");
#endif
    if (file == nullptr)
        throw std::runtime_error("Failed to open file");

    std::fseek(file, 0, SEEK_END);
    const long fileSize = std::ftell(file);
    std::fseek(file, 0, SEEK_SET);
    if (fileSize <= 0)
    {
        std::fclose(file);
        throw std::runtime_error("Invalid file size");
    }

    out.resize(static_cast<size_t>(fileSize));
    const size_t got = std::fread(out.data(), 1, out.size(), file);
    std::fclose(file);
    if (got != out.size())
        throw std::runtime_error("Failed to read file");
}

ImagePluginResult DecodeStb(const Source& src)
{
    ImagePluginResult result = { .code = IMAGE_PLUGIN_UNKNOWN_ERROR, .data = nullptr };
    lastError.clear();

    std::vector<uint8_t> owned;
    const uint8_t* bytes = src.span;
    size_t         count = src.size;

    if (bytes == nullptr)
    {
        ReadWholeFile(src.path, owned);
        bytes = owned.data();
        count = owned.size();
    }
    else if (count == 0)
    {
        lastError = "Empty input";
        return ImagePluginResult{ IMAGE_PLUGIN_FILE_ERROR, nullptr };
    }

    /* stb's length parameter is `int`.  Reject rather than truncate: a cast
     * that wraps would hand stb a negative length and the failure would show up
     * as an unrelated decode error. */
    if (count > static_cast<size_t>(INT_MAX))
    {
        lastError = "Input too large for stb (over INT_MAX bytes)";
        return ImagePluginResult{ IMAGE_PLUGIN_FILE_ERROR, nullptr };
    }

    const int len = static_cast<int>(count);

    /* Detect whether the input carries HDR (32-bit float) data.  The
     * *_from_memory probe reads only the header. */
    const bool isHdr = (stbi_is_hdr_from_memory(bytes, len) != 0);

    int width = 0;
    int height = 0;
    int componentsPerPixel = 0;

    ImageColorSpace colorSpace;

    void* pixels = nullptr;
    size_t bytesPerChannel;

    if (isHdr)
    {
        pixels          = stbi_loadf_from_memory(bytes, len, &width, &height, &componentsPerPixel, 0);
        colorSpace      = IMAGE_COLOR_SPACE_LINEAR;
        bytesPerChannel = 4;
    }
    else
    {
        pixels          = stbi_load_from_memory(bytes, len, &width, &height, &componentsPerPixel, 0);
        colorSpace      = IMAGE_COLOR_SPACE_SRGB;
        bytesPerChannel = 1;
    }

    if (!pixels)
    {
        const char* stbiErr = stbi_failure_reason();
        throw std::runtime_error(std::string("stb_image decode failed: ") + (stbiErr ? stbiErr : "unknown"));
    }

    // Build per-component format descriptor
    IWImageFormat fmt = {};
    {
        const uint16_t bw  = static_cast<uint16_t>(bytesPerChannel * 8);
        const IWComponentClass cls = isHdr ? IW_COMPONENT_CLASS_FLOAT : IW_COMPONENT_CLASS_UNORM;
        fmt.componentCount = static_cast<uint16_t>(componentsPerPixel);
        fmt.bitsPerPixel   = static_cast<uint16_t>(componentsPerPixel) * bw;
        switch (componentsPerPixel) {
        case 1:
            fmt.components[0] = { IW_COMPONENT_SEMANTIC_GRAY, cls, 0, bw };
            break;
        case 2:
            fmt.components[0] = { IW_COMPONENT_SEMANTIC_GRAY, cls, 0,  bw };
            fmt.components[1] = { IW_COMPONENT_SEMANTIC_A,    cls, bw, bw };
            break;
        case 3:
            fmt.components[0] = { IW_COMPONENT_SEMANTIC_R, cls, static_cast<uint16_t>(0 * bw), bw };
            fmt.components[1] = { IW_COMPONENT_SEMANTIC_G, cls, static_cast<uint16_t>(1 * bw), bw };
            fmt.components[2] = { IW_COMPONENT_SEMANTIC_B, cls, static_cast<uint16_t>(2 * bw), bw };
            break;
        default:
            stbi_image_free(pixels);
            throw std::runtime_error("Unsupported number of channels: " + std::to_string(componentsPerPixel));
        case 4:
            fmt.components[0] = { IW_COMPONENT_SEMANTIC_R, cls, static_cast<uint16_t>(0 * bw), bw };
            fmt.components[1] = { IW_COMPONENT_SEMANTIC_G, cls, static_cast<uint16_t>(1 * bw), bw };
            fmt.components[2] = { IW_COMPONENT_SEMANTIC_B, cls, static_cast<uint16_t>(2 * bw), bw };
            fmt.components[3] = { IW_COMPONENT_SEMANTIC_A, cls, static_cast<uint16_t>(3 * bw), bw };
            break;
        }
    }

    size_t dataSize = static_cast<size_t>(width) * height * componentsPerPixel * bytesPerChannel;
    auto block = static_cast<ImagePluginData*>(std::malloc(sizeof(ImagePluginData) + dataSize));
    if (!block)
    {
        stbi_image_free(pixels);
        throw std::runtime_error("Failed to allocate image buffer");
    }

    block->width      = width;
    block->height     = height;
    block->stride     = static_cast<int>(static_cast<size_t>(width) * componentsPerPixel * bytesPerChannel);
    block->colorSpace = colorSpace;
    block->size       = dataSize;
    block->data       = static_cast<uint8_t*>(static_cast<void*>(block)) + sizeof(ImagePluginData);
    block->format     = fmt;

    std::memcpy(block->data, pixels, dataSize);
    stbi_image_free(pixels);

    result.code = IMAGE_PLUGIN_OK;
    result.data = block;
    return result;
}

} // namespace

/* One barrier for both entry points; see the note in ImageLoaderWebp.  The
 * existing IMAGE_PLUGIN_ALLOC_ERROR covers bad_alloc — this loader allocates
 * with malloc, so a throw only reaches here from the file buffer or a string. */
export extern "C" IMAGEPLUGIN_API ImagePluginResult LoadImageFromFile(const ImagePluginPath filePath)
{
    try
    {
        Source src;
        src.path = filePath;
        return DecodeStb(src);
    }
    catch (const std::bad_alloc&)
    {
        lastError = "Out of memory";
        return ImagePluginResult{ IMAGE_PLUGIN_ALLOC_ERROR, nullptr };
    }
    catch (const std::runtime_error& e)
    {
        lastError = e.what();
        return ImagePluginResult{ IMAGE_PLUGIN_UNKNOWN_ERROR, nullptr };
    }
    catch (...)
    {
        lastError = "Unexpected failure";
        return ImagePluginResult{ IMAGE_PLUGIN_UNKNOWN_ERROR, nullptr };
    }
}

export extern "C" IMAGEPLUGIN_API ImagePluginResult LoadImageFromMemory(const uint8_t* data, size_t size)
{
    /* A null span is not a path and must not be treated as one: this entry
     * point has no filesystem fallback, so null is rejected like empty input. */
    if (data == nullptr || size == 0)
    {
        lastError = "Empty input";
        return ImagePluginResult{ IMAGE_PLUGIN_FILE_ERROR, nullptr };
    }
    try
    {
        Source src;
        src.span = data;
        src.size = size;
        return DecodeStb(src);
    }
    catch (const std::bad_alloc&)
    {
        lastError = "Out of memory";
        return ImagePluginResult{ IMAGE_PLUGIN_ALLOC_ERROR, nullptr };
    }
    catch (const std::runtime_error& e)
    {
        lastError = e.what();
        return ImagePluginResult{ IMAGE_PLUGIN_UNKNOWN_ERROR, nullptr };
    }
    catch (...)
    {
        lastError = "Unexpected failure";
        return ImagePluginResult{ IMAGE_PLUGIN_UNKNOWN_ERROR, nullptr };
    }
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
