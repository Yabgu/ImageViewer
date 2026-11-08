module;

#include <cstdio>
#include <cstring>
#include <cstdint>
#include <filesystem>
#include <new>
#include <vector>
#include "ImagePluginDef.h"
#include <webp/decode.h>

export module ImageLoaderWebp;

static thread_local char lastError[256] = "";

namespace {

/*
 * Where the compressed bytes come from.
 *
 * Exactly one field is set: `span` for LoadImageFromMemory, `path` for
 * LoadImageFromFile.  This loader reads the whole file into a buffer before
 * handing it to libwebp either way, so the only thing the file path adds is
 * the read.
 */
struct Source
{
    const uint8_t*  span = nullptr;
    size_t          size = 0;
    ImagePluginPath path = nullptr;
};

/* Read a whole file into `out`, setting lastError on failure.  Returns the
 * result code the caller should report: this loader used to fail the file path
 * with IMAGE_PLUGIN_FILE_ERROR, and that is preserved. */
ImagePluginResultCode ReadWholeFile(ImagePluginPath path, std::vector<uint8_t>& out)
{
    FILE* file = nullptr;
#ifdef _WIN32
    _wfopen_s(&file, path, L"rb");
#else
    file = std::fopen(path, "rb");
#endif
    if (file == nullptr)
    {
        snprintf(lastError, sizeof(lastError), "Failed to open file");
        return IMAGE_PLUGIN_FILE_ERROR;
    }

    std::fseek(file, 0, SEEK_END);
    const long fileSize = std::ftell(file);
    std::fseek(file, 0, SEEK_SET);

    if (fileSize <= 0)
    {
        std::fclose(file);
        snprintf(lastError, sizeof(lastError), "Invalid file size");
        return IMAGE_PLUGIN_FILE_ERROR;
    }

    out.resize(static_cast<size_t>(fileSize));
    const size_t got = std::fread(out.data(), 1, out.size(), file);
    std::fclose(file);

    if (got != out.size())
    {
        snprintf(lastError, sizeof(lastError), "Failed to read file");
        return IMAGE_PLUGIN_FILE_ERROR;
    }
    return IMAGE_PLUGIN_OK;
}

ImagePluginData* MakeRgba(const uint8_t* decoded, int width, int height)
{
    constexpr int components = 4;
    IWImageFormat fmt = {};
    fmt.componentCount = components;
    fmt.bitsPerPixel   = components * 8;
    fmt.components[0]  = { IW_COMPONENT_SEMANTIC_R, IW_COMPONENT_CLASS_UNORM,  0, 8 };
    fmt.components[1]  = { IW_COMPONENT_SEMANTIC_G, IW_COMPONENT_CLASS_UNORM,  8, 8 };
    fmt.components[2]  = { IW_COMPONENT_SEMANTIC_B, IW_COMPONENT_CLASS_UNORM, 16, 8 };
    fmt.components[3]  = { IW_COMPONENT_SEMANTIC_A, IW_COMPONENT_CLASS_UNORM, 24, 8 };

    const size_t bytes = static_cast<size_t>(width) * static_cast<size_t>(height) * components;
    ImagePluginData* data = new ImagePluginData{
        .width      = width,
        .height     = height,
        .stride     = width * components,
        .colorSpace = IMAGE_COLOR_SPACE_SRGB,
        .size       = bytes,
        .data       = new uint8_t[bytes],
        .format     = fmt
    };
    std::memcpy(data->data, decoded, bytes);
    return data;
}

ImagePluginData* MakeRgb(const uint8_t* decoded, int width, int height)
{
    constexpr int components = 3;
    IWImageFormat fmt = {};
    fmt.componentCount = components;
    fmt.bitsPerPixel   = components * 8;
    fmt.components[0]  = { IW_COMPONENT_SEMANTIC_R, IW_COMPONENT_CLASS_UNORM,  0, 8 };
    fmt.components[1]  = { IW_COMPONENT_SEMANTIC_G, IW_COMPONENT_CLASS_UNORM,  8, 8 };
    fmt.components[2]  = { IW_COMPONENT_SEMANTIC_B, IW_COMPONENT_CLASS_UNORM, 16, 8 };

    const size_t bytes = static_cast<size_t>(width) * static_cast<size_t>(height) * components;
    ImagePluginData* data = new ImagePluginData{
        .width      = width,
        .height     = height,
        .stride     = width * components,
        .colorSpace = IMAGE_COLOR_SPACE_SRGB,
        .size       = bytes,
        .data       = new uint8_t[bytes],
        .format     = fmt
    };
    std::memcpy(data->data, decoded, bytes);
    return data;
}

ImagePluginResult DecodeWebp(const Source& src)
{
    ImagePluginResult result = { .code = IMAGE_PLUGIN_UNKNOWN_ERROR, .data = nullptr };
    lastError[0] = '\0';

    std::vector<uint8_t> owned;
    const uint8_t* fileData = src.span;
    size_t         fileSize = src.size;

    if (fileData == nullptr)
    {
        const ImagePluginResultCode rc = ReadWholeFile(src.path, owned);
        if (rc != IMAGE_PLUGIN_OK)
        {
            result.code = rc;   /* ReadWholeFile recorded the reason */
            return result;
        }
        fileData = owned.data();
        fileSize = owned.size();
    }
    else if (fileSize == 0)
    {
        snprintf(lastError, sizeof(lastError), "Empty input");
        result.code = IMAGE_PLUGIN_FILE_ERROR;
        return result;
    }

    int width = 0;
    int height = 0;
    WebPBitstreamFeatures features{};
    if (WebPGetFeatures(fileData, fileSize, &features) == VP8_STATUS_OK && features.has_alpha)
    {
        uint8_t* decoded = WebPDecodeRGBA(fileData, fileSize, &width, &height);
        if (!decoded)
        {
            snprintf(lastError, sizeof(lastError), "WebP RGBA decode failed");
            result.code = IMAGE_PLUGIN_DECODE_ERROR;
            return result;
        }
        result.data = MakeRgba(decoded, width, height);
        WebPFree(decoded);
    }
    else
    {
        uint8_t* decoded = WebPDecodeRGB(fileData, fileSize, &width, &height);
        if (!decoded)
        {
            snprintf(lastError, sizeof(lastError), "WebP RGB decode failed");
            result.code = IMAGE_PLUGIN_DECODE_ERROR;
            return result;
        }
        result.data = MakeRgb(decoded, width, height);
        WebPFree(decoded);
    }

    result.code = IMAGE_PLUGIN_OK;
    return result;
}

} // namespace

/*
 * Every export below is wrapped, because an exception escaping an extern "C"
 * function calls std::terminate.  The allocator inside the decode is the only
 * thrower, and a caller cannot tell that from a crash.  bad_alloc maps to the
 * existing IMAGE_PLUGIN_ALLOC_ERROR; nothing else here is expected to throw,
 * so anything else is IMAGE_PLUGIN_UNKNOWN_ERROR.
 */
export extern "C" IMAGEPLUGIN_API ImagePluginResult LoadImageFromFile(const ImagePluginPath filePath)
{
    try
    {
        Source src;
        src.path = filePath;
        return DecodeWebp(src);
    }
    catch (const std::bad_alloc&)
    {
        snprintf(lastError, sizeof(lastError), "Out of memory");
        return ImagePluginResult{ .code = IMAGE_PLUGIN_ALLOC_ERROR, .data = nullptr };
    }
    catch (...)
    {
        snprintf(lastError, sizeof(lastError), "Unexpected failure");
        return ImagePluginResult{ .code = IMAGE_PLUGIN_UNKNOWN_ERROR, .data = nullptr };
    }
}

export extern "C" IMAGEPLUGIN_API ImagePluginResult LoadImageFromMemory(const uint8_t* data, size_t size)
{
    /* A null span is not a path and must not be treated as one: this entry
     * point has no filesystem fallback, so null is rejected like empty input. */
    if (data == nullptr || size == 0)
    {
        snprintf(lastError, sizeof(lastError), "Empty input");
        return ImagePluginResult{ IMAGE_PLUGIN_FILE_ERROR, nullptr };
    }
    try
    {
        Source src;
        src.span = data;
        src.size = size;
        return DecodeWebp(src);
    }
    catch (const std::bad_alloc&)
    {
        snprintf(lastError, sizeof(lastError), "Out of memory");
        return ImagePluginResult{ .code = IMAGE_PLUGIN_ALLOC_ERROR, .data = nullptr };
    }
    catch (...)
    {
        snprintf(lastError, sizeof(lastError), "Unexpected failure");
        return ImagePluginResult{ .code = IMAGE_PLUGIN_UNKNOWN_ERROR, .data = nullptr };
    }
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
