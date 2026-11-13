/*
 * IwiFile.c — the .iwi container format as a plain C API.
 *
 * See IwiFile.h for the contract.  This file is C99 and deliberately uses only
 * <stdint.h>/<stddef.h>/<stdio.h>/<string.h>, so a game engine can compile it
 * directly and a C++ module unit can include the header in its global module
 * fragment.
 *
 * # The one thing worth reading before editing
 *
 * The order of the checks below, and the wording of every message, are part of
 * the observable contract:
 *
 *   - ImageViewer's plugin ABI tests assert substrings of the reason text
 *     ("magic", "planar", "offset", "semantic", "class", "8-bit", "declares",
 *     "addressable", "dimensions", "header").
 *   - Tension's Rust reader makes the same malformed-vs-unsupported split
 *     (-EINVAL vs -ENOSYS) for the same reasons.
 *
 * So a check may move or be added, but the existing ones keep their order and
 * their words.  Changing either is a coordinated change across both repos, not
 * a local edit.
 */

#include "IwiFile.h"

#include <stdarg.h>
#include <stdio.h>
#include <string.h>

/*
 * The offsets and sizes live in IwiFile.h, not here.  A second copy in this
 * file is exactly the failure this API exists to stop: the four previous
 * readers each had their own, and they disagreed.
 */

/* Byte-wise little-endian reads.  Deliberately not memcpy into a u32: that
 * would inherit host endianness, and this container is little-endian
 * everywhere. */
static uint16_t iwi_rd16(const uint8_t* p)
{
    return (uint16_t)((uint16_t)p[0] | (uint16_t)((uint16_t)p[1] << 8u));
}

static uint32_t iwi_rd32(const uint8_t* p)
{
    return (uint32_t)p[0]
         | ((uint32_t)p[1] << 8u)
         | ((uint32_t)p[2] << 16u)
         | ((uint32_t)p[3] << 24u);
}

static uint64_t iwi_rd64(const uint8_t* p)
{
    uint64_t v = 0;
    unsigned i;
    for (i = 0; i < 8u; ++i)
        v |= (uint64_t)p[i] << (8u * i);
    return v;
}

/* Format a reason into the caller's buffer.  Safe with a NULL/zero buffer, so
 * every call site can pass its arguments through unconditionally. */
static void iwi_reason(char* error, size_t errorSize, const char* fmt, ...)
{
    va_list args;
    if (error == NULL || errorSize == 0u)
        return;
    va_start(args, fmt);
    (void)vsnprintf(error, errorSize, fmt, args);
    va_end(args);
}

IwiFileStatus iwi_file_parse_header(const uint8_t* data,
                                    size_t size,
                                    IwiFileHeader* out,
                                    char* error,
                                    size_t errorSize)
{
    uint8_t      storage;
    uint32_t     width;
    uint32_t     height;
    uint16_t     components;
    uint16_t     bitsPerPx;
    uint64_t     pixelBytes;
    uint64_t     pixels;
    uint64_t     expected;
    uint64_t     maxSize;
    uint16_t     i;

    if (out != NULL)
        memset(out, 0, sizeof(*out));
    if (error != NULL && errorSize > 0u)
        error[0] = '\0';

    if (data == NULL) {
        iwi_reason(error, errorSize, "No input");
        return IWI_FILE_ERR_NO_INPUT;
    }

    /* The magic is checked only after the length, because comparing four bytes
     * of a shorter buffer would read past it.  A file too short to hold the
     * header is broken; a long file with the wrong magic is not an IWI. */
    if (size < (size_t)IWI_FILE_HEADER_BYTES) {
        iwi_reason(error, errorSize,
                   "IWI is %zu bytes; the header alone is %zu",
                   size, (size_t)IWI_FILE_HEADER_BYTES);
        return IWI_FILE_ERR_TRUNCATED_HEADER;
    }

    if (memcmp(data + IWI_FILE_OFF_MAGIC, "IWI1", 4) != 0) {
        iwi_reason(error, errorSize,
                   "not an IWI1 container (magic mismatch)");
        return IWI_FILE_ERR_BAD_MAGIC;
    }

    width       = iwi_rd32(data + IWI_FILE_OFF_WIDTH);
    height      = iwi_rd32(data + IWI_FILE_OFF_HEIGHT);
    components  = iwi_rd16(data + IWI_FILE_OFF_COMPONENTS);
    bitsPerPx   = iwi_rd16(data + IWI_FILE_OFF_BITS_PER_PX);
    storage     = data[IWI_FILE_OFF_STORAGE];
    pixelBytes  = iwi_rd64(data + IWI_FILE_OFF_PIXEL_BYTES);

    /* --- Checks that make the bytes a structurally valid IWI ------------- */

    if (width == 0u || height == 0u) {
        iwi_reason(error, errorSize, "IWI dimensions are %ux%u", width, height);
        return IWI_FILE_ERR_ZERO_DIMENSION;
    }

    /* The component table has IW_MAX_COMPONENTS slots in a fixed-size header.
     * A count past that is not a layout this format can describe, whoever
     * wrote it. */
    if (components == 0u || components > (uint16_t)IW_MAX_COMPONENTS) {
        iwi_reason(error, errorSize,
                   "IWI declares %u components; the header has room for %u",
                   (unsigned)components, (unsigned)IW_MAX_COMPONENTS);
        return IWI_FILE_ERR_COMPONENT_COUNT;
    }

    /* ImagePluginData carries width/height/stride as int, and this API's
     * callers index with size_t, so the geometry has to fit before anything is
     * sized from it.  The multiplication is done in u64 and overflow-checked
     * rather than trusted: a header claiming 4 billion by 4 billion would
     * otherwise wrap into a small allocation and a huge copy. */
    if (width > 0x7FFFFFFFu || height > 0x7FFFFFFFu
        || (uint64_t)width > (uint64_t)0x7FFFFFFF / (uint64_t)components) {
        iwi_reason(error, errorSize,
                   "IWI dimensions %ux%u exceed the addressable range",
                   width, height);
        return IWI_FILE_ERR_UNADDRESSABLE;
    }

    pixels    = (uint64_t)width * (uint64_t)height;
    maxSize   = (uint64_t)(size_t)-1;
    if (pixels > maxSize / (uint64_t)components) {
        iwi_reason(error, errorSize,
                   "IWI geometry %ux%u x %u components is too large",
                   width, height, (unsigned)components);
        return IWI_FILE_ERR_TOO_LARGE;
    }

    expected = pixels * (uint64_t)components;

    if (pixelBytes < expected) {
        iwi_reason(error, errorSize,
                   "IWI declares %llu pixel bytes; %ux%u x %u components "
                   "needs %llu",
                   (unsigned long long)pixelBytes,
                   width, height, (unsigned)components,
                   (unsigned long long)expected);
        return IWI_FILE_ERR_PAYLOAD_DECLARED;
    }

    /* Validated without adding, so a declared pixelDataSize near UINT64_MAX
     * cannot wrap the comparison and pass. */
    if (pixelBytes > (uint64_t)size - (uint64_t)IWI_FILE_HEADER_BYTES) {
        iwi_reason(error, errorSize,
                   "IWI is %zu bytes; the header plus %llu declared pixel "
                   "bytes needs more",
                   size, (unsigned long long)pixelBytes);
        return IWI_FILE_ERR_PAYLOAD_TRUNCATED;
    }

    /* --- Layout checks: valid bytes, but is the layout one we serve? ----- */
    /*
     * These come after the structural checks on purpose.  A file that is both
     * planar and truncated should be reported as truncated: the structural
     * defect is the one the author has to fix first, and the layout may well
     * change when the file is rewritten.
     */

    if (storage != IW_STORAGE_INTERLEAVED) {
        iwi_reason(error, errorSize, "IWI planar storage is not supported");
        return IWI_FILE_ERR_PLANAR;
    }

    if (components != 3u && components != 4u) {
        iwi_reason(error, errorSize,
                   "IWI has %u components; only RGB8 (3) and RGBA8 (4) are "
                   "supported",
                   (unsigned)components);
        return IWI_FILE_ERR_COMPONENT_COUNT;
    }

    if (bitsPerPx != (uint16_t)(components * 8u)) {
        iwi_reason(error, errorSize,
                   "IWI declares %u bits per pixel for %u components; only "
                   "8-bit components are supported",
                   (unsigned)bitsPerPx, (unsigned)components);
        return IWI_FILE_ERR_COMPONENT_DEPTH;
    }

    /* Each component must be an 8-bit unsigned sample sitting exactly where
     * the interleaved layout puts it, with the semantics that make it colour.
     * Every one of these is a place a wrong guess would render a plausible but
     * incorrect image, so each is checked and named in the error. */
    for (i = 0; i < components; ++i) {
        static const uint8_t expectedSemantics[4] = {
            (uint8_t)IW_COMPONENT_SEMANTIC_R,
            (uint8_t)IW_COMPONENT_SEMANTIC_G,
            (uint8_t)IW_COMPONENT_SEMANTIC_B,
            (uint8_t)IW_COMPONENT_SEMANTIC_A
        };
        const uint8_t* rec = data + IWI_FILE_OFF_COMPONENT_TAB
                           + (size_t)i * (size_t)IWI_FILE_COMPONENT_BYTES;
        const uint8_t  semantic = rec[IWI_FILE_COMP_OFF_SEMANTIC];
        const uint8_t  cls      = rec[IWI_FILE_COMP_OFF_CLASS];
        const uint16_t bitOff   = iwi_rd16(rec + IWI_FILE_COMP_OFF_BIT_OFFSET);
        const uint16_t bitWidth = iwi_rd16(rec + IWI_FILE_COMP_OFF_BIT_WIDTH);

        if (semantic != expectedSemantics[i]) {
            iwi_reason(error, errorSize,
                       "IWI component %u has semantic %u; expected R, G, B%s "
                       "in that order",
                       (unsigned)i, (unsigned)semantic,
                       components == 4u ? ", A" : "");
            return IWI_FILE_ERR_COMPONENT_ORDER;
        }

        if (cls != (uint8_t)IW_COMPONENT_CLASS_UINT
            && cls != (uint8_t)IW_COMPONENT_CLASS_UNORM) {
            iwi_reason(error, errorSize,
                       "IWI component %u has class %u; only UINT (0) and "
                       "UNORM (2) are understood",
                       (unsigned)i, (unsigned)cls);
            return IWI_FILE_ERR_COMPONENT_CLASS;
        }

        if (bitWidth != 8u || bitOff != (uint16_t)(i * 8u)) {
            iwi_reason(error, errorSize,
                       "IWI component %u is %u bits at offset %u; expected "
                       "8 bits at offset %u",
                       (unsigned)i, (unsigned)bitWidth, (unsigned)bitOff,
                       (unsigned)(i * 8u));
            return IWI_FILE_ERR_COMPONENT_LAYOUT;
        }

        if (out != NULL) {
            out->components[i].semantic       =
                (IWComponentSemantic)semantic;
            out->components[i].componentClass =
                (IWComponentClass)cls;
            out->components[i].bitOffset      = bitOff;
            out->components[i].bitWidth       = bitWidth;
        }
    }

    if (out != NULL) {
        out->width          = width;
        out->height         = height;
        out->colorSpace     = data[IWI_FILE_OFF_COLOR_SPACE];
        out->componentCount = components;
        out->bitsPerPixel   = bitsPerPx;
        out->storageLayout  = storage;
        out->pixelBytes     = pixelBytes;
        out->packedBytes    = expected;
    }

    return IWI_FILE_OK;
}

uint64_t iwi_file_required_bytes(uint32_t width,
                                 uint32_t height,
                                 uint16_t componentCount)
{
    const uint64_t pixels = (uint64_t)width * (uint64_t)height;
    const uint64_t maxSize = (uint64_t)(size_t)-1;

    if (componentCount == 0u)
        return 0;
    if (pixels != 0u && (uint64_t)componentCount > maxSize / pixels)
        return 0;   /* overflow: not a size any image can have */
    return pixels * (uint64_t)componentCount;
}
