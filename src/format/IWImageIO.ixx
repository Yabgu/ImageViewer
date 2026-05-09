module;

#include <cstdint>
#include <cstring>
#include <fstream>
#include <stdexcept>
#include <string>
#include <vector>

#include "ImagePluginDef.h"

export module IWImageIO;

export namespace iw {

// ── In-memory image representation ───────────────────────────────────────────

struct Image {
    uint32_t        width      = 0;
    uint32_t        height     = 0;
    ImageColorSpace colorSpace = IMAGE_COLOR_SPACE_UNKNOWN;
    IWImageFormat   format     = {};
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
// 12       1    colorSpace      uint8_t  (ImageColorSpace value)
// 13       2    componentCount  uint16_t
// 15       2    bitsPerPixel    uint16_t
// 17       1    storageLayout   uint8_t  (0=INTERLEAVED, 1=PLANAR)
// 18      48    components[8]   each slot is 6 bytes:
//                   semantic       uint8_t
//                   componentClass uint8_t
//                   bitOffset      uint16_t
//                   bitWidth       uint16_t
// 66       8    pixelDataSize   uint64_t
// 74       *    pixelData       uint8_t[]

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

inline uint8_t  ReadU8 (const uint8_t* p) { return *p; }
inline uint16_t ReadU16(const uint8_t* p)
{
    return static_cast<uint16_t>(p[0]) | (static_cast<uint16_t>(p[1]) << 8);
}
inline uint32_t ReadU32(const uint8_t* p)
{
    return  static_cast<uint32_t>(p[0])
         | (static_cast<uint32_t>(p[1]) << 8)
         | (static_cast<uint32_t>(p[2]) << 16)
         | (static_cast<uint32_t>(p[3]) << 24);
}
inline uint64_t ReadU64(const uint8_t* p)
{
    uint64_t v = 0;
    for (int i = 0; i < 8; ++i)
        v |= static_cast<uint64_t>(p[i]) << (8 * i);
    return v;
}

} // namespace detail

// ── Serialize / Deserialize ───────────────────────────────────────────────────

inline std::vector<uint8_t> SerializeImage(const Image& img)
{
    using namespace detail;
    std::vector<uint8_t> buf;
    buf.reserve(74 + img.pixelData.size());

    static constexpr char kMagic[4] = {'I', 'W', 'I', '1'};
    buf.insert(buf.end(), kMagic, kMagic + 4);

    WriteU32(buf, img.width);
    WriteU32(buf, img.height);
    WriteU8 (buf, static_cast<uint8_t>(img.colorSpace));
    WriteU16(buf, img.format.componentCount);
    WriteU16(buf, img.format.bitsPerPixel);
    WriteU8 (buf, img.format.storageLayout == IW_STORAGE_PLANAR ? 1 : 0);

    for (int i = 0; i < IW_MAX_COMPONENTS; ++i) {
        const IWComponentDef& cd = img.format.components[i];
        WriteU8 (buf, static_cast<uint8_t>(cd.semantic));
        WriteU8 (buf, static_cast<uint8_t>(cd.componentClass));
        WriteU16(buf, cd.bitOffset);
        WriteU16(buf, cd.bitWidth);
    }

    WriteU64(buf, static_cast<uint64_t>(img.pixelData.size()));
    buf.insert(buf.end(), img.pixelData.begin(), img.pixelData.end());
    return buf;
}

inline Image DeserializeImage(const uint8_t* data, size_t size)
{
    using namespace detail;
    // 4 (magic) + 4 (width) + 4 (height) + 1 (colorSpace) + 2 (componentCount)
    // + 2 (bitsPerPixel) + 1 (storageLayout) + 8*6 (components) + 8 (pixelDataSize) = 74
    constexpr size_t kMinSize = 74;
    if (size < kMinSize)
        throw std::runtime_error("IWI data too short");

    const uint8_t* p = data;
    static constexpr char kMagic[4] = {'I', 'W', 'I', '1'};
    if (std::memcmp(data, kMagic, 4) != 0)
        throw std::runtime_error("IWI magic mismatch — not an IWI file");
    p += 4;

    Image img;
    img.width  = ReadU32(p); p += 4;
    img.height = ReadU32(p); p += 4;
    img.colorSpace = static_cast<ImageColorSpace>(ReadU8(p)); p += 1;

    img.format.componentCount = ReadU16(p); p += 2;
    img.format.bitsPerPixel   = ReadU16(p); p += 2;
    img.format.storageLayout  = ReadU8(p) ? IW_STORAGE_PLANAR
                                           : IW_STORAGE_INTERLEAVED;
    p += 1;

    for (int i = 0; i < IW_MAX_COMPONENTS; ++i) {
        img.format.components[i].semantic =
            static_cast<IWComponentSemantic>(ReadU8(p)); p += 1;
        img.format.components[i].componentClass =
            static_cast<IWComponentClass>(ReadU8(p)); p += 1;
        img.format.components[i].bitOffset = ReadU16(p); p += 2;
        img.format.components[i].bitWidth  = ReadU16(p); p += 2;
    }

    const uint64_t pixelBytes = ReadU64(p); p += 8;
    // p is now exactly data + 74 (kMinSize), which is <= data + size.
    const size_t consumed  = static_cast<size_t>(p - data);
    const size_t remaining = size - consumed;
    if (remaining < static_cast<size_t>(pixelBytes))
        throw std::runtime_error("IWI pixel data truncated");

    img.pixelData.assign(p, p + pixelBytes);
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
