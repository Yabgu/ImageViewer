module;

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cstdint>
#include <cstddef>
#include <new>
#include <vector>

#include "ImagePluginDef.h"
#include "IwiFile.h"

export module ImageLoaderIWI;

import IwiFormat;

/*
 * IWI ("ImageViewer Image") is the container Tension writes its 2D textures,
 * font atlases and sprite sheets in.  It is the same descriptor ImagePluginDef.h
 * already models, serialized verbatim, so this loader has no decoder library to
 * wrap: it parses a fixed header and hands the pixel bytes through.
 *
 * The container is 36 bytes of base header followed by 6 bytes per component,
 * then any metadata, then the payload; the acceptance rules live behind the C
 * API in IwiFile.h (src/format/IwiFile.cpp), shared with the Windows shell
 * thumbnail provider.  That is deliberate: Explorer and iview must not disagree
 * about which files are valid, and a second copy of those rules would be a
 * second thing to keep correct.  The refusals below are that parser's statuses,
 * reported through ImagePluginGetLastError() with the wording the ABI tests
 * assert.
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

/*
 * Report a container status in the wording the ABI tests read.
 *
 * Two sentences, because they call for two different actions: a corrupt file is
 * the user's problem to re-obtain, while a valid file this build cannot display
 * is not, and calling it corrupt sends them looking for a re-download that will
 * not help.  The status says which case it is; iwi_status_message() supplies the
 * reason, which is where the "magic", "planar", "offset" and "header"
 * substrings come from.
 */
ImagePluginResult FailForStatus(int rc)
{
    char message[512];
    if (iwi_status_is_unsupported(rc))
    {
        std::snprintf(message, sizeof(message),
                      "valid IWI file, but this viewer cannot display it: "
                      "%s (try iwconv for conversion)", iwi_status_message(rc));
    }
    else
    {
        std::snprintf(message, sizeof(message), "corrupt file: %s",
                      iwi_status_message(rc));
    }

    const ImagePluginResultCode code =
        (rc == IWI_STATUS_GEOMETRY_TOO_LARGE)
            ? IMAGE_PLUGIN_ALLOC_ERROR
            : IMAGE_PLUGIN_DECODE_ERROR;
    return Fail(code, message);
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

    /*
     * Container grammar, into a blob this loader owns.
     *
     * One allocation holds the 36-byte base header and the component table, so
     * the descriptor can point straight at iwi_file_components(blob): no
     * translation loop, no second table, and no fixed-size array for a
     * many-component file to overrun.  The blob is handed to the caller as
     * ownedBlob and released through FreeImageData().
     *
     * iwi_file_required_size() returns 0 when the base header cannot be read at
     * all - bad magic, shorter than the header, or zero components.
     */
    const std::size_t blobBytes = iwi_file_required_size(bytes, size);
    if (blobBytes == 0)
    {
        /*
         * The parser decides those cases before it looks at the output buffer,
         * so a base-sized scratch buffer gets the real status - and therefore
         * the real reason string - out of it.
         */
        IwiFileHeader probe{};
        return FailForStatus(iwi_parse_structural(&probe, IWI_FILE_BASE_HEADER_BYTES,
                                                  bytes, size));
    }

    IwiFileHeader* const blob =
        static_cast<IwiFileHeader*>(std::malloc(blobBytes));
    if (blob == nullptr)
        return Fail(IMAGE_PLUGIN_ALLOC_ERROR, "Out of memory");

    int rc = iwi_parse_structural(blob, blobBytes, bytes, size);
    if (rc == IWI_STATUS_OK)
    {
        /*
         * Renderability, which is this loader's opinion rather than a container
         * rule.  The grammar accepts far more than this viewer displays -
         * planar layouts, 16-bit samples, any component count - and each of
         * those would render wrong or not at all.  The policy is therefore
         * applied here, and the refusal is reported as "valid, cannot display"
         * rather than as corruption.
         *
         * iw::ViewerPolicyStatus is the same policy the thumbnail provider and
         * iwi::ParseHeader use, so all three accept one set of files.  It is
         * applied to the blob rather than re-parsed, so there is still exactly
         * one parse.
         */
        rc = iwi::ViewerPolicyStatus(blob);
    }
    if (rc != IWI_STATUS_OK)
    {
        iwi_file_free(blob);
        return FailForStatus(rc);
    }

    const std::uint16_t components = blob->componentCount;
    const IwiFileComponent* const comps = iwi_file_components(blob);

    /*
     * Every width is 8 bits after the policy, so a pixel is `components` bytes.
     * The sum is accumulated in 32 bits and narrowed deliberately: a uint16
     * accumulator would wrap on a file with many narrow components, and the
     * 3-or-4 policy above should not be what keeps the arithmetic correct.
     */
    std::uint32_t bitsPerPixel = 0;
    for (std::uint16_t i = 0; i < components; ++i)
        bitsPerPixel += comps[i].bitWidth;
    if (bitsPerPixel > 0xFFFFu) bitsPerPixel = 0xFFFFu;

    IWImageFormat format = {};
    format.componentCount = components;
    format.bitsPerPixel   = static_cast<std::uint16_t>(bitsPerPixel);
    format.storageLayout  = blob->storageLayout;
    /*
     * IwiFileComponent and IWComponentDef are the same six bytes in the same
     * order, which test_abi_constants.cpp pins with static_asserts.  That is
     * what makes this a view of the blob's tail rather than a reinterpretation
     * of unrelated data - and it is why no translation loop exists here.
     */
    format.components = reinterpret_cast<const IWComponentDef*>(comps);

    /* Tightly packed rows: the container defines one row of (width x
     * components) bytes after the next, and the policy has already refused any
     * declared stride.  The payload starts after the component table and any
     * metadata, which is what iwi_parse_structural validated. */
    const std::size_t headerBytes = IWI_FILE_BASE_HEADER_BYTES +
        static_cast<std::size_t>(components) * IWI_FILE_COMPONENT_BYTES +
        blob->metaBytes;
    const std::size_t stride = static_cast<std::size_t>(blob->width) * components;
    const std::size_t total  = stride * blob->height;

    uint8_t* pixelData = nullptr;
    try
    {
        pixelData = new uint8_t[total];
    }
    catch (...)
    {
        iwi_file_free(blob);   /* owned from here on; the caller never sees it */
        throw;
    }
    std::memcpy(pixelData, bytes + headerBytes, total);

    /* The descriptor is a third allocation, so it can fail after the pixels
     * exist; release both rather than leaking on the throw path. */
    ImagePluginData* data = nullptr;
    try
    {
        data = new ImagePluginData{};
        data->width      = static_cast<int>(blob->width);
        data->height     = static_cast<int>(blob->height);
        data->stride     = static_cast<int>(stride);
        data->colorSpace = DecodeColorSpace(blob->colorSpace);
        data->size       = total;
        data->data       = pixelData;
        data->format     = format;
        /* The table is the tail of `blob`, so the descriptor owns the blob and
         * releases it through FreeImageData().  No copy of the table exists. */
        iw_plugin_data_bind(*data, format.components, format.componentCount);
        iw_plugin_data_own(*data, blob, iwi_file_free);
    }
    catch (...)
    {
        delete[] pixelData;
        iwi_file_free(blob);
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
        /* The component table lives inside ownedBlob when the loader produced
         * one; releasing it is the only thing that keeps the descriptor's two
         * views from outliving their storage. */
        if (imageData->freeOwnedBlob)
            imageData->freeOwnedBlob(imageData->ownedBlob);
        delete[] imageData->data;
        delete imageData;
    }
}

export extern "C" IMAGEPLUGIN_API const char* ImagePluginGetLastError()
{
    return lastError;
}
