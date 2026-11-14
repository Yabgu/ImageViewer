/*
 * test_iwi_thumbnail.cpp — the portable half of the .iwi thumbnail provider.
 *
 * IwiThumbnail.cpp is what decides what an Explorer thumbnail of an .iwi looks
 * like: which files are accepted, how they are reduced, and what the resulting
 * bytes mean.  It never touches COM, so every claim below is checked directly,
 * including the ones a shell test cannot make cheaply (exact averaging, the
 * premultiply, aspect fitting, refusals).
 *
 * The last suites read the committed .iwi fixtures from disk, so the file path
 * the shell actually uses (header first, then positional row reads) is covered
 * with real files and not only with bytes built in memory.
 *
 * The final suite checks the identifiers the registration depends on.  A CLSID
 * that differs between the DLL and the install scripts fails silently in
 * Explorer — no handler, no error, just the file-type icon — so it is asserted
 * here rather than left to a manual check after installing.
 */

/* The other suites' main TU does the same: doctest is header-only here and
 * this target has one translation unit, so the implementation lives in it. */
#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>

#include <cmath>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>
#include <vector>

#ifdef _WIN32
#include <objbase.h>
/* IID_IThumbnailProvider, for the check that the IID this project defines
 * locally matches the one the shell actually asks for. */
#include <thumbcache.h>
#endif

/* The C ABI's colour-space and storage-layout constants.  ImagePluginDef.h is
 * deliberately not a module: it is a C header that the plugins, the ABI tests
 * and embedders share, so it is included rather than imported. */
#include "ImagePluginDef.h"
#include "IwiFile.h"

import IwiFormat;
import IwiThumbnail;
import ThumbnailProviderIds;

#include "repo_root.hpp"

namespace {

void PutU16(std::vector<uint8_t>& b, std::size_t at, uint16_t v)
{
    b[at]     = static_cast<uint8_t>(v & 0xFFu);
    b[at + 1] = static_cast<uint8_t>((v >> 8) & 0xFFu);
}

void PutU32(std::vector<uint8_t>& b, std::size_t at, uint32_t v)
{
    for (int i = 0; i < 4; ++i)
        b[at + static_cast<std::size_t>(i)] =
            static_cast<uint8_t>((v >> (8 * i)) & 0xFFu);
}

void PutU64(std::vector<uint8_t>& b, std::size_t at, uint64_t v)
{
    for (int i = 0; i < 8; ++i)
        b[at + static_cast<std::size_t>(i)] =
            static_cast<uint8_t>((v >> (8 * i)) & 0xFFu);
}

struct Spec {
    uint32_t width  = 0;
    uint32_t height = 0;
    uint16_t components = 3;
    uint8_t  colorSpace = IMAGE_COLOR_SPACE_SRGB;
};

std::vector<uint8_t> MakeIwi(const Spec& spec,
                             const std::vector<uint8_t>& pixels,
                             std::size_t trailingSlack = 0)
{
    const std::size_t headerBytes = 36u + static_cast<std::size_t>(spec.components) * 6u;
    const std::size_t total = headerBytes + pixels.size() + trailingSlack;
    std::vector<uint8_t> b(total, 0);

    std::memcpy(b.data(), iwi::kMagic, 4);
    auto* hdr = reinterpret_cast<IwiFileHeader*>(b.data());
    hdr->width = spec.width;
    hdr->height = spec.height;
    hdr->metaBytes = 0;
    hdr->storageLayout = IW_STORAGE_INTERLEAVED;
    std::memcpy(hdr->payloadEncoding, "raw\0\0\0\0\0", 8);
    hdr->colorSpace = static_cast<ImageColorSpace>(spec.colorSpace);
    hdr->reserved = 0;
    hdr->componentCount = spec.components;

    for (uint16_t i = 0; i < spec.components; ++i) {
        hdr->components[i].semantic = static_cast<IWComponentSemantic>(i + 1);
        hdr->components[i].componentClass = IW_COMPONENT_CLASS_UNORM;
        hdr->components[i].bitOffset = static_cast<uint16_t>(i * 8u);
        hdr->components[i].bitWidth = 8;
    }

    std::memcpy(b.data() + headerBytes,
                pixels.data(), pixels.size());
    return b;
}

/* The four-byte pixel writes below keep the samples easy to read. */
std::vector<uint8_t> Pixels3(uint32_t w, uint32_t h,
                             uint8_t (*sample)(uint32_t x, uint32_t y, int c))
{
    std::vector<uint8_t> p(static_cast<std::size_t>(w) * h * 3u);
    for (uint32_t y = 0; y < h; ++y) {
        for (uint32_t x = 0; x < w; ++x) {
            for (int c = 0; c < 3; ++c) {
                p[(static_cast<std::size_t>(y) * w + x) * 3u +
                  static_cast<std::size_t>(c)] = sample(x, y, c);
            }
        }
    }
    return p;
}

std::vector<uint8_t> Pixels4(uint32_t w, uint32_t h,
                             uint8_t (*sample)(uint32_t x, uint32_t y, int c))
{
    std::vector<uint8_t> p(static_cast<std::size_t>(w) * h * 4u);
    for (uint32_t y = 0; y < h; ++y) {
        for (uint32_t x = 0; x < w; ++x) {
            for (int c = 0; c < 4; ++c) {
                p[(static_cast<std::size_t>(y) * w + x) * 4u +
                  static_cast<std::size_t>(c)] = sample(x, y, c);
            }
        }
    }
    return p;
}

class MemorySource final : public iwi::SampleSource {
public:
    explicit MemorySource(std::vector<uint8_t> bytes) : bytes_(std::move(bytes)) {}

    uint64_t Size() const override { return bytes_.size(); }

    bool ReadAt(uint64_t offset, void* dst, std::size_t bytes) override
    {
        if (offset > bytes_.size() || bytes > bytes_.size() - offset)
            return false;
        std::memcpy(dst, bytes_.data() + offset, bytes);
        return true;
    }

private:
    std::vector<uint8_t> bytes_;
};

/*
 * A source backed by a real file, which is what the shell's IStream ultimately
 * reads from.  Positional reads are deliberate: it exercises the same access
 * pattern as StreamSource in the provider (seek per row) rather than slurping
 * the file, so a row-addressing bug shows up here too.
 */
class FileSource final : public iwi::SampleSource {
public:
    explicit FileSource(const std::filesystem::path& path)
        : file_(path, std::ios::binary)
    {
        if (!file_)
            return;
        file_.seekg(0, std::ios::end);
        const auto end = file_.tellg();
        if (end >= 0)
            size_ = static_cast<uint64_t>(end);
    }

    bool ok() const { return file_.is_open() && size_ > 0; }

    uint64_t Size() const override { return size_; }

    bool ReadAt(uint64_t offset, void* dst, std::size_t bytes) override
    {
        if (offset > size_ || bytes > size_ - offset) return false;
        file_.clear();
        file_.seekg(static_cast<std::streamoff>(offset), std::ios::beg);
        if (!file_) return false;
        file_.read(static_cast<char*>(dst), static_cast<std::streamsize>(bytes));
        return static_cast<std::size_t>(file_.gcount()) == bytes;
    }

private:
    std::ifstream file_;
    uint64_t size_ = 0;
};

/* One 2x2 block white, one black, two solid colour blocks: the layout that
 * makes both exact averaging and linear-space averaging visible in four
 * output pixels.  See the "averages in linear light" case below. */
uint8_t BlockSample(uint32_t x, uint32_t y, int c)
{
    if (x < 2 && y < 2)
        return ((x + y) % 2 == 0) ? 0 : 255;          /* black / white chess */
    if (x >= 2 && y < 2)
        return static_cast<uint8_t>(10 + 10 * c);     /* 10, 20, 30           */
    if (x < 2 && y >= 2)
        return static_cast<uint8_t>(200 - 50 * c);    /* 200, 150, 100        */
    return 0;
}

const uint8_t* PixelAt(const iwi::ThumbnailImage& img,
                       uint32_t x, uint32_t y)
{
    return img.bgra.data() +
           (static_cast<std::size_t>(y) * img.width + x) * 4u;
}

std::string ReadTextFile(const std::filesystem::path& path)
{
    std::ifstream in(path, std::ios::binary);
    if (!in) return {};
    return std::string(std::istreambuf_iterator<char>(in),
                       std::istreambuf_iterator<char>());
}

std::vector<uint8_t> ReadFileBytes(const std::filesystem::path& path)
{
    std::ifstream in(path, std::ios::binary);
    if (!in) return {};
    return std::vector<uint8_t>(std::istreambuf_iterator<char>(in),
                                std::istreambuf_iterator<char>());
}

bool WriteFileBytes(const std::filesystem::path& path,
                    const std::vector<uint8_t>& bytes)
{
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    if (!out) return false;
    out.write(reinterpret_cast<const char*>(bytes.data()),
              static_cast<std::streamsize>(bytes.size()));
    return static_cast<bool>(out);
}

bool Contains(const std::string& haystack, const std::string& needle)
{
    return haystack.find(needle) != std::string::npos;
}

/* The committed fixtures.  They are the same files the plugin ABI suite
 * decodes, so a drift between the two readers shows up in both suites. */
std::filesystem::path Fixture(const char* name)
{
    const std::filesystem::path root = iwtest::RepoRoot();
    if (root.empty()) return {};
    return root / "src" / "tests" / "abi" / "fixtures" / name;
}

} // namespace

TEST_SUITE("IWI thumbnail: colour transfer")
{
    TEST_CASE("sRGB byte -> linear -> byte is the identity on all 256 values")
    {
        /* This is what lets a 1:1 thumbnail reproduce its source exactly; if it
         * ever fails, small IWIs would come out silently shifted. */
        for (unsigned v = 0; v < 256u; ++v) {
            const uint8_t back =
                iwi::LinearToSrgbByte(iwi::SrgbByteToLinear(static_cast<uint8_t>(v)));
            INFO("value " << v);
            CHECK(static_cast<unsigned>(back) == v);
        }
    }

    TEST_CASE("The transfer function is monotonic and pinned at both ends")
    {
        CHECK(iwi::LinearToSrgbByte(0.0) == 0);
        CHECK(iwi::LinearToSrgbByte(1.0) == 255);
        CHECK(iwi::SrgbByteToLinear(0) == doctest::Approx(0.0));
        CHECK(iwi::SrgbByteToLinear(255) == doctest::Approx(1.0));

        for (unsigned v = 1; v < 256u; ++v) {
            CHECK(iwi::LinearToSrgbByte(iwi::SrgbByteToLinear(
                      static_cast<uint8_t>(v - 1))) <=
                  iwi::LinearToSrgbByte(iwi::SrgbByteToLinear(
                      static_cast<uint8_t>(v))));
        }
    }
}

TEST_SUITE("IWI thumbnail: geometry")
{
    TEST_CASE("A source that already fits is not upscaled")
    {
        std::vector<uint8_t> bytes = MakeIwi(
            Spec{ 4, 4, 3 }, Pixels3(4, 4, BlockSample));
        MemorySource source(std::move(bytes));

        iwi::ThumbnailImage image;
        REQUIRE(iwi::RenderThumbnail(source, 256, image, nullptr) ==
                iwi::ThumbnailStatus::Ok);
        CHECK(image.width  == 4u);
        CHECK(image.height == 4u);
    }

    TEST_CASE("Wide, tall and square sources keep their aspect ratio")
    {
        struct Case {
            uint32_t width, height, maxEdge, expectWidth, expectHeight;
        };
        const Case cases[] = {
            { 64, 32, 16, 16,  8 },
            { 32, 64, 16,  8, 16 },
            { 40, 40, 16, 16, 16 },
            { 100, 3, 10, 10,  1 },   /* a 1-pixel floor, never 0 */
        };

        for (const Case& c : cases) {
            auto make = [&c] {
                return MakeIwi(Spec{ c.width, c.height, 3 },
                               Pixels3(c.width, c.height,
                                       [](uint32_t, uint32_t, int) {
                                           return static_cast<uint8_t>(128);
                                       }));
            };
            MemorySource source(make());

            iwi::ThumbnailImage image;
            INFO(c.width << "x" << c.height << " at " << c.maxEdge);
            REQUIRE(iwi::RenderThumbnail(source, c.maxEdge, image, nullptr) ==
                    iwi::ThumbnailStatus::Ok);
            CHECK(image.width  == c.expectWidth);
            CHECK(image.height == c.expectHeight);
        }
    }

    TEST_CASE("The requested edge is clamped to a sane maximum")
    {
        MemorySource source(MakeIwi(Spec{ 8, 8, 3 },
                                    Pixels3(8, 8, [](uint32_t, uint32_t, int) {
                                        return static_cast<uint8_t>(64);
                                    })));
        iwi::ThumbnailImage image;
        REQUIRE(iwi::RenderThumbnail(source, 1u << 20, image, nullptr) ==
                iwi::ThumbnailStatus::Ok);
        /* The source is smaller than any clamp, so the clamp is invisible here;
         * what matters is that an absurd request returns rather than trying to
         * allocate. */
        CHECK(image.width  == 8u);
        CHECK(image.height == 8u);
    }
}

TEST_SUITE("IWI thumbnail: pixels")
{
    TEST_CASE("A 1:1 thumbnail reproduces the source bytes, premultiplied")
    {
        /* 200,100,50 with alpha 128: the premultiply must round, and the sRGB
         * round trip must leave the colour channels exact. */
        const auto sample = [](uint32_t, uint32_t, int c) -> uint8_t {
            switch (c) {
            case 0: return 200;
            case 1: return 100;
            case 2: return 50;
            default: return 128;
            }
        };

        MemorySource source(MakeIwi(Spec{ 4, 4, 4 }, Pixels4(4, 4, sample)));
        iwi::ThumbnailImage image;
        REQUIRE(iwi::RenderThumbnail(source, 256, image, nullptr) ==
                iwi::ThumbnailStatus::Ok);
        REQUIRE(image.width == 4u);
        REQUIRE(image.height == 4u);
        CHECK(image.hasAlpha);

        const uint8_t* p = PixelAt(image, 0, 0);
        CHECK(static_cast<unsigned>(p[3]) == 128u);   /* A */
        CHECK(static_cast<unsigned>(p[2]) == 100u);   /* R: 200*128/255 -> 100 */
        CHECK(static_cast<unsigned>(p[1]) == 50u);    /* G: 100*128/255 -> 50  */
        CHECK(static_cast<unsigned>(p[0]) == 25u);    /* B:  50*128/255 -> 25  */
    }

    TEST_CASE("An opaque three-component source stays opaque")
    {
        const auto sample = [](uint32_t, uint32_t, int c) -> uint8_t {
            return static_cast<uint8_t>(10 + 10 * c);
        };
        MemorySource source(MakeIwi(Spec{ 3, 2, 3 }, Pixels3(3, 2, sample)));

        iwi::ThumbnailImage image;
        REQUIRE(iwi::RenderThumbnail(source, 16, image, nullptr) ==
                iwi::ThumbnailStatus::Ok);
        CHECK_FALSE(image.hasAlpha);

        const uint8_t* p = PixelAt(image, 1, 1);
        CHECK(static_cast<unsigned>(p[3]) == 255u);
        CHECK(static_cast<unsigned>(p[2]) == 10u);
        CHECK(static_cast<unsigned>(p[1]) == 20u);
        CHECK(static_cast<unsigned>(p[0]) == 30u);
    }

    TEST_CASE("A 2x2 reduction averages in linear light, not in sRGB bytes")
    {
        /* Black next to white.  Averaging the encoded bytes would give 127;
         * averaging light gives 0.5, which encodes to 188.  A thumbnail that
         * reported 127 here would be visibly too dark on every checkerboard
         * and every fine pattern. */
        MemorySource source(MakeIwi(Spec{ 4, 4, 3 },
                                    Pixels3(4, 4, BlockSample)));

        iwi::ThumbnailImage image;
        REQUIRE(iwi::RenderThumbnail(source, 2, image, nullptr) ==
                iwi::ThumbnailStatus::Ok);
        REQUIRE(image.width  == 2u);
        REQUIRE(image.height == 2u);

        const uint8_t* mixed = PixelAt(image, 0, 0);
        for (int c = 0; c < 3; ++c) {
            INFO("channel " << c);
            CHECK(std::abs(static_cast<int>(mixed[c]) - 188) <= 1);
        }

        /* The solid blocks average to themselves, exactly. */
        const uint8_t* colour = PixelAt(image, 1, 0);
        CHECK(static_cast<unsigned>(colour[2]) == 10u);
        CHECK(static_cast<unsigned>(colour[1]) == 20u);
        CHECK(static_cast<unsigned>(colour[0]) == 30u);

        const uint8_t* warm = PixelAt(image, 0, 1);
        CHECK(static_cast<unsigned>(warm[2]) == 200u);
        CHECK(static_cast<unsigned>(warm[1]) == 150u);
        CHECK(static_cast<unsigned>(warm[0]) == 100u);
    }

    TEST_CASE("Alpha is averaged then premultiplied")
    {
        const auto sample = [](uint32_t x, uint32_t, int c) -> uint8_t {
            if (c == 3) return static_cast<uint8_t>(x == 0 ? 0 : 255);
            return 255;
        };
        MemorySource source(MakeIwi(Spec{ 2, 1, 4 }, Pixels4(2, 1, sample)));

        iwi::ThumbnailImage image;
        REQUIRE(iwi::RenderThumbnail(source, 1, image, nullptr) ==
                iwi::ThumbnailStatus::Ok);
        REQUIRE(image.width == 1u);
        CHECK(image.hasAlpha);

        const uint8_t* p = PixelAt(image, 0, 0);
        /* Half-opaque on average, so the premultiplied colour is about half. */
        CHECK(std::abs(static_cast<int>(p[3]) - 188) <= 1);
        for (int c = 0; c < 3; ++c)
            CHECK(std::abs(static_cast<int>(p[c]) - p[3]) <= 2);
    }

    TEST_CASE("A linear-light source is averaged without a gamma round trip")
    {
        /* 0 and 255 are 0.0 and 1.0 in both spaces, so the average is the same
         * 188 as the sRGB case; what this pins is that the linear path does not
         * decode an already-linear file. */
        MemorySource source(MakeIwi(Spec{ 2, 1, 3, IMAGE_COLOR_SPACE_LINEAR },
                                    Pixels3(2, 1,
                                            [](uint32_t x, uint32_t, int) {
                                                return static_cast<uint8_t>(
                                                    x == 0 ? 0 : 255);
                                            })));
        iwi::ThumbnailImage image;
        REQUIRE(iwi::RenderThumbnail(source, 1, image, nullptr) ==
                iwi::ThumbnailStatus::Ok);
        CHECK(std::abs(static_cast<int>(PixelAt(image, 0, 0)[2]) - 188) <= 1);
    }

    TEST_CASE("Trailing slack beyond the declared pixels is ignored")
    {
        MemorySource source(MakeIwi(Spec{ 2, 2, 3 },
                                    Pixels3(2, 2, [](uint32_t, uint32_t, int) {
                                        return static_cast<uint8_t>(77);
                                    }),
                                    16));
        iwi::ThumbnailImage image;
        REQUIRE(iwi::RenderThumbnail(source, 8, image, nullptr) ==
                iwi::ThumbnailStatus::Ok);
        CHECK(static_cast<unsigned>(PixelAt(image, 0, 0)[2]) == 77u);
    }
}

TEST_SUITE("IWI thumbnail: refusals")
{
    TEST_CASE("A non-IWI buffer is refused")
    {
        MemorySource source(std::vector<uint8_t>(200, 0x41));
        iwi::ThumbnailImage image;
        std::string reason;
        CHECK(iwi::RenderThumbnail(source, 64, image, &reason) ==
              iwi::ThumbnailStatus::Malformed);
        CHECK(Contains(reason, "magic"));
        CHECK(image.bgra.empty());
    }

    TEST_CASE("A buffer shorter than the header is refused")
    {
        MemorySource source(std::vector<uint8_t>(10, 0));
        iwi::ThumbnailImage image;
        std::string reason;
        CHECK(iwi::RenderThumbnail(source, 64, image, &reason) ==
              iwi::ThumbnailStatus::Malformed);
        CHECK(Contains(reason, "header"));
    }

    TEST_CASE("Planar storage is refused, not reinterpreted")
    {
        std::vector<uint8_t> bytes = MakeIwi(Spec{ 2, 2, 3 },
                                             Pixels3(2, 2, [](uint32_t, uint32_t, int) {
                                                 return static_cast<uint8_t>(1);
                                             }));
        reinterpret_cast<IwiFileHeader*>(bytes.data())->storageLayout = IW_STORAGE_PLANAR;

        MemorySource source(std::move(bytes));
        iwi::ThumbnailImage image;
        std::string reason;
        CHECK(iwi::RenderThumbnail(source, 64, image, &reason) ==
              iwi::ThumbnailStatus::Malformed);
        CHECK(Contains(reason, "planar"));
    }

    TEST_CASE("Two components are refused")
    {
        std::vector<uint8_t> pixels(2u * 2u * 2u, 0x10);
        std::vector<uint8_t> bytes = MakeIwi(Spec{ 2, 2, 2 }, pixels);

        MemorySource source(std::move(bytes));
        iwi::ThumbnailImage image;
        CHECK(iwi::RenderThumbnail(source, 64, image, nullptr) ==
              iwi::ThumbnailStatus::Malformed);
    }

    TEST_CASE("A truncated payload is an I/O failure, not a crash")
    {
        std::vector<uint8_t> bytes = MakeIwi(Spec{ 4, 4, 3 },
                                             Pixels3(4, 4, [](uint32_t, uint32_t, int) {
                                                 return static_cast<uint8_t>(9);
                                             }));
        bytes.resize(bytes.size() - 1);

        MemorySource source(std::move(bytes));
        iwi::ThumbnailImage image;
        std::string reason;
        /* The header declares more payload than the source holds, so the parse
         * fails first — either status is a refusal, and neither may read past
         * the buffer. */
        const iwi::ThumbnailStatus status =
            iwi::RenderThumbnail(source, 64, image, &reason);
        CHECK((status == iwi::ThumbnailStatus::Malformed ||
               status == iwi::ThumbnailStatus::IoError));
    }

    TEST_CASE("An HDR transfer function is refused rather than guessed")
    {
        MemorySource source(MakeIwi(Spec{ 2, 2, 3, IMAGE_COLOR_SPACE_HDR_PQ },
                                    Pixels3(2, 2, [](uint32_t, uint32_t, int) {
                                        return static_cast<uint8_t>(128);
                                    })));
        iwi::ThumbnailImage image;
        std::string reason;
        CHECK(iwi::RenderThumbnail(source, 64, image, &reason) ==
              iwi::ThumbnailStatus::Unsupported);
        CHECK(Contains(reason, "HDR"));
    }

    TEST_CASE("An implausibly large texture is refused by this provider")
    {
        /* A legal container whose width is past kMaxSourceEdge: the bytes are
         * all present, so ParseHeader accepts it, and the refusal comes from
         * this provider's own cap.  The viewer opens such a file; Explorer
         * falls back to the file-type icon. */
        Spec spec{ iwi::kMaxSourceEdge + 1u, 1, 3 };
        std::vector<uint8_t> bytes = MakeIwi(
            spec, Pixels3(spec.width, 1u,
                          [](uint32_t, uint32_t, int) -> uint8_t {
                              return static_cast<uint8_t>(48);
                          }));

        /* The header itself is valid, which is what makes this a test of the
         * provider's limit rather than of the parser. */
        REQUIRE(iwi::ParseHeader(bytes.data(), bytes.size()).ok());

        MemorySource source(std::move(bytes));
        iwi::ThumbnailImage image;
        std::string reason;
        CHECK(iwi::RenderThumbnail(source, 64, image, &reason) ==
              iwi::ThumbnailStatus::Unsupported);
        CHECK(Contains(reason, "limit"));
    }
}

TEST_SUITE("IWI thumbnail: format agreement with the viewer")
{
    /* The provider and ImageLoaderIWI share IwiFormat.hpp, and the ABI suite
     * (test_imageloader_iwi.cpp) asserts the loader's own refusals.  These
     * cases pin the same decisions from the shared parser directly, so a
     * divergence shows up even in a build with no plugins. */

    TEST_CASE("The accepted shape of the shipped fixtures parses")
    {
        const auto rgba = MakeIwi(Spec{ 4, 4, 4 },
                                  Pixels4(4, 4, [](uint32_t, uint32_t, int) {
                                      return static_cast<uint8_t>(1);
                                  }));
        const iwi::IwiParseResult parsed =
            iwi::ParseHeader(rgba.data(), rgba.size());
        REQUIRE(parsed.ok());
        CHECK(parsed.header.width  == 4u);
        CHECK(parsed.header.height == 4u);
        CHECK(parsed.header.components == 4u);
        CHECK(parsed.header.packedBytes == 64u);
    }

    TEST_CASE("Every refusal carries a non-empty reason")
    {
        std::vector<uint8_t> shortBuffer(4, 0);
        const iwi::IwiParseResult parsed =
            iwi::ParseHeader(shortBuffer.data(), shortBuffer.size());
        CHECK_FALSE(parsed.ok());
        CHECK(std::strlen(parsed.reason) > 0);
    }

    TEST_CASE("A null buffer is refused instead of dereferenced")
    {
        const iwi::IwiParseResult parsed = iwi::ParseHeader(nullptr, 0);
        CHECK_FALSE(parsed.ok());
    }
}

TEST_SUITE("IWI thumbnail: real files on disk")
{
    /*
     * The fixtures committed at src/tests/abi/fixtures are real .iwi files, not
     * bytes built by this test.  The plugin ABI suite decodes the same two
     * files, so these cases and those pin the same pixel values from both
     * readers: if ImageLoaderIWI and the thumbnail provider ever disagree about
     * a file, one of the two suites fails.
     *
     * Their sample values (generate_format_fixtures.c):
     *   R = 16 + 16x, G = 32 + 16y, B = 200 - 16(x + y), A = 240 + x
     *
     * A 4x4 source is not scaled, and the sRGB round trip is exact, so the only
     * transformation left is the premultiply.
     */

    TEST_CASE("The committed rgba_4x4.iwi decodes from its file")
    {
        const std::filesystem::path path = Fixture("rgba_4x4.iwi");
        REQUIRE_FALSE(path.empty());
        REQUIRE(std::filesystem::exists(path));

        FileSource source(path);
        REQUIRE(source.ok());
        CHECK(source.Size() == 124u);   /* 60-byte header + 4x4x4 */

        iwi::ThumbnailImage image;
        std::string reason;
        REQUIRE(iwi::RenderThumbnail(source, 256, image, &reason) ==
                iwi::ThumbnailStatus::Ok);
        CHECK(image.width  == 4u);
        CHECK(image.height == 4u);
        CHECK(image.hasAlpha);

        /* (0,0): R=16 G=32 B=200 A=240. */
        const uint8_t* p00 = PixelAt(image, 0, 0);
        CHECK(static_cast<unsigned>(p00[0]) == 188u);   /* B: 200*240/255 */
        CHECK(static_cast<unsigned>(p00[1]) == 30u);    /* G:  32*240/255 */
        CHECK(static_cast<unsigned>(p00[2]) == 15u);    /* R:  16*240/255 */
        CHECK(static_cast<unsigned>(p00[3]) == 240u);

        /* (3,2): R=64 G=64 B=120 A=243. */
        const uint8_t* p32 = PixelAt(image, 3, 2);
        CHECK(static_cast<unsigned>(p32[0]) == 114u);
        CHECK(static_cast<unsigned>(p32[1]) == 61u);
        CHECK(static_cast<unsigned>(p32[2]) == 61u);
        CHECK(static_cast<unsigned>(p32[3]) == 243u);
    }

    TEST_CASE("The committed rgb_4x4.iwi decodes from its file and stays opaque")
    {
        const std::filesystem::path path = Fixture("rgb_4x4.iwi");
        REQUIRE_FALSE(path.empty());
        REQUIRE(std::filesystem::exists(path));

        FileSource source(path);
        REQUIRE(source.ok());
        CHECK(source.Size() == 102u);   /* 54-byte header + 4x4x3 */

        iwi::ThumbnailImage image;
        REQUIRE(iwi::RenderThumbnail(source, 256, image, nullptr) ==
                iwi::ThumbnailStatus::Ok);
        CHECK(image.width  == 4u);
        CHECK(image.height == 4u);
        CHECK_FALSE(image.hasAlpha);

        /* (0,0): R=16 G=32 B=200, no fourth component to premultiply by. */
        const uint8_t* p00 = PixelAt(image, 0, 0);
        CHECK(static_cast<unsigned>(p00[0]) == 200u);
        CHECK(static_cast<unsigned>(p00[1]) == 32u);
        CHECK(static_cast<unsigned>(p00[2]) == 16u);
        CHECK(static_cast<unsigned>(p00[3]) == 255u);

        /* (3,3): R=64 G=80 B=104. */
        const uint8_t* p33 = PixelAt(image, 3, 3);
        CHECK(static_cast<unsigned>(p33[0]) == 104u);
        CHECK(static_cast<unsigned>(p33[1]) == 80u);
        CHECK(static_cast<unsigned>(p33[2]) == 64u);
    }

    TEST_CASE("The file source and the memory source agree byte for byte")
    {
        /* The same claim the ABI suite makes about the loader: reading a real
         * file and decoding the same bytes in memory must not produce
         * different pixels.  Anything else means the row addressing depends on
         * how the bytes arrived. */
        for (const char* name : { "rgba_4x4.iwi", "rgb_4x4.iwi" }) {
            const std::filesystem::path path = Fixture(name);
            REQUIRE_FALSE(path.empty());

            FileSource fileSource(path);
            REQUIRE(fileSource.ok());

            MemorySource memorySource(ReadFileBytes(path));

            iwi::ThumbnailImage fromFile;
            iwi::ThumbnailImage fromMemory;
            INFO(name);
            REQUIRE(iwi::RenderThumbnail(fileSource, 256, fromFile, nullptr) ==
                    iwi::ThumbnailStatus::Ok);
            REQUIRE(iwi::RenderThumbnail(memorySource, 256, fromMemory, nullptr) ==
                    iwi::ThumbnailStatus::Ok);

            CHECK(fromFile.width  == fromMemory.width);
            CHECK(fromFile.height == fromMemory.height);
            CHECK(fromFile.hasAlpha == fromMemory.hasAlpha);
            CHECK(fromFile.bgra == fromMemory.bgra);
        }
    }

    TEST_CASE("A truncated copy of a real fixture on disk is refused")
    {
        const std::filesystem::path path = Fixture("rgba_4x4.iwi");
        REQUIRE_FALSE(path.empty());

        std::vector<uint8_t> bytes = ReadFileBytes(path);
        REQUIRE(bytes.size() > sizeof(IwiFileHeader));
        bytes.pop_back();

        const std::filesystem::path broken =
            std::filesystem::temp_directory_path() / "iview_thumb_test_truncated.iwi";
        WriteFileBytes(broken, bytes);

        FileSource source(broken);
        REQUIRE(source.ok());

        iwi::ThumbnailImage image;
        std::string reason;
        /* The header is intact and valid, so the refusal has to come from the
         * payload bounds check rather than from a crash or a silent guess. */
        CHECK(iwi::RenderThumbnail(source, 256, image, &reason) ==
              iwi::ThumbnailStatus::Malformed);
        CHECK(image.bgra.empty());

        std::error_code ec;
        std::filesystem::remove(broken, ec);
    }

    TEST_CASE("A larger real file on disk is downscaled, keeping its colours")
    {
        /* 96x48 is past every small-file path: the reduction is real, and it
         * runs through positional file reads rather than a memory buffer. */
        const uint32_t width = 96, height = 48;
        std::vector<uint8_t> pixels(static_cast<std::size_t>(width) * height * 4u);
        for (uint32_t y = 0; y < height; ++y) {
            for (uint32_t x = 0; x < width; ++x) {
                uint8_t* p = pixels.data() +
                    (static_cast<std::size_t>(y) * width + x) * 4u;
                const bool leftHalf = x < width / 2;
                /* IWI stores R,G,B[,A]; the thumbnail comes back BGRA. */
                p[0] = static_cast<uint8_t>(leftHalf ? 255 : 0);   /* R */
                p[1] = 0;                                          /* G */
                p[2] = static_cast<uint8_t>(leftHalf ? 0 : 255);   /* B */
                p[3] = 255;
            }
        }

        std::vector<uint8_t> bytes = MakeIwi(
            Spec{ width, height, 4 }, pixels);

        const std::filesystem::path path =
            std::filesystem::temp_directory_path() / "iview_thumb_test_large.iwi";
        WriteFileBytes(path, bytes);

        FileSource source(path);
        REQUIRE(source.ok());

        iwi::ThumbnailImage image;
        std::string reason;
        REQUIRE(iwi::RenderThumbnail(source, 24, image, &reason) ==
                iwi::ThumbnailStatus::Ok);
        CHECK(image.width  == 24u);
        CHECK(image.height == 12u);
        CHECK(image.hasAlpha);

        const uint8_t* left  = PixelAt(image, 0, 0);
        const uint8_t* right = PixelAt(image, 23, 11);
        CHECK(static_cast<unsigned>(left[2])  == 255u);
        CHECK(static_cast<unsigned>(left[0])  == 0u);
        CHECK(static_cast<unsigned>(right[0]) == 255u);
        CHECK(static_cast<unsigned>(right[2]) == 0u);
        CHECK(static_cast<unsigned>(left[3])  == 255u);

        std::error_code ec;
        std::filesystem::remove(path, ec);
    }
}

TEST_SUITE("IWI thumbnail: registration identifiers")
{
    TEST_CASE("The compiled CLSID matches its string form")
    {
#ifdef _WIN32
        GUID parsed{};
        const std::wstring text(iwthumb::kProviderClsidString,
                                iwthumb::kProviderClsidString +
                                    std::strlen(iwthumb::kProviderClsidString));
        REQUIRE(SUCCEEDED(CLSIDFromString(text.c_str(), &parsed)));
        CHECK(IsEqualCLSID(parsed, iwthumb::CLSID_IwiThumbnailProvider));
#else
        /* The GUID type is Windows-only; the string is still checked against
         * the scripts below. */
        CHECK(std::strlen(iwthumb::kProviderClsidString) == 38u);
#endif
    }

    TEST_CASE("The IID used for QueryInterface is the thumbnail interface GUID")
    {
        /* ThumbnailProviderIds.ixx defines this IID itself because MSYS2's
         * libuuid.a does not carry it.  If the copy ever drifted from the real
         * interface, QueryInterface would return E_NOINTERFACE for the
         * interface the shell actually asks for, and no thumbnail would ever
         * be drawn. */
#ifdef _WIN32
        GUID parsed{};
        const std::wstring text(iwthumb::kThumbnailProviderIidString,
                                iwthumb::kThumbnailProviderIidString +
                                    std::strlen(iwthumb::kThumbnailProviderIidString));
        REQUIRE(SUCCEEDED(CLSIDFromString(text.c_str(), &parsed)));
        CHECK(IsEqualIID(parsed, iwthumb::kIID_IThumbnailProvider));

#  if defined(_MSC_VER)
        /* Compare against the SDK's own symbol where it links.  MSYS2's
         * libuuid.a has no IID_IThumbnailProvider — that gap is the reason the
         * constant exists — so this stronger check can only run on MSVC. */
        CHECK(IsEqualIID(iwthumb::kIID_IThumbnailProvider, IID_IThumbnailProvider));
#  endif
#else
        CHECK(std::strlen(iwthumb::kThumbnailProviderIidString) == 38u);
#endif

        /* The shell registers thumbnail handlers under this very GUID, so the
         * two constants are the same value by definition.  Asserting it keeps
         * a future edit from "fixing" one of them. */
        CHECK(std::string(iwthumb::kThumbnailProviderIidString) ==
              std::string(iwthumb::kThumbnailShellExGuidString));
    }

    TEST_CASE("Both install scripts carry the same CLSID, ShellEx GUID and DLL")
    {
        const std::filesystem::path root = iwtest::RepoRoot();
        REQUIRE_FALSE(root.empty());

        const std::filesystem::path scripts[] = {
            root / "scripts" / "install-desktop-windows.ps1",
            root / "scripts" / "install-desktop-windows.bat",
        };

        for (const auto& script : scripts) {
            INFO(script.string());
            const std::string text = ReadTextFile(script);
            REQUIRE_FALSE(text.empty());

            /* Case-sensitive on purpose: these are literal key names, and the
             * point is identical text, so a reader can see it is the same
             * identifier the DLL was built with. */
            CHECK(Contains(text, iwthumb::kProviderClsidString));
            CHECK(Contains(text, iwthumb::kThumbnailShellExGuidString));
            CHECK(Contains(text, iwthumb::kProviderDllName));
        }
    }
}
