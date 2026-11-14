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

/* The storage flag bits this build implements.  Everything else is a layout
 * this decoder does not apply, and accepting it silently would render the
 * image transposed or mirrored with no error. */
constexpr uint32_t kSupportedStorageFlags = 0u;

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
 * The shared body of iwi_parse and iwi_parse_prefix.
 *
 * `head`/`headLen` are the bytes the caller can vouch for; `fileSize` is the
 * size of the container they came from.  For iwi_parse the two are equal.
 * Every read of the header and the component table is bounded by headLen, and
 * every bounds decision about the payload is made against fileSize - which is
 * the distinction that keeps a prefix reader from reading past its buffer.
 */
static int ParseCore(IwiFileHeader* out, size_t out_size,
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
    const uint64_t layoutRaw   = ReadU64LE(reinterpret_cast<const uint8_t*>(&base.storageLayout));
    const uint16_t componentCount =
        ReadU16LE(reinterpret_cast<const uint8_t*>(&base.componentCount));

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

    if (componentCount < IWI_FILE_MIN_COMPONENTS) {
        return IWI_STATUS_UNSUPPORTED_COMPONENT_COUNT;
    }

    /* Payload encoding: full-field compare. */
    if (!EncodingIsRaw(base.payloadEncoding)) {
        return IWI_STATUS_UNSUPPORTED_ENCODING;
    }

    /*
     * Storage layout is a bitmask, so this is a mask test, not a comparison.
     *
     * PLANAR gets its own status because "one plane per component" is a layout
     * this build could plausibly learn, while column-major, mirrored and
     * stride-aligned variants are further away.  The message for the planar
     * case is what the loader tests match on.
     */
    const uint32_t flags  = static_cast<uint32_t>(layoutRaw & 0xFFFFFFFFull);
    const uint32_t stride = static_cast<uint32_t>(layoutRaw >> 32u);

    if ((flags & static_cast<uint32_t>(IW_STORAGE_FLAG_PLANAR)) != 0u) {
        return IWI_STATUS_UNSUPPORTED_STORAGE;
    }
    if (flags != kSupportedStorageFlags) {
        return IWI_STATUS_UNSUPPORTED_STORAGE;
    }
    if (stride != 0u) {
        /* A declared row stride would change the payload arithmetic; until it
         * is honoured, a file that sets it is not one this build can read. */
        return IWI_STATUS_UNSUPPORTED_STORAGE;
    }

    /* Header + component table + metadata must all be readable. */
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
     * Geometry.  pixels * componentCount is the payload size in bytes because
     * every supported component is 8 bits wide - checked below, before this
     * multiplication is trusted.
     */
    const uint64_t pixels = static_cast<uint64_t>(width) * static_cast<uint64_t>(height);
    const uint64_t maxSize = static_cast<uint64_t>(std::numeric_limits<size_t>::max());
    if (pixels != 0u && static_cast<uint64_t>(componentCount) > maxSize / pixels) {
        return IWI_STATUS_GEOMETRY_TOO_LARGE;
    }
    const uint64_t expectedPayload = pixels * static_cast<uint64_t>(componentCount);

    /* Against the file, not the head: a prefix reader holds only the head. */
    if (static_cast<uint64_t>(fileSize) < headerAndMeta + expectedPayload) {
        return IWI_STATUS_PAYLOAD_TRUNCATED;
    }

    /*
     * Component table.
     *
     * Read from the caller's head at 36 + i*6.  The first three components
     * must be R, G, B and a fourth, when present, must be A: that is the
     * interleaved colour raster this format encodes.  Components past the
     * fourth are unconstrained - extra bands, so a 200-component spectral
     * capture is a legitimate container.
     *
     * The count itself is never rejected for being large; see iwi_decode for
     * the presentation limit.
     */
    const uint8_t* table = head + IWI_FILE_BASE_HEADER_BYTES;

    static const uint8_t kRequiredSemantics[4] = {
        static_cast<uint8_t>(IW_COMPONENT_SEMANTIC_R),
        static_cast<uint8_t>(IW_COMPONENT_SEMANTIC_G),
        static_cast<uint8_t>(IW_COMPONENT_SEMANTIC_B),
        static_cast<uint8_t>(IW_COMPONENT_SEMANTIC_A)
    };

    if (componentCount < 3u) {
        return IWI_STATUS_UNEXPECTED_SEMANTIC;
    }

    const uint32_t constrained = componentCount < 4u ? componentCount : 4u;

    for (uint32_t i = 0; i < componentCount; ++i) {
        const uint8_t* rec = table + static_cast<size_t>(i) * IWI_FILE_COMPONENT_BYTES;
        const uint8_t  semantic = rec[0];
        const uint8_t  cls      = rec[1];
        const uint16_t bitOff   = ReadU16LE(rec + 2);
        const uint16_t bitWidth = ReadU16LE(rec + 4);

        if (i < constrained && semantic != kRequiredSemantics[i]) {
            return IWI_STATUS_UNEXPECTED_SEMANTIC;
        }
        if (cls != static_cast<uint8_t>(IW_COMPONENT_CLASS_UINT) &&
            cls != static_cast<uint8_t>(IW_COMPONENT_CLASS_UNORM)) {
            return IWI_STATUS_UNSUPPORTED_COMPONENT_CLASS;
        }
        if (bitWidth != 8u) {
            return IWI_STATUS_UNSUPPORTED_BIT_DEPTH;
        }
        if (bitOff != static_cast<uint16_t>(i * 8u)) {
            return IWI_STATUS_COMPONENT_LAYOUT;
        }
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

int iwi_parse(IwiFileHeader* out, size_t out_size,
              const uint8_t* bytes, size_t size)
{
    return ParseCore(out, out_size, bytes, size, size);
}

int iwi_parse_prefix(IwiFileHeader* out, size_t out_size,
                     const uint8_t* head, size_t headLen,
                     size_t fileSize)
{
    return ParseCore(out, out_size, head, headLen, fileSize);
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

const char* iwi_status_message(int status)
{
    switch (static_cast<IwiFileStatus>(status)) {
    case IWI_STATUS_OK:
        return "ok";
    case IWI_STATUS_NO_INPUT:
        return "no input";
    case IWI_STATUS_TRUNCATED_HEADER:
        return "IWI is shorter than its header and component table";
    case IWI_STATUS_BAD_MAGIC:
        return "not an IWI container (magic mismatch)";
    case IWI_STATUS_ZERO_DIMENSION:
        return "IWI dimensions are zero; an image needs a non-zero width and height";
    case IWI_STATUS_GEOMETRY_TOO_LARGE:
        return "IWI geometry exceeds the addressable range";
    case IWI_STATUS_PAYLOAD_TRUNCATED:
        return "IWI payload is shorter than its geometry requires";
    case IWI_STATUS_COMPONENT_LAYOUT:
        return "IWI component bit offset or width does not match its position";
    case IWI_STATUS_ENOSPC:
        return "destination buffer is too small for the IWI header";
    case IWI_STATUS_UNSUPPORTED_ENCODING:
        return "IWI payload encoding is not supported";
    case IWI_STATUS_UNSUPPORTED_STORAGE:
        return "IWI storage layout is planar or uses flags this build does not implement";
    case IWI_STATUS_UNSUPPORTED_COMPONENT_COUNT:
        return "IWI component count cannot be presented as RGBA";
    case IWI_STATUS_UNSUPPORTED_COMPONENT_CLASS:
        return "IWI component class is not understood";
    case IWI_STATUS_UNSUPPORTED_BIT_DEPTH:
        return "IWI component bit depth is not 8-bit";
    case IWI_STATUS_UNEXPECTED_SEMANTIC:
        return "IWI component semantics are not an RGB(A) raster";
    case IWI_STATUS_UNADDRESSABLE_GEOMETRY:
        return "IWI geometry exceeds the addressable range";
    }
    return "unknown IWI status";
}

} // extern "C"
