/*
 * iwi_file_cpp_test.cpp — the C++ face of the container API.
 *
 * The C core is covered by iwi_file_c_test.c, which is compiled as C on
 * purpose.  This covers the header-only C++ wrapper: that it compiles as
 * C++17, that it adds no parsing of its own, and that its ergonomics do what
 * they claim (bool conversion, message(), unsupported()).
 *
 * It is written as plain C++17 with no modules and no doctest, so it is also a
 * worked example of how an external C++ consumer — an engine, a tool — links
 * one static library and includes one header.
 */

#include "IwiFile.hpp"

#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include <type_traits>

/*
 * The enums restate the C `#define`s.  That claim is cheap to make in a comment
 * and expensive to be wrong about, so it is a compile-time check: a value
 * edited on one side and not the other does not build.
 */
static_assert(std::is_same_v<std::underlying_type_t<iwi_file::Offset>,
                             std::size_t>);
static_assert(std::is_same_v<std::underlying_type_t<iwi_file::Extent>,
                             std::size_t>);
static_assert(std::is_same_v<std::underlying_type_t<iwi_file::ComponentOffset>,
                             std::size_t>);

static_assert(iwi_file::toSize(iwi_file::Offset::Magic) == IWI_FILE_OFF_MAGIC);
static_assert(iwi_file::toSize(iwi_file::Offset::Width) == IWI_FILE_OFF_WIDTH);
static_assert(iwi_file::toSize(iwi_file::Offset::Height) == IWI_FILE_OFF_HEIGHT);
static_assert(iwi_file::toSize(iwi_file::Offset::ColorSpace)
              == IWI_FILE_OFF_COLOR_SPACE);
static_assert(iwi_file::toSize(iwi_file::Offset::ComponentCount)
              == IWI_FILE_OFF_COMPONENTS);
static_assert(iwi_file::toSize(iwi_file::Offset::BitsPerPixel)
              == IWI_FILE_OFF_BITS_PER_PX);
static_assert(iwi_file::toSize(iwi_file::Offset::Storage)
              == IWI_FILE_OFF_STORAGE);
static_assert(iwi_file::toSize(iwi_file::Offset::ComponentTable)
              == IWI_FILE_OFF_COMPONENT_TAB);
static_assert(iwi_file::toSize(iwi_file::Offset::PixelBytes)
              == IWI_FILE_OFF_PIXEL_BYTES);

static_assert(iwi_file::toSize(iwi_file::Extent::Magic)
              == IWI_FILE_MAGIC_BYTES);
static_assert(iwi_file::toSize(iwi_file::Extent::Header)
              == IWI_FILE_HEADER_BYTES);
static_assert(iwi_file::toSize(iwi_file::Extent::Component)
              == IWI_FILE_COMPONENT_BYTES);
static_assert(iwi_file::toSize(iwi_file::Extent::MaxComponents)
              == IWI_FILE_MAX_COMPONENTS);

static_assert(iwi_file::toSize(iwi_file::ComponentOffset::Semantic)
              == IWI_FILE_COMP_OFF_SEMANTIC);
static_assert(iwi_file::toSize(iwi_file::ComponentOffset::Class)
              == IWI_FILE_COMP_OFF_CLASS);
static_assert(iwi_file::toSize(iwi_file::ComponentOffset::BitOffset)
              == IWI_FILE_COMP_OFF_BIT_OFFSET);
static_assert(iwi_file::toSize(iwi_file::ComponentOffset::BitWidth)
              == IWI_FILE_COMP_OFF_BIT_WIDTH);

/* The record layout the enums describe has to be self-consistent: a record
 * starts at the table, and its last field must still end inside the header. */
static_assert(iwi_file::toSize(iwi_file::Offset::ComponentTable)
                  + iwi_file::toSize(iwi_file::Extent::MaxComponents)
                        * iwi_file::toSize(iwi_file::Extent::Component)
              == iwi_file::toSize(iwi_file::Offset::PixelBytes));

/* recordField is the documented spelling of "base plus field". */
static_assert(iwi_file::recordField(
                  iwi_file::toSize(iwi_file::Offset::ComponentTable),
                  iwi_file::ComponentOffset::BitWidth)
              == iwi_file::toSize(iwi_file::Offset::ComponentTable) + 4u);
namespace {

int failures = 0;

void check(bool condition, const char* what, const char* detail = nullptr)
{
    if (condition)
        return;
    std::printf("FAIL: %s", what);
    if (detail != nullptr)
        std::printf("  (%s)", detail);
    std::printf("\n");
    ++failures;
}

/* A 2x2 container with `components` channels, every sample 0x5A. */
std::vector<std::uint8_t> makeIwi(std::uint32_t width,
                                  std::uint32_t height,
                                  std::uint16_t components)
{
    const std::size_t payload =
        static_cast<std::size_t>(width) * height * components;
    std::vector<std::uint8_t> b(
        iwi_file::toSize(iwi_file::Extent::Header) + payload, 0);

    std::memcpy(b.data(), iwi_file::kMagic,
                iwi_file::toSize(iwi_file::Extent::Magic));
    b[iwi_file::toSize(iwi_file::Offset::Width)] =
        static_cast<std::uint8_t>(width);
    b[iwi_file::toSize(iwi_file::Offset::Height)] =
        static_cast<std::uint8_t>(height);
    b[iwi_file::toSize(iwi_file::Offset::ColorSpace)] = 0;
    b[iwi_file::toSize(iwi_file::Offset::ComponentCount)] =
        static_cast<std::uint8_t>(components);
    b[iwi_file::toSize(iwi_file::Offset::BitsPerPixel)] =
        static_cast<std::uint8_t>(components * 8u);
    b[iwi_file::toSize(iwi_file::Offset::Storage)] = IW_STORAGE_INTERLEAVED;

    for (std::uint16_t i = 0; i < components; ++i) {
        const std::size_t rec =
            iwi_file::toSize(iwi_file::Offset::ComponentTable)
            + static_cast<std::size_t>(i)
                  * iwi_file::toSize(iwi_file::Extent::Component);
        b[iwi_file::recordField(rec, iwi_file::ComponentOffset::Semantic)] =
            static_cast<std::uint8_t>(i + 1u);
        b[iwi_file::recordField(rec, iwi_file::ComponentOffset::Class)] =
            IW_COMPONENT_CLASS_UNORM;
        b[iwi_file::recordField(rec, iwi_file::ComponentOffset::BitOffset)] =
            static_cast<std::uint8_t>(i * 8u);
        b[iwi_file::recordField(rec, iwi_file::ComponentOffset::BitWidth)] = 8;
    }

    b[iwi_file::toSize(iwi_file::Offset::PixelBytes)] =
        static_cast<std::uint8_t>(payload);
    b[67] = static_cast<std::uint8_t>(payload >> 8);
    for (std::size_t i = 0; i < payload; ++i)
        b[iwi_file::toSize(iwi_file::Extent::Header) + i] = 0x5A;
    return b;
}

} // namespace

int main()
{
    /* --- Container overload works for vector<uint8_t> ------------------ */
    {
        const std::vector<std::uint8_t> bytes = makeIwi(2, 2, 4);
        const iwi_file::Result r = iwi_file::parse(bytes);

        check(static_cast<bool>(r), "valid RGBA parses");
        check(r.status() == iwi_file::Status::Ok, "status is Ok");
        check(r.message().empty(), "no message on success");
        check(r.header().width == 2 && r.header().height == 2, "geometry");
        check(r.header().componentCount == 4, "component count");
        check(r.header().packedBytes == 16, "packed bytes");
        check(r.header().components[0].semantic == IW_COMPONENT_SEMANTIC_R,
              "semantic R");
        check(!r.unsupported(), "not unsupported");

        check(iwi_file::looksLikeIwi(bytes), "sniff accepts an IWI");
        check(iwi_file::magic() == "IWI1", "magic is published");
    }

    /* --- std::array and raw pointer+size overloads agree ---------------- */
    {
        std::array<std::uint8_t, 128> buf{};
        const std::vector<std::uint8_t> src = makeIwi(2, 2, 3);
        std::memcpy(buf.data(), src.data(), src.size());

        const iwi_file::Result fromArray =
            iwi_file::parse(buf.data(), src.size());
        const iwi_file::Result fromContainer = iwi_file::parse(buf);

        check(static_cast<bool>(fromArray), "raw pointer parses");
        check(static_cast<bool>(fromContainer), "array parses");
        check(fromArray.header().width == fromContainer.header().width,
              "both overloads agree");
        check(fromArray.header().componentCount == 3, "RGB count");
        check(fromArray.header().components[3].semantic == 0,
              "unused slot zeroed");
    }

    /* --- Failure carries a usable message and a zeroed header ---------- */
    {
        std::vector<std::uint8_t> bytes = makeIwi(2, 2, 4);
        bytes[0] = 'P';

        const iwi_file::Result r = iwi_file::parse(bytes);
        check(!static_cast<bool>(r), "bad magic fails");
        check(r.status() == iwi_file::Status::BadMagic, "status is BadMagic");
        check(r.message().find("magic") != std::string::npos,
              "message names the magic");
        check(r.header().width == 0, "header zeroed on failure");
        check(!r.unsupported(), "malformed is not 'unsupported'");
    }

    /* --- The malformed/unsupported split a caller relies on ------------- */
    {
        std::vector<std::uint8_t> planar = makeIwi(2, 2, 4);
        planar[iwi_file::toSize(iwi_file::Offset::Storage)] = IW_STORAGE_PLANAR;

        const iwi_file::Result r = iwi_file::parse(planar);
        check(!static_cast<bool>(r), "planar fails");
        check(r.status() == iwi_file::Status::Planar, "status is Planar");
        check(r.unsupported(), "planar is 'valid but unsupported'");
    }

    /* --- The C++ wrapper does not re-implement the rules ---------------- */
    {
        /* Every status the C core can return must round-trip through the C++
         * enum, or the two would silently drift. */
        const IwiFileStatus all[] = {
            IWI_FILE_OK,
            IWI_FILE_ERR_NO_INPUT,
            IWI_FILE_ERR_TRUNCATED_HEADER,
            IWI_FILE_ERR_BAD_MAGIC,
            IWI_FILE_ERR_ZERO_DIMENSION,
            IWI_FILE_ERR_UNADDRESSABLE,
            IWI_FILE_ERR_TOO_LARGE,
            IWI_FILE_ERR_PAYLOAD_DECLARED,
            IWI_FILE_ERR_PAYLOAD_TRUNCATED,
            IWI_FILE_ERR_PLANAR,
            IWI_FILE_ERR_COMPONENT_COUNT,
            IWI_FILE_ERR_COMPONENT_DEPTH,
            IWI_FILE_ERR_COMPONENT_ORDER,
            IWI_FILE_ERR_COMPONENT_CLASS,
            IWI_FILE_ERR_COMPONENT_LAYOUT,
        };
        for (IwiFileStatus s : all) {
            const iwi_file::Status converted = static_cast<iwi_file::Status>(s);
            check(static_cast<IwiFileStatus>(converted) == s,
                  "status value round-trips");
        }
    }

    /* --- Helpers -------------------------------------------------------- */
    {
        check(iwi_file::requiredBytes(4, 4, 4) == 64, "required bytes");
        check(iwi_file::requiredBytes(0xFFFFFFFFu, 0xFFFFFFFFu, 8) == 0,
              "overflow returns 0");
        check(!iwi_file::looksLikeIwi(nullptr, 0), "null sniffs false");
        check(iwi_file::parse(static_cast<const std::uint8_t*>(nullptr), 0)
                  .status() == iwi_file::Status::NoInput,
              "null input status");
    }

    /* --- Structured binding: the result reads like a value -------------- */
    {
        const std::vector<std::uint8_t> bytes = makeIwi(1, 1, 3);
        const iwi_file::Result r = iwi_file::parse(bytes);
        const iwi_file::Header& h = r.header();
        check(h.width == 1 && h.height == 1, "1x1 geometry");
        check(h.bitsPerPixel == 24, "24 bpp");
    }

    if (failures != 0) {
        std::printf("%d check(s) failed\n", failures);
        return 1;
    }
    std::printf("iwi_file_cpp_test: all checks passed\n");
    return 0;
}
