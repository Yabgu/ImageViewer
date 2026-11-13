#pragma once

/*
 * IwiFile.hpp — the C++ face of the .iwi container API.
 *
 * This is a thin, header-only wrapper over IwiFile.h.  It adds no parsing: it
 * calls iwi_file_parse_header and reshapes the result into something a C++
 * caller can use without naming a C error code.  There is exactly one
 * implementation of the format rules, and it is the C one.
 *
 * # Why a wrapper instead of "just cast the header"
 *
 * Casting `const uint8_t*` to a packed header struct looks simpler and is
 * wrong in three ways that were measured, not assumed:
 *
 *   1. Two of the fields sit on odd offsets.  The table is
 *          magic@0  width@4  height@8  colorSpace@12
 *          componentCount@13  bitsPerPixel@15  storage@17
 *      so a struct read of componentCount is an *unaligned* 16-bit load.
 *      On x86-64 it is one `movzwl 13(%rcx)`.  On aarch64 the compiler has to
 *      emit `ldurh` — the unaligned-halform load — because it knows from the
 *      packing attribute that the access is unaligned.
 *
 *   2. The packing attribute is load-bearing and silent when missing.  Without
 *      it the same struct puts componentCount at 14 and has sizeof 80 instead
 *      of 13 and 74.  Nothing warns; every field after colorSpace shifts.
 *
 *   3. In C++ a reinterpret_cast from a byte buffer to a struct is a strict
 *      aliasing violation, and the cast also inherits *host* endianness.  The
 *      file is little-endian by definition, so a cast reader is correct only
 *      on little-endian hosts — it happens to work rather than being right.
 *
 * The explicit byte reads in IwiFile.c have none of those properties, and they
 * cost nothing: clang lowers `iwi_rd32` to a single `movl (%rcx), %eax` at
 * -O2, identical to the cast.  Precision here is free.
 *
 * Note that "the file is little-endian" is the format's only byte-order rule:
 * big-endian hosts are out of scope by design, not by accident.  The reasoning
 * is in IwiFile.h; the short version is that byte reads make this wrapper
 * *report* the same values on any host, but nothing here swaps bytes for a
 * big-endian machine, and nothing promises to.
 *
 * # Why there is still a parse() and not just a read()
 *
 * Reading is the caller's job, and it has to stay that way.  The Windows shell
 * thumbnail provider deliberately never reads a whole file: it reads the
 * 74-byte header, works out which source rows the output grid samples, and
 * reads only those.  A combined "open and read into a header" call cannot
 * serve that, and it would also fuse two failure modes — I/O error and
 * malformed file — which callers need to tell apart.
 *
 * C++17, no allocation, no exceptions.  A malformed file is a status value.
 */

#include "IwiFile.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <string_view>
#include <type_traits>

namespace iwi_file {

/* Mirrors IwiFileStatus, with the same values so the two convert for free. */
enum class Status {
    Ok                        = IWI_FILE_OK,
    NoInput                   = IWI_FILE_ERR_NO_INPUT,
    TruncatedHeader           = IWI_FILE_ERR_TRUNCATED_HEADER,
    BadMagic                  = IWI_FILE_ERR_BAD_MAGIC,
    ZeroDimension             = IWI_FILE_ERR_ZERO_DIMENSION,
    Unaddressable             = IWI_FILE_ERR_UNADDRESSABLE,
    TooLarge                  = IWI_FILE_ERR_TOO_LARGE,
    PayloadDeclared           = IWI_FILE_ERR_PAYLOAD_DECLARED,
    PayloadTruncated          = IWI_FILE_ERR_PAYLOAD_TRUNCATED,
    Planar                    = IWI_FILE_ERR_PLANAR,
    ComponentCount            = IWI_FILE_ERR_COMPONENT_COUNT,
    ComponentDepth            = IWI_FILE_ERR_COMPONENT_DEPTH,
    ComponentOrder            = IWI_FILE_ERR_COMPONENT_ORDER,
    ComponentClass            = IWI_FILE_ERR_COMPONENT_CLASS,
    ComponentLayout           = IWI_FILE_ERR_COMPONENT_LAYOUT,
};

/* Valid IWI, but outside the subset this build reads.  The distinction a
 * caller needs in order to fall back rather than report a corrupt file. */
constexpr bool isUnsupportedLayout(Status s) noexcept
{
    return s == Status::Planar
        || s == Status::ComponentCount
        || s == Status::ComponentDepth
        || s == Status::ComponentOrder
        || s == Status::ComponentClass
        || s == Status::ComponentLayout;
}

struct Component {
    std::uint8_t  semantic       = 0;
    std::uint8_t  componentClass = 0;
    std::uint16_t bitOffset      = 0;
    std::uint16_t bitWidth       = 0;
};

struct Header {
    std::uint32_t width          = 0;
    std::uint32_t height         = 0;
    std::uint8_t  colorSpace     = 0;
    std::uint16_t componentCount = 0;
    std::uint16_t bitsPerPixel   = 0;
    std::uint8_t  storageLayout  = 0;
    std::uint64_t pixelBytes     = 0;   /* as declared by the file  */
    std::uint64_t packedBytes    = 0;   /* width * height * comps   */
    Component     components[IWI_FILE_MAX_COMPONENTS]{};
};

/*
 * A parse attempt.
 *
 * `header()` is only meaningful when the result is true; on failure it is
 * zeroed, matching the C contract, so a caller that ignores the status gets
 * zeros rather than a half-filled struct.
 *
 * `message()` is the detailed reason from the C parser — the same wording the
 * plugin ABI tests assert — and is empty on success.
 */
class Result {
public:
    Result() = default;

    [[nodiscard]] explicit operator bool() const noexcept
    {
        return status_ == Status::Ok;
    }

    [[nodiscard]] Status status() const noexcept { return status_; }

    [[nodiscard]] const Header& header() const noexcept { return header_; }

    /* Fixed buffer, no allocation: the reason never outlives the Result and
     * never requires the heap on an error path. */
    [[nodiscard]] std::string_view message() const noexcept
    {
        return std::string_view(reason_.data());
    }

    /* True when the bytes are a valid IWI this build simply cannot read. */
    [[nodiscard]] bool unsupported() const noexcept
    {
        return isUnsupportedLayout(status_);
    }

private:
    friend Result parse(const std::uint8_t*, std::size_t) noexcept;

    Status   status_ = Status::Ok;
    Header   header_{};
    std::array<char, 192> reason_{};
};

/*
 * Parse a container image.
 *
 * `size` must be the whole container, not just the header: the payload bounds
 * check needs the real length, so a caller holding only the first 74 bytes
 * cannot use this to accept a file.
 */
[[nodiscard]] inline Result parse(const std::uint8_t* data,
                                  std::size_t size) noexcept
{
    Result result;

    IwiFileHeader raw{};
    result.status_ = static_cast<Status>(
        iwi_file_parse_header(data, size, &raw, result.reason_.data(),
                              result.reason_.size()));
    if (result.status_ != Status::Ok)
        return result;

    result.header_.width          = raw.width;
    result.header_.height         = raw.height;
    result.header_.colorSpace     = raw.colorSpace;
    result.header_.componentCount = raw.componentCount;
    result.header_.bitsPerPixel   = raw.bitsPerPixel;
    result.header_.storageLayout  = raw.storageLayout;
    result.header_.pixelBytes     = raw.pixelBytes;
    result.header_.packedBytes    = raw.packedBytes;

    for (std::size_t i = 0; i < IWI_FILE_MAX_COMPONENTS; ++i) {
        result.header_.components[i].semantic =
            static_cast<std::uint8_t>(raw.components[i].semantic);
        result.header_.components[i].componentClass =
            static_cast<std::uint8_t>(raw.components[i].componentClass);
        result.header_.components[i].bitOffset = raw.components[i].bitOffset;
        result.header_.components[i].bitWidth  = raw.components[i].bitWidth;
    }
    return result;
}

/* Any contiguous byte container: std::vector<std::uint8_t>, std::array,
 * std::string, std::span (C++20), a custom buffer type.  SFINAE rather than a
 * std::span parameter so this header stays C++17. */
template <class Container,
          class = decltype(std::declval<const Container&>().data()),
          class = decltype(std::declval<const Container&>().size())>
[[nodiscard]] inline Result parse(const Container& bytes) noexcept
{
    return parse(reinterpret_cast<const std::uint8_t*>(bytes.data()),
                 static_cast<std::size_t>(bytes.size()));
}

/* Payload bytes the geometry requires; 0 on overflow.
 *
 * Not constexpr: it calls the C implementation rather than restating the
 * arithmetic. A second copy of the overflow rule in this header is exactly the
 * kind of duplicate this API exists to remove. */
[[nodiscard]] inline std::uint64_t requiredBytes(std::uint32_t width,
                                                 std::uint32_t height,
                                                 std::uint16_t components) noexcept
{
    return iwi_file_required_bytes(width, height, components);
}

/* The magic bytes, for a caller that wants to sniff before parsing. */
[[nodiscard]] constexpr std::string_view magic() noexcept
{
    return std::string_view(kMagic, kMagicBytes);
}

/* Convenience: does this buffer start with the IWI magic?  Cheaper than a
 * parse when all a caller needs is "is this even ours". */
[[nodiscard]] inline bool looksLikeIwi(const std::uint8_t* data,
                                       std::size_t size) noexcept
{
    return data != nullptr && size >= kHeaderBytes
        && std::memcmp(data, kMagic, kMagicBytes) == 0;
}

template <class Container,
          class = decltype(std::declval<const Container&>().data()),
          class = decltype(std::declval<const Container&>().size())>
[[nodiscard]] inline bool looksLikeIwi(const Container& bytes) noexcept
{
    return looksLikeIwi(reinterpret_cast<const std::uint8_t*>(bytes.data()),
                        static_cast<std::size_t>(bytes.size()));
}

} // namespace iwi_file
