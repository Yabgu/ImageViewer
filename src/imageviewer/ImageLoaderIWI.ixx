module;

#include <cstdio>
#include <cstring>
#include <cstdint>
#include <cstddef>
#include <new>
#include <vector>

#include "ImagePluginDef.h"

export module ImageLoaderIWI;

/*
 * IWI ("ImageViewer Image") is the container Tension writes its 2D textures,
 * font atlases and sprite sheets in.  It is the same descriptor ImagePluginDef.h
 * already models, serialized verbatim, so this loader has no decoder library to
 * wrap: it parses a fixed header and hands the pixel bytes through.
 *
 * The header is 74 bytes, all integers little-endian:
 *
 *   offset  size  field
 *        0     4  magic "IWI1"
 *        4     4  width           u32
 *        8     4  height          u32
 *       12     1  colorSpace      u8   (ImageColorSpace)
 *       13     2  componentCount  u16
 *       15     2  bitsPerPixel    u16
 *       17     1  storageLayout   u8   (0 interleaved, 1 planar)
 *       18    48  components[8]   6 bytes each:
 *                                    semantic       u8
 *                                    componentClass u8
 *                                    bitOffset      u16
 *                                    bitWidth       u16
 *       66     8  pixelDataSize   u64
 *       74     *  pixelData       u8[]
 *
 * Written by src/format/IWImageIO.ixx; read back by Tension in
 * tension-core/src/render2d/iwi.rs.  This is the third reader of the same
 * layout, and it keeps the field offsets as named constants rather than a
 * packed struct so no compiler is asked to agree about alignment at offset 13.
 *
 * # What this loader accepts
 *
 * Interleaved RGB8 and RGBA8: 3 or 4 components, 8 bits each, at offsets
 * i * 8, semantics R/G/B[/A] in order.  That is everything Tension currently
 * ships (ui.iwi and sprites.iwi are both this shape) and everything the host
 * can upload without a conversion.
 *
 * Everything else in the header — planar storage, sub-8-bit or mixed-width
 * components, components parked off their i * 8 slot, exotic semantics — is
 * refused with a decode error rather than reinterpreted.  A wrong guess here
 * renders wrong colours, which is worse than an honest refusal: the file says
 * exactly how its bits are arranged, and if that arrangement is not one this
 * loader implements, saying so is the correct answer.
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

/* Field offsets, named so the two readers in this file cannot drift. */
constexpr std::size_t kHeaderBytes     = 74;
constexpr std::size_t kOffMagic        = 0;
constexpr std::size_t kOffWidth        = 4;
constexpr std::size_t kOffHeight       = 8;
constexpr std::size_t kOffColorSpace   = 12;
constexpr std::size_t kOffComponentNum = 13;
constexpr std::size_t kOffBitsPerPixel = 15;
constexpr std::size_t kOffStorage      = 17;
constexpr std::size_t kOffComponents   = 18;
constexpr std::size_t kComponentStride = 6;
constexpr std::size_t kOffPixelBytes   = 66;

/* Component record fields, relative to its own start. */
constexpr std::size_t kCompOffSemantic = 0;
constexpr std::size_t kCompOffClass    = 1;
constexpr std::size_t kCompOffBitOff   = 2;
constexpr std::size_t kCompOffBitWidth = 4;

/* The only component classes an 8-bit colour sample can mean.  UINT and UNORM
 * both describe an unsigned byte; SINT/SNORM/FLOAT would change what the
 * sample value means, so they are refused instead of silently treated as
 * colour. */
constexpr uint8_t kClassUint  = IW_COMPONENT_CLASS_UINT;
constexpr uint8_t kClassUnorm = IW_COMPONENT_CLASS_UNORM;

/* Byte-wise little-endian reads.  Deliberately not memcpy into a u32: that
 * would inherit host endianness, and this container is specified little-endian
 * everywhere. */
uint16_t ReadU16(const uint8_t* p)
{
    return static_cast<uint16_t>(p[0])
         | static_cast<uint16_t>(static_cast<uint16_t>(p[1]) << 8u);
}

uint32_t ReadU32(const uint8_t* p)
{
    return  static_cast<uint32_t>(p[0])
         | (static_cast<uint32_t>(p[1]) << 8u)
         | (static_cast<uint32_t>(p[2]) << 16u)
         | (static_cast<uint32_t>(p[3]) << 24u);
}

uint64_t ReadU64(const uint8_t* p)
{
    uint64_t v = 0;
    for (unsigned i = 0; i < 8u; ++i)
        v |= static_cast<uint64_t>(p[i]) << (8u * i);
    return v;
}

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

    /* The magic is checked only after the length, because comparing four bytes
     * of a shorter buffer would read past it.  A file too short to hold the
     * header is broken; a long file with the wrong magic is not an IWI. */
    if (size < kHeaderBytes)
    {
        char reason[128];
        std::snprintf(reason, sizeof(reason),
                      "IWI is %zu bytes; the header alone is %zu",
                      size, kHeaderBytes);
        return Fail(IMAGE_PLUGIN_DECODE_ERROR, reason);
    }
    if (std::memcmp(bytes + kOffMagic, "IWI1", 4) != 0)
        return Fail(IMAGE_PLUGIN_DECODE_ERROR,
                    "not an IWI1 container (magic mismatch)");

    const uint32_t width      = ReadU32(bytes + kOffWidth);
    const uint32_t height     = ReadU32(bytes + kOffHeight);
    const uint8_t  colorSpace = bytes[kOffColorSpace];
    const uint16_t components = ReadU16(bytes + kOffComponentNum);
    const uint16_t bitsPerPx  = ReadU16(bytes + kOffBitsPerPixel);
    const uint8_t  storage    = bytes[kOffStorage];
    const uint64_t pixelBytes = ReadU64(bytes + kOffPixelBytes);

    if (storage != IW_STORAGE_INTERLEAVED)
        return Fail(IMAGE_PLUGIN_DECODE_ERROR,
                    "IWI planar storage is not supported");

    if (components != 3u && components != 4u)
    {
        char reason[160];
        std::snprintf(reason, sizeof(reason),
                      "IWI has %u components; only RGB8 (3) and RGBA8 (4) "
                      "are supported",
                      static_cast<unsigned>(components));
        return Fail(IMAGE_PLUGIN_DECODE_ERROR, reason);
    }

    if (bitsPerPx != static_cast<uint16_t>(components * 8u))
    {
        char reason[160];
        std::snprintf(reason, sizeof(reason),
                      "IWI declares %u bits per pixel for %u components; only "
                      "8-bit components are supported",
                      static_cast<unsigned>(bitsPerPx),
                      static_cast<unsigned>(components));
        return Fail(IMAGE_PLUGIN_DECODE_ERROR, reason);
    }

    /* Each component must be an 8-bit unsigned sample sitting exactly where
     * the interleaved layout puts it, with the semantics that make it colour.
     * Every one of these is a place a wrong guess would render a plausible but
     * incorrect image, so each is checked and named in the error. */
    IWImageFormat format = {};
    format.componentCount = components;
    format.bitsPerPixel   = bitsPerPx;
    format.storageLayout  = IW_STORAGE_INTERLEAVED;

    static constexpr uint8_t kExpectedSemantics[4] = {
        IW_COMPONENT_SEMANTIC_R,
        IW_COMPONENT_SEMANTIC_G,
        IW_COMPONENT_SEMANTIC_B,
        IW_COMPONENT_SEMANTIC_A,
    };

    for (uint16_t i = 0; i < components; ++i)
    {
        const uint8_t* rec = bytes + kOffComponents + i * kComponentStride;
        const uint8_t  semantic = rec[kCompOffSemantic];
        const uint8_t  cls      = rec[kCompOffClass];
        const uint16_t bitOff   = ReadU16(rec + kCompOffBitOff);
        const uint16_t bitWidth = ReadU16(rec + kCompOffBitWidth);

        if (semantic != kExpectedSemantics[i])
        {
            char reason[160];
            std::snprintf(reason, sizeof(reason),
                          "IWI component %u has semantic %u; expected R, G, B"
                          "%s in that order",
                          static_cast<unsigned>(i),
                          static_cast<unsigned>(semantic),
                          components == 4u ? ", A" : "");
            return Fail(IMAGE_PLUGIN_DECODE_ERROR, reason);
        }
        if (cls != kClassUint && cls != kClassUnorm)
        {
            char reason[160];
            std::snprintf(reason, sizeof(reason),
                          "IWI component %u has class %u; only UINT (0) and "
                          "UNORM (2) are understood",
                          static_cast<unsigned>(i), static_cast<unsigned>(cls));
            return Fail(IMAGE_PLUGIN_DECODE_ERROR, reason);
        }
        if (bitWidth != 8u || bitOff != static_cast<uint16_t>(i * 8u))
        {
            char reason[180];
            std::snprintf(reason, sizeof(reason),
                          "IWI component %u is %u bits at offset %u; expected "
                          "8 bits at offset %u",
                          static_cast<unsigned>(i),
                          static_cast<unsigned>(bitWidth),
                          static_cast<unsigned>(bitOff),
                          static_cast<unsigned>(i * 8u));
            return Fail(IMAGE_PLUGIN_DECODE_ERROR, reason);
        }

        /* Stored as read: the file is the authority on what its bytes mean,
         * and the checks above have already narrowed that to unsigned 8-bit
         * colour in interleaved order. */
        format.components[i] = IWComponentDef{
            .semantic       = static_cast<IWComponentSemantic>(semantic),
            .componentClass = static_cast<IWComponentClass>(cls),
            .bitOffset      = bitOff,
            .bitWidth       = bitWidth,
        };
    }

    if (width == 0u || height == 0u)
    {
        char reason[128];
        std::snprintf(reason, sizeof(reason),
                      "IWI dimensions are %ux%u", width, height);
        return Fail(IMAGE_PLUGIN_DECODE_ERROR, reason);
    }

    /* ImagePluginData carries width/height/stride as int, so the geometry has
     * to fit before anything is sized from it.  The multiplication is done in
     * u64 and overflow-checked rather than trusted: a header claiming
     * 4 billion by 4 billion would otherwise wrap into a small allocation and
     * a huge copy. */
    constexpr uint64_t kIntMax = static_cast<uint64_t>(0x7FFFFFFF);
    if (width > kIntMax || height > kIntMax ||
        static_cast<uint64_t>(width) > kIntMax / components)
    {
        char reason[160];
        std::snprintf(reason, sizeof(reason),
                      "IWI dimensions %ux%u exceed the addressable range",
                      width, height);
        return Fail(IMAGE_PLUGIN_DECODE_ERROR, reason);
    }

    const uint64_t pixels      = static_cast<uint64_t>(width) * height;
    constexpr uint64_t kMaxSize = static_cast<uint64_t>(static_cast<std::size_t>(-1));
    if (pixels > kMaxSize / components)
    {
        char reason[160];
        std::snprintf(reason, sizeof(reason),
                      "IWI geometry %ux%u x %u components is too large",
                      width, height, static_cast<unsigned>(components));
        return Fail(IMAGE_PLUGIN_ALLOC_ERROR, reason);
    }
    const uint64_t expected = pixels * components;

    if (pixelBytes < expected)
    {
        char reason[192];
        std::snprintf(reason, sizeof(reason),
                      "IWI declares %llu pixel bytes; %ux%u x %u components "
                      "needs %llu",
                      static_cast<unsigned long long>(pixelBytes),
                      width, height, static_cast<unsigned>(components),
                      static_cast<unsigned long long>(expected));
        return Fail(IMAGE_PLUGIN_DECODE_ERROR, reason);
    }

    /* payloadEnd is validated without adding, so a declared pixelDataSize near
     * UINT64_MAX cannot wrap the comparison and pass. */
    if (pixelBytes > static_cast<uint64_t>(size) - kHeaderBytes)
    {
        char reason[192];
        std::snprintf(reason, sizeof(reason),
                      "IWI is %zu bytes; the header plus %llu declared pixel "
                      "bytes needs more",
                      size, static_cast<unsigned long long>(pixelBytes));
        return Fail(IMAGE_PLUGIN_DECODE_ERROR, reason);
    }

    /* Tightly packed rows: the container has no stride field, so the only
     * layout it defines is one row of (width x components) bytes after the
     * next.  Any bytes declared beyond that are trailing slack and are not
     * copied. */
    const std::size_t stride = static_cast<std::size_t>(width) * components;
    const std::size_t total  = static_cast<std::size_t>(expected);

    uint8_t* pixelData = new uint8_t[total];
    std::memcpy(pixelData, bytes + kHeaderBytes, total);

    /* The descriptor is a second allocation, so it can fail after the pixels
     * exist; release them rather than leaking on the throw path. */
    ImagePluginData* data = nullptr;
    try
    {
        data = new ImagePluginData{
            .width      = static_cast<int>(width),
            .height     = static_cast<int>(height),
            .stride     = static_cast<int>(stride),
            .colorSpace = DecodeColorSpace(colorSpace),
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
