module;

#include <cstdarg>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>

#include "ImagePluginDef.h"
#include "IwiFile.h"

export module IwiFormat;

/*
 * IwiFormat — the C++ module view of the .iwi container header.
 *
 * Implements header validation and parsing for .iwi consumers (ImageLoaderIWI
 * and the Windows shell thumbnail provider).
 *
 *   offset  size  field
 *        0     4  magic "IWI1"
 *        4     4  width           u32
 *        8     4  height          u32
 *       12     4  metaBytes       u32  (optional metadata blob length)
 *       16     8  storageLayout   IWStorageLayout (flags u32, stride u32)
 *       24     8  payloadEncoding char[8] (e.g. "raw\0\0\0\0\0")
 *       32     1  colorSpace      u8   (ImageColorSpace)
 *       33     1  reserved        u8   (zeroed padding)
 *       34     2  componentCount  u16  (number of components, min 1)
 *       36   N*6  components[N]   6 bytes each (N = componentCount):
 *                                    semantic       u8
 *                                    componentClass u8
 *                                    bitOffset      u16
 *                                    bitWidth       u16
 *   36+N*6     *  metaBlob        u8[metaBytes]
 * 36+N*6+M     *  pixelData       u8[]
 *
 * Every multi-byte field above is little-endian.
 */

export namespace iwi {

inline constexpr char kMagic[] = IWI_FILE_MAGIC;
inline constexpr std::size_t kBaseHeaderBytes = IWI_FILE_BASE_HEADER_BYTES;
inline constexpr std::size_t kHeaderBytes = sizeof(IwiFileHeader);

export using ::IwiFileComponent;
export using ::IwiFileHeader;

/*
 * Status codes for header parsing.
 */
enum class IwiStatus {
    Ok = 0,
    NullInput,
    TruncatedHeader,
    BadMagic,
    ZeroDimension,
    UnaddressableGeometry,
    GeometryTooLarge,
    PayloadBeyondBuffer,
    UnsupportedEncoding,
    UnsupportedStorage,
    UnsupportedComponentCount,
    UnsupportedBitDepth,
    UnexpectedSemantic,
    UnsupportedComponentClass,
    UnexpectedComponentLayout,
};

/*
 * The validated header. `packedBytes` is what the geometry needs.
 */
struct IwiHeader {
    uint32_t        width          = 0;
    uint32_t        height         = 0;
    uint8_t         colorSpace     = 0;
    uint16_t        components     = 0;
    uint16_t        bitsPerPixel   = 0;
    IWStorageLayout storageLayout{};
    uint32_t        metaBytes      = 0;
    uint32_t        metaOffset     = 0;
    uint32_t        headerBytes    = 0; // offset to pixelData: 36 + components * 6 + metaBytes
    uint64_t        packedBytes    = 0;
    char            payloadEncoding[8] = {};

    struct Component {
        uint8_t  semantic       = 0;
        uint8_t  componentClass = 0;
        uint16_t bitOffset      = 0;
        uint16_t bitWidth       = 0;
    };
    Component component[4];
};

struct IwiParseResult {
    IwiStatus status = IwiStatus::Ok;
    IwiHeader header{};
    char reason[192] = {};

    bool ok() const { return status == IwiStatus::Ok; }
};

} // export namespace iwi

namespace iwi {

namespace detail {

inline uint16_t ReadU16(const uint8_t* p) noexcept
{
    return static_cast<uint16_t>(p[0]) |
           static_cast<uint16_t>(static_cast<uint16_t>(p[1]) << 8u);
}

inline uint32_t ReadU32(const uint8_t* p) noexcept
{
    return static_cast<uint32_t>(p[0]) |
           (static_cast<uint32_t>(p[1]) << 8u) |
           (static_cast<uint32_t>(p[2]) << 16u) |
           (static_cast<uint32_t>(p[3]) << 24u);
}

inline uint64_t ReadU64(const uint8_t* p) noexcept
{
    uint64_t v = 0;
    for (unsigned i = 0; i < 8u; ++i) {
        v |= static_cast<uint64_t>(p[i]) << (8u * i);
    }
    return v;
}

inline IwiParseResult Fail(IwiStatus status, const char* format, ...)
{
    IwiParseResult r;
    r.status = status;
    va_list args;
    va_start(args, format);
    (void)std::vsnprintf(r.reason, sizeof(r.reason), format, args);
    va_end(args);
    return r;
}

} // namespace detail

/*
 * Validate and parse a container header.
 *
 * `size` is the whole container — the payload bounds check needs it, so a
 * caller holding only the first few bytes cannot use this to accept a file.
 */
export inline IwiParseResult ParseHeader(const uint8_t* bytes, std::size_t size)
{
    if (bytes == nullptr) {
        return detail::Fail(IwiStatus::NullInput, "No input");
    }

    if (size < sizeof(IwiFileHeader)) {
        return detail::Fail(IwiStatus::TruncatedHeader,
                            "IWI is %zu bytes; the minimum header alone is %zu",
                            size, sizeof(IwiFileHeader));
    }

    const auto* fileHdr = reinterpret_cast<const IwiFileHeader*>(bytes);

    if (std::memcmp(fileHdr->magic, IWI_FILE_MAGIC, IWI_FILE_MAGIC_BYTES) != 0) {
        return detail::Fail(IwiStatus::BadMagic,
                            "not an IWI1 container (magic mismatch)");
    }

    const uint32_t width       = detail::ReadU32(reinterpret_cast<const uint8_t*>(&fileHdr->width));
    const uint32_t height      = detail::ReadU32(reinterpret_cast<const uint8_t*>(&fileHdr->height));
    const uint32_t metaBytes   = detail::ReadU32(reinterpret_cast<const uint8_t*>(&fileHdr->metaBytes));
    const uint8_t  colorSpace  = static_cast<uint8_t>(fileHdr->colorSpace);
    const uint16_t components  = detail::ReadU16(reinterpret_cast<const uint8_t*>(&fileHdr->componentCount));

    if (width == 0u || height == 0u) {
        return detail::Fail(IwiStatus::ZeroDimension,
                            "IWI dimensions are %ux%u", width, height);
    }

    if (components < IWI_FILE_MIN_COMPONENTS) {
        return detail::Fail(IwiStatus::UnsupportedComponentCount,
                            "IWI declares %u components; minimum is 1",
                            static_cast<unsigned>(components));
    }

    const uint64_t baseHeader = IWI_FILE_BASE_HEADER_BYTES +
                                static_cast<uint64_t>(components) * sizeof(IwiFileComponent);
    const uint64_t totalHeaderAndMeta = baseHeader + static_cast<uint64_t>(metaBytes);

    if (static_cast<uint64_t>(size) < totalHeaderAndMeta) {
        return detail::Fail(IwiStatus::TruncatedHeader,
                            "IWI is %zu bytes; header and metadata require %llu bytes",
                            size, static_cast<unsigned long long>(totalHeaderAndMeta));
    }

    // Encoding check: only uncompressed / "raw" (or zeroed) is currently decoded
    if (fileHdr->payloadEncoding[0] != '\0' &&
        std::memcmp(fileHdr->payloadEncoding, IWI_PAYLOAD_RAW, 3) != 0) {
        return detail::Fail(IwiStatus::UnsupportedEncoding,
                            "IWI payload encoding '%.8s' is not supported",
                            fileHdr->payloadEncoding);
    }

    if (fileHdr->storageLayout.isPlanar()) {
        return detail::Fail(IwiStatus::UnsupportedStorage,
                            "IWI planar storage is not supported");
    }

    if (components != 3u && components != 4u) {
        return detail::Fail(IwiStatus::UnsupportedComponentCount,
                            "IWI has %u components; only RGB8 (3) and RGBA8 (4) are supported",
                            static_cast<unsigned>(components));
    }

    if (width > 0x7FFFFFFFu || height > 0x7FFFFFFFu
        || static_cast<uint64_t>(width) > static_cast<uint64_t>(0x7FFFFFFF) / static_cast<uint64_t>(components)) {
        return detail::Fail(IwiStatus::UnaddressableGeometry,
                            "IWI dimensions %ux%u exceed the addressable range",
                            width, height);
    }

    const uint64_t pixels  = static_cast<uint64_t>(width) * static_cast<uint64_t>(height);
    const uint64_t maxSize = static_cast<uint64_t>(static_cast<size_t>(-1));
    if (pixels > maxSize / static_cast<uint64_t>(components)) {
        return detail::Fail(IwiStatus::GeometryTooLarge,
                            "IWI geometry %ux%u x %u components is too large",
                            width, height, static_cast<unsigned>(components));
    }

    const uint64_t expected = pixels * static_cast<uint64_t>(components);

    if (static_cast<uint64_t>(size) - totalHeaderAndMeta < expected) {
        return detail::Fail(IwiStatus::PayloadBeyondBuffer,
                            "IWI is %zu bytes; %ux%u x %u components needs %llu payload bytes",
                            size, width, height, static_cast<unsigned>(components),
                            static_cast<unsigned long long>(expected));
    }

    static const uint8_t expectedSemantics[4] = {
        static_cast<uint8_t>(IW_COMPONENT_SEMANTIC_R),
        static_cast<uint8_t>(IW_COMPONENT_SEMANTIC_G),
        static_cast<uint8_t>(IW_COMPONENT_SEMANTIC_B),
        static_cast<uint8_t>(IW_COMPONENT_SEMANTIC_A)
    };

    IwiParseResult result;
    result.status = IwiStatus::Ok;
    result.header.width         = width;
    result.header.height        = height;
    result.header.colorSpace    = static_cast<uint8_t>(colorSpace);
    result.header.components    = static_cast<uint16_t>(components);
    result.header.storageLayout = fileHdr->storageLayout;
    result.header.metaBytes     = metaBytes;
    result.header.metaOffset    = static_cast<uint32_t>(baseHeader);
    result.header.headerBytes   = static_cast<uint32_t>(totalHeaderAndMeta);
    result.header.packedBytes   = expected;
    std::memcpy(result.header.payloadEncoding, fileHdr->payloadEncoding, 8);

    uint16_t bitsPerPx = 0;
    for (uint32_t i = 0; i < components; ++i) {
        const auto&    rec      = fileHdr->components[i];
        const uint8_t  semantic = static_cast<uint8_t>(rec.semantic);
        const uint8_t  cls      = static_cast<uint8_t>(rec.componentClass);
        const uint16_t bitOff   = detail::ReadU16(reinterpret_cast<const uint8_t*>(&rec.bitOffset));
        const uint16_t bitWidth = detail::ReadU16(reinterpret_cast<const uint8_t*>(&rec.bitWidth));

        if (semantic != expectedSemantics[i]) {
            return detail::Fail(IwiStatus::UnexpectedSemantic,
                                "IWI component %u has semantic %u; expected R, G, B%s in that order",
                                static_cast<unsigned>(i), static_cast<unsigned>(semantic),
                                components == 4u ? ", A" : "");
        }

        if (cls != static_cast<uint8_t>(IW_COMPONENT_CLASS_UINT)
            && cls != static_cast<uint8_t>(IW_COMPONENT_CLASS_UNORM)) {
            return detail::Fail(IwiStatus::UnsupportedComponentClass,
                                "IWI component %u has class %u; only UINT (0) and UNORM (2) are understood",
                                static_cast<unsigned>(i), static_cast<unsigned>(cls));
        }

        if (bitWidth != 8u) {
            return detail::Fail(IwiStatus::UnsupportedBitDepth,
                                "IWI component %u is %u bits; only 8-bit components are supported",
                                static_cast<unsigned>(i), static_cast<unsigned>(bitWidth));
        }

        if (bitOff != static_cast<uint16_t>(i * 8u)) {
            return detail::Fail(IwiStatus::UnexpectedComponentLayout,
                                "IWI component %u is at offset %u; expected offset %u",
                                static_cast<unsigned>(i), static_cast<unsigned>(bitOff),
                                static_cast<unsigned>(i * 8u));
        }

        bitsPerPx += bitWidth;
        result.header.component[i].semantic       = semantic;
        result.header.component[i].componentClass = cls;
        result.header.component[i].bitOffset      = bitOff;
        result.header.component[i].bitWidth       = bitWidth;
    }

    result.header.bitsPerPixel = bitsPerPx;

    return result;
}

} // namespace iwi
