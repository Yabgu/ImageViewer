#pragma once

/*
 * ImagePluginDef.h - the loader plugin ABI.
 *
 * This header is consumed by both C++ (the viewer, the loaders) and C
 * (embedders, FFI, and the CI gate).  The two standard headers that differ are
 * selected here rather than assuming C++: the plugin ABI is a C ABI, and a
 * header that only compiles as C++ cannot be handed to a C caller.
 *
 * # The enumeration split
 *
 * These declarations used to be `enum X : uint8_t`, which is a C23 feature
 * (and a Microsoft C extension).  The CI gate compiles this header as C11 and
 * rejected five of them, so each enumeration is now written twice:
 *
 *   - C++: a real enum with a fixed underlying type, so enumerator names are
 *     typed and usable in constant expressions.
 *   - C:   a typedef of the underlying integer plus `#define`d values.
 *
 * Neither language accepts the other's form, so the duplication is forced.
 * It is pinned from both sides rather than trusted: src/tests/test_abi_constants.cpp
 * static_asserts every enumerator against its literal as C++, and the CI gate
 * _Static_asserts every macro against the same literal as C11.  A value edited
 * on one side and not the other fails one of the two builds.
 */

#ifdef __cplusplus
#include <cstdint>
#include <cstddef>
#else
#include <stdint.h>
#include <stddef.h>
#endif

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Forward typedefs, so a C caller can name every type this ABI exposes.
 *
 * C has no implicit struct-to-typedef promotion: without these, a C caller can
 * write `struct ImagePluginData*` but not `ImagePluginData*`, and the exported
 * function typedefs below are unusable.  The definitions follow; the layouts
 * are unchanged and the tags are the same ones the definitions use.
 */
typedef struct IWComponentDef     IWComponentDef;
typedef struct IWImageFormat      IWImageFormat;
typedef struct ImagePluginData    ImagePluginData;
typedef struct ImagePluginResult  ImagePluginResult;

/* Export macro for cross-platform symbol visibility */
#ifndef IMAGEPLUGIN_API
#ifdef _WIN32
#define IMAGEPLUGIN_API __declspec(dllexport)
#else
#define IMAGEPLUGIN_API __attribute__((visibility("default")))
#endif
#endif

/* -- Result code ---------------------------------------------------------- */

#ifdef __cplusplus
typedef enum ImagePluginResultCode : uint8_t {
    IMAGE_PLUGIN_OK            = 0,
    IMAGE_PLUGIN_FILE_ERROR    = 1,
    IMAGE_PLUGIN_DECODE_ERROR  = 2,
    IMAGE_PLUGIN_ALLOC_ERROR   = 3,
    IMAGE_PLUGIN_UNKNOWN_ERROR = 255
} ImagePluginResultCode;
#else
typedef uint8_t ImagePluginResultCode;
#define IMAGE_PLUGIN_OK            ((ImagePluginResultCode)0)
#define IMAGE_PLUGIN_FILE_ERROR    ((ImagePluginResultCode)1)
#define IMAGE_PLUGIN_DECODE_ERROR  ((ImagePluginResultCode)2)
#define IMAGE_PLUGIN_ALLOC_ERROR   ((ImagePluginResultCode)3)
#define IMAGE_PLUGIN_UNKNOWN_ERROR ((ImagePluginResultCode)255)
#endif

/* -- Colour space / transfer function ------------------------------------- */

#ifdef __cplusplus
typedef enum ImageColorSpace : uint8_t {
    IMAGE_COLOR_SPACE_SRGB    = 0,   /* Standard sRGB (gamma ~2.2)       */
    IMAGE_COLOR_SPACE_LINEAR  = 1,   /* Linear light, no gamma encoding  */
    IMAGE_COLOR_SPACE_HDR_PQ  = 2,   /* SMPTE ST.2084 PQ (HDR10)         */
    IMAGE_COLOR_SPACE_HDR_HLG = 3,   /* Hybrid Log-Gamma (HLG)           */
    IMAGE_COLOR_SPACE_UNKNOWN = 255  /* Unrecognised / not signalled     */
} ImageColorSpace;
#else
typedef uint8_t ImageColorSpace;
#define IMAGE_COLOR_SPACE_SRGB    ((ImageColorSpace)0)
#define IMAGE_COLOR_SPACE_LINEAR  ((ImageColorSpace)1)
#define IMAGE_COLOR_SPACE_HDR_PQ  ((ImageColorSpace)2)
#define IMAGE_COLOR_SPACE_HDR_HLG ((ImageColorSpace)3)
#define IMAGE_COLOR_SPACE_UNKNOWN ((ImageColorSpace)255)
#endif

/* -- Per-component class: how stored bits are interpreted ------------------ */

#ifdef __cplusplus
typedef enum IWComponentClass : uint8_t {
    IW_COMPONENT_CLASS_UINT  = 0,  /* unsigned integer, raw value                   */
    IW_COMPONENT_CLASS_SINT  = 1,  /* signed integer, raw value                     */
    IW_COMPONENT_CLASS_UNORM = 2,  /* unsigned integer normalised to [0, 1]         */
    IW_COMPONENT_CLASS_SNORM = 3,  /* signed integer normalised to [-1, 1]          */
    IW_COMPONENT_CLASS_FLOAT = 4   /* IEEE floating-point (16-bit or 32-bit)        */
} IWComponentClass;
#else
typedef uint8_t IWComponentClass;
#define IW_COMPONENT_CLASS_UINT  ((IWComponentClass)0)
#define IW_COMPONENT_CLASS_SINT  ((IWComponentClass)1)
#define IW_COMPONENT_CLASS_UNORM ((IWComponentClass)2)
#define IW_COMPONENT_CLASS_SNORM ((IWComponentClass)3)
#define IW_COMPONENT_CLASS_FLOAT ((IWComponentClass)4)
#endif

/* -- Per-component semantic: what a component represents ------------------- */

#ifdef __cplusplus
typedef enum IWComponentSemantic : uint8_t {
    /*
     * IW_COMPONENT_SEMANTIC_NONE = 0 is RESERVED.
     *
     * Used as a sentinel for unused or padding slots.
     */
    IW_COMPONENT_SEMANTIC_NONE    = 0,   /* reserved - unused / padding slot     */

    IW_COMPONENT_SEMANTIC_R       = 1,
    IW_COMPONENT_SEMANTIC_G       = 2,
    IW_COMPONENT_SEMANTIC_B       = 3,
    IW_COMPONENT_SEMANTIC_A       = 4,
    IW_COMPONENT_SEMANTIC_H       = 5,   /* hue                                  */
    IW_COMPONENT_SEMANTIC_S       = 6,   /* saturation                           */
    IW_COMPONENT_SEMANTIC_L       = 7,   /* lightness / luminance                */
    IW_COMPONENT_SEMANTIC_GRAY    = 8,
    IW_COMPONENT_SEMANTIC_UNKNOWN = 255  /* active component, semantic unspecified */
} IWComponentSemantic;
#else
typedef uint8_t IWComponentSemantic;
#define IW_COMPONENT_SEMANTIC_NONE    ((IWComponentSemantic)0)
#define IW_COMPONENT_SEMANTIC_R       ((IWComponentSemantic)1)
#define IW_COMPONENT_SEMANTIC_G       ((IWComponentSemantic)2)
#define IW_COMPONENT_SEMANTIC_B       ((IWComponentSemantic)3)
#define IW_COMPONENT_SEMANTIC_A       ((IWComponentSemantic)4)
#define IW_COMPONENT_SEMANTIC_H       ((IWComponentSemantic)5)
#define IW_COMPONENT_SEMANTIC_S       ((IWComponentSemantic)6)
#define IW_COMPONENT_SEMANTIC_L       ((IWComponentSemantic)7)
#define IW_COMPONENT_SEMANTIC_GRAY    ((IWComponentSemantic)8)
#define IW_COMPONENT_SEMANTIC_UNKNOWN ((IWComponentSemantic)255)
#endif

/* -- Per-component layout descriptor -------------------------------------- */

struct IWComponentDef {
    IWComponentSemantic semantic;       /* what this component represents        */
    IWComponentClass    componentClass; /* how the stored bits are interpreted   */
    uint16_t            bitOffset;      /* bit offset from the start of a pixel (interleaved only) */
    uint16_t            bitWidth;       /* number of bits occupied               */
};

/* -- Storage layout ------------------------------------------------------- */

/*
 * Orthogonal layout flags.  A bitmask, never an enumeration to compare with
 * `==`: combinations are meaningful and two files can differ in one bit.
 */
#ifdef __cplusplus
typedef enum IWStorageFlags : uint32_t {
    IW_STORAGE_FLAG_INTERLEAVED  = 0u,       /* default: components packed per pixel       */
    IW_STORAGE_FLAG_PLANAR       = (1u << 0),/* one contiguous plane per component         */
    IW_STORAGE_FLAG_COLUMNAR     = (1u << 1),/* column-major order (default: row-major)    */
    IW_STORAGE_FLAG_FLIP_X       = (1u << 2),/* origin right (traversal right-to-left)     */
    IW_STORAGE_FLAG_FLIP_Y       = (1u << 3),/* origin bottom (invert by design / OpenGL)  */
    IW_STORAGE_FLAG_STRIDE_8BIT  = (1u << 4),/* row stride aligned to 8-bit (1B) boundary  */
    IW_STORAGE_FLAG_STRIDE_16BIT = (1u << 5),/* row stride aligned to 16-bit (2B) boundary */
    IW_STORAGE_FLAG_STRIDE_32BIT = (1u << 6),/* row stride aligned to 32-bit (4B) boundary */
} IWStorageFlags;
#else
typedef uint32_t IWStorageFlags;
#define IW_STORAGE_FLAG_INTERLEAVED  ((IWStorageFlags)0u)
#define IW_STORAGE_FLAG_PLANAR       ((IWStorageFlags)(1u << 0))
#define IW_STORAGE_FLAG_COLUMNAR     ((IWStorageFlags)(1u << 1))
#define IW_STORAGE_FLAG_FLIP_X       ((IWStorageFlags)(1u << 2))
#define IW_STORAGE_FLAG_FLIP_Y       ((IWStorageFlags)(1u << 3))
#define IW_STORAGE_FLAG_STRIDE_8BIT  ((IWStorageFlags)(1u << 4))
#define IW_STORAGE_FLAG_STRIDE_16BIT ((IWStorageFlags)(1u << 5))
#define IW_STORAGE_FLAG_STRIDE_32BIT ((IWStorageFlags)(1u << 6))
#endif

/* Camera / EXIF orientation convenience aliases (mappings to traversal flags) */
#define IW_STORAGE_ORIENT_NORMAL     0u
#define IW_STORAGE_ORIENT_MIRROR_H   IW_STORAGE_FLAG_FLIP_X
#define IW_STORAGE_ORIENT_ROTATE_180 (IW_STORAGE_FLAG_FLIP_X | IW_STORAGE_FLAG_FLIP_Y)
#define IW_STORAGE_ORIENT_MIRROR_V   IW_STORAGE_FLAG_FLIP_Y
#define IW_STORAGE_ORIENT_TRANSPOSE  IW_STORAGE_FLAG_COLUMNAR
#define IW_STORAGE_ORIENT_ROTATE_90  (IW_STORAGE_FLAG_COLUMNAR | IW_STORAGE_FLAG_FLIP_X)
#define IW_STORAGE_ORIENT_TRANSVERSE (IW_STORAGE_FLAG_COLUMNAR | IW_STORAGE_FLAG_FLIP_X | IW_STORAGE_FLAG_FLIP_Y)
#define IW_STORAGE_ORIENT_ROTATE_270 (IW_STORAGE_FLAG_COLUMNAR | IW_STORAGE_FLAG_FLIP_Y)

#define IW_STORAGE_INTERLEAVED 0u
#define IW_STORAGE_PLANAR      1u

/*
 * 64-bit layout descriptor: 32 bits of flags, then a 32-bit row stride in
 * bytes (0 = tightly packed / derived).
 *
 * The anonymous struct member is accepted by both C11 and C++.  In C++ the
 * union also carries constructors and flag predicates; in C it is a plain
 * union and the same questions are answered with IW_STORAGE_LAYOUT_HAS below.
 */
typedef union IWStorageLayout {
    uint64_t raw;
    struct {
        uint32_t flags;   /* Bitmask of IWStorageFlags */
        uint32_t stride;  /* Row stride in bytes (0 = tightly packed / derived) */
    };
#ifdef __cplusplus
    constexpr IWStorageLayout() : raw(0) {}
    constexpr IWStorageLayout(uint64_t v) : raw(v) {}
    constexpr IWStorageLayout(uint32_t f, uint32_t s = 0) : flags(f), stride(s) {}
    constexpr bool operator==(const IWStorageLayout& o) const noexcept { return raw == o.raw; }
    constexpr bool operator!=(const IWStorageLayout& o) const noexcept { return raw != o.raw; }
    constexpr bool isPlanar() const noexcept
    {
        return (flags & static_cast<uint32_t>(IW_STORAGE_FLAG_PLANAR)) != 0u;
    }
    constexpr bool isInterleaved() const noexcept { return !isPlanar(); }
#endif
} IWStorageLayout;

/* Flag test usable from C: the layout is a bitmask, so this is a bit test. */
#define IW_STORAGE_LAYOUT_HAS(layout, flag) \
    ((((layout).flags) & (uint32_t)(flag)) != 0u)

/*
 * Complete format descriptor for a single image.
 *
 * Every component is described independently, so mixed-precision and
 * non-RGB layouts (HSL, UV, infrared+visible+depth, ...) are expressible
 * without adding new enumerators.  There is no component cap: `components`
 * points at componentCount descriptors, and ownership belongs to whatever
 * built the descriptor.
 *
 * bitsPerPixel is the total number of bits consumed per logical pixel
 * for IW_STORAGE_INTERLEAVED, or per sample (one component of one pixel)
 * for IW_STORAGE_PLANAR.
 */
struct IWImageFormat {
    uint16_t              componentCount;                 /* number of active components                        */
    uint16_t              bitsPerPixel;                   /* bits per pixel (interleaved) / per sample (planar) */
    IWStorageLayout       storageLayout;                  /* how components are arranged in memory              */
    const IWComponentDef* components;                     /* pointer to per-component descriptors               */
#ifdef __cplusplus
    constexpr IWImageFormat()
        : componentCount(0), bitsPerPixel(0), storageLayout{}, components(nullptr) {}
    constexpr IWImageFormat(uint16_t count, uint16_t bpp, IWStorageLayout layout, const IWComponentDef* comps)
        : componentCount(count), bitsPerPixel(bpp), storageLayout(layout), components(comps) {}
#endif
};

/*
 * Data block returned by a loader plugin.
 *
 * The `format` field fully describes the pixel layout.  The renderer
 * normalises this into a canonical internal format after load, so the
 * pipeline never has to handle arbitrary packed layouts at draw time.
 */
struct ImagePluginData {
    int             width;          /* image width in pixels              */
    int             height;         /* image height in pixels             */
    int             stride;         /* row stride in bytes                */
    ImageColorSpace colorSpace;     /* transfer function                  */
    size_t          size;           /* total buffer size in bytes         */
    uint8_t*        data;           /* raw pixel buffer                   */
    IWImageFormat   format;         /* per-component pixel layout         */
};

struct ImagePluginResult {
    ImagePluginResultCode code;
    ImagePluginData* data;
};

#ifdef _WIN32
typedef const wchar_t* ImagePluginPath;
#else
typedef const char* ImagePluginPath;
#endif

typedef ImagePluginResult (*LoadImageFromFileFunc)(ImagePluginPath);
typedef void (*FreeImageDataFunc)(ImagePluginData*);
typedef const char* (*ImagePluginGetLastErrorFunc)(void);

/*
 * OPTIONAL export: "LoadImagePreviewFromFile".
 *
 * Decode only enough of the file to produce a preview bounded by
 * maxDimension, reporting the full image size back through the out-parameters
 * so a caller can place the preview without loading the whole image.  A plugin
 * that does not export it is still usable; the host degrades to
 * LoadImageFromFile() rather than failing the load.
 */
typedef ImagePluginResult (*LoadImagePreviewFromFileFunc)(
    ImagePluginPath filePath,
    uint32_t maxDimension,
    int* fullWidth,
    int* fullHeight);

/*
 * OPTIONAL export: "LoadImageFromMemory".
 *
 * The memory counterpart of LoadImageFromFile.  A plugin that only reads files
 * does not export it, and the host treats the absence as "this plugin is
 * file-only", not as a load error.
 */
typedef ImagePluginResult (*LoadImageFromMemoryFunc)(const uint8_t* data, size_t size);

/*
 * OPTIONAL export: "LoadImagePreviewFromMemory".
 *
 * The memory counterpart of LoadImagePreviewFromFile.  A plugin may export a
 * file preview, a memory preview, both or neither; the host resolves them
 * independently.
 */
typedef ImagePluginResult (*LoadImagePreviewFromMemoryFunc)(
    const uint8_t* data, size_t size, uint32_t maxDimension,
    int* outWidth, int* outHeight);

/*
 * Component tables for the common layouts.
 *
 * A loader can point IWImageFormat::components at one of these instead of
 * allocating.  They have static storage duration, so they outlive every image
 * and a loader that uses one needs no ownership handling for its table.
 *
 * C++ only: a constexpr array of struct literals is not expressible in C
 * without repeating the type at every element, and a C consumer of this ABI
 * supplies its own table anyway.
 */
#ifdef __cplusplus
namespace iw {
inline constexpr IWComponentDef kGray8Components[1] = {
    { IW_COMPONENT_SEMANTIC_GRAY, IW_COMPONENT_CLASS_UNORM, 0, 8 }
};
inline constexpr IWComponentDef kGray16Components[1] = {
    { IW_COMPONENT_SEMANTIC_GRAY, IW_COMPONENT_CLASS_UNORM, 0, 16 }
};
inline constexpr IWComponentDef kGrayAlpha8Components[2] = {
    { IW_COMPONENT_SEMANTIC_GRAY, IW_COMPONENT_CLASS_UNORM, 0, 8 },
    { IW_COMPONENT_SEMANTIC_A,    IW_COMPONENT_CLASS_UNORM, 8, 8 }
};
/*
 * Gray + alpha at 16 bits per component.
 *
 * Added with the PNG loader fix: a PNG_COLOR_TYPE_GRAY_ALPHA image at bit depth
 * 16 is 2 x 16 bits at offsets 0 and 16, and the loader used to hand it the
 * 8-bit table regardless of depth, producing a descriptor that disagreed with
 * the pixels it accompanied.
 */
inline constexpr IWComponentDef kGrayAlpha16Components[2] = {
    { IW_COMPONENT_SEMANTIC_GRAY, IW_COMPONENT_CLASS_UNORM,  0, 16 },
    { IW_COMPONENT_SEMANTIC_A,    IW_COMPONENT_CLASS_UNORM, 16, 16 }
};
inline constexpr IWComponentDef kRgb8Components[3] = {
    { IW_COMPONENT_SEMANTIC_R, IW_COMPONENT_CLASS_UNORM,  0, 8 },
    { IW_COMPONENT_SEMANTIC_G, IW_COMPONENT_CLASS_UNORM,  8, 8 },
    { IW_COMPONENT_SEMANTIC_B, IW_COMPONENT_CLASS_UNORM, 16, 8 }
};
inline constexpr IWComponentDef kRgb16Components[3] = {
    { IW_COMPONENT_SEMANTIC_R, IW_COMPONENT_CLASS_UNORM,  0, 16 },
    { IW_COMPONENT_SEMANTIC_G, IW_COMPONENT_CLASS_UNORM, 16, 16 },
    { IW_COMPONENT_SEMANTIC_B, IW_COMPONENT_CLASS_UNORM, 32, 16 }
};
inline constexpr IWComponentDef kBgr8Components[3] = {
    { IW_COMPONENT_SEMANTIC_B, IW_COMPONENT_CLASS_UNORM,  0, 8 },
    { IW_COMPONENT_SEMANTIC_G, IW_COMPONENT_CLASS_UNORM,  8, 8 },
    { IW_COMPONENT_SEMANTIC_R, IW_COMPONENT_CLASS_UNORM, 16, 8 }
};
inline constexpr IWComponentDef kRgba8Components[4] = {
    { IW_COMPONENT_SEMANTIC_R, IW_COMPONENT_CLASS_UNORM,  0, 8 },
    { IW_COMPONENT_SEMANTIC_G, IW_COMPONENT_CLASS_UNORM,  8, 8 },
    { IW_COMPONENT_SEMANTIC_B, IW_COMPONENT_CLASS_UNORM, 16, 8 },
    { IW_COMPONENT_SEMANTIC_A, IW_COMPONENT_CLASS_UNORM, 24, 8 }
};
inline constexpr IWComponentDef kRgba16Components[4] = {
    { IW_COMPONENT_SEMANTIC_R, IW_COMPONENT_CLASS_UNORM,  0, 16 },
    { IW_COMPONENT_SEMANTIC_G, IW_COMPONENT_CLASS_UNORM, 16, 16 },
    { IW_COMPONENT_SEMANTIC_B, IW_COMPONENT_CLASS_UNORM, 32, 16 },
    { IW_COMPONENT_SEMANTIC_A, IW_COMPONENT_CLASS_UNORM, 48, 16 }
};
inline constexpr IWComponentDef kBgra8Components[4] = {
    { IW_COMPONENT_SEMANTIC_B, IW_COMPONENT_CLASS_UNORM,  0, 8 },
    { IW_COMPONENT_SEMANTIC_G, IW_COMPONENT_CLASS_UNORM,  8, 8 },
    { IW_COMPONENT_SEMANTIC_R, IW_COMPONENT_CLASS_UNORM, 16, 8 },
    { IW_COMPONENT_SEMANTIC_A, IW_COMPONENT_CLASS_UNORM, 24, 8 }
};
inline constexpr IWComponentDef kRgb32FComponents[3] = {
    { IW_COMPONENT_SEMANTIC_R, IW_COMPONENT_CLASS_FLOAT,  0, 32 },
    { IW_COMPONENT_SEMANTIC_G, IW_COMPONENT_CLASS_FLOAT, 32, 32 },
    { IW_COMPONENT_SEMANTIC_B, IW_COMPONENT_CLASS_FLOAT, 64, 32 }
};
inline constexpr IWComponentDef kRgba32FComponents[4] = {
    { IW_COMPONENT_SEMANTIC_R, IW_COMPONENT_CLASS_FLOAT,  0, 32 },
    { IW_COMPONENT_SEMANTIC_G, IW_COMPONENT_CLASS_FLOAT, 32, 32 },
    { IW_COMPONENT_SEMANTIC_B, IW_COMPONENT_CLASS_FLOAT, 64, 32 },
    { IW_COMPONENT_SEMANTIC_A, IW_COMPONENT_CLASS_FLOAT, 96, 32 }
};
} // namespace iw
#endif /* __cplusplus */

#ifdef __cplusplus
} /* extern "C" */
#endif
