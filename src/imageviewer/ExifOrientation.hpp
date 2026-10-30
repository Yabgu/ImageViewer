#pragma once
/*
 * ExifOrientation.hpp — EXIF Orientation parsing and upright raster rotation.
 *
 * JPEG files can record how the stored raster should be turned for display:
 * EXIF tag 0x0112 in IFD0, values 1..8.  Phone cameras depend on it — a
 * portrait shot is stored as a landscape raster plus Orientation=6 rather than
 * re-encoding twelve megapixels — so a loader that ignores the tag hands back a
 * sideways image whose width and height are also swapped.
 *
 * libjpeg-turbo decodes the stored raster and has no opinion about the tag, so
 * the loader does the work here.  Two pieces:
 *
 *   ParseOrientation()  reads the tag from a TIFF/EXIF block, defensively:
 *                       the block comes from the file, so every offset and
 *                       count is bounds-checked and any malformation means
 *                       "normal" rather than an out-of-bounds read.
 *
 *   OrientPixels()      writes the upright raster into a second buffer.  The
 *                       caller keeps the decoded buffer only until this returns.
 *
 * Orientation semantics (EXIF 2.32, table 6).  Values 5..8 swap the image's
 * width and height; 1..4 do not:
 *
 *   1  normal                  5  transpose (mirror along the main diagonal)
 *   2  mirror horizontal       6  rotate 90° clockwise
 *   3  rotate 180°             7  transverse (mirror along the anti-diagonal)
 *   4  mirror vertical         8  rotate 270° clockwise
 *
 * Rotating in the loader rather than at draw time is deliberate: every consumer
 * (viewer, iwconv, tests) receives upright pixels and upright dimensions
 * instead of each having to know about the tag.  The cost is one permuted copy
 * of the image, once, at load time.
 */
#include <cstddef>
#include <cstdint>
#include <cstring>

namespace exif {

/* True for the four orientations that exchange width and height. */
constexpr bool SwapsAxes(int orientation) noexcept
{
    return orientation >= 5 && orientation <= 8;
}

/* Dimensions of the upright image.  Values outside 1..8 behave as 1. */
constexpr int OrientedWidth(int width, int height, int orientation) noexcept
{
    return SwapsAxes(orientation) ? height : width;
}

constexpr int OrientedHeight(int width, int height, int orientation) noexcept
{
    return SwapsAxes(orientation) ? width : height;
}

namespace detail {

inline uint16_t ReadU16(const uint8_t* p, bool littleEndian) noexcept
{
    return littleEndian
        ? static_cast<uint16_t>(p[0] | (p[1] << 8))
        : static_cast<uint16_t>((p[0] << 8) | p[1]);
}

inline uint32_t ReadU32(const uint8_t* p, bool littleEndian) noexcept
{
    return littleEndian
        ? (static_cast<uint32_t>(p[0])       | (static_cast<uint32_t>(p[1]) << 8) |
           (static_cast<uint32_t>(p[2]) << 16) | (static_cast<uint32_t>(p[3]) << 24))
        : ((static_cast<uint32_t>(p[0]) << 24) | (static_cast<uint32_t>(p[1]) << 16) |
           (static_cast<uint32_t>(p[2]) << 8)  |  static_cast<uint32_t>(p[3]));
}

/*
 * Stored-raster coordinate that supplies upright pixel (x, y).
 *
 * Derived from the EXIF definition of where row 0 and column 0 of the stored
 * raster appear in the displayed image; the unit tests pin all eight to
 * hand-written expected grids.
 */
inline void SourceCoords(int x, int y, int width, int height,
                         int orientation, int& sx, int& sy) noexcept
{
    switch (orientation) {
    case 2: sx = width - 1 - x; sy = y;             break;
    case 3: sx = width - 1 - x; sy = height - 1 - y; break;
    case 4: sx = x;             sy = height - 1 - y; break;
    case 5: sx = y;             sy = x;             break;
    case 6: sx = y;             sy = height - 1 - x; break;
    case 7: sx = width - 1 - y; sy = height - 1 - x; break;
    case 8: sx = width - 1 - y; sy = x;             break;
    default: sx = x;            sy = y;             break;  /* orientation 1 */
    }
}

/* Bytes-per-pixel known at compile time, so the inner copy is a fixed-size
 * move (3-byte pixels become three byte moves, not a memcpy call). */
template <int BytesPerPixel>
inline void OrientFixed(const uint8_t* src, int width, int height,
                        int orientation, uint8_t* dst) noexcept
{
    const int outWidth  = OrientedWidth(width, height, orientation);
    const int outHeight = OrientedHeight(width, height, orientation);
    const size_t srcStride = static_cast<size_t>(width) * BytesPerPixel;
    const size_t dstStride = static_cast<size_t>(outWidth) * BytesPerPixel;

    for (int y = 0; y < outHeight; ++y) {
        uint8_t* out = dst + static_cast<size_t>(y) * dstStride;
        for (int x = 0; x < outWidth; ++x) {
            int sx, sy;
            SourceCoords(x, y, width, height, orientation, sx, sy);
            const uint8_t* in = src + static_cast<size_t>(sy) * srcStride +
                                static_cast<size_t>(sx) * BytesPerPixel;
            for (int b = 0; b < BytesPerPixel; ++b) {
                out[b] = in[b];
            }
            out += BytesPerPixel;
        }
    }
}

/* Fallback for the pixel sizes the loaders do not currently produce. */
inline void OrientGeneric(const uint8_t* src, int width, int height,
                          int bytesPerPixel, int orientation,
                          uint8_t* dst) noexcept
{
    const int outWidth  = OrientedWidth(width, height, orientation);
    const int outHeight = OrientedHeight(width, height, orientation);
    const size_t srcStride = static_cast<size_t>(width) * bytesPerPixel;
    const size_t dstStride = static_cast<size_t>(outWidth) * bytesPerPixel;

    for (int y = 0; y < outHeight; ++y) {
        for (int x = 0; x < outWidth; ++x) {
            int sx, sy;
            SourceCoords(x, y, width, height, orientation, sx, sy);
            std::memcpy(dst + static_cast<size_t>(y) * dstStride +
                            static_cast<size_t>(x) * bytesPerPixel,
                        src + static_cast<size_t>(sy) * srcStride +
                            static_cast<size_t>(sx) * bytesPerPixel,
                        static_cast<size_t>(bytesPerPixel));
        }
    }
}

} // namespace detail

/*
 * Orientation tag (0x0112) from a TIFF block: the bytes *after* the six-byte
 * "Exif\0\0" identifier, i.e. starting at the "II"/"MM" byte-order mark.
 *
 * Returns 1 (normal) when the tag is absent, malformed, or outside 1..8.
 * Never reads outside [tiff, tiff + size).
 */
inline int ParseOrientation(const uint8_t* tiff, size_t size) noexcept
{
    constexpr int      kNormal         = 1;
    constexpr uint16_t kOrientationTag = 0x0112;
    constexpr uint16_t kTypeShort      = 3;
    constexpr uint16_t kTypeLong       = 4;

    if (tiff == nullptr || size < 8) return kNormal;

    bool littleEndian;
    if (tiff[0] == 'I' && tiff[1] == 'I')      littleEndian = true;
    else if (tiff[0] == 'M' && tiff[1] == 'M') littleEndian = false;
    else                                       return kNormal;

    if (detail::ReadU16(tiff + 2, littleEndian) != 42) return kNormal;

    const uint32_t ifdOffset = detail::ReadU32(tiff + 4, littleEndian);
    if (ifdOffset > size || size - ifdOffset < 2) return kNormal;

    const uint8_t* ifd = tiff + ifdOffset;
    const uint16_t entryCount = detail::ReadU16(ifd, littleEndian);

    /* The entry count is file-controlled: reject a claim that would walk past
     * the block rather than trusting it. */
    const size_t remaining = size - ifdOffset - 2;
    if (entryCount > remaining / 12) return kNormal;

    for (uint16_t i = 0; i < entryCount; ++i) {
        const uint8_t* entry = ifd + 2 + static_cast<size_t>(i) * 12;
        if (detail::ReadU16(entry, littleEndian) != kOrientationTag) continue;

        const uint16_t type  = detail::ReadU16(entry + 2, littleEndian);
        const uint32_t count = detail::ReadU32(entry + 4, littleEndian);
        if (count != 1) return kNormal;

        /* The spec says SHORT; LONG with count 1 is a writer bug that is still
         * unambiguous, so accept it instead of dropping the tag. */
        uint32_t value;
        if (type == kTypeShort)      value = detail::ReadU16(entry + 8, littleEndian);
        else if (type == kTypeLong)  value = detail::ReadU32(entry + 8, littleEndian);
        else                         return kNormal;

        return (value >= 1 && value <= 8) ? static_cast<int>(value) : kNormal;
    }
    return kNormal;
}

/*
 * Write the upright image into `dst`.
 *
 * `src` is the stored raster, tightly packed: width * height * bytesPerPixel
 * bytes.  `dst` must hold
 * OrientedWidth(...) * OrientedHeight(...) * bytesPerPixel bytes and must not
 * overlap `src`.
 *
 * `orientation` is an EXIF value 1..8.  Out-of-range values are treated as 1,
 * and orientation 1 is a plain copy, so calling this unconditionally is safe —
 * the JPEG loader skips the call for 1 to avoid the pointless allocation.
 */
inline void OrientPixels(const uint8_t* src, int width, int height,
                         int bytesPerPixel, int orientation,
                         uint8_t* dst) noexcept
{
    if (width <= 0 || height <= 0 || bytesPerPixel <= 0) return;

    const size_t storedBytes = static_cast<size_t>(width) *
                               static_cast<size_t>(height) *
                               static_cast<size_t>(bytesPerPixel);

    if (orientation < 2 || orientation > 8) {
        if (src != dst) std::memcpy(dst, src, storedBytes);
        return;
    }

    switch (bytesPerPixel) {
    case 1: detail::OrientFixed<1>(src, width, height, orientation, dst); break;
    case 2: detail::OrientFixed<2>(src, width, height, orientation, dst); break;
    case 3: detail::OrientFixed<3>(src, width, height, orientation, dst); break;
    case 4: detail::OrientFixed<4>(src, width, height, orientation, dst); break;
    default: detail::OrientGeneric(src, width, height, bytesPerPixel, orientation, dst); break;
    }
}

} // namespace exif
