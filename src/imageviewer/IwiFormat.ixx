module;

#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>

#include "ImagePluginDef.h"

export module IwiFormat;

/*
 * IwiFormat — the IWI container header, parsed once for every consumer.
 *
 * Three readers have to agree about these 74 bytes: ImageLoaderIWI (the plugin
 * the viewer loads .iwi through), the Windows shell thumbnail provider, and
 * whatever writes the files (src/format/IWImageIO.ixx).  Keeping the field
 * offsets and the acceptance rules in one module means the viewer and Explorer
 * cannot disagree about which files are valid: both call ParseHeader(), so a
 * file Explorer refuses is a file iview refuses.
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
 * # The accepted subset
 *
 * Interleaved RGB8 and RGBA8: 3 or 4 components, 8 bits each, at offsets
 * i * 8, semantics R/G/B[/A] in order.  That is everything Tension writes
 * (ui.iwi and sprites.iwi are both this shape) and everything the host can
 * upload without a conversion.
 *
 * Everything else — planar storage, sub-8-bit or mixed-width components,
 * components parked off their i * 8 slot, exotic semantics — is refused with a
 * reason rather than reinterpreted.  A wrong guess renders plausible but wrong
 * colours, which is worse than an honest refusal.
 *
 * # What is *not* decided here
 *
 * Colour space.  The header carries one and it is reported as-is; whether sRGB,
 * linear, PQ or HLG can be shown is a display question that belongs to the
 * consumer, not to the container.  ImageLoaderIWI passes any of the four
 * through; the thumbnail provider refuses the two HDR transfer functions
 * because it has no tone mapping to offer.
 */

export namespace iwi {

/* Field offsets, named so readers cannot drift. */
inline constexpr std::size_t kHeaderBytes     = 74;
inline constexpr std::size_t kOffMagic        = 0;
inline constexpr std::size_t kOffWidth        = 4;
inline constexpr std::size_t kOffHeight       = 8;
inline constexpr std::size_t kOffColorSpace   = 12;
inline constexpr std::size_t kOffComponentNum = 13;
inline constexpr std::size_t kOffBitsPerPixel = 15;
inline constexpr std::size_t kOffStorage      = 17;
inline constexpr std::size_t kOffComponents   = 18;
inline constexpr std::size_t kComponentStride = 6;
inline constexpr std::size_t kOffPixelBytes   = 66;

/* Component record fields, relative to its own start. */
inline constexpr std::size_t kCompOffSemantic = 0;
inline constexpr std::size_t kCompOffClass    = 1;
inline constexpr std::size_t kCompOffBitOff   = 2;
inline constexpr std::size_t kCompOffBitWidth = 4;

enum class IwiStatus {
    Ok = 0,
    NullInput,
    TruncatedHeader,
    BadMagic,
    UnsupportedStorage,
    UnsupportedComponentCount,
    UnsupportedBitDepth,
    UnexpectedSemantic,
    UnsupportedComponentClass,
    UnexpectedComponentLayout,
    ZeroDimension,
    UnaddressableGeometry,
    GeometryTooLarge,
    PayloadSmallerThanGeometry,
    PayloadBeyondBuffer,
};

/*
 * The validated header.  `packedBytes` is width * height * components — the
 * bytes an interleaved image actually occupies.  `pixelBytes` is what the file
 * declares, which may be larger (trailing slack) but never smaller.
 */
struct IwiHeader {
    uint32_t width          = 0;
    uint32_t height         = 0;
    uint8_t  colorSpace     = 0;
    uint16_t components     = 0;
    uint16_t bitsPerPixel   = 0;
    uint8_t  storageLayout  = 0;
    uint64_t pixelBytes     = 0;
    uint64_t packedBytes    = 0;

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
    /* Human-readable reason, formatted exactly like ImageLoaderIWI's error
     * text.  The ABI tests assert substrings of it ("magic", "planar",
     * "offset", …), so the wording is part of the contract.  Empty when Ok. */
    char reason[192] = {};

    bool ok() const { return status == IwiStatus::Ok; }
};

/* Byte-wise little-endian reads.  Deliberately not memcpy into a u32: that
 * would inherit host endianness, and this container is little-endian
 * everywhere. */
inline uint16_t ReadU16(const uint8_t* p)
{
    return static_cast<uint16_t>(p[0])
         | static_cast<uint16_t>(static_cast<uint16_t>(p[1]) << 8u);
}

inline uint32_t ReadU32(const uint8_t* p)
{
    return  static_cast<uint32_t>(p[0])
         | (static_cast<uint32_t>(p[1]) << 8u)
         | (static_cast<uint32_t>(p[2]) << 16u)
         | (static_cast<uint32_t>(p[3]) << 24u);
}

inline uint64_t ReadU64(const uint8_t* p)
{
    uint64_t v = 0;
    for (unsigned i = 0; i < 8u; ++i)
        v |= static_cast<uint64_t>(p[i]) << (8u * i);
    return v;
}

/*
 * Validate a container header.
 *
 * `size` is the total number of bytes available — the buffer length for a
 * memory decode, the stream length for a file.  The payload checks need it, so
 * a caller that does not know the size cannot use this to accept a file.
 *
 * The order of the checks is the order ImageLoaderIWI has always used, and the
 * ABI tests mutate one field at a time against it; keep it.
 */
IwiParseResult ParseHeader(const uint8_t* bytes, std::size_t size);

} // export namespace iwi

namespace iwi {

namespace detail {

/*
 * The no-argument overload exists for two reasons: it is the common case, and
 * it lets the format string reach snprintf as an argument to "%s" rather than
 * as the format itself.  Without it, clang's -Wformat-security flags every
 * Fail(status, "literal") call, because the template cannot prove the pointer
 * is a literal and a stray '%' in it would then be interpreted.
 */
inline IwiParseResult Fail(IwiStatus status, const char* reason)
{
    IwiParseResult r;
    r.status = status;
    std::snprintf(r.reason, sizeof(r.reason), "%s", reason);
    return r;
}

template <typename... Args>
inline IwiParseResult Fail(IwiStatus status, const char* format, Args... args)
{
    IwiParseResult r;
    r.status = status;
    std::snprintf(r.reason, sizeof(r.reason), format, args...);
    return r;
}

} // namespace detail

inline IwiParseResult ParseHeader(const uint8_t* bytes, std::size_t size)
{
    if (bytes == nullptr)
        return detail::Fail(IwiStatus::NullInput, "No input");

    /* The magic is checked only after the length, because comparing four bytes
     * of a shorter buffer would read past it.  A file too short to hold the
     * header is broken; a long file with the wrong magic is not an IWI. */
    if (size < kHeaderBytes)
        return detail::Fail(IwiStatus::TruncatedHeader,
                            "IWI is %zu bytes; the header alone is %zu",
                            size, kHeaderBytes);

    if (std::memcmp(bytes + kOffMagic, "IWI1", 4) != 0)
        return detail::Fail(IwiStatus::BadMagic,
                            "not an IWI1 container (magic mismatch)");

    IwiHeader h;
    h.width         = ReadU32(bytes + kOffWidth);
    h.height        = ReadU32(bytes + kOffHeight);
    h.colorSpace    = bytes[kOffColorSpace];
    h.components    = ReadU16(bytes + kOffComponentNum);
    h.bitsPerPixel  = ReadU16(bytes + kOffBitsPerPixel);
    h.storageLayout = bytes[kOffStorage];
    h.pixelBytes    = ReadU64(bytes + kOffPixelBytes);

    if (h.storageLayout != IW_STORAGE_INTERLEAVED)
        return detail::Fail(IwiStatus::UnsupportedStorage,
                            "IWI planar storage is not supported");

    if (h.components != 3u && h.components != 4u)
        return detail::Fail(IwiStatus::UnsupportedComponentCount,
                            "IWI has %u components; only RGB8 (3) and RGBA8 "
                            "(4) are supported",
                            static_cast<unsigned>(h.components));

    if (h.bitsPerPixel != static_cast<uint16_t>(h.components * 8u))
        return detail::Fail(IwiStatus::UnsupportedBitDepth,
                            "IWI declares %u bits per pixel for %u components; "
                            "only 8-bit components are supported",
                            static_cast<unsigned>(h.bitsPerPixel),
                            static_cast<unsigned>(h.components));

    /* Each component must be an 8-bit unsigned sample sitting exactly where
     * the interleaved layout puts it, with the semantics that make it colour.
     * Every one of these is a place a wrong guess would render a plausible but
     * incorrect image, so each is checked and named in the error. */
    static constexpr uint8_t kExpectedSemantics[4] = {
        IW_COMPONENT_SEMANTIC_R,
        IW_COMPONENT_SEMANTIC_G,
        IW_COMPONENT_SEMANTIC_B,
        IW_COMPONENT_SEMANTIC_A,
    };

    for (uint16_t i = 0; i < h.components; ++i)
    {
        const uint8_t* rec = bytes + kOffComponents + i * kComponentStride;
        const uint8_t  semantic = rec[kCompOffSemantic];
        const uint8_t  cls      = rec[kCompOffClass];
        const uint16_t bitOff   = ReadU16(rec + kCompOffBitOff);
        const uint16_t bitWidth = ReadU16(rec + kCompOffBitWidth);

        if (semantic != kExpectedSemantics[i])
            return detail::Fail(IwiStatus::UnexpectedSemantic,
                                "IWI component %u has semantic %u; expected "
                                "R, G, B%s in that order",
                                static_cast<unsigned>(i),
                                static_cast<unsigned>(semantic),
                                h.components == 4u ? ", A" : "");

        if (cls != IW_COMPONENT_CLASS_UINT && cls != IW_COMPONENT_CLASS_UNORM)
            return detail::Fail(IwiStatus::UnsupportedComponentClass,
                                "IWI component %u has class %u; only UINT (0) "
                                "and UNORM (2) are understood",
                                static_cast<unsigned>(i),
                                static_cast<unsigned>(cls));

        if (bitWidth != 8u || bitOff != static_cast<uint16_t>(i * 8u))
            return detail::Fail(IwiStatus::UnexpectedComponentLayout,
                                "IWI component %u is %u bits at offset %u; "
                                "expected 8 bits at offset %u",
                                static_cast<unsigned>(i),
                                static_cast<unsigned>(bitWidth),
                                static_cast<unsigned>(bitOff),
                                static_cast<unsigned>(i * 8u));

        h.component[i].semantic       = semantic;
        h.component[i].componentClass = cls;
        h.component[i].bitOffset      = bitOff;
        h.component[i].bitWidth       = bitWidth;
    }

    if (h.width == 0u || h.height == 0u)
        return detail::Fail(IwiStatus::ZeroDimension,
                            "IWI dimensions are %ux%u", h.width, h.height);

    /* ImagePluginData carries width/height/stride as int, so the geometry has
     * to fit before anything is sized from it.  The multiplication is done in
     * u64 and overflow-checked rather than trusted: a header claiming
     * 4 billion by 4 billion would otherwise wrap into a small allocation and
     * a huge copy. */
    constexpr uint64_t kIntMax = static_cast<uint64_t>(0x7FFFFFFF);
    if (h.width > kIntMax || h.height > kIntMax ||
        static_cast<uint64_t>(h.width) > kIntMax / h.components)
        return detail::Fail(IwiStatus::UnaddressableGeometry,
                            "IWI dimensions %ux%u exceed the addressable range",
                            h.width, h.height);

    const uint64_t pixels = static_cast<uint64_t>(h.width) * h.height;
    constexpr uint64_t kMaxSize =
        static_cast<uint64_t>(static_cast<std::size_t>(-1));
    if (pixels > kMaxSize / h.components)
        return detail::Fail(IwiStatus::GeometryTooLarge,
                            "IWI geometry %ux%u x %u components is too large",
                            h.width, h.height,
                            static_cast<unsigned>(h.components));

    const uint64_t expected = pixels * h.components;
    h.packedBytes = expected;

    if (h.pixelBytes < expected)
        return detail::Fail(IwiStatus::PayloadSmallerThanGeometry,
                            "IWI declares %llu pixel bytes; %ux%u x %u "
                            "components needs %llu",
                            static_cast<unsigned long long>(h.pixelBytes),
                            h.width, h.height,
                            static_cast<unsigned>(h.components),
                            static_cast<unsigned long long>(expected));

    /* payloadEnd is validated without adding, so a declared pixelDataSize near
     * UINT64_MAX cannot wrap the comparison and pass. */
    if (h.pixelBytes > static_cast<uint64_t>(size) - kHeaderBytes)
        return detail::Fail(IwiStatus::PayloadBeyondBuffer,
                            "IWI is %zu bytes; the header plus %llu declared "
                            "pixel bytes needs more",
                            size,
                            static_cast<unsigned long long>(h.pixelBytes));

    IwiParseResult result;
    result.status = IwiStatus::Ok;
    result.header = h;
    return result;
}

} // namespace iwi
