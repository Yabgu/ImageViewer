#pragma once

/*
 * IwiFile.h - the .iwi container format as a C API.
 *
 * # Why this is C
 *
 * The header has been parsed independently at least four times: this viewer's
 * loader, its shell thumbnail provider, Tension's Rust reader
 * (tension-core/src/render2d/iwi.rs) and Tension's JS writer
 * (tension-framework/tools/lib/iwi.mjs).  The Rust file says so in its own
 * header comment - "a byte-for-byte port of tension-2d/src/texture.cpp, which
 * parsed the format IWImageIO.ixx writes rather than depending on it as a C++
 * module".  That port exists because there was no C interface to depend on.
 *
 * This is that interface.  A C++ module cannot be consumed by Rust, by a build
 * script, or by a C engine; a C header can.  The implementation happens to be
 * C++ (IwiFile.cpp), but the contract is C: no classes, no templates, no
 * `std::`, no module.
 *
 * The CI gate compiles this header as C11 (ci/check-iwi-c-header.sh and the
 * build workflow), so a regression to C++-only declarations fails the build
 * rather than a downstream consumer.
 *
 * # The header on disk
 *
 *   offset  size  field
 *        0     4  magic "IWI1"
 *        4     4  width           u32
 *        8     4  height          u32
 *       12     4  metaBytes       u32  (metadata blob size, 0 = none)
 *       16     8  storageLayout   u64  (flags u32, stride u32)
 *       24     8  payloadEncoding char[8] ("raw" or zero; see below)
 *       32     1  colorSpace      u8   (ImageColorSpace)
 *       33     1  reserved        u8   (zeroed)
 *       34     2  componentCount  u16
 *       36   6*N  components[N]   N x IwiFileComponent, immediately after the
 *                                  base header, in the same allocation
 *     36+6N   M  metadata        M = metaBytes, then:
 *   36+6N+M   *  pixelData       tightly packed, interleaved, 8 bits each
 *
 * N is `componentCount` and is not capped.  Components are data, not metadata:
 * the base header is a fixed 36 bytes and the component table follows it.
 *
 * # Byte order
 *
 * Little-endian, always.  Big-endian hosts are out of scope by design: the
 * reads in IwiFile.cpp are byte-wise and therefore host-independent, while a
 * cast to a struct would inherit host byte order.
 *
 * # Two-call pattern
 *
 *   size_t need = iwi_file_required_size(bytes, size);   // 36 + 6N, or 0
 *   IwiFileHeader* f = malloc(need);
 *   iwi_parse(f, need, bytes, size);                     // 0 on success
 *   const IwiFileComponent* c = iwi_file_components(f);  // walk the blob
 *   iwi_file_free(f);
 *
 * One allocation, one free.  iwi_parse writes the base header and the
 * component table into the same block, which is why the accessor below can
 * walk off the end of the struct instead of needing a pointer field: a stored
 * pointer would either dangle or force a second allocation.
 */

#include <stddef.h>
#include <stdint.h>

#include "ImagePluginDef.h"

#ifdef __cplusplus
extern "C" {
#endif

/* -- Constants ------------------------------------------------------------ */

#define IWI_FILE_MAGIC              "IWI1"
#define IWI_FILE_MAGIC_BYTES        4u

/*
 * The fixed part of the header, before the component table.  Also the offset
 * at which the component table starts, and the size of IwiFileHeader itself.
 */
#define IWI_FILE_BASE_HEADER_BYTES  36u

/* Bytes per component record in the file.  Same size as IWComponentDef; the
 * two are asserted layout-identical in src/tests/test_abi_constants.cpp. */
#define IWI_FILE_COMPONENT_BYTES    6u

/* A container with zero components has no pixel format and no payload size. */
#define IWI_FILE_MIN_COMPONENTS     1u

/* The payload encodings this build understands.  Any other 8-byte value is
 * rejected; the field is compared in full, not by prefix. */
#define IWI_PAYLOAD_RAW             "raw"
#define IWI_PAYLOAD_LZ4             "lz4"
#define IWI_PAYLOAD_ZSTD            "zstd"

/*
 * Extensible metadata entry wire format.  An .iwi file may carry an optional
 * metadata blob of `metaBytes` bytes; when it does, the blob is a sequence of
 * (IwiMetaHeader, payload) records addressed by a four-character tag.
 *
 * These tags and the record header document the on-disk format for consumers
 * that want to read the blob.  This build parses `metaBytes` and exposes the
 * blob through iwi_file_metadata, but no loader consumes the contents yet -
 * they are format extension points, defined now so the tag values are stable.
 *
 * IWCameraMeta was removed: it is one specific payload shape rather than part
 * of the container grammar, and nothing ever read it.
 */
#define IWI_META_TAG_CAMERA 0x414D4143u /* "CAMA": 3D camera intrinsics & extrinsics */
#define IWI_META_TAG_CBOR   0x524F4243u /* "CBOR": IETF RFC 8949 nestable binary JSON*/
#define IWI_META_TAG_BSON   0x4E4F5342u /* "BSON": BSON document                    */
#define IWI_META_TAG_MSGP   0x5047534Du /* "MSGP": MessagePack                      */
#define IWI_META_TAG_JSON   0x4E4F534Au /* "JSON": UTF-8 JSON text                  */
#define IWI_META_TAG_TEXT   0x54584554u /* "TEXT": UTF-8 key-value string metadata  */

typedef struct IwiMetaHeader {
    uint32_t tag;    /* FourCC tag identifying entry type */
    uint32_t size;   /* Byte length of entry payload      */
} IwiMetaHeader;

/* -- On-disk structures --------------------------------------------------- */

/*
 * One component record, exactly as stored: 6 bytes, no padding.
 *
 * Field types are the integer forms rather than the enum types so the struct
 * is byte-for-byte IWComponentDef without depending on the enum's underlying
 * representation, and so this header still compiles as plain C.
 * test_abi_constants.cpp asserts the two structs are layout-identical.
 */
typedef struct IwiFileComponent {
    uint8_t  semantic;       /* IWComponentSemantic value (1 byte) */
    uint8_t  componentClass; /* IWComponentClass value    (1 byte) */
    uint16_t bitOffset;      /* Bit offset from start of pixel     */
    uint16_t bitWidth;       /* Bit width of component             */
} IwiFileComponent;

/*
 * The fixed 36-byte header.  The component table is NOT a member: it follows
 * this struct in the same allocation, and iwi_file_components() walks to it.
 *
 * Natural alignment already produces the on-disk offsets - the 64-bit layout
 * descriptor forces offset 16, and every later field lands where the format
 * says - so this struct needs no packing attribute, and copying fields out of
 * an aligned local of this type is neither UB nor an unaligned access.
 *
 * SIZE, and the one rule that follows from it: the last field ends at byte 36,
 * but sizeof(IwiFileHeader) is 40 because the structure's alignment is 8 and
 * it is padded to a multiple of that.  The trailing four bytes are padding,
 * not data.  Every copy of this header in IwiFile.cpp and IwiFormat.ixx is
 * therefore `memcpy(x, y, IWI_FILE_BASE_HEADER_BYTES)` and never
 * `memcpy(x, y, sizeof(IwiFileHeader))` - the latter would write four bytes
 * into the first component record, because the blob is exactly 36 + 6N bytes.
 * test_abi_constants.cpp pins every offset and asserts that the last field
 * ends at 36.
 */
typedef struct IwiFileHeader {
    char             magic[4];        /*  0: "IWI1"                                  */
    uint32_t         width;           /*  4: image width in pixels                   */
    uint32_t         height;          /*  8: image height in pixels                  */
    uint32_t         metaBytes;       /* 12: metadata blob size in bytes (0 = none)  */
    IWStorageLayout  storageLayout;   /* 16: flags u32 + stride u32                  */
    char             payloadEncoding[8]; /* 24: "raw" or all-zero                    */
    uint8_t          colorSpace;      /* 32: ImageColorSpace value                   */
    uint8_t          reserved;        /* 33: zeroed                                  */
    uint16_t         componentCount;  /* 34: number of component records that follow */
} IwiFileHeader;

/* -- Status --------------------------------------------------------------- */

/*
 * Return codes for the functions below.  Zero is success so a caller can test
 * `if (rc)`.
 *
 * MALFORMED and UNSUPPORTED are deliberately distinct, the same distinction
 * Tension's Rust reader makes between -EINVAL and -ENOSYS: malformed means
 * "this is not a valid IWI", unsupported means "valid IWI this build cannot
 * present".  A caller that wants to fall back rather than report corruption
 * needs the difference.
 */
typedef enum IwiFileStatus {
    IWI_STATUS_OK = 0,

    /* Malformed container */
    IWI_STATUS_NO_INPUT = 1,
    IWI_STATUS_TRUNCATED_HEADER = 2,
    IWI_STATUS_BAD_MAGIC = 3,
    IWI_STATUS_ZERO_DIMENSION = 4,
    IWI_STATUS_GEOMETRY_TOO_LARGE = 5,
    IWI_STATUS_PAYLOAD_TRUNCATED = 6,
    IWI_STATUS_COMPONENT_LAYOUT = 7,   /* bitOffset / bitWidth disagree with position */
    IWI_STATUS_ENOSPC = 8,             /* caller's buffer smaller than required       */

    /* Valid container, unsupported feature */
    IWI_STATUS_UNSUPPORTED_ENCODING = 9,
    IWI_STATUS_UNSUPPORTED_STORAGE = 10,
    IWI_STATUS_UNSUPPORTED_COMPONENT_COUNT = 11,
    IWI_STATUS_UNSUPPORTED_COMPONENT_CLASS = 12,
    IWI_STATUS_UNSUPPORTED_BIT_DEPTH = 13,
    IWI_STATUS_UNEXPECTED_SEMANTIC = 14,

    /* Geometry that is legal in a u32 but not describable (past INT_MAX).
     * Distinct from GEOMETRY_TOO_LARGE, which is an overflow: the viewer maps
     * that one to an allocation failure and this one to a decode failure. */
    IWI_STATUS_UNADDRESSABLE_GEOMETRY = 15
} IwiFileStatus;

/* -- API ------------------------------------------------------------------ */

/*
 * Bytes the caller must allocate for iwi_parse to fill: the base header plus
 * one record per component, i.e. 36 + 6 * componentCount.
 *
 * Returns 0 when the base header is not readable (null, shorter than 36 bytes,
 * bad magic, or componentCount == 0).  That is enough to size the allocation;
 * full validation is iwi_parse's job.
 *
 * N is not capped.  A file declaring 200 components asks for 36 + 1200 bytes.
 */
size_t iwi_file_required_size(const uint8_t* bytes, size_t size);

/*
 * Validate a whole container and write its header and component table into
 * `out`, which must be at least iwi_file_required_size() bytes.
 *
 * `size` is the whole file: the payload bounds check needs the real length, so
 * a caller holding only the header bytes cannot use this to accept a file.
 *
 * On success `*out` holds the base header and iwi_file_components(out) points
 * at the componentCount records that follow.  On any failure the contents of
 * `out` are unspecified and must not be used.
 *
 * Rejects: bad magic, truncation, zero dimensions, an unknown payload
 * encoding, payload shorter than the geometry needs, and any storage flag this
 * build does not implement.  It does NOT reject an unusual component count -
 * that is a rendering question, answered by iwi_decode.
 */
int iwi_parse(IwiFileHeader* out, size_t out_size,
              const uint8_t* bytes, size_t size);

/*
 * Same validation, but the bytes the caller actually holds are separate from
 * the size of the file they came from.
 *
 * This exists for readers that deliberately never load a whole file - the
 * Windows shell thumbnail provider reads a head, works out which rows it needs,
 * and seeks to them.  Such a caller holds `headLen` readable bytes out of a
 * `fileSize`-byte container.  Passing the container size as the readable
 * length would invite the validator to read past the buffer; passing the head
 * length as the container size would reject every file for having no payload.
 *
 * The component table must be fully present in `head`, since it is read from
 * there.  `iwi_parse` is this function with both lengths equal.
 */
int iwi_parse_prefix(IwiFileHeader* out, size_t out_size,
                     const uint8_t* head, size_t headLen,
                     size_t fileSize);

/*
 * The component table of a parsed blob: the bytes immediately after the base
 * header.  Valid for exactly componentCount records.
 *
 * `f` must be a blob filled by iwi_parse (or an equivalent 36-byte base header
 * followed by its records), not a stack header on its own.
 *
 * Two spellings on purpose.  In C it is `static inline`, which keeps a
 * header-only helper out of every translation unit's symbol table.  In C++ it
 * needs external linkage to be inline-usable from a module's global module
 * fragment - MSVC rejects the static form there with C2129 - so the C++ form is
 * plain `inline` inside the extern "C" block, which gives it C linkage and the
 * one-definition rule's blessing.
 */
#ifdef __cplusplus
inline const IwiFileComponent* iwi_file_components(const IwiFileHeader* f)
{
    return (const IwiFileComponent*)((const uint8_t*)f + IWI_FILE_BASE_HEADER_BYTES);
}
#else
static inline const IwiFileComponent* iwi_file_components(const IwiFileHeader* f)
{
    return (const IwiFileComponent*)((const uint8_t*)f + IWI_FILE_BASE_HEADER_BYTES);
}
#endif

/*
 * Convert the payload to tightly packed RGBA8, one byte per channel.
 *
 * This is the presentation step, and it is where component count matters: only
 * 3 (RGB, alpha synthesised to 255) and 4 (RGBA, passed through) components
 * have an obvious RGBA form.  Anything else - grayscale, two components, a
 * 200-band spectral capture - is a valid container that this function refuses
 * with IWI_STATUS_UNSUPPORTED_COMPONENT_COUNT, because guessing a mapping
 * would be inventing data.
 *
 * `out_cap` must be at least width * height * 4.  Returns 0 on success.
 */
int iwi_decode(const IwiFileHeader* f, const uint8_t* bytes, size_t size,
               uint8_t* out_rgba, size_t out_cap);

/*
 * The metadata blob, if any: `*out_meta` points into `bytes` at the byte after
 * the component table, and `*out_meta_len` is metaBytes.
 *
 * Points into the caller's source buffer - no copy, no allocation - so it
 * lives exactly as long as `bytes` does.  When metaBytes is 0 both outputs are
 * NULL/0 and the call still succeeds.
 */
int iwi_file_metadata(const IwiFileHeader* f, const uint8_t* bytes, size_t size,
                      const uint8_t** out_meta, size_t* out_meta_len);

/*
 * Release a blob allocated by a consumer of this API.
 *
 * Freeing with the same allocator that produced the block is the caller's
 * responsibility; this exists so a plugin can export one teardown callback
 * (a function pointer cannot be `free` portably across module boundaries).
 */
void iwi_file_free(void* blob);

/*
 * A short, stable English description of a status, suitable for a log line or
 * an error dialog.  Never NULL.
 *
 * The words "magic", "planar", "offset" and "header" appear in the messages
 * for their respective statuses; the viewer's error paths and the ABI tests
 * match on those substrings, so the wording is a contract.
 */
const char* iwi_status_message(int status);

#ifdef __cplusplus
} /* extern "C" */
#endif
