module;

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

#include "ImagePluginDef.h"

export module IwiThumbnail;

import IwiFormat;

/*
 * IwiThumbnail — decode an .iwi into a small premultiplied BGRA raster.
 *
 * This is the part of the Windows thumbnail provider that is worth testing
 * without COM: everything here reads through a SampleSource, never through a
 * file handle or an IStream.  The COM wrapper in ThumbnailProviderIWI supplies
 * an IStream-backed source and turns the result into an HBITMAP; the tests
 * supply a memory-backed one.
 *
 * # Why the source is an abstraction
 *
 * Explorer runs thumbnail providers inside a surrogate process and asks for a
 * folder of files at once.  Reading every byte of a 4096x4096 RGBA IWI (64 MB)
 * to produce a 256 px square is wasted I/O and wasted memory.  The header comes
 * first, the output grid is derived from it, and only the source rows that grid
 * samples are read.  A 4x4 RGB IWI is read completely because it is 48 bytes; a
 * large one costs a few hundred row reads.
 *
 * # Colour
 *
 * Samples are converted to linear light, averaged there, and encoded back to
 * sRGB for display — averaging sRGB bytes directly darkens edges.  A source
 * already marked linear skips the decode step.  PQ and HLG are refused: this
 * code has no tone mapping to offer, and a guessed curve renders a wrong
 * picture, which is worse than falling back to the file-type icon.
 *
 * The output is premultiplied because that is what WIC's 32bppPBGRA means and
 * what Explorer composites correctly against the folder background.  A
 * three-component source is opaque, so premultiplying it is the identity.
 */

export namespace iwi {

/*
 * Samples taken per output pixel, per axis.  Reduction is exact up to this
 * many source pixels per axis; beyond it a fixed sparse grid is used, which is
 * past the point a 256 px thumbnail can show the difference and keeps the read
 * volume constant rather than proportional to the source size.
 */
inline constexpr unsigned kMaxSamplesPerAxis = 4;

/*
 * Textures are at most a few thousand pixels on a side.  This cap is not part
 * of the container format — it is this provider refusing to allocate a 128 KB
 * row buffer for a header claiming a 4-billion-pixel width.  A file past the
 * cap still opens in the viewer; it just gets the file-type icon here.
 */
inline constexpr uint32_t kMaxSourceEdge = 32768;

/* Largest edge this code will produce.  Explorer asks for much less; the clamp
 * exists so a bad caller cannot ask for a 100000 px bitmap. */
inline constexpr uint32_t kMaxThumbnailEdge = 4096;

class SampleSource {
public:
    virtual ~SampleSource() = default;

    /* Total size of the container, used for the payload bounds check. */
    virtual uint64_t Size() const = 0;

    /* Read exactly `bytes` at `offset`.  False on a short read or I/O error. */
    virtual bool ReadAt(uint64_t offset, void* dst, std::size_t bytes) = 0;
};

enum class ThumbnailStatus {
    Ok = 0,
    Malformed,    /* the bytes are not an IWI this code accepts   */
    Unsupported,  /* valid IWI, but no representable display form */
    IoError,      /* the source failed to produce the bytes       */
};

struct ThumbnailImage {
    uint32_t width    = 0;
    uint32_t height   = 0;
    bool     hasAlpha = false;
    /* width * height * 4 bytes, BGRA order, premultiplied. */
    std::vector<uint8_t> bgra;
};

/*
 * Render a thumbnail that fits inside `maxEdge` x `maxEdge`, preserving aspect
 * ratio and never upscaling.  `reason` receives a one-line explanation on
 * failure and is left alone on success.
 */
ThumbnailStatus RenderThumbnail(SampleSource& source,
                                uint32_t maxEdge,
                                ThumbnailImage& out,
                                std::string* reason);

/* The sRGB transfer function, exposed so tests can assert the pair is an exact
 * round trip on all 256 byte values.  A round trip is what lets a 1:1
 * thumbnail reproduce its source byte for byte. */
double  SrgbByteToLinear(uint8_t srgb);
uint8_t LinearToSrgbByte(double linear);

} // export namespace iwi

namespace iwi {

namespace {

/* IEC 61966-2-1. */
double SrgbToLinearExact(uint8_t v)
{
    const double c = static_cast<double>(v) / 255.0;
    return c <= 0.04045 ? c / 12.92
                        : std::pow((c + 0.055) / 1.055, 2.4);
}

uint8_t LinearToSrgbExact(double l)
{
    /* NaN and negatives both land on 0; this is fed by averages of in-range
     * samples, so the clamp only has to be defensive. */
    if (!(l > 0.0)) return 0;
    if (l >= 1.0)   return 255;
    const double c = l <= 0.0031308 ? l * 12.92
                                    : 1.055 * std::pow(l, 1.0 / 2.4) - 0.055;
    const long v = std::lround(c * 255.0);
    return static_cast<uint8_t>(std::clamp<long>(v, 0, 255));
}

/*
 * The decode table is built once per process.  `std::pow` 256 times is not
 * slow, but GetThumbnail runs once per file in a folder, and the table also
 * makes the round-trip property cheap to state in a test.
 */
struct LinearTable {
    double toLinear[256];
    LinearTable()
    {
        for (unsigned i = 0; i < 256u; ++i)
            toLinear[i] = SrgbToLinearExact(static_cast<uint8_t>(i));
    }
};

const LinearTable& Table()
{
    static const LinearTable table;
    return table;
}

/* Ascending, distinct source indices covering [begin, end). */
void PickSamples(uint32_t begin, uint32_t end, std::vector<uint32_t>& out)
{
    out.clear();
    if (end <= begin) {
        out.push_back(begin);
        return;
    }
    const uint32_t span = end - begin;
    if (span <= kMaxSamplesPerAxis) {
        for (uint32_t i = begin; i < end; ++i)
            out.push_back(i);
        return;
    }
    for (unsigned k = 0; k < kMaxSamplesPerAxis; ++k) {
        const uint32_t offset = static_cast<uint32_t>(
            (static_cast<uint64_t>(2u * k + 1u) * span) /
            (2u * kMaxSamplesPerAxis));
        out.push_back(begin + std::min(span - 1u, offset));
    }
}

struct Geometry {
    uint32_t width  = 0;
    uint32_t height = 0;
};

Geometry Fit(uint32_t sourceWidth, uint32_t sourceHeight, uint32_t maxEdge)
{
    if (maxEdge == 0u) maxEdge = 1u;
    if (maxEdge > kMaxThumbnailEdge) maxEdge = kMaxThumbnailEdge;

    /* Never upscale: a 4x4 IWI asked for at 256 px is still 4x4. */
    if (sourceWidth <= maxEdge && sourceHeight <= maxEdge)
        return Geometry{ sourceWidth, sourceHeight };

    if (sourceWidth >= sourceHeight) {
        Geometry g;
        g.width  = maxEdge;
        g.height = static_cast<uint32_t>(
            std::max<uint64_t>(1, static_cast<uint64_t>(sourceHeight) *
                                     maxEdge / sourceWidth));
        return g;
    }

    Geometry g;
    g.height = maxEdge;
    g.width  = static_cast<uint32_t>(
        std::max<uint64_t>(1, static_cast<uint64_t>(sourceWidth) *
                                 maxEdge / sourceHeight));
    return g;
}

ThumbnailStatus Fail(ThumbnailStatus status, std::string* reason,
                     const char* text)
{
    if (reason) *reason = text;
    return status;
}

} // namespace

double SrgbByteToLinear(uint8_t srgb)
{
    return Table().toLinear[srgb];
}

uint8_t LinearToSrgbByte(double linear)
{
    return LinearToSrgbExact(linear);
}

ThumbnailStatus RenderThumbnail(SampleSource& source,
                                uint32_t maxEdge,
                                ThumbnailImage& out,
                                std::string* reason)
{
    out = ThumbnailImage{};

    const uint64_t containerBytes = source.Size();
    if (containerBytes < sizeof(IwiFileHeader))
        return Fail(ThumbnailStatus::Malformed, reason,
                    "IWI is smaller than its own header");

    const std::size_t toRead = static_cast<std::size_t>(
        std::min<uint64_t>(containerBytes, 256u));
    uint8_t headerBytes[256];
    if (!source.ReadAt(0, headerBytes, toRead))
        return Fail(ThumbnailStatus::IoError, reason,
                    "could not read the IWI header");

    const IwiParseResult parsed =
        ParseHeader(headerBytes, static_cast<std::size_t>(containerBytes));
    if (!parsed.ok()) {
        if (reason) *reason = parsed.reason;
        return ThumbnailStatus::Malformed;
    }

    const uint8_t colorSpace = parsed.header.colorSpace;
    if (colorSpace == IMAGE_COLOR_SPACE_HDR_PQ ||
        colorSpace == IMAGE_COLOR_SPACE_HDR_HLG)
        return Fail(ThumbnailStatus::Unsupported, reason,
                    "IWI uses an HDR transfer function (PQ/HLG); thumbnails "
                    "have no tone mapping to render it");

    /* UNKNOWN is what an unrecognised transfer function byte decodes to.  The
     * viewer uploads such a file unchanged, so the thumbnail treats it as
     * sRGB rather than showing nothing; that is a guess, and it is the same
     * guess the rest of the pipeline makes. */
    const bool linearSource = (colorSpace == IMAGE_COLOR_SPACE_LINEAR);

    if (parsed.header.width > kMaxSourceEdge ||
        parsed.header.height > kMaxSourceEdge)
        return Fail(ThumbnailStatus::Unsupported, reason,
                    "IWI dimensions exceed the thumbnail provider's limit");

    const uint32_t sourceWidth  = parsed.header.width;
    const uint32_t sourceHeight = parsed.header.height;
    const uint32_t components   = parsed.header.components;
    const std::size_t stride    = static_cast<std::size_t>(sourceWidth) *
                                  components;

    const Geometry geometry = Fit(sourceWidth, sourceHeight, maxEdge);
    if (geometry.width == 0u || geometry.height == 0u)
        return Fail(ThumbnailStatus::Malformed, reason, "empty output geometry");

    std::vector<uint8_t> pixels(static_cast<std::size_t>(geometry.width) *
                                geometry.height * 4u);

    /* Which source columns each output column averages, computed once. */
    std::vector<std::vector<uint32_t>> columns(geometry.width);
    for (uint32_t ox = 0; ox < geometry.width; ++ox) {
        const uint32_t begin = static_cast<uint32_t>(
            static_cast<uint64_t>(ox) * sourceWidth / geometry.width);
        uint32_t end = static_cast<uint32_t>(
            static_cast<uint64_t>(ox + 1u) * sourceWidth / geometry.width);
        if (end <= begin) end = begin + 1u;
        PickSamples(begin, std::min(end, sourceWidth), columns[ox]);
    }

    std::vector<uint8_t> row(stride);
    std::vector<double>  sumR(geometry.width), sumG(geometry.width),
                         sumB(geometry.width), sumA(geometry.width);
    std::vector<uint32_t> count(geometry.width);
    std::vector<uint32_t> rows;

    const double* toLinear = Table().toLinear;

    for (uint32_t oy = 0; oy < geometry.height; ++oy) {
        const uint32_t begin = static_cast<uint32_t>(
            static_cast<uint64_t>(oy) * sourceHeight / geometry.height);
        uint32_t end = static_cast<uint32_t>(
            static_cast<uint64_t>(oy + 1u) * sourceHeight / geometry.height);
        if (end <= begin) end = begin + 1u;
        PickSamples(begin, std::min(end, sourceHeight), rows);

        std::fill(sumR.begin(), sumR.end(), 0.0);
        std::fill(sumG.begin(), sumG.end(), 0.0);
        std::fill(sumB.begin(), sumB.end(), 0.0);
        std::fill(sumA.begin(), sumA.end(), 0.0);
        std::fill(count.begin(), count.end(), 0u);

        for (const uint32_t sy : rows) {
            const uint64_t offset =
                parsed.header.headerBytes + static_cast<uint64_t>(sy) * stride;
            if (!source.ReadAt(offset, row.data(), stride)) {
                if (reason) *reason = "the IWI pixel data is truncated";
                return ThumbnailStatus::IoError;
            }

            for (uint32_t ox = 0; ox < geometry.width; ++ox) {
                double r = 0.0, g = 0.0, b = 0.0, a = 0.0;
                for (const uint32_t sx : columns[ox]) {
                    const uint8_t* p = row.data() +
                                       static_cast<std::size_t>(sx) * components;
                    const uint8_t alpha = components == 4u ? p[3] : 255u;
                    if (linearSource) {
                        r += static_cast<double>(p[0]) / 255.0;
                        g += static_cast<double>(p[1]) / 255.0;
                        b += static_cast<double>(p[2]) / 255.0;
                        a += static_cast<double>(alpha) / 255.0;
                    } else {
                        r += toLinear[p[0]];
                        g += toLinear[p[1]];
                        b += toLinear[p[2]];
                        a += toLinear[alpha];
                    }
                }
                sumR[ox] += r;
                sumG[ox] += g;
                sumB[ox] += b;
                sumA[ox] += a;
                count[ox] += static_cast<uint32_t>(columns[ox].size());
            }
        }

        for (uint32_t ox = 0; ox < geometry.width; ++ox) {
            const double scale = count[ox] != 0u
                ? 1.0 / static_cast<double>(count[ox]) : 0.0;

            const uint8_t red   = LinearToSrgbExact(sumR[ox] * scale);
            const uint8_t green = LinearToSrgbExact(sumG[ox] * scale);
            const uint8_t blue  = LinearToSrgbExact(sumB[ox] * scale);
            const uint8_t alpha = LinearToSrgbExact(sumA[ox] * scale);

            /* Premultiplied: WIC hands this straight to the folder view, which
             * composites the bitmap over the background.  Rounding rather than
             * truncating keeps a fully opaque pixel fully opaque. */
            const auto premultiply = [alpha](uint8_t v) {
                return static_cast<uint8_t>(
                    (static_cast<unsigned>(v) * alpha + 127u) / 255u);
            };

            uint8_t* dst = pixels.data() +
                           (static_cast<std::size_t>(oy) * geometry.width + ox) * 4u;
            dst[0] = premultiply(blue);
            dst[1] = premultiply(green);
            dst[2] = premultiply(red);
            dst[3] = alpha;
        }
    }

    out.width    = geometry.width;
    out.height   = geometry.height;
    out.hasAlpha = components == 4u;
    out.bgra     = std::move(pixels);
    return ThumbnailStatus::Ok;
}

} // namespace iwi
