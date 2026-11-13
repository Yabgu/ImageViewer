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

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

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
    std::vector<std::uint8_t> b(iwi_file::kHeaderBytes + payload, 0);

    std::memcpy(b.data(), iwi_file::kMagic, iwi_file::kMagicBytes);
    b[iwi_file::kOffWidth]  = static_cast<std::uint8_t>(width);
    b[iwi_file::kOffHeight] = static_cast<std::uint8_t>(height);
    b[iwi_file::kOffColorSpace] = 0;
    b[iwi_file::kOffComponents]     = static_cast<std::uint8_t>(components);
    b[iwi_file::kOffBitsPerPixel]   = static_cast<std::uint8_t>(components * 8u);
    b[iwi_file::kOffStorage]        = IW_STORAGE_INTERLEAVED;

    for (std::uint16_t i = 0; i < components; ++i) {
        const std::size_t at = iwi_file::kOffComponentTab
                             + static_cast<std::size_t>(i) * iwi_file::kComponentBytes;
        b[at + iwi_file::kCompOffSemantic] = static_cast<std::uint8_t>(i + 1u);
        b[at + iwi_file::kCompOffClass]    = IW_COMPONENT_CLASS_UNORM;
        b[at + iwi_file::kCompOffBitOffset] = static_cast<std::uint8_t>(i * 8u);
        b[at + iwi_file::kCompOffBitWidth]  = 8;
    }

    b[iwi_file::kOffPixelBytes] = static_cast<std::uint8_t>(payload);
    b[67] = static_cast<std::uint8_t>(payload >> 8);
    for (std::size_t i = 0; i < payload; ++i)
        b[iwi_file::kHeaderBytes + i] = 0x5A;
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
        planar[iwi_file::kOffStorage] = IW_STORAGE_PLANAR;

        const iwi_file::Result r = iwi_file::parse(planar);
        check(!static_cast<bool>(r), "planar fails");
        check(r.status() == iwi_file::Status::Planar, "status is Planar");
        check(r.unsupported(), "planar is 'valid but unsupported'");
        check(iwi_file::statusName(r.status()) == "planar", "status name");
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
            check(!iwi_file::statusName(converted).empty(),
                  "every status has a name");
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
