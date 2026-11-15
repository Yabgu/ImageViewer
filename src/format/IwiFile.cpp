/*
 * IwiFile.cpp - the one implementation of the .iwi container rules.
 *
 * This file is C++ because the rest of the project is, and because it is
 * easier to write safely (RAII-free, but with static_asserts and typed locals).
 * What it exports is C: every function below is declared in IwiFile.h inside
 * an `extern "C"` block, and nothing C++-specific appears in a signature.
 *
 * # One parser
 *
 * Everything that needs to validate an .iwi file calls iwi_parse.  The viewer's
 * loader, the shell thumbnail provider, and the format module
 * (src/imageviewer/IwiFormat.ixx, which is now a thin C++ skin over this ABI)
 * all go through it.  IWImageIO::DeserializeImage used to carry its own copy of
 * these rules and drifted from them; that copy is gone.
 *
 * # No reinterpret_cast on a packed struct
 *
 * The on-disk header is little-endian and tightly packed.  Reading it through
 * `reinterpret_cast<const IwiFileHeader*>` would alias a byte buffer as a
 * struct, which is undefined behaviour in C++ and inherits host byte order.
 * Instead the 36 base bytes are copied with memcpy into an aligned local
 * IwiFileHeader, and the fields are read from that local.  memcpy is the
 * standard's own escape hatch for exactly this, and IwiFileHeader's natural
 * layout already matches the wire format (asserted in test_abi_constants.cpp),
 * so the copy lands the fields where the format says they are.
 *
 * Byte order: the local read assumes a little-endian host.  That is the
 * documented scope of this format.
 */

#include "IwiFile.h"

#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <limits>

namespace {

/* Bounded little-endian read of the 2-byte component fields.  Used when
 * writing the component table back into the caller's blob, so the blob is a
 * byte-exact image of what the format defines regardless of host padding. */
inline void WriteU16LE(uint8_t* p, uint16_t v)
{
    p[0] = static_cast<uint8_t>(v & 0xFFu);
    p[1] = static_cast<uint8_t>((v >> 8u) & 0xFFu);
}

inline uint16_t ReadU16LE(const uint8_t* p)
{
    return static_cast<uint16_t>(
        static_cast<uint16_t>(p[0]) |
        static_cast<uint16_t>(static_cast<uint16_t>(p[1]) << 8u));
}

inline uint32_t ReadU32LE(const uint8_t* p)
{
    return static_cast<uint32_t>(p[0]) |
           (static_cast<uint32_t>(p[1]) << 8u) |
           (static_cast<uint32_t>(p[2]) << 16u) |
           (static_cast<uint32_t>(p[3]) << 24u);
}

inline uint64_t ReadU64LE(const uint8_t* p)
{
    uint64_t v = 0;
    for (unsigned i = 0; i < 8u; ++i) {
        v |= static_cast<uint64_t>(p[i]) << (8u * i);
    }
    return v;
}

/*
 * Is the payload encoding one this build understands?
 *
 * Compares the whole 8-byte field rather than a prefix.  The old reader used
 * memcmp(..., "raw", 3), which accepted "rawgarbage" as raw - a codec name
 * that is not "raw" would have been silently treated as one.
 */
bool EncodingIsRaw(const char (&field)[8])
{
    /* All-zero is the historical default and means raw. */
    bool allZero = true;
    for (int i = 0; i < 8; ++i) {
        if (field[i] != 0) { allZero = false; break; }
    }
    if (allZero) {
        return true;
    }

    /* Otherwise it must be exactly "raw" followed by five zero bytes. */
    if (field[0] != 'r' || field[1] != 'a' || field[2] != 'w') {
        return false;
    }
    for (int i = 3; i < 8; ++i) {
        if (field[i] != 0) {
            return false;
        }
    }
    return true;
}

} // namespace

extern "C" {

size_t iwi_file_required_size(const uint8_t* bytes, size_t size)
{
    if (bytes == nullptr || size < IWI_FILE_BASE_HEADER_BYTES) {
        return 0;
    }

    IwiFileHeader base{};
    std::memcpy(&base, bytes, IWI_FILE_BASE_HEADER_BYTES);

    if (std::memcmp(base.magic, IWI_FILE_MAGIC, IWI_FILE_MAGIC_BYTES) != 0) {
        return 0;
    }

    const uint32_t count = ReadU16LE(reinterpret_cast<const uint8_t*>(&base.componentCount));
    if (count < IWI_FILE_MIN_COMPONENTS) {
        return 0;
    }

    /* 36 + 6N.  Cannot overflow size_t for any representable count. */
    return IWI_FILE_BASE_HEADER_BYTES +
           static_cast<size_t>(count) * IWI_FILE_COMPONENT_BYTES;
}

/*
 * Structural validation: the container grammar, and nothing about what this
 * build can display.
 *
 * `head`/`headLen` are the bytes the caller can vouch for; `fileSize` is the
 * size of the container they came from.  For a whole-buffer caller the two are
 * equal.  Every read of the header and the component table is bounded by
 * headLen, and every bounds decision about the payload is made against
 * fileSize - the distinction that keeps a prefix reader from reading past its
 * buffer.
 *
 * What is checked here is only what makes a file *well formed*: the magic, the
 * dimensions, that the component table is present, that the declared payload
 * actually exists in the file, and that none of the arithmetic overflows.  The
 * payload size is derived from the component bit widths, so a 16-bit sample is
 * accounted as two bytes and a file that is short by exactly that much is
 * refused here rather than being accepted and truncated later.
 *
 * Deliberately absent: component count limits, semantics, component class,
 * bit depth, bit offsets, storage flags, and the payload codec.  All of those
 * are capability questions, and they are answered where the capability lives:
 * the viewer's policy is in IwiFormat.ixx, the converter's in IWImageIO.ixx. A
 * converter has different answers from a viewer, which is why this file stops
 * at the grammar.
 */
static int ParseStructural(IwiFileHeader* out, size_t out_size,
                           const uint8_t* head, size_t headLen, size_t fileSize)
{
    if (head == nullptr || out == nullptr) {
        return IWI_STATUS_NO_INPUT;
    }
    if (headLen < IWI_FILE_BASE_HEADER_BYTES) {
        return IWI_STATUS_TRUNCATED_HEADER;
    }

    /* Aligned local copy: no aliasing, no unaligned access. */
    IwiFileHeader base{};
    std::memcpy(&base, head, IWI_FILE_BASE_HEADER_BYTES);

    if (std::memcmp(base.magic, IWI_FILE_MAGIC, IWI_FILE_MAGIC_BYTES) != 0) {
        return IWI_STATUS_BAD_MAGIC;
    }

    const uint32_t width       = ReadU32LE(reinterpret_cast<const uint8_t*>(&base.width));
    const uint32_t height      = ReadU32LE(reinterpret_cast<const uint8_t*>(&base.height));
    const uint32_t metaBytes   = ReadU32LE(reinterpret_cast<const uint8_t*>(&base.metaBytes));
    const uint16_t componentCount =
        ReadU16LE(reinterpret_cast<const uint8_t*>(&base.componentCount));

    /*
     * Zero dimensions are grammar, not policy: no consumer of any kind has a
     * use for a container describing no pixels.
     */
    if (width == 0u || height == 0u) {
        return IWI_STATUS_ZERO_DIMENSION;
    }

    /*
     * The addressable range.  A u32 dimension is legal in the header and a lie
     * about the buffer: the descriptor's stride is an int, so anything past
     * INT_MAX cannot be described, and sizing from it would overflow before the
     * payload check ever ran.  This fires before anything is computed from the
     * geometry.
     */
    if (width > 0x7FFFFFFFu || height > 0x7FFFFFFFu) {
        return IWI_STATUS_UNADDRESSABLE_GEOMETRY;
    }

    /* A container with no components describes no pixels.  That is a fact
     * about the grammar, not a cap: one component is fine, seven hundred is
     * fine, zero is not a container. */
    if (componentCount < IWI_FILE_MIN_COMPONENTS) {
        return IWI_STATUS_UNSUPPORTED_COMPONENT_COUNT;
    }

    /* Header + component table + metadata must all be readable from `head`. */
    const uint64_t headerAndMeta =
        static_cast<uint64_t>(IWI_FILE_BASE_HEADER_BYTES) +
        static_cast<uint64_t>(componentCount) * IWI_FILE_COMPONENT_BYTES +
        static_cast<uint64_t>(metaBytes);

    if (static_cast<uint64_t>(headLen) < headerAndMeta) {
        return IWI_STATUS_TRUNCATED_HEADER;
    }

    /* Caller's blob must hold the base header plus the whole table. */
    const size_t required = IWI_FILE_BASE_HEADER_BYTES +
                            static_cast<size_t>(componentCount) * IWI_FILE_COMPONENT_BYTES;
    if (out_size < required) {
        return IWI_STATUS_ENOSPC;
    }

    /*
     * Bits per pixel, summed from the component table.
     *
     * The payload size has to come from here rather than from componentCount:
     * a component wider than eight bits occupies more than one byte, and
     * assuming otherwise accepts a file that is short by exactly half.  That
     * was a real consequence of doing the arithmetic before looking at the
     * table, and it is why this loop precedes the payload check.
     */
    const uint8_t* table = head + IWI_FILE_BASE_HEADER_BYTES;

    uint64_t bitsPerPixel = 0;
    for (uint32_t i = 0; i < componentCount; ++i) {
        const uint8_t* rec = table + static_cast<size_t>(i) * IWI_FILE_COMPONENT_BYTES;
        bitsPerPixel += ReadU16LE(rec + 4);
    }

    if (bitsPerPixel == 0u) {
        /* Every component is zero bits wide: the table describes no pixels. */
        return IWI_STATUS_COMPONENT_LAYOUT;
    }

    const uint64_t pixels  = static_cast<uint64_t>(width) * static_cast<uint64_t>(height);
    const uint64_t maxSize = static_cast<uint64_t>(std::numeric_limits<size_t>::max());

    if (pixels > maxSize / bitsPerPixel) {
        return IWI_STATUS_GEOMETRY_TOO_LARGE;
    }
    const uint64_t totalBits = pixels * bitsPerPixel;
    if (totalBits > maxSize - 7u) {
        return IWI_STATUS_GEOMETRY_TOO_LARGE;
    }
    /* Whole bytes; a sub-byte remainder still occupies a byte. */
    const uint64_t expectedPayload = (totalBits + 7u) / 8u;

    /* Against the file, not the head: a prefix reader holds only the head. */
    if (static_cast<uint64_t>(fileSize) < headerAndMeta) {
        return IWI_STATUS_TRUNCATED_HEADER;
    }
    if (static_cast<uint64_t>(fileSize) - headerAndMeta < expectedPayload) {
        return IWI_STATUS_PAYLOAD_TRUNCATED;
    }

    /* Write the blob: base header first, then the component table.  Always
     * IWI_FILE_BASE_HEADER_BYTES, never sizeof(IwiFileHeader): the struct has
     * four bytes of tail padding and the blob is exactly 36 + 6N. */
    std::memcpy(out, &base, IWI_FILE_BASE_HEADER_BYTES);

    uint8_t* destTable = reinterpret_cast<uint8_t*>(out) + IWI_FILE_BASE_HEADER_BYTES;
    for (uint32_t i = 0; i < componentCount; ++i) {
        const uint8_t* rec  = table + static_cast<size_t>(i) * IWI_FILE_COMPONENT_BYTES;
        uint8_t*       dest = destTable + static_cast<size_t>(i) * IWI_FILE_COMPONENT_BYTES;

        dest[0] = rec[0];
        dest[1] = rec[1];
        WriteU16LE(dest + 2, ReadU16LE(rec + 2));
        WriteU16LE(dest + 4, ReadU16LE(rec + 4));
    }

    return IWI_STATUS_OK;
}

int iwi_parse_structural(IwiFileHeader* out, size_t out_size,
                         const uint8_t* bytes, size_t size)
{
    return ParseStructural(out, out_size, bytes, size, size);
}

int iwi_file_peek_components(const uint8_t* bytes, size_t len, uint16_t* out_count)
{
    if (bytes == nullptr || out_count == nullptr || len < IWI_FILE_BASE_HEADER_BYTES) {
        return IWI_STATUS_NO_INPUT;
    }
    if (std::memcmp(bytes, IWI_FILE_MAGIC, IWI_FILE_MAGIC_BYTES) != 0) {
        return IWI_STATUS_BAD_MAGIC;
    }

    /* componentCount is the little-endian u16 at offset 34; asserted by
     * test_abi_constants.cpp against offsetof. */
    *out_count = ReadU16LE(bytes + offsetof(IwiFileHeader, componentCount));
    return IWI_STATUS_OK;
}

int iwi_payload_encoding_is_raw(const IwiFileHeader* f)
{
    if (f == nullptr) {
        return 0;
    }
    return EncodingIsRaw(f->payloadEncoding) ? 1 : 0;
}

int iwi_parse_prefix(IwiFileHeader* out, size_t out_size,
                     const uint8_t* head, size_t headLen,
                     size_t fileSize)
{
    return ParseStructural(out, out_size, head, headLen, fileSize);
}

int iwi_status_is_unsupported(int status)
{
    switch (static_cast<IwiFileStatus>(status)) {
    case IWI_STATUS_UNSUPPORTED:
    case IWI_STATUS_UNSUPPORTED_ENCODING:
    case IWI_STATUS_UNSUPPORTED_STORAGE:
    case IWI_STATUS_UNSUPPORTED_COMPONENT_COUNT:
    case IWI_STATUS_UNSUPPORTED_COMPONENT_CLASS:
    case IWI_STATUS_UNSUPPORTED_BIT_DEPTH:
    case IWI_STATUS_UNEXPECTED_SEMANTIC:
        return 1;
    default:
        return 0;
    }
}
int iwi_decode(const IwiFileHeader* f, const uint8_t* bytes, size_t size,
               uint8_t* out_rgba, size_t out_cap)
{
    if (f == nullptr || bytes == nullptr || out_rgba == nullptr) {
        return IWI_STATUS_NO_INPUT;
    }

    const uint32_t width  = ReadU32LE(reinterpret_cast<const uint8_t*>(&f->width));
    const uint32_t height = ReadU32LE(reinterpret_cast<const uint8_t*>(&f->height));
    const uint32_t metaBytes = ReadU32LE(reinterpret_cast<const uint8_t*>(&f->metaBytes));
    const uint16_t componentCount =
        ReadU16LE(reinterpret_cast<const uint8_t*>(&f->componentCount));

    /*
     * Presentation limit, not a container limit.  Only 3 and 4 components have
     * an obvious RGBA form; anything else would require inventing a mapping.
     */
    if (componentCount != 3u && componentCount != 4u) {
        return IWI_STATUS_UNSUPPORTED_COMPONENT_COUNT;
    }

    const uint64_t pixels = static_cast<uint64_t>(width) * static_cast<uint64_t>(height);
    if (pixels > static_cast<uint64_t>(std::numeric_limits<size_t>::max() / 4u)) {
        return IWI_STATUS_GEOMETRY_TOO_LARGE;
    }
    const size_t needed = static_cast<size_t>(pixels) * 4u;
    if (out_cap < needed) {
        return IWI_STATUS_ENOSPC;
    }

    const uint64_t payloadOffset =
        static_cast<uint64_t>(IWI_FILE_BASE_HEADER_BYTES) +
        static_cast<uint64_t>(componentCount) * IWI_FILE_COMPONENT_BYTES +
        static_cast<uint64_t>(metaBytes);

    if (static_cast<uint64_t>(size) < payloadOffset) {
        return IWI_STATUS_TRUNCATED_HEADER;
    }
    if (static_cast<uint64_t>(size) - payloadOffset <
        pixels * static_cast<uint64_t>(componentCount)) {
        return IWI_STATUS_PAYLOAD_TRUNCATED;
    }

    const uint8_t* src = bytes + payloadOffset;

    if (componentCount == 4u) {
        std::memcpy(out_rgba, src, needed);
        return IWI_STATUS_OK;
    }

    /* RGB -> RGBA, alpha fully opaque. */
    for (uint64_t p = 0; p < pixels; ++p) {
        const uint8_t* s = src + p * 3u;
        uint8_t*       d = out_rgba + p * 4u;
        d[0] = s[0];
        d[1] = s[1];
        d[2] = s[2];
        d[3] = 0xFFu;
    }
    return IWI_STATUS_OK;
}

int iwi_file_metadata(const IwiFileHeader* f, const uint8_t* bytes, size_t size,
                      const uint8_t** out_meta, size_t* out_meta_len)
{
    if (f == nullptr || bytes == nullptr) {
        return IWI_STATUS_NO_INPUT;
    }

    const uint32_t metaBytes = ReadU32LE(reinterpret_cast<const uint8_t*>(&f->metaBytes));
    const uint16_t componentCount =
        ReadU16LE(reinterpret_cast<const uint8_t*>(&f->componentCount));

    const uint64_t offset =
        static_cast<uint64_t>(IWI_FILE_BASE_HEADER_BYTES) +
        static_cast<uint64_t>(componentCount) * IWI_FILE_COMPONENT_BYTES;

    if (static_cast<uint64_t>(size) < offset + metaBytes) {
        return IWI_STATUS_TRUNCATED_HEADER;
    }

    if (metaBytes == 0u) {
        if (out_meta)     { *out_meta = nullptr; }
        if (out_meta_len) { *out_meta_len = 0; }
        return IWI_STATUS_OK;
    }

    if (out_meta)     { *out_meta = bytes + offset; }
    if (out_meta_len) { *out_meta_len = static_cast<size_t>(metaBytes); }
    return IWI_STATUS_OK;
}

void iwi_file_free(void* blob)
{
    std::free(blob);
}

/*
 * Messages.
 *
 * Two things these strings must do, and one of them is a contract:
 *
 *   - separate corruption from capability.  "not an IWI container" is a
 *     different sentence from "valid IWI, but this build cannot display it",
 *     and a user who sees the second should not go looking for a corrupt file
 *     or a re-download.
 *
 *   - keep the words "magic", "planar", "offset", "header", "8-bit",
 *     "addressable" and "dimensions" in the messages for their statuses.  The
 *     loader and the shell provider tests match on those substrings, so the
 *     wording is asserted, not decorative.
 */
const char* iwi_status_message(int status)
{
    switch (static_cast<IwiFileStatus>(status)) {
    case IWI_STATUS_OK:
        return "ok";

    /* ---- malformed: this is not a valid IWI ---- */
    case IWI_STATUS_NO_INPUT:
        return "no input";
    case IWI_STATUS_TRUNCATED_HEADER:
        return "IWI is shorter than its header and component table";
    case IWI_STATUS_BAD_MAGIC:
        return "not an IWI container (magic mismatch)";
    case IWI_STATUS_ZERO_DIMENSION:
        return "IWI dimensions are zero; an image needs a non-zero width and height";
    case IWI_STATUS_GEOMETRY_TOO_LARGE:
        return "IWI geometry overflows the addressable range";
    case IWI_STATUS_PAYLOAD_TRUNCATED:
        return "IWI payload is shorter than its geometry requires";
    case IWI_STATUS_COMPONENT_LAYOUT:
        return "IWI component bit offset or width does not match its position";
    case IWI_STATUS_ENOSPC:
        return "destination buffer is too small for the IWI header";
    case IWI_STATUS_UNADDRESSABLE_GEOMETRY:
        return "IWI dimensions exceed the addressable range";

    /* ---- unsupported: a valid IWI this build cannot display ---- */
    case IWI_STATUS_UNSUPPORTED:
        return "valid IWI, but it uses a feature this build does not support";
    case IWI_STATUS_UNSUPPORTED_ENCODING:
        return "valid IWI, but its payload encoding is not one this build "
               "decodes; only \"raw\" is supported";
    case IWI_STATUS_UNSUPPORTED_STORAGE:
        return "valid IWI, but its storage layout is planar, columnar, "
               "mirrored or stride-aligned; this build reads only interleaved, "
               "tightly packed rows";
    case IWI_STATUS_UNSUPPORTED_COMPONENT_COUNT:
        return "valid IWI, but this viewer displays only 3- or 4-component "
               "RGB(A) rasters; grayscale and multi-band component counts have "
               "no RGBA form";
    case IWI_STATUS_UNSUPPORTED_COMPONENT_CLASS:
        return "valid IWI, but its component class is not integer or unsigned "
               "normalised data";
    case IWI_STATUS_UNSUPPORTED_BIT_DEPTH:
        return "valid IWI, but its component bit depth is not 8-bit; 16-bit "
               "and wider samples are not displayed by this build";
    case IWI_STATUS_UNEXPECTED_SEMANTIC:
        return "valid IWI, but its component semantics are not an RGB(A) raster";
    }
    return "unknown IWI status";
}
} // extern "C"
