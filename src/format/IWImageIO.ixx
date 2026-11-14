module;

#include <cstdint>
#include <cstring>
#include <fstream>
#include <stdexcept>
#include <string>
#include <vector>

#include "ImagePluginDef.h"
#include "IwiFile.h"

export module IWImageIO;

export namespace iw {

// ── In-memory image representation ───────────────────────────────────────────

struct Image {
    uint32_t        width      = 0;
    uint32_t        height     = 0;
    ImageColorSpace colorSpace = IMAGE_COLOR_SPACE_UNKNOWN;
    IWImageFormat   format     = {};
    std::vector<IWComponentDef> components;
    std::vector<uint8_t> pixelData;
};

// ── Builder ───────────────────────────────────────────────────────────────────

inline Image BuildImage(int width, int height,
                        ImageColorSpace colorSpace,
                        const IWImageFormat& fmt,
                        const uint8_t* pixels, size_t pixelBytes)
{
    Image img;
    img.width      = static_cast<uint32_t>(width);
    img.height     = static_cast<uint32_t>(height);
    img.colorSpace = colorSpace;
    img.format     = fmt;
    if (fmt.components && fmt.componentCount > 0) {
        img.components.assign(fmt.components, fmt.components + fmt.componentCount);
        img.format.components = img.components.data();
    }
    img.pixelData.assign(pixels, pixels + pixelBytes);
    return img;
}

// ── Binary .iwi format ────────────────────────────────────────────────────────
//
// All integers are stored in little-endian byte order.
//
// Offset  Size  Field
// ------  ----  -----
//  0       4    magic           char[4]  = {'I','W','I','1'}
//  4       4    width           uint32_t
//  8       4    height          uint32_t
// 12       4    metaBytes       uint32_t
// 16       8    storageLayout   uint64_t (IWStorageLayout: flags u32, stride u32)
// 24       8    payloadEncoding char[8]  ("raw\0\0\0\0\0")
// 32       1    colorSpace      uint8_t  (ImageColorSpace enum)
// 33       1    reserved        uint8_t  (0)
// 34       2    componentCount  uint16_t
// 36     N*6    components[N]   each slot is 6 bytes:
//                   semantic       uint8_t
//                   componentClass uint8_t
//                   bitOffset      uint16_t
//                   bitWidth       uint16_t
// 36+N*6   *    metaBlob        uint8_t[metaBytes]
//    *     *    pixelData       uint8_t[]

namespace detail {

inline void WriteU8(std::vector<uint8_t>& buf, uint8_t v)
{
    buf.push_back(v);
}

inline void WriteU16(std::vector<uint8_t>& buf, uint16_t v)
{
    buf.push_back(static_cast<uint8_t>(v));
    buf.push_back(static_cast<uint8_t>(v >> 8));
}

inline void WriteU32(std::vector<uint8_t>& buf, uint32_t v)
{
    buf.push_back(static_cast<uint8_t>(v));
    buf.push_back(static_cast<uint8_t>(v >> 8));
    buf.push_back(static_cast<uint8_t>(v >> 16));
    buf.push_back(static_cast<uint8_t>(v >> 24));
}

inline void WriteU64(std::vector<uint8_t>& buf, uint64_t v)
{
    for (int i = 0; i < 8; ++i)
        buf.push_back(static_cast<uint8_t>(v >> (8 * i)));
}

} // namespace detail

// ── Serialize / Deserialize ───────────────────────────────────────────────────

inline std::vector<uint8_t> SerializeImage(const Image& img)
{
    using namespace detail;
    const size_t headerBytes = 36 + static_cast<size_t>(img.format.componentCount) * 6;
    std::vector<uint8_t> buf;
    buf.reserve(headerBytes + img.pixelData.size());

    static constexpr char kMagic[4] = {'I', 'W', 'I', '1'};
    buf.insert(buf.end(), kMagic, kMagic + 4);

    WriteU32(buf, img.width);
    WriteU32(buf, img.height);
    WriteU32(buf, 0); // metaBytes = 0
    WriteU64(buf, img.format.storageLayout.raw);
    static constexpr char kEncoding[8] = {'r', 'a', 'w', 0, 0, 0, 0, 0};
    buf.insert(buf.end(), kEncoding, kEncoding + 8);
    WriteU8 (buf, static_cast<uint8_t>(img.colorSpace));
    WriteU8 (buf, 0); // reserved
    WriteU16(buf, img.format.componentCount);

    for (uint16_t i = 0; i < img.format.componentCount; ++i) {
        const IWComponentDef& cd = img.format.components[i];
        WriteU8 (buf, static_cast<uint8_t>(cd.semantic));
        WriteU8 (buf, static_cast<uint8_t>(cd.componentClass));
        WriteU16(buf, cd.bitOffset);
        WriteU16(buf, cd.bitWidth);
    }

    buf.insert(buf.end(), img.pixelData.begin(), img.pixelData.end());
    return buf;
}

/*
 * Read a container into an Image, for conversion rather than display.
 *
 * The grammar is IwiFile.cpp's - this function does not parse.  That is the
 * whole point of the split: this reader and the viewer's loader used to carry
 * two copies of the header rules and they drifted, so iwconv wrote 16-bit and
 * grayscale files that iview then refused.
 *
 * This is the *converter's* policy, and it is deliberately wider than the
 * viewer's: one component is fine, 16-bit samples are fine, planar planes are
 * fine, because the output carries them through untouched.  Its only
 * capability rule is the codec, since it cannot decompress either.
 *
 * The payload is returned as stored, not converted to RGBA.  iwi_decode
 * produces RGBA and belongs to the display path; rounding 16-bit samples
 * through RGBA8 here would silently destroy the precision this function exists
 * to preserve.
 */
inline Image DeserializeImage(const uint8_t* data, size_t size)
{
    if (data == nullptr) {
        throw std::runtime_error("IWI: no input");
    }

    /* Size the blob the grammar fills: 36 + 6N. */
    const size_t required = iwi_file_required_size(data, size);
    std::vector<uint8_t> blob(required == 0 ? IWI_FILE_BASE_HEADER_BYTES : required, 0);

    const int rc = iwi_parse_structural(
        reinterpret_cast<IwiFileHeader*>(blob.data()), blob.size(), data, size);

    if (rc != IWI_STATUS_OK) {
        throw std::runtime_error(std::string("IWI: ") + iwi_status_message(rc));
    }

    /* The validated base header, copied out of the blob the same way the C
     * API wrote it: IWI_FILE_BASE_HEADER_BYTES, never sizeof, because the
     * struct's four tail-padding bytes overlap the component table. */
    IwiFileHeader base{};
    std::memcpy(&base, blob.data(), IWI_FILE_BASE_HEADER_BYTES);

    if (!iwi_payload_encoding_is_raw(&base)) {
        throw std::runtime_error(std::string("IWI: ") +
                                 iwi_status_message(IWI_STATUS_UNSUPPORTED_ENCODING));
    }

    Image img;
    img.width                 = base.width;
    img.height                = base.height;
    img.colorSpace            = static_cast<ImageColorSpace>(base.colorSpace);
    img.format.storageLayout  = base.storageLayout;
    img.format.componentCount = base.componentCount;

    const IwiFileComponent* comps = iwi_file_components(
        reinterpret_cast<const IwiFileHeader*>(blob.data()));

    img.components.resize(base.componentCount);
    uint32_t bitsPerPixel = 0;
    for (uint16_t i = 0; i < base.componentCount; ++i) {
        img.components[i].semantic = static_cast<IWComponentSemantic>(comps[i].semantic);
        img.components[i].componentClass = static_cast<IWComponentClass>(comps[i].componentClass);
        img.components[i].bitOffset = comps[i].bitOffset;
        img.components[i].bitWidth  = comps[i].bitWidth;
        bitsPerPixel += comps[i].bitWidth;
    }
    img.format.components   = img.components.data();
    /* The ABI's field is uint16_t and there is no component cap, so a wide
     * table is saturated rather than wrapped.  Three or four 8-bit components
     * make this a no-op; it is here so the narrowing is deliberate. */
    img.format.bitsPerPixel = static_cast<uint16_t>(
        bitsPerPixel > 0xFFFFu ? 0xFFFFu : bitsPerPixel);

    const size_t payloadOffset =
        IWI_FILE_BASE_HEADER_BYTES +
        static_cast<size_t>(base.componentCount) * IWI_FILE_COMPONENT_BYTES +
        static_cast<size_t>(base.metaBytes);

    /* Everything after the header, metadata and table.  Trailing slack is
     * included rather than trimmed, which is the behaviour this function has
     * always had and what the round-trip tests assert. */
    img.pixelData.assign(data + payloadOffset, data + size);
    return img;
}
// ── File I/O ──────────────────────────────────────────────────────────────────

inline void SaveIWI(const std::string& path, const Image& img)
{
    const auto buf = SerializeImage(img);
    std::ofstream ofs(path, std::ios::binary);
    if (!ofs)
        throw std::runtime_error("Cannot open for writing: " + path);
    ofs.write(reinterpret_cast<const char*>(buf.data()),
              static_cast<std::streamsize>(buf.size()));
    if (!ofs)
        throw std::runtime_error("Write failed: " + path);
}

inline Image LoadIWI(const std::string& path)
{
    std::ifstream ifs(path, std::ios::binary);
    if (!ifs)
        throw std::runtime_error("Cannot open for reading: " + path);
    const std::vector<uint8_t> buf{std::istreambuf_iterator<char>(ifs),
                                   std::istreambuf_iterator<char>()};
    return DeserializeImage(buf.data(), buf.size());
}

} // namespace iw
