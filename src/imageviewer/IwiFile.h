#pragma once

/*
 * IwiFile.h — the .iwi container format as a plain C API.
 *
 * # Why this file exists
 *
 * The 74-byte header has been parsed independently at least four times:
 * ImageViewer's loader, ImageViewer's shell thumbnail provider, Tension's Rust
 * reader (tension-core/src/render2d/iwi.rs) and Tension's JS writer
 * (tension-framework/tools/lib/iwi.mjs).  The Rust file says so in its own
 * header comment — "a byte-for-byte port of tension-2d/src/texture.cpp, which
 * parsed the format IWImageIO.ixx writes rather than depending on it as a C++
 * module".  That port exists because there was no C interface to depend on.
 *
 * This is that interface.  It is pure C99 (no modules, no namespaces, no
 * templates, no allocation), so it can be consumed by:
 *
 *   - C++ module units, from their global module fragment
 *     (ImageLoaderIWI.ixx, IwiThumbnail.ixx)
 *   - ordinary C and C++ translation units
 *   - a game engine through FFI — build IwiFile.c into a static library and
 *     bind four functions, or compile the header's inline path
 *
 * # What it is not
 *
 * This describes and validates the *file*.  It does not own pixels, decode
 * into a display format, or know about plugins.  The in-memory descriptor those
 * need is `IWImageFormat` in ImagePluginDef.h, which this header includes for
 * IW_MAX_COMPONENTS and IWComponentDef: the file's component table and the
 * in-memory component array are deliberately the same shape, so they cannot
 * drift apart.
 *
 * # Contract
 *
 * - No allocation.  Every output is caller-provided.
 * - No global state.  No thread-local error string; the reason goes into the
 *   caller's buffer, so two threads cannot clobber each other's message.
 * - No failure mode other than the returned status.  A malformed file is a
 *   return value, never an exception or a crash.
 * - The parsed header always describes the container as written, including
 *   fields this build may not be able to *render* (planar, sub-8-bit, HDR).
 *   Deciding what is displayable is the caller's job; the loader and the
 *   thumbnail provider make different choices and both are legitimate.
 */

#include <stddef.h>
#include <stdint.h>

#include "ImagePluginDef.h"

/*
 * Set IWI_FILE_API to __declspec(dllexport) / a visibility attribute when
 * building this as a shared library.  Left empty for static linking, which is
 * the default and what ImageViewer itself uses.
 */
#ifndef IWI_FILE_API
#define IWI_FILE_API
#endif

#ifdef __cplusplus
extern "C" {
#endif

/*
 * The container's constants.
 *
 * Published three ways on purpose, because three kinds of consumer need
 * different things and duplicating a number in each of them is how these
 * readers drifted apart in the first place:
 *
 *   - C and the preprocessor: `#define`, so the values work in a static
 *     initialiser and in `#if`.
 *   - C++: typed `inline constexpr` values in namespace `iwi_file`, at the
 *     bottom of this header, so they are scoped, debuggable, and cannot be
 *     macro-substituted into unrelated identifiers.
 *
 * Both forms are compile-time constants with no storage.
 */

/* The ASCII bytes at offset 0, and their count. */
#define IWI_FILE_MAGIC        "IWI1"
#define IWI_FILE_MAGIC_BYTES  4u

/* The fixed header, before any pixel data. */
#define IWI_FILE_HEADER_BYTES 74u

/* Bytes per component record in the file.  Not sizeof(IWComponentDef): the
 * file stores 6 bytes (u8, u8, u16, u16) while the in-memory struct uses the
 * ABI's wider enum fields. */
#define IWI_FILE_COMPONENT_BYTES 6u

/* Component records the fixed header has room for: 48 table bytes / 6.  This
 * is how many slots the *container* describes, which is why componentCount is
 * carried as a u16 and why a reader must reject a count past it. */
#define IWI_FILE_MAX_COMPONENTS 8u

/* Field offsets, in bytes from the start of the container. */
#define IWI_FILE_OFF_MAGIC          0u
#define IWI_FILE_OFF_WIDTH          4u
#define IWI_FILE_OFF_HEIGHT         8u
#define IWI_FILE_OFF_COLOR_SPACE   12u
#define IWI_FILE_OFF_COMPONENTS    13u
#define IWI_FILE_OFF_BITS_PER_PX   15u
#define IWI_FILE_OFF_STORAGE       17u
#define IWI_FILE_OFF_COMPONENT_TAB 18u
#define IWI_FILE_OFF_PIXEL_BYTES   66u

/* Fields within one component record, from the record's start. */
#define IWI_FILE_COMP_OFF_SEMANTIC   0u
#define IWI_FILE_COMP_OFF_CLASS      1u
#define IWI_FILE_COMP_OFF_BIT_OFFSET 2u
#define IWI_FILE_COMP_OFF_BIT_WIDTH  4u

/*
 * Result of a parse.  OK is 0 so a caller can test `if (status)`.
 *
 * MALFORMED and UNSUPPORTED are deliberately distinct, the same distinction
 * Tension's Rust reader makes between -EINVAL and -ENOSYS: MALFORMED means
 * "this is not a valid IWI", UNSUPPORTED means "this is a valid IWI this build
 * does not serve".  A caller that can do something else useful with an
 * unsupported-but-valid file (show metadata, convert it, ask another backend)
 * needs to tell the two apart.
 */
typedef enum IwiFileStatus {
    IWI_FILE_OK = 0,

    /* --- The bytes are not a valid IWI --------------------------------- */
    IWI_FILE_ERR_NO_INPUT          = 1,  /* null pointer                     */
    IWI_FILE_ERR_TRUNCATED_HEADER  = 2,  /* shorter than 74 bytes            */
    IWI_FILE_ERR_BAD_MAGIC         = 3,  /* not "IWI1"                       */
    IWI_FILE_ERR_ZERO_DIMENSION    = 4,  /* width or height is 0             */
    IWI_FILE_ERR_UNADDRESSABLE     = 5,  /* geometry exceeds int/stride range */
    IWI_FILE_ERR_TOO_LARGE         = 6,  /* geometry overflows size_t        */
    IWI_FILE_ERR_PAYLOAD_DECLARED  = 7,  /* pixelDataSize < width*h*comps    */
    IWI_FILE_ERR_PAYLOAD_TRUNCATED = 8,  /* buffer shorter than declared     */

    /* --- Valid IWI, outside this build's supported subset -------------- */
    IWI_FILE_ERR_PLANAR            = 20, /* storageLayout == PLANAR          */
    IWI_FILE_ERR_COMPONENT_COUNT   = 21, /* not 3 and not 4                  */
    IWI_FILE_ERR_COMPONENT_DEPTH   = 22, /* bitsPerPixel != components * 8   */
    IWI_FILE_ERR_COMPONENT_ORDER   = 23, /* expected R,G,B[,A] in order      */
    IWI_FILE_ERR_COMPONENT_CLASS   = 24, /* not UINT and not UNORM           */
    IWI_FILE_ERR_COMPONENT_LAYOUT  = 25  /* not 8 bits at offset i*8         */
} IwiFileStatus;

/*
 * A validated header.
 *
 * `pixelBytes` is what the file declares; `packedBytes` is what the geometry
 * actually needs (width * height * componentCount).  The declared value may be
 * larger — trailing slack is legal and ignored — but never smaller.
 */
typedef struct IwiFileHeader {
    uint32_t width;
    uint32_t height;
    uint8_t  colorSpace;      /* ImageColorSpace value, unvalidated            */
    uint16_t componentCount;  /* <= IW_MAX_COMPONENTS                          */
    uint16_t bitsPerPixel;
    uint8_t  storageLayout;   /* IW_STORAGE_INTERLEAVED or IW_STORAGE_PLANAR   */
    uint64_t pixelBytes;      /* as declared by the file                       */
    uint64_t packedBytes;     /* width * height * componentCount               */
    IWComponentDef components[IW_MAX_COMPONENTS];
} IwiFileHeader;

/*
 * Parse and validate the header of a container image.
 *
 * `data` / `size` are the whole container — the payload bounds check needs the
 * real length, so a caller that has only the first 74 bytes cannot use this to
 * accept a file.
 *
 * On IWI_FILE_OK, `*out` is fully populated.  On any other status, `*out` is
 * zeroed and must not be used.
 *
 * `error` / `errorSize` receive a human-readable reason when non-NULL and
 * errorSize > 0.  The wording is stable — ImageViewer's plugin ABI tests
 * assert substrings of it — and includes the offending values, because "not an
 * IWI" without the magic that was found is a support ticket.
 */
IWI_FILE_API IwiFileStatus iwi_file_parse_header(const uint8_t* data,
                                                 size_t size,
                                                 IwiFileHeader* out,
                                                 char* error,
                                                 size_t errorSize);

/*
 * How many payload bytes the geometry requires.  Returns 0 on overflow, which
 * is a value no valid image can have, so callers can test for it.
 */
IWI_FILE_API uint64_t iwi_file_required_bytes(uint32_t width,
                                              uint32_t height,
                                              uint16_t componentCount);

#ifdef __cplusplus
} /* extern "C" */

/*
 * The C++ view of the same constants.
 *
 * `inline constexpr` and not an unscoped enum: these are byte counts used in
 * pointer arithmetic and size comparisons, so they should be `std::size_t`, not
 * enumerators silently converting to int.  `enum class` would be wrong for the
 * opposite reason — every value here is used directly as an integer in offset
 * arithmetic, so every use would need a cast and gain nothing.
 *
 * The `#define`s above still exist and still expand; these are the same
 * numbers, so the two cannot disagree.
 */
namespace iwi_file {

inline constexpr char        kMagic[]        = IWI_FILE_MAGIC;
inline constexpr std::size_t kMagicBytes     = IWI_FILE_MAGIC_BYTES;
inline constexpr std::size_t kHeaderBytes    = IWI_FILE_HEADER_BYTES;
inline constexpr std::size_t kComponentBytes = IWI_FILE_COMPONENT_BYTES;
inline constexpr std::size_t kMaxComponents  = IWI_FILE_MAX_COMPONENTS;

inline constexpr std::size_t kOffMagic         = IWI_FILE_OFF_MAGIC;
inline constexpr std::size_t kOffWidth         = IWI_FILE_OFF_WIDTH;
inline constexpr std::size_t kOffHeight        = IWI_FILE_OFF_HEIGHT;
inline constexpr std::size_t kOffColorSpace    = IWI_FILE_OFF_COLOR_SPACE;
inline constexpr std::size_t kOffComponents    = IWI_FILE_OFF_COMPONENTS;
inline constexpr std::size_t kOffBitsPerPixel  = IWI_FILE_OFF_BITS_PER_PX;
inline constexpr std::size_t kOffStorage       = IWI_FILE_OFF_STORAGE;
inline constexpr std::size_t kOffComponentTab  = IWI_FILE_OFF_COMPONENT_TAB;
inline constexpr std::size_t kOffPixelBytes    = IWI_FILE_OFF_PIXEL_BYTES;

inline constexpr std::size_t kCompOffSemantic  = IWI_FILE_COMP_OFF_SEMANTIC;
inline constexpr std::size_t kCompOffClass     = IWI_FILE_COMP_OFF_CLASS;
inline constexpr std::size_t kCompOffBitOffset = IWI_FILE_COMP_OFF_BIT_OFFSET;
inline constexpr std::size_t kCompOffBitWidth  = IWI_FILE_COMP_OFF_BIT_WIDTH;

} // namespace iwi_file
#endif
