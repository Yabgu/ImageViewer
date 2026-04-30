#pragma once
#include <cstdint>
#include <cstddef>

#ifdef __cplusplus
extern "C" {
#endif

/* Export macro for cross-platform symbol visibility */
#ifndef IMAGEPLUGIN_API
#ifdef _WIN32
#define IMAGEPLUGIN_API __declspec(dllexport)
#else
#define IMAGEPLUGIN_API __attribute__((visibility("default")))
#endif
#endif

typedef enum ImagePluginResultCode {
    IMAGE_PLUGIN_OK            = 0,
    IMAGE_PLUGIN_FILE_ERROR    = 1,
    IMAGE_PLUGIN_DECODE_ERROR  = 2,
    IMAGE_PLUGIN_ALLOC_ERROR   = 3,
    IMAGE_PLUGIN_UNKNOWN_ERROR = 255
} ImagePluginResultCode;

/* Transfer function / colour space encoding */
typedef enum ImageColorSpace {
    IMAGE_COLOR_SPACE_SRGB    = 0,   /* Standard sRGB (gamma ~2.2)       */
    IMAGE_COLOR_SPACE_LINEAR  = 1,   /* Linear light, no gamma encoding  */
    IMAGE_COLOR_SPACE_HDR_PQ  = 2,   /* SMPTE ST.2084 PQ (HDR10)         */
    IMAGE_COLOR_SPACE_HDR_HLG = 3,   /* Hybrid Log-Gamma (HLG)           */
    IMAGE_COLOR_SPACE_UNKNOWN = 255  /* Unrecognised / not signalled      */
} ImageColorSpace;

/* ── Per-component class: how stored bits are interpreted ──────────────────── */
typedef uint32_t IWComponentClass;
enum {
    IW_COMPONENT_CLASS_UINT  = 0,  /* unsigned integer, raw value                   */
    IW_COMPONENT_CLASS_SINT  = 1,  /* signed integer, raw value                     */
    IW_COMPONENT_CLASS_UNORM = 2,  /* unsigned integer normalised to [0, 1]         */
    IW_COMPONENT_CLASS_SNORM = 3,  /* signed integer normalised to [-1, 1]          */
    IW_COMPONENT_CLASS_FLOAT = 4   /* IEEE floating-point (16-bit or 32-bit)        */
};

/* ── Per-component semantic: what a component represents ───────────────────── */
typedef uint32_t IWComponentSemantic;
enum {
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
};

/* ── Per-component layout descriptor ──────────────────────────────────────── */
typedef struct IWComponentDef {
    IWComponentSemantic semantic;       /* what this component represents        */
    IWComponentClass    componentClass; /* how the stored bits are interpreted   */
    uint16_t            bitOffset;      /* bit offset from the start of a pixel (interleaved only) */
    uint16_t            bitWidth;       /* number of bits occupied               */
} IWComponentDef;

/* Maximum number of components per pixel supported by the ABI */
#define IW_MAX_COMPONENTS 8

/*
 * How component data is laid out in the pixel buffer.
 *
 * IW_STORAGE_INTERLEAVED (default)
 *   All components of one pixel are packed together before the next pixel:
 *   [RGBRGBRGB...].  bitOffset in each IWComponentDef is measured from the
 *   start of the pixel word.
 *
 * IW_STORAGE_PLANAR
 *   All values of component 0 come first (one full plane), followed by all
 *   values of component 1, and so on:  [RRR...GGG...BBB...].
 *   Each plane occupies  (height × width × ceil(bitWidth/8))  bytes.
 *   bitOffset in IWComponentDef is unused for this layout.
 */
typedef uint32_t IWStorageLayout;
enum {
    IW_STORAGE_INTERLEAVED = 0u, /* default; components packed per pixel     */
    IW_STORAGE_PLANAR      = 1u  /* one contiguous plane per component       */
};

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
typedef struct IWImageFormat {
    uint16_t        componentCount;                 /* number of active components (<= IW_MAX_COMPONENTS) */
    uint16_t        bitsPerPixel;                   /* bits per pixel (interleaved) / per sample (planar) */
    IWStorageLayout storageLayout;                  /* how components are arranged in memory              */
    IWComponentDef  components[IW_MAX_COMPONENTS];  /* per-component descriptors                          */
} IWImageFormat;

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

#ifdef __cplusplus
}
#endif
