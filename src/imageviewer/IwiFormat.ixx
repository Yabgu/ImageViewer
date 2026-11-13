module;

#include <cstddef>
#include <cstdint>
#include <cstring>

#include "ImagePluginDef.h"
#include "IwiFile.h"

export module IwiFormat;

/*
 * IwiFormat — the C++ view of the .iwi container header.
 *
 * The implementation is not here.  It is `iwi_file_parse_header` in IwiFile.c,
 * a plain C API shared with Tension and with anything else that binds the
 * format.  This module exists so module units can consume the result
 * ergonomically — a parsed struct with a status enum — without every call site
 * naming a C error code.
 *
 * # Why the C API is the implementation
 *
 * The 74-byte header had been parsed independently in at least four places:
 * this viewer's loader, its shell thumbnail provider, Tension's Rust reader,
 * and Tension's JS writer.  The Rust file documents the cost in its own header
 * comment — it is "a byte-for-byte port of tension-2d/src/texture.cpp ... rather
 * than depending on it as a C++ module".  A C++ module cannot fix that: Rust, a
 * build script and a C engine cannot import one.  A C header can.
 *
 * So the rules live in IwiFile.c, once, and every consumer in this repository
 * goes through it.  A file Explorer refuses is a file iview refuses, by
 * construction rather than by discipline.
 *
 * # The header on disk
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
 * # What is *not* decided here
 *
 * Colour space.  The header carries one and it is reported as-is; whether sRGB,
 * linear, PQ or HLG can be shown is a display question that belongs to the
 * consumer, not to the container.  ImageLoaderIWI passes any of the four
 * through; the thumbnail provider refuses the two HDR transfer functions
 * because it has no tone mapping to offer.
 */

export namespace iwi {

/* Field offsets, taken from the C header rather than restated.  A second copy
 * here is the exact drift this API exists to prevent. */
inline constexpr std::size_t kHeaderBytes     = iwi_file::kHeaderBytes;
inline constexpr std::size_t kOffMagic        = iwi_file::kOffMagic;
inline constexpr std::size_t kOffWidth        = iwi_file::kOffWidth;
inline constexpr std::size_t kOffHeight       = iwi_file::kOffHeight;
inline constexpr std::size_t kOffColorSpace   = iwi_file::kOffColorSpace;
inline constexpr std::size_t kOffComponentNum = iwi_file::kOffComponents;
inline constexpr std::size_t kOffBitsPerPixel = iwi_file::kOffBitsPerPixel;
inline constexpr std::size_t kOffStorage      = iwi_file::kOffStorage;
inline constexpr std::size_t kOffComponents   = iwi_file::kOffComponentTab;
inline constexpr std::size_t kComponentStride = iwi_file::kComponentBytes;
inline constexpr std::size_t kOffPixelBytes   = iwi_file::kOffPixelBytes;

/* Component record fields. */
inline constexpr std::size_t kCompOffSemantic = iwi_file::kCompOffSemantic;
inline constexpr std::size_t kCompOffClass    = iwi_file::kCompOffClass;
inline constexpr std::size_t kCompOffBitOff   = iwi_file::kCompOffBitOffset;
inline constexpr std::size_t kCompOffBitWidth = iwi_file::kCompOffBitWidth;

/*
 * Status, with the same values as the C enum so the two can be compared and
 * converted without a lookup table.  `ok()` is the common test.
 */
enum class IwiStatus {
    Ok                       = IWI_FILE_OK,
    NullInput                = IWI_FILE_ERR_NO_INPUT,
    TruncatedHeader          = IWI_FILE_ERR_TRUNCATED_HEADER,
    BadMagic                 = IWI_FILE_ERR_BAD_MAGIC,
    ZeroDimension            = IWI_FILE_ERR_ZERO_DIMENSION,
    UnaddressableGeometry    = IWI_FILE_ERR_UNADDRESSABLE,
    GeometryTooLarge         = IWI_FILE_ERR_TOO_LARGE,
    PayloadSmallerThanGeometry = IWI_FILE_ERR_PAYLOAD_DECLARED,
    PayloadBeyondBuffer      = IWI_FILE_ERR_PAYLOAD_TRUNCATED,
    UnsupportedStorage       = IWI_FILE_ERR_PLANAR,
    UnsupportedComponentCount = IWI_FILE_ERR_COMPONENT_COUNT,
    UnsupportedBitDepth      = IWI_FILE_ERR_COMPONENT_DEPTH,
    UnexpectedSemantic       = IWI_FILE_ERR_COMPONENT_ORDER,
    UnsupportedComponentClass = IWI_FILE_ERR_COMPONENT_CLASS,
    UnexpectedComponentLayout = IWI_FILE_ERR_COMPONENT_LAYOUT,
};

/*
 * The validated header, in the shape the loader and the thumbnail provider
 * already use.  `packedBytes` is what the geometry needs; `pixelBytes` is what
 * the file declares, which may be larger (trailing slack) but never smaller.
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
    /* The C API's reason text.  The ABI tests assert substrings of it
     * ("magic", "planar", "offset", …), so the wording is a contract. */
    char reason[192] = {};

    bool ok() const { return status == IwiStatus::Ok; }
};

/*
 * Validate a container header.
 *
 * `size` is the whole container — the payload bounds check needs it, so a
 * caller holding only the first 74 bytes cannot use this to accept a file.
 */
inline IwiParseResult ParseHeader(const uint8_t* bytes, std::size_t size)
{
    IwiFileHeader raw{};
    IwiParseResult result;

    result.status = static_cast<IwiStatus>(
        iwi_file_parse_header(bytes, size, &raw, result.reason,
                              sizeof(result.reason)));

    if (result.status != IwiStatus::Ok)
        return result;

    result.header.width         = raw.width;
    result.header.height        = raw.height;
    result.header.colorSpace    = raw.colorSpace;
    result.header.components    = raw.componentCount;
    result.header.bitsPerPixel  = raw.bitsPerPixel;
    result.header.storageLayout = raw.storageLayout;
    result.header.pixelBytes    = raw.pixelBytes;
    result.header.packedBytes   = raw.packedBytes;

    for (std::size_t i = 0; i < 4 && i < raw.componentCount; ++i) {
        result.header.component[i].semantic =
            static_cast<uint8_t>(raw.components[i].semantic);
        result.header.component[i].componentClass =
            static_cast<uint8_t>(raw.components[i].componentClass);
        result.header.component[i].bitOffset = raw.components[i].bitOffset;
        result.header.component[i].bitWidth  = raw.components[i].bitWidth;
    }
    return result;
}

} // export namespace iwi
