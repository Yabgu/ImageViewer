module;

#include <cstdio>
#include <cstring>
#include <cstdint>
#include <cstddef>
#include <new>
#include <vector>

#include "ImagePluginDef.h"

export module ImageLoaderIWI;

import IwiFormat;

/*
 * IWI ("ImageViewer Image") is the container Tension writes its 2D textures,
 * font atlases and sprite sheets in.  It is the same descriptor ImagePluginDef.h
 * already models, serialized verbatim, so this loader has no decoder library to
 * wrap: it parses a fixed header and hands the pixel bytes through.
 *
 * The 74-byte header layout and the acceptance rules live in IwiFormat.hpp,
 * shared with the Windows shell thumbnail provider.  That is deliberate:
 * Explorer and iview must not disagree about which files are valid, and a
 * second copy of those rules would be a second thing to keep correct.  The
 * refusals below are ParseHeader's, reported through
 * ImagePluginGetLastError() with the wording the ABI tests assert.
 *
 * # What this loader does not export
 *
 * There is no LoadImagePreviewFromFile.  A preview implies a cheaper decode,
 * and this container has no cheaper decode: producing a half-size raster would
 * mean reading the same pixels and scaling them, which costs more than the
 * full load and allocates a second buffer to buy nothing.  The host already
 * treats an absent preview export as "no preview", not as a failure.
 */

static thread_local char lastError[256] = "";

namespace {

/*
 * Where the container bytes come from.  Exactly one field is set: `span` for
 * LoadImageFromMemory, `path` for LoadImageFromFile.  Both paths end up with a
 * contiguous buffer, because the header and the pixels are read together and a
 * short read has to be detected before anything is sized from the declared
 * dimensions.
 */
struct Source
{
    const uint8_t*  span = nullptr;
    std::size_t     size = 0;
    ImagePluginPath path = nullptr;
};

/* Read a whole file into `out`, recording lastError on failure. */
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
        std::snprintf(lastError, sizeof(lastError), "Failed to open file");
        return IMAGE_PLUGIN_FILE_ERROR;
    }

    std::fseek(file, 0, SEEK_END);
    const long fileSize = std::ftell(file);
    std::fseek(file, 0, SEEK_SET);

    if (fileSize <= 0)
    {
        std::fclose(file);
        std::snprintf(lastError, sizeof(lastError), "Invalid file size");
        return IMAGE_PLUGIN_FILE_ERROR;
    }

    out.resize(static_cast<std::size_t>(fileSize));
    const std::size_t got = std::fread(out.data(), 1, out.size(), file);
    std::fclose(file);

    if (got != out.size())
    {
        std::snprintf(lastError, sizeof(lastError), "Failed to read file");
        return IMAGE_PLUGIN_FILE_ERROR;
    }
    return IMAGE_PLUGIN_OK;
}

/* The transfer function is a bare byte in the file; only the values the ABI
 * defines are meaningful, and an unrecognised one becomes UNKNOWN rather than
 * an out-of-range enum that later comparisons would misbehave on. */
ImageColorSpace DecodeColorSpace(uint8_t raw)
{
    switch (raw)
    {
    case IMAGE_COLOR_SPACE_SRGB:    return IMAGE_COLOR_SPACE_SRGB;
    case IMAGE_COLOR_SPACE_LINEAR:  return IMAGE_COLOR_SPACE_LINEAR;
    case IMAGE_COLOR_SPACE_HDR_PQ:  return IMAGE_COLOR_SPACE_HDR_PQ;
    case IMAGE_COLOR_SPACE_HDR_HLG: return IMAGE_COLOR_SPACE_HDR_HLG;
    default:                        return IMAGE_COLOR_SPACE_UNKNOWN;
    }
}

/* Fail with a formatted reason.  Keeps every rejection in DecodeIwi one line
 * and makes the reported text part of the contract the ABI tests read. */
ImagePluginResult Fail(ImagePluginResultCode code, const char* reason)
{
    std::snprintf(lastError, sizeof(lastError), "%s", reason);
    return ImagePluginResult{ .code = code, .data = nullptr };
}

ImagePluginResult DecodeIwi(const Source& src)
{
    lastError[0] = '\0';

    std::vector<uint8_t> owned;
    const uint8_t* bytes = src.span;
    std::size_t    size  = src.size;

    if (bytes == nullptr)
    {
        const ImagePluginResultCode rc = ReadWholeFile(src.path, owned);
        if (rc != IMAGE_PLUGIN_OK)
            return ImagePluginResult{ .code = rc, .data = nullptr };
        bytes = owned.data();
        size  = owned.size();
    }
    else if (size == 0)
    {
        return Fail(IMAGE_PLUGIN_FILE_ERROR, "Empty input");
    }

    /* Every header decision — magic, storage layout, component shape, the
     * addressable range, and the declared payload against the bytes actually
     * present — is ParseHeader's, so this loader and the shell thumbnail
     * provider accept exactly the same files. */
    const iwi::IwiParseResult parsed = iwi::ParseHeader(bytes, size);
    if (!parsed.ok())
    {
        const ImagePluginResultCode code =
            parsed.status == iwi::IwiStatus::GeometryTooLarge
                ? IMAGE_PLUGIN_ALLOC_ERROR
                : IMAGE_PLUGIN_DECODE_ERROR;
        return Fail(code, parsed.reason);
    }

    const iwi::IwiHeader& header = parsed.header;

    IWImageFormat format = {};
    format.componentCount = header.components;
    format.bitsPerPixel   = header.bitsPerPixel;
    format.storageLayout  = IW_STORAGE_INTERLEAVED;

    for (uint16_t i = 0; i < header.components; ++i)
    {
        /* Stored as read: the file is the authority on what its bytes mean,
         * and ParseHeader has already narrowed that to unsigned 8-bit colour
         * in interleaved order. */
        format.components[i] = IWComponentDef{
            .semantic       = static_cast<IWComponentSemantic>(
                                  header.component[i].semantic),
            .componentClass = static_cast<IWComponentClass>(
                                  header.component[i].componentClass),
            .bitOffset      = header.component[i].bitOffset,
            .bitWidth       = header.component[i].bitWidth,
        };
    }

    /* Tightly packed rows: the container has no stride field, so the only
     * layout it defines is one row of (width x components) bytes after the
     * next.  Any bytes declared beyond that are trailing slack and are not
     * copied. */
    const std::size_t stride = static_cast<std::size_t>(header.width) *
                               header.components;
    const std::size_t total  = static_cast<std::size_t>(header.packedBytes);

    uint8_t* pixelData = new uint8_t[total];
    std::memcpy(pixelData, bytes + iwi::toSize(iwi::Extent::Header), total);

    /* The descriptor is a second allocation, so it can fail after the pixels
     * exist; release them rather than leaking on the throw path. */
    ImagePluginData* data = nullptr;
    try
    {
        data = new ImagePluginData{
            .width      = static_cast<int>(header.width),
            .height     = static_cast<int>(header.height),
            .stride     = static_cast<int>(stride),
            .colorSpace = DecodeColorSpace(header.colorSpace),
            .size       = total,
            .data       = pixelData,
            .format     = format,
        };
    }
    catch (...)
    {
        delete[] pixelData;
        throw;
    }

    return ImagePluginResult{ .code = IMAGE_PLUGIN_OK, .data = data };
}

} // namespace

/*
 * Every export below is wrapped, because an exception escaping an extern "C"
 * function calls std::terminate.  The allocator inside the decode is the only
 * thrower (the vector that holds a file, and the pixel buffer), and
 * bad_alloc maps to IMAGE_PLUGIN_ALLOC_ERROR so a caller can tell "too big"
 * from "malformed".
 */
export extern "C" IMAGEPLUGIN_API ImagePluginResult LoadImageFromFile(const ImagePluginPath filePath)
{
    try
    {
        Source src;
        src.path = filePath;
        return DecodeIwi(src);
    }
    catch (const std::bad_alloc&)
    {
        std::snprintf(lastError, sizeof(lastError), "Out of memory");
        return ImagePluginResult{ .code = IMAGE_PLUGIN_ALLOC_ERROR, .data = nullptr };
    }
    catch (...)
    {
        std::snprintf(lastError, sizeof(lastError), "Unexpected failure");
        return ImagePluginResult{ .code = IMAGE_PLUGIN_UNKNOWN_ERROR, .data = nullptr };
    }
}

export extern "C" IMAGEPLUGIN_API ImagePluginResult LoadImageFromMemory(const uint8_t* data, size_t size)
{
    /* A null span is not a path and must not be treated as one: this entry
     * point has no filesystem fallback, so null is rejected like empty input. */
    if (data == nullptr || size == 0)
    {
        std::snprintf(lastError, sizeof(lastError), "Empty input");
        return ImagePluginResult{ .code = IMAGE_PLUGIN_FILE_ERROR, .data = nullptr };
    }
    try
    {
        Source src;
        src.span = data;
        src.size = size;
        return DecodeIwi(src);
    }
    catch (const std::bad_alloc&)
    {
        std::snprintf(lastError, sizeof(lastError), "Out of memory");
        return ImagePluginResult{ .code = IMAGE_PLUGIN_ALLOC_ERROR, .data = nullptr };
    }
    catch (...)
    {
        std::snprintf(lastError, sizeof(lastError), "Unexpected failure");
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
