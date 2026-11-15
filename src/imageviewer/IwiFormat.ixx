module;

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <vector>

#include "ImagePluginDef.h"
#include "IwiFile.h"

export module IwiFormat;

/*
 * IwiFormat — the C++ module view of the .iwi container header.
 *
 * # There is no parser here
 *
 * The rules live in IwiFile.cpp, behind the C API in IwiFile.h. This module is
 * a thin skin over that API: it allocates the blob the C API fills, maps its
 * status codes into an enum class so module code can switch on them, and hands
 * back a struct with the fields the loader and the thumbnail provider read.
 *
 * That direction is deliberate and is the only one allowed: the C ABI is the
 * contract (Rust, FFI and a C engine can reach it; a module cannot), so the
 * module depends on the C API and never the reverse. If this file ever grows a
 * bit test or a bounds check that IwiFile.cpp does not have, the two have
 * drifted and the validation belongs back in the C implementation.
 *
 * # The header on disk
 *
 *   offset  size  field
 *        0     4  magic "IWI1"
 *        4     4  width           u32
 *        8     4  height          u32
 *       12     4  metaBytes       u32
 *       16     8  storageLayout   u64 (flags u32, stride u32)
 *       24     8  payloadEncoding char[8]
 *       32     1  colorSpace      u8
 *       33     1  reserved        u8
 *       34     2  componentCount  u16
 *       36   6*N  components[N]   6 bytes each
 *     36+6N   M  metadata        M = metaBytes
 *   36+6N+M   *  pixelData       tightly packed, interleaved
 *
 * Every multi-byte field is little-endian. Big-endian hosts are out of scope,
 * as documented in IwiFile.h.
 *
 * # What is *not* decided here
 *
 * Colour space. The header carries one and it is reported as-is; whether sRGB,
 * linear, PQ or HLG can be shown is a display question that belongs to the
 * consumer, not to the container. ImageLoaderIWI passes all four through; the
 * thumbnail provider refuses the two HDR transfer functions because it has no
 * tone mapping to offer.
 */

export namespace iwi {

/*
 * Status, carrying the same meaning as the C IwiStatus but as a scoped enum so
 * module code can switch on it without dragging in the C spellings.
 *
 * `Ok` is 0 and every failure is distinct. MapStatus in the detail namespace
 * below is the single conversion point from IwiFile.h's IwiStatus, and it is
 * total over that enum.
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
    OutOfSpace,
    Unsupported,
    NoComponents,
};

/*
 * The validated header.
 *
 * `packedBytes` is what the geometry needs; `headerBytes` is the offset at
 * which the pixel data starts, i.e. 36 + 6N + metaBytes.
 *
 * There is no component array here. The component table is variable-length and
 * lives in the caller's blob; a consumer that needs it uses the C API's
 * iwi_file_components() on that blob, or reads what it needs from `components`
 * (the count) and `bitsPerPixel` (the derived total). Dropping the old
 * `component[4]` member removed a fixed cap that nothing was reading.
 */
struct IwiHeader {
    uint32_t        width          = 0;
    uint32_t        height         = 0;
    uint8_t         colorSpace     = 0;
    uint16_t        components     = 0;
    uint32_t        bitsPerPixel   = 0;
    IWStorageLayout storageLayout{};
    uint32_t        metaBytes      = 0;
    uint32_t        metaOffset     = 0;
    uint32_t        headerBytes    = 0;
    uint64_t        packedBytes    = 0;
    char            payloadEncoding[8] = {};
};

struct IwiParseResult {
    IwiStatus status = IwiStatus::Ok;
    IwiHeader header{};
    /* The C API's reason text. The ABI tests assert substrings of it
     * ("magic", "planar", "offset", "header"), so the wording is a contract. */
    char reason[192] = {};

    /*
     * The same status as the C enum, kept raw so `unsupported()` can classify
     * it without a second mapping table. A caller that renders needs the
     * difference between "corrupt" and "valid, not displayable".
     */
    int nativeStatus = IWI_STATUS_OK;

    bool ok() const { return status == IwiStatus::Ok; }

    /* Valid IWI this build cannot display. Only meaningful when !ok(). */
    bool unsupported() const { return iwi_status_is_unsupported(nativeStatus) != 0; }
};

namespace detail {

/* The single conversion from C status to module status. */
inline IwiStatus MapStatus(int rc) noexcept
{
    switch (rc) {
    case IWI_STATUS_OK:                          return IwiStatus::Ok;
    case IWI_STATUS_NO_INPUT:                    return IwiStatus::NullInput;
    case IWI_STATUS_TRUNCATED_HEADER:            return IwiStatus::TruncatedHeader;
    case IWI_STATUS_BAD_MAGIC:                   return IwiStatus::BadMagic;
    case IWI_STATUS_ZERO_DIMENSION:              return IwiStatus::ZeroDimension;
    case IWI_STATUS_GEOMETRY_TOO_LARGE:          return IwiStatus::GeometryTooLarge;
    case IWI_STATUS_PAYLOAD_TRUNCATED:           return IwiStatus::PayloadBeyondBuffer;
    case IWI_STATUS_COMPONENT_LAYOUT:            return IwiStatus::UnexpectedComponentLayout;
    case IWI_STATUS_ENOSPC:                      return IwiStatus::OutOfSpace;
    case IWI_STATUS_UNSUPPORTED_ENCODING:        return IwiStatus::UnsupportedEncoding;
    case IWI_STATUS_UNSUPPORTED_STORAGE:         return IwiStatus::UnsupportedStorage;
    case IWI_STATUS_UNSUPPORTED_COMPONENT_COUNT: return IwiStatus::UnsupportedComponentCount;
    case IWI_STATUS_UNSUPPORTED_COMPONENT_CLASS: return IwiStatus::UnsupportedComponentClass;
    case IWI_STATUS_UNSUPPORTED_BIT_DEPTH:       return IwiStatus::UnsupportedBitDepth;
    case IWI_STATUS_UNEXPECTED_SEMANTIC:         return IwiStatus::UnexpectedSemantic;
    case IWI_STATUS_UNADDRESSABLE_GEOMETRY:      return IwiStatus::UnaddressableGeometry;
    case IWI_STATUS_UNSUPPORTED:                 return IwiStatus::Unsupported;
    case IWI_STATUS_NO_COMPONENTS:               return IwiStatus::NoComponents;
    }
    return IwiStatus::UnexpectedComponentLayout;
}

inline void CopyReason(char (&dest)[192], int rc) noexcept
{
    const char* msg = iwi_status_message(rc);
    const std::size_t len = std::strlen(msg);
    const std::size_t copyLen = len < sizeof(dest) - 1u ? len : sizeof(dest) - 1u;
    std::memcpy(dest, msg, copyLen);
    dest[copyLen] = '\0';
}


/*
 * This build's capability policy, applied to a header the grammar accepted.
 *
 * Everything here is a statement about what ImageViewer can put on a screen,
 * not about what an IWI file may contain.  A 1-component 16-bit planar capture
 * is a well-formed file; it is just not an RGBA raster.
 *
 * Order matters for the message a user sees.  Bit depth and class come before
 * the count, so a 16-bit grayscale file is reported as "not 8-bit" rather than
 * as an unremarkable count problem - the depth is the more specific fact.
 */
/* The storage flag bits this build implements.  Everything else is a layout
 * this decoder does not apply, and accepting it silently would render the
 * image transposed or mirrored with no error. */
inline constexpr uint32_t kSupportedStorageFlags = 0u;

inline int ApplyViewerPolicy(const IwiFileHeader* f) noexcept
{
    if (f == nullptr) {
        return IWI_STATUS_NO_INPUT;
    }

    /*
     * Storage layout is a bitmask, so this is a mask test plus an exact test,
     * never an equality against one flag.  This build implements interleaved,
     * tightly packed rows and nothing else; a declared stride would change the
     * payload arithmetic, so a file that sets one is refused rather than
     * silently read as if it were absent.
     */
    const uint32_t flags  = f->storageLayout.flags;
    const uint32_t stride = f->storageLayout.stride;

    if ((flags & static_cast<uint32_t>(IW_STORAGE_FLAG_PLANAR)) != 0u ||
        flags != kSupportedStorageFlags ||
        stride != 0u) {
        return IWI_STATUS_UNSUPPORTED_STORAGE;
    }

    /* Payload codec: full-field compare.  "lz4" and "zstd" are valid container
     * values this build cannot decode, and a name that merely starts with
     * "raw" is not "raw". */
    if (!iwi_payload_encoding_is_raw(f)) {
        return IWI_STATUS_UNSUPPORTED_ENCODING;
    }

    const uint16_t n = f->componentCount;
    const IwiFileComponent* comps = iwi_file_components(f);

    for (uint16_t i = 0; i < n; ++i) {
        if (comps[i].bitWidth != 8u) {
            return IWI_STATUS_UNSUPPORTED_BIT_DEPTH;
        }
    }
    for (uint16_t i = 0; i < n; ++i) {
        if (comps[i].componentClass != static_cast<uint8_t>(IW_COMPONENT_CLASS_UINT) &&
            comps[i].componentClass != static_cast<uint8_t>(IW_COMPONENT_CLASS_UNORM)) {
            return IWI_STATUS_UNSUPPORTED_COMPONENT_CLASS;
        }
    }

    /* Only 3 and 4 components have an RGBA form. */
    if (n != 3u && n != 4u) {
        return IWI_STATUS_UNSUPPORTED_COMPONENT_COUNT;
    }

    static const uint8_t kRequiredSemantics[4] = {
        static_cast<uint8_t>(IW_COMPONENT_SEMANTIC_R),
        static_cast<uint8_t>(IW_COMPONENT_SEMANTIC_G),
        static_cast<uint8_t>(IW_COMPONENT_SEMANTIC_B),
        static_cast<uint8_t>(IW_COMPONENT_SEMANTIC_A)
    };

    for (uint16_t i = 0; i < n; ++i) {
        if (comps[i].semantic != kRequiredSemantics[i]) {
            return IWI_STATUS_UNEXPECTED_SEMANTIC;
        }
        if (comps[i].bitOffset != static_cast<uint16_t>(i * 8u)) {
            return IWI_STATUS_COMPONENT_LAYOUT;
        }
    }

    return IWI_STATUS_OK;
}


} // namespace detail

/*
 * Validate a container header.
 *
 * `size` is the whole container — the payload bounds check needs it, so a
 * caller holding only the first 36 bytes cannot use this to accept a file.
 *
 * This is the *viewer* path: the grammar plus this build's capability policy,
 * so a grayscale, 16-bit or planar file is refused with a member of the
 * unsupported family rather than being reported as corrupt. That is the
 * distinction a user needs - "your file is fine, this viewer cannot show it".
 *
 * The container itself has no component cap and no depth limit; a consumer
 * that converts rather than displays wants IwiFile.h's iwi_parse_structural and
 * its own policy, which is what IWImageIO does.
 */
inline IwiParseResult ParseHeader(const uint8_t* bytes,
                                  std::size_t available,
                                  std::uint64_t containerSize)
{
    IwiParseResult result;

    if (bytes == nullptr) {
        result.status = IwiStatus::NullInput;
        detail::CopyReason(result.reason, IWI_STATUS_NO_INPUT);
        return result;
    }
    if (available < IWI_FILE_BASE_HEADER_BYTES) {
        result.status = IwiStatus::TruncatedHeader;
        detail::CopyReason(result.reason, IWI_STATUS_TRUNCATED_HEADER);
        return result;
    }

    /*
     * Size the blob the C API will fill: 36 + 6N. One allocation, one parse.
     *
     * `required == 0` means the base header is not readable: bad magic or a
     * zero component count, both of which iwi_parse decides before it looks at
     * the output buffer. Give it a base-sized buffer so those checks run and
     * the status that comes back is the real one.
     */
    const std::size_t required = iwi_file_required_size(bytes, available);

    std::vector<uint8_t> blob(required == 0 ? IWI_FILE_BASE_HEADER_BYTES : required, 0);

    /*
     * Grammar over the head, then this build's capability policy.
     *
     * The prefix form is grammar-only by design: the shell thumbnail provider
     * holds a head rather than the file, so it needs the two-length call.  The
     * policy is then applied to the same blob, which is why this module does
     * not need a copy of the capability rules - and why both consumers here
     * (the loader and the thumbnail provider) accept exactly the same files.
     */
    IwiFileHeader* const parsed = reinterpret_cast<IwiFileHeader*>(blob.data());

    int rc = iwi_parse_prefix(parsed, blob.size(), bytes, available,
                              static_cast<std::size_t>(containerSize));
    if (rc == IWI_STATUS_OK) {
        rc = detail::ApplyViewerPolicy(parsed);
    }

    result.nativeStatus = rc;
    detail::CopyReason(result.reason, rc);
    result.status = detail::MapStatus(rc);

    if (result.status != IwiStatus::Ok) {
        return result;
    }

    /* Read the validated base header back out of the blob. */
    IwiFileHeader base{};
    std::memcpy(&base, blob.data(), IWI_FILE_BASE_HEADER_BYTES);

    result.header.width         = base.width;
    result.header.height        = base.height;
    result.header.colorSpace    = base.colorSpace;
    result.header.components    = base.componentCount;
    result.header.metaBytes     = base.metaBytes;
    result.header.storageLayout = base.storageLayout;
    std::memcpy(result.header.payloadEncoding, base.payloadEncoding, 8);

    /* 36 + 6N is the end of the component table, which is where metadata
     * starts; add metaBytes for the offset of the first pixel. */
    result.header.metaOffset =
        IWI_FILE_BASE_HEADER_BYTES +
        static_cast<uint32_t>(result.header.components) * IWI_FILE_COMPONENT_BYTES;
    result.header.headerBytes = result.header.metaOffset + result.header.metaBytes;

    /*
     * bitsPerPixel is the sum of the component widths, widened to uint32_t:
     * with no component cap a uint16_t accumulator can wrap, and a wrapped
     * value is worse than a large one.
     */
    uint32_t bits = 0;
    const IwiFileComponent* comps = iwi_file_components(
        reinterpret_cast<const IwiFileHeader*>(blob.data()));
    for (uint16_t i = 0; i < result.header.components; ++i) {
        bits += comps[i].bitWidth;
    }
    result.header.bitsPerPixel = bits;

    result.header.packedBytes =
        static_cast<uint64_t>(base.width) * base.height * base.componentCount;

    return result;
}

/*
 * Whole-buffer convenience: the bytes held are the whole container.
 *
 * A prefix reader - the shell thumbnail provider is the only one - calls the
 * three-argument form with the head it read and the container size it got from
 * the filesystem. Passing the container size as the readable length would let
 * the validator walk past the buffer; passing the head length as the container
 * size would reject every file for having no payload.
 */
inline IwiParseResult ParseHeader(const uint8_t* bytes, std::size_t size)
{
    return ParseHeader(bytes, size, static_cast<std::uint64_t>(size));
}

} // export namespace iwi
