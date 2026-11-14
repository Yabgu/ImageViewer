/*
 * test_abi_constants.cpp - compile-time pinning of the C ABI's numbers.
 *
 * ImagePluginDef.h and IwiFile.h each write several values twice: once as an
 * enumerator for C++ and once as a `#define` for C.  That duplication is forced
 * by the languages, and this file is half of what stops it drifting.  The other
 * half is the CI C11 gate, which _Static_asserts the same literals against the
 * C macros.
 *
 * Everything here is a static_assert, so a mismatch is a build failure rather
 * than a runtime surprise.  main() exists only so ctest has something to run;
 * if this file compiles, every claim in it already holds.
 */

#include "ImagePluginDef.h"
#include "IwiFile.h"

#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <type_traits>

namespace {

/* -- The component record ------------------------------------------------- */

/* IWComponentDef (in memory) and IwiFileComponent (on disk) are the same six
 * bytes.  The loader relies on this: it points IWImageFormat::components
 * straight at the parsed table instead of translating record by record, which
 * is what the old eight-slot struct forced. */
static_assert(sizeof(IWComponentDef) == sizeof(IwiFileComponent),
              "in-memory and on-disk component records are the same size");
static_assert(sizeof(IwiFileComponent) == IWI_FILE_COMPONENT_BYTES,
              "component record matches the documented stride");

static_assert(offsetof(IWComponentDef, semantic) == 0);
static_assert(offsetof(IWComponentDef, componentClass) == 1);
static_assert(offsetof(IWComponentDef, bitOffset) == 2);
static_assert(offsetof(IWComponentDef, bitWidth) == 4);

static_assert(offsetof(IwiFileComponent, semantic) == 0, "on-disk semantic byte");
static_assert(offsetof(IwiFileComponent, componentClass) == 1, "on-disk class byte");
static_assert(offsetof(IwiFileComponent, bitOffset) == 2, "on-disk bit offset");
static_assert(offsetof(IwiFileComponent, bitWidth) == 4, "on-disk bit width");

static_assert(sizeof(IWComponentDef) == 6, "no padding in the in-memory record");

/* -- The base header ------------------------------------------------------ */

static_assert(offsetof(IwiFileHeader, magic) == 0, "magic at 0");
static_assert(offsetof(IwiFileHeader, width) == 4, "width at 4");
static_assert(offsetof(IwiFileHeader, height) == 8, "height at 8");
static_assert(offsetof(IwiFileHeader, metaBytes) == 12, "metaBytes at 12");
static_assert(offsetof(IwiFileHeader, storageLayout) == 16, "storageLayout at 16");
static_assert(offsetof(IwiFileHeader, payloadEncoding) == 24, "payloadEncoding at 24");
static_assert(offsetof(IwiFileHeader, colorSpace) == 32, "colorSpace at 32");
static_assert(offsetof(IwiFileHeader, reserved) == 33, "reserved at 33");
static_assert(offsetof(IwiFileHeader, componentCount) == 34, "componentCount at 34");

/* The last field must end exactly where the component table begins. */
static_assert(offsetof(IwiFileHeader, componentCount) + sizeof(uint16_t)
                  == IWI_FILE_BASE_HEADER_BYTES,
              "the base header ends exactly where the component table starts");

/* sizeof is larger than the wire header because the 64-bit layout descriptor
 * gives the struct alignment 8 and it is padded to a multiple of that.  The
 * parser copies IWI_FILE_BASE_HEADER_BYTES, never sizeof, so this is only a
 * fact to be aware of - asserted so nobody assumes the two are equal. */
static_assert(sizeof(IwiFileHeader) >= IWI_FILE_BASE_HEADER_BYTES);
static_assert(alignof(IwiFileHeader) == alignof(IWStorageLayout));

/* The component table starts on a boundary a component can be read at. */
static_assert(IWI_FILE_BASE_HEADER_BYTES % alignof(IwiFileComponent) == 0,
              "component table is aligned for IwiFileComponent");

static_assert(sizeof(IWStorageLayout) == 8, "flags + stride is eight bytes");
static_assert(sizeof(IwiMetaHeader) == 8, "tag + size is eight bytes");

static_assert(IWI_FILE_MAGIC_BYTES == 4u, "magic is four bytes");
static_assert(IWI_FILE_MIN_COMPONENTS >= 1u, "a container has at least one component");

/* -- The enumeration literals --------------------------------------------- */

/*
 * Each enumerator against the literal the C path defines.  The C11 gate
 * asserts the same literals against the macros, so the two definitions agree by
 * transitivity through this number.
 */

static_assert(static_cast<int>(IMAGE_PLUGIN_OK) == 0);
static_assert(static_cast<int>(IMAGE_PLUGIN_FILE_ERROR) == 1);
static_assert(static_cast<int>(IMAGE_PLUGIN_DECODE_ERROR) == 2);
static_assert(static_cast<int>(IMAGE_PLUGIN_ALLOC_ERROR) == 3);
static_assert(static_cast<int>(IMAGE_PLUGIN_UNKNOWN_ERROR) == 255);

static_assert(static_cast<int>(IMAGE_COLOR_SPACE_SRGB) == 0);
static_assert(static_cast<int>(IMAGE_COLOR_SPACE_LINEAR) == 1);
static_assert(static_cast<int>(IMAGE_COLOR_SPACE_HDR_PQ) == 2);
static_assert(static_cast<int>(IMAGE_COLOR_SPACE_HDR_HLG) == 3);
static_assert(static_cast<int>(IMAGE_COLOR_SPACE_UNKNOWN) == 255);

static_assert(static_cast<int>(IW_COMPONENT_CLASS_UINT) == 0);
static_assert(static_cast<int>(IW_COMPONENT_CLASS_SINT) == 1);
static_assert(static_cast<int>(IW_COMPONENT_CLASS_UNORM) == 2);
static_assert(static_cast<int>(IW_COMPONENT_CLASS_SNORM) == 3);
static_assert(static_cast<int>(IW_COMPONENT_CLASS_FLOAT) == 4);

static_assert(static_cast<int>(IW_COMPONENT_SEMANTIC_NONE) == 0);
static_assert(static_cast<int>(IW_COMPONENT_SEMANTIC_R) == 1);
static_assert(static_cast<int>(IW_COMPONENT_SEMANTIC_G) == 2);
static_assert(static_cast<int>(IW_COMPONENT_SEMANTIC_B) == 3);
static_assert(static_cast<int>(IW_COMPONENT_SEMANTIC_A) == 4);
static_assert(static_cast<int>(IW_COMPONENT_SEMANTIC_H) == 5);
static_assert(static_cast<int>(IW_COMPONENT_SEMANTIC_S) == 6);
static_assert(static_cast<int>(IW_COMPONENT_SEMANTIC_L) == 7);
static_assert(static_cast<int>(IW_COMPONENT_SEMANTIC_GRAY) == 8);
static_assert(static_cast<int>(IW_COMPONENT_SEMANTIC_UNKNOWN) == 255);

static_assert(static_cast<uint32_t>(IW_STORAGE_FLAG_INTERLEAVED) == 0u);
static_assert(static_cast<uint32_t>(IW_STORAGE_FLAG_PLANAR) == (1u << 0));
static_assert(static_cast<uint32_t>(IW_STORAGE_FLAG_COLUMNAR) == (1u << 1));
static_assert(static_cast<uint32_t>(IW_STORAGE_FLAG_FLIP_X) == (1u << 2));
static_assert(static_cast<uint32_t>(IW_STORAGE_FLAG_FLIP_Y) == (1u << 3));
static_assert(static_cast<uint32_t>(IW_STORAGE_FLAG_STRIDE_8BIT) == (1u << 4));
static_assert(static_cast<uint32_t>(IW_STORAGE_FLAG_STRIDE_16BIT) == (1u << 5));
static_assert(static_cast<uint32_t>(IW_STORAGE_FLAG_STRIDE_32BIT) == (1u << 6));

/* The storage flags are distinct bits: an "is planar" test that used equality
 * would be wrong for any combination. */
static_assert((static_cast<uint32_t>(IW_STORAGE_FLAG_FLIP_X) &
               static_cast<uint32_t>(IW_STORAGE_FLAG_FLIP_Y)) == 0u);

/* The C status codes keep OK at zero so `if (rc)` reads correctly. */
static_assert(static_cast<int>(IWI_STATUS_OK) == 0);
static_assert(static_cast<int>(IWI_STATUS_BAD_MAGIC) != 0);
static_assert(static_cast<int>(IWI_STATUS_UNSUPPORTED_STORAGE) != 0);

/* The metadata tags are the four-character codes the format documents. */
static_assert(IWI_META_TAG_CAMERA == 0x414D4143u);
static_assert(IWI_META_TAG_CBOR   == 0x524F4243u);
static_assert(IWI_META_TAG_BSON   == 0x4E4F5342u);
static_assert(IWI_META_TAG_MSGP   == 0x5047534Du);
static_assert(IWI_META_TAG_JSON   == 0x4E4F534Au);
static_assert(IWI_META_TAG_TEXT   == 0x54584554u);

/* The enumerations really are one byte on the wire. */
static_assert(sizeof(ImagePluginResultCode) == 1);
static_assert(sizeof(ImageColorSpace) == 1);
static_assert(sizeof(IWComponentClass) == 1);
static_assert(sizeof(IWComponentSemantic) == 1);
static_assert(sizeof(IWStorageFlags) == 4);

/* The common component tables have the sizes their names claim. */
static_assert(sizeof(iw::kGray8Components) == 1 * sizeof(IWComponentDef));
static_assert(sizeof(iw::kGrayAlpha16Components) == 2 * sizeof(IWComponentDef));
static_assert(sizeof(iw::kRgb8Components) == 3 * sizeof(IWComponentDef));
static_assert(sizeof(iw::kRgba8Components) == 4 * sizeof(IWComponentDef));
static_assert(iw::kGrayAlpha16Components[0].bitWidth == 16);
static_assert(iw::kGrayAlpha16Components[1].bitOffset == 16);

} // namespace

int main()
{
    std::printf("abi constants: layout and enumeration assertions hold\n");
    return 0;
}
