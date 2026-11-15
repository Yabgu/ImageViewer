/*
 * iwi_c_gate_body.h - the body of the C11 compatibility gate.
 *
 * This is not a header anybody includes in production and it is not a source
 * file in its own right: it is the payload of a translation unit that the gate
 * manufactures, either in the build tree (src/tests/CMakeLists.txt) or in a
 * temporary directory (ci/check-iwi-c-header.sh).  Keeping the assertions here
 * means both routes compile the same text - a gate that checks a different
 * thing depending on who ran it is not a gate.
 *
 * It must be valid C11 with no compiler extensions, because that is the point:
 * ImagePluginDef.h and IwiFile.h claim to be C headers, Tension binds the
 * format through FFI, and a C engine would consume it the same way.
 *
 * The translation unit that includes this must be compiled as C, not C++.
 * Including it from C++ would defeat the entire exercise.
 */

#include <stddef.h>
#include <stdint.h>

#include "ImagePluginDef.h"
#include "IwiFile.h"

/* -- Container constants --------------------------------------------------- */

_Static_assert(IWI_FILE_MAGIC_BYTES == 4u, "magic is four bytes");
_Static_assert(IWI_FILE_BASE_HEADER_BYTES == 36u, "base header is 36 bytes");
_Static_assert(IWI_FILE_COMPONENT_BYTES == 6u, "component stride is 6 bytes");
_Static_assert(IWI_FILE_MIN_COMPONENTS == 1u, "at least one component");

/* -- Result codes ---------------------------------------------------------- */

_Static_assert(IMAGE_PLUGIN_OK == 0, "ok is zero");
_Static_assert(IMAGE_PLUGIN_FILE_ERROR == 1, "file error is 1");
_Static_assert(IMAGE_PLUGIN_DECODE_ERROR == 2, "decode error is 2");
_Static_assert(IMAGE_PLUGIN_ALLOC_ERROR == 3, "alloc error is 3");
_Static_assert(IMAGE_PLUGIN_UNKNOWN_ERROR == 255, "unknown is 255");

/* -- Colour space ---------------------------------------------------------- */

_Static_assert(IMAGE_COLOR_SPACE_SRGB == 0, "sRGB is zero");
_Static_assert(IMAGE_COLOR_SPACE_LINEAR == 1, "linear is 1");
_Static_assert(IMAGE_COLOR_SPACE_HDR_PQ == 2, "PQ is 2");
_Static_assert(IMAGE_COLOR_SPACE_HDR_HLG == 3, "HLG is 3");
_Static_assert(IMAGE_COLOR_SPACE_UNKNOWN == 255, "unknown is 255");

/* -- Component class ------------------------------------------------------- */

_Static_assert(IW_COMPONENT_CLASS_UINT == 0, "UINT is zero");
_Static_assert(IW_COMPONENT_CLASS_SINT == 1, "SINT is 1");
_Static_assert(IW_COMPONENT_CLASS_UNORM == 2, "UNORM is 2");
_Static_assert(IW_COMPONENT_CLASS_SNORM == 3, "SNORM is 3");
_Static_assert(IW_COMPONENT_CLASS_FLOAT == 4, "FLOAT is 4");

/* -- Component semantic ---------------------------------------------------- */

_Static_assert(IW_COMPONENT_SEMANTIC_NONE == 0, "NONE is zero");
_Static_assert(IW_COMPONENT_SEMANTIC_R == 1, "R is 1");
_Static_assert(IW_COMPONENT_SEMANTIC_G == 2, "G is 2");
_Static_assert(IW_COMPONENT_SEMANTIC_B == 3, "B is 3");
_Static_assert(IW_COMPONENT_SEMANTIC_A == 4, "A is 4");
_Static_assert(IW_COMPONENT_SEMANTIC_H == 5, "H is 5");
_Static_assert(IW_COMPONENT_SEMANTIC_S == 6, "S is 6");
_Static_assert(IW_COMPONENT_SEMANTIC_L == 7, "L is 7");
_Static_assert(IW_COMPONENT_SEMANTIC_GRAY == 8, "GRAY is 8");
_Static_assert(IW_COMPONENT_SEMANTIC_UNKNOWN == 255, "unknown is 255");

/* -- Storage flags are bits, not ordinals ---------------------------------- */

_Static_assert(IW_STORAGE_FLAG_INTERLEAVED == 0u, "interleaved is zero");
_Static_assert(IW_STORAGE_FLAG_PLANAR == (1u << 0), "planar is bit 0");
_Static_assert(IW_STORAGE_FLAG_COLUMNAR == (1u << 1), "columnar is bit 1");
_Static_assert(IW_STORAGE_FLAG_FLIP_X == (1u << 2), "flip x is bit 2");
_Static_assert(IW_STORAGE_FLAG_FLIP_Y == (1u << 3), "flip y is bit 3");
_Static_assert(IW_STORAGE_FLAG_STRIDE_8BIT == (1u << 4), "stride8 is bit 4");
_Static_assert(IW_STORAGE_FLAG_STRIDE_16BIT == (1u << 5), "stride16 is bit 5");
_Static_assert(IW_STORAGE_FLAG_STRIDE_32BIT == (1u << 6), "stride32 is bit 6");

/* -- Status codes ---------------------------------------------------------- */

_Static_assert(IWI_STATUS_OK == 0, "ok is zero");
_Static_assert(IWI_STATUS_BAD_MAGIC != 0, "bad magic is a failure");
_Static_assert(IWI_STATUS_UNSUPPORTED != 0, "unsupported is a failure");

/* -- Metadata tags --------------------------------------------------------- */

_Static_assert(IWI_META_TAG_CAMERA == 0x414D4143u, "CAMA tag");
_Static_assert(IWI_META_TAG_CBOR == 0x524F4243u, "CBOR tag");
_Static_assert(IWI_META_TAG_BSON == 0x4E4F5342u, "BSON tag");
_Static_assert(IWI_META_TAG_MSGP == 0x5047534Du, "MSGP tag");
_Static_assert(IWI_META_TAG_JSON == 0x4E4F534Au, "JSON tag");
_Static_assert(IWI_META_TAG_TEXT == 0x54584554u, "TEXT tag");

/* -- The on-disk record, as C sees it -------------------------------------- */

struct IwiGateProbe {
    char            magic[4];
    uint32_t        width;
    uint32_t        height;
    uint32_t        metaBytes;
    IWStorageLayout storageLayout;
    char            payloadEncoding[8];
    uint8_t         colorSpace;
    uint8_t         reserved;
    uint16_t        componentCount;
};

_Static_assert(sizeof(struct IwiGateProbe) >= IWI_FILE_BASE_HEADER_BYTES,
               "the base header fields fit in 36 bytes");
_Static_assert(offsetof(struct IwiGateProbe, componentCount) + 2
                   == IWI_FILE_BASE_HEADER_BYTES,
               "the last base field ends at 36");
_Static_assert(sizeof(IwiFileComponent) == IWI_FILE_COMPONENT_BYTES,
               "the component record is exactly six bytes");
_Static_assert(offsetof(IwiFileComponent, semantic) == 0, "semantic at 0");
_Static_assert(offsetof(IwiFileComponent, componentClass) == 1, "class at 1");
_Static_assert(offsetof(IwiFileComponent, bitOffset) == 2, "bit offset at 2");
_Static_assert(offsetof(IwiFileComponent, bitWidth) == 4, "bit width at 4");

/* -- Every exported function is callable from C ---------------------------- */

/*
 * Taking the address of each function fails to compile if a signature has grown
 * a type C cannot express.  This is the check that would have caught the five
 * C23 enum declarations and any `std::` that crept into a parameter.
 */
static size_t (*const gate_required_size)(const uint8_t*, size_t) =
    &iwi_file_required_size;
static int (*const gate_peek)(const uint8_t*, size_t, uint16_t*) =
    &iwi_file_peek_components;
static int (*const gate_parse_structural)(IwiFileHeader*, size_t,
                                          const uint8_t*, size_t) =
    &iwi_parse_structural;
static int (*const gate_parse_prefix)(IwiFileHeader*, size_t,
                                      const uint8_t*, size_t, size_t) =
    &iwi_parse_prefix;
static int (*const gate_encoding)(const IwiFileHeader*) =
    &iwi_payload_encoding_is_raw;
static int (*const gate_decode)(const IwiFileHeader*, const uint8_t*, size_t,
                                uint8_t*, size_t) = &iwi_decode;
static int (*const gate_metadata)(const IwiFileHeader*, const uint8_t*, size_t,
                                  const uint8_t**, size_t*) = &iwi_file_metadata;
static void (*const gate_free)(void*) = &iwi_file_free;
static const char* (*const gate_message)(int) = &iwi_status_message;
static int (*const gate_is_unsupported)(int) = &iwi_status_is_unsupported;

int iwi_c_gate_touch(void);
int iwi_c_gate_touch(void)
{
    const IwiFileComponent* (*accessor)(const IwiFileHeader*) =
        &iwi_file_components;

    return (int)(gate_required_size != 0) + (gate_peek != 0)
         + (gate_parse_structural != 0) + (gate_parse_prefix != 0)
         + (gate_encoding != 0) + (gate_decode != 0)
         + (gate_metadata != 0) + (gate_free != 0)
         + (gate_message != 0) + (gate_is_unsupported != 0)
         + (accessor != 0);
}
