#pragma once

/*
 * This header is consumed by both C++ (the viewer, the loaders) and C (embedders
 * and the ABI tests).  The two standard headers that differ are selected here
 * rather than assuming C++: the plugin ABI is a C ABI, and a header that only
 * compiles as C++ cannot be handed to a C caller.
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

typedef enum ImagePluginResultCode : uint8_t {
    IMAGE_PLUGIN_OK            = 0,
    IMAGE_PLUGIN_FILE_ERROR    = 1,
    IMAGE_PLUGIN_DECODE_ERROR  = 2,
    IMAGE_PLUGIN_ALLOC_ERROR   = 3,
    IMAGE_PLUGIN_UNKNOWN_ERROR = 255
} ImagePluginResultCode;

/* Transfer function / colour space encoding */
typedef enum ImageColorSpace : uint8_t {
    IMAGE_COLOR_SPACE_SRGB    = 0,   /* Standard sRGB (gamma ~2.2)       */
    IMAGE_COLOR_SPACE_LINEAR  = 1,   /* Linear light, no gamma encoding  */
    IMAGE_COLOR_SPACE_HDR_PQ  = 2,   /* SMPTE ST.2084 PQ (HDR10)         */
    IMAGE_COLOR_SPACE_HDR_HLG = 3,   /* Hybrid Log-Gamma (HLG)           */
    IMAGE_COLOR_SPACE_UNKNOWN = 255  /* Unrecognised / not signalled      */
} ImageColorSpace;

/* ── Per-component class: how stored bits are interpreted ──────────────────── */
typedef enum IWComponentClass : uint8_t {
    IW_COMPONENT_CLASS_UINT  = 0,  /* unsigned integer, raw value                   */
    IW_COMPONENT_CLASS_SINT  = 1,  /* signed integer, raw value                     */
    IW_COMPONENT_CLASS_UNORM = 2,  /* unsigned integer normalised to [0, 1]         */
    IW_COMPONENT_CLASS_SNORM = 3,  /* signed integer normalised to [-1, 1]          */
    IW_COMPONENT_CLASS_FLOAT = 4   /* IEEE floating-point (16-bit or 32-bit)        */
} IWComponentClass;

/* ── Per-component semantic: what a component represents ───────────────────── */
typedef enum IWComponentSemantic : uint8_t {
    /*
     * IW_COMPONENT_SEMANTIC_NONE = 0 is RESERVED.
     *
     * IWImageFormat zero-initialises its components[IW_MAX_COMPONENTS] array,
     * so every slot beyond componentCount will have semantic == 0.  Using 0 as
     * a sentinel for "unused / padding" lets callers detect unset slots without
     * inspecting componentCount, and avoids mistaking an uninitialised slot for
     * a real R component.
     */
    IW_COMPONENT_SEMANTIC_NONE    = 0,   /* reserved – unused / padding slot     */

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

/* ── Per-component layout descriptor ──────────────────────────────────────── */
struct IWComponentDef {
    IWComponentSemantic semantic;       /* what this component represents        */
    IWComponentClass    componentClass; /* how the stored bits are interpreted   */
    uint16_t            bitOffset;      /* bit offset from the start of a pixel (interleaved only) */
    uint16_t            bitWidth;       /* number of bits occupied               */
};

/* Maximum number of components per pixel supported by the ABI */
#define IW_MAX_COMPONENTS 8

/*
 * How component data is laid out in the pixel buffer.
 *
 * Stored as a 64-bit descriptor: 32 bits of orthogonal layout flags,
 * followed by a 32-bit row stride in bytes (0 = tightly packed).
 */
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
    constexpr bool isPlanar() const noexcept { return (flags & 0x00000001u) != 0; }
    constexpr bool isInterleaved() const noexcept { return !isPlanar(); }
#endif
} IWStorageLayout;

/* ── Extensible Metadata Entry & Tags ────────────────────────────────────── */

#define IWI_META_TAG_CAMERA 0x414D4143u /* "CAMA": 3D Camera intrinsics & extrinsics */
#define IWI_META_TAG_CBOR   0x524F4243u /* "CBOR": IETF RFC 8949 nestable binary JSON*/
#define IWI_META_TAG_BSON   0x4E4F5342u /* "BSON": BSON document                    */
#define IWI_META_TAG_MSGP   0x5047534Du /* "MSGP": MessagePack                       */
#define IWI_META_TAG_JSON   0x4E4F534Au /* "JSON": UTF-8 JSON text                   */
#define IWI_META_TAG_TEXT   0x54584554u /* "TEXT": UTF-8 key-value string metadata   */

typedef struct IWCameraMeta {
    float fovDegrees;       /* Vertical FOV in degrees (e.g. 60.0f)     */
    float nearClip;         /* Near clipping plane (e.g. 0.1f)          */
    float farClip;          /* Far clipping plane (e.g. 1000.0f)        */
    float position[3];      /* Camera eye position (X, Y, Z)            */
    float upVector[3];      /* Camera up direction (normalized X, Y, Z) */
    float target[3];        /* Look-at target point (X, Y, Z)           */
} IWCameraMeta;

typedef struct IwiMetaHeader {
    uint32_t tag;    /* FourCC tag identifying entry type */
    uint32_t size;   /* Byte length of entry payload      */
} IwiMetaHeader;

/*
 * Complete format descriptor for a single image.
 *
 * Every component is described independently, so mixed-precision and
 * non-RGB layouts (HSL, UV, infrared+visible+depth, …) are expressible
 * without adding new enumerators.
 *
 * bitsPerPixel is the total number of bits consumed per logical pixel
 * for IW_STORAGE_INTERLEAVED, or per sample (one component of one pixel)
 * for IW_STORAGE_PLANAR.
 *
 * Example – RGB8 packed (interleaved):
 *   componentCount = 3, bitsPerPixel = 24,
 *   storageLayout  = IW_STORAGE_INTERLEAVED
 *   components[0] = { IW_COMPONENT_SEMANTIC_R, IW_COMPONENT_CLASS_UNORM,  0, 8 }
 *   components[1] = { IW_COMPONENT_SEMANTIC_G, IW_COMPONENT_CLASS_UNORM,  8, 8 }
 *   components[2] = { IW_COMPONENT_SEMANTIC_B, IW_COMPONENT_CLASS_UNORM, 16, 8 }
 *
 * Example – HSL mixed-precision (interleaved):
 *   componentCount = 3, bitsPerPixel = 30,
 *   storageLayout  = IW_STORAGE_INTERLEAVED
 *   components[0] = { IW_COMPONENT_SEMANTIC_H, IW_COMPONENT_CLASS_UINT,   0,  8 }
 *   components[1] = { IW_COMPONENT_SEMANTIC_S, IW_COMPONENT_CLASS_UNORM,  8,  6 }
 *   components[2] = { IW_COMPONENT_SEMANTIC_L, IW_COMPONENT_CLASS_FLOAT, 14, 16 }
 *
 * Example – 16-bit grayscale planar (one plane):
 *   componentCount = 1, bitsPerPixel = 16,
 *   storageLayout  = IW_STORAGE_PLANAR
 *   components[0] = { IW_COMPONENT_SEMANTIC_GRAY, IW_COMPONENT_CLASS_UNORM, 0, 16 }
 */
struct IWImageFormat {
    uint16_t        componentCount;                 /* number of active components (<= IW_MAX_COMPONENTS) */
    uint16_t        bitsPerPixel;                   /* bits per pixel (interleaved) / per sample (planar) */
    IWStorageLayout storageLayout;                  /* how components are arranged in memory              */
    IWComponentDef  components[IW_MAX_COMPONENTS];  /* per-component descriptors                          */
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
typedef const char* (*ImagePluginGetLastErrorFunc)();

/*
 * OPTIONAL export: "LoadImagePreviewFromFile".
 *
 * Decodes a reduced-resolution version of the file for display while the
 * full-resolution decode runs.  Loaders that can produce a cheaper preview
 * export this symbol; the host resolves it with GetProcAddress/dlsym and
 * silently falls back to LoadImageFromFile when it is absent.
 *
 *   maxDimension   largest side (width or height) wanted for the preview.
 *                  0 asks for the decoder's fastest useful preview.  This is
 *                  best effort: a format that cannot scale far enough may
 *                  return a larger preview.
 *   fullWidth      optional out-parameter: upright width of the full image
 *   fullHeight     optional out-parameter: upright height of the full image
 *
 * The preview is returned through the normal ImagePluginResult/ImagePluginData
 * contract and is released with FreeImageData().  fullWidth/fullHeight let the
 * renderer size the preview quad like the final image, so replacing the
 * preview with the full raster does not move or resize the picture.  They may
 * be left at 0 when the loader cannot determine them; the host then treats the
 * preview dimensions as authoritative.
 *
 * A loader may return the full-resolution image from this call (for example
 * when the file already fits inside maxDimension).  The host compares the
 * returned dimensions with fullWidth/fullHeight and skips the second decode in
 * that case.
 *
 * On failure the loader returns a non-OK code; the host then performs a normal
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
 * Decodes an image that is already in memory, for hosts that read assets
 * from a container rather than a filesystem.  Same ImagePluginResult /
 * ImagePluginData contract as LoadImageFromFile, released with the same
 * FreeImageData; only the source differs.
 *
 *   data  must remain valid for the duration of the call.  The loader does
 *         not retain it and does not take ownership; the returned pixels
 *         are the loader's own allocation.
 *   size  bytes available at data.  Zero is IMAGE_PLUGIN_FILE_ERROR.
 *
 * Absent on plugins that have not implemented it.  The host resolves it
 * with dlsym/GetProcAddress and treats absence as "this loader is
 * file-only", not as a load error.
 */
typedef ImagePluginResult (*LoadImageFromMemoryFunc)(const uint8_t* data, size_t size);

/*
 * OPTIONAL export: "LoadImagePreviewFromMemory".
 *
 * The memory counterpart of LoadImagePreviewFromFile, with the same preview
 * contract (maxDimension, best effort, fullWidth/fullHeight may be 0).  Only
 * the source differs.  A loader may implement the memory decode without the
 * memory preview; the two are resolved independently.
 */
typedef ImagePluginResult (*LoadImagePreviewFromMemoryFunc)(
    const uint8_t* data, size_t size, uint32_t maxDimension,
    int* outWidth, int* outHeight);

#ifdef __cplusplus
}
#endif
