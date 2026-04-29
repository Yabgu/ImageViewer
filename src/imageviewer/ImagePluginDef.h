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

/* -----------------------------------------------------------------------
 * Per-component pixel format descriptor (plugin ABI)
 * ----------------------------------------------------------------------- */

/** Maximum number of component descriptors carried in IWImageFormat. */
#define IW_MAX_COMPONENTS 8

/**
 * Numeric interpretation of a component's stored bits.
 *
 * uint32_t is used for the underlying type so the enum has a stable,
 * fixed-width ABI representation across compilers and platforms.
 */
typedef uint32_t IWComponentClass;
enum {
    IW_COMPONENT_CLASS_UINT  = 0u, /**< Unsigned integer, no normalisation          */
    IW_COMPONENT_CLASS_SINT  = 1u, /**< Signed integer, no normalisation            */
    IW_COMPONENT_CLASS_UNORM = 2u, /**< Unsigned integer, normalised to [0, 1]      */
    IW_COMPONENT_CLASS_SNORM = 3u, /**< Signed integer, normalised to [-1, 1]       */
    IW_COMPONENT_CLASS_FLOAT = 4u  /**< IEEE-754 float (bitWidth: 16, 32, or 64)    */
};

/**
 * Semantic meaning of a component within the pixel.
 *
 * Components that carry no recognised meaning should use
 * IW_COMPONENT_SEMANTIC_UNKNOWN.
 */
typedef uint32_t IWComponentSemantic;
enum {
    IW_COMPONENT_SEMANTIC_R       = 0u,
    IW_COMPONENT_SEMANTIC_G       = 1u,
    IW_COMPONENT_SEMANTIC_B       = 2u,
    IW_COMPONENT_SEMANTIC_A       = 3u,
    IW_COMPONENT_SEMANTIC_H       = 4u,  /**< Hue                  */
    IW_COMPONENT_SEMANTIC_S       = 5u,  /**< Saturation           */
    IW_COMPONENT_SEMANTIC_L       = 6u,  /**< Lightness/Luminance  */
    IW_COMPONENT_SEMANTIC_GRAY    = 7u,
    IW_COMPONENT_SEMANTIC_UNKNOWN = 255u
};

/**
 * Describes a single component within a packed pixel word.
 *
 * Components are stored from the least-significant bit upward.
 * bitOffset and bitWidth are always in bits. For byte-aligned formats
 * bitOffset is a multiple of 8.
 *
 * Example – RGB8 packed into 24 bits:
 *   { IW_COMPONENT_SEMANTIC_R, IW_COMPONENT_CLASS_UNORM,  0, 8 }
 *   { IW_COMPONENT_SEMANTIC_G, IW_COMPONENT_CLASS_UNORM,  8, 8 }
 *   { IW_COMPONENT_SEMANTIC_B, IW_COMPONENT_CLASS_UNORM, 16, 8 }
 *
 * Example – HSL mixed-precision (8-bit H, 6-bit S UNORM, 16-bit float L):
 *   { IW_COMPONENT_SEMANTIC_H, IW_COMPONENT_CLASS_UINT,   0,  8 }
 *   { IW_COMPONENT_SEMANTIC_S, IW_COMPONENT_CLASS_UNORM,  8,  6 }
 *   { IW_COMPONENT_SEMANTIC_L, IW_COMPONENT_CLASS_FLOAT, 14, 16 }
 */
typedef struct IWComponentDef {
    IWComponentSemantic semantic;       /**< What this component represents         */
    IWComponentClass    componentClass; /**< How to interpret the stored bits        */
    uint16_t            bitOffset;      /**< Bit offset from pixel start (LSB = 0)  */
    uint16_t            bitWidth;       /**< Number of bits used by this component   */
} IWComponentDef;

/**
 * Per-pixel format descriptor reported by an image plugin.
 *
 * The fixed-size components array (IW_MAX_COMPONENTS entries) avoids
 * pointer-lifetime issues and makes the struct trivially copyable.
 * Only the first componentCount entries are meaningful; the rest are
 * zero-initialised.
 *
 * The host normalises this into one of its canonical internal formats
 * (RGB8, RGBA8, RGB16, RGBA16, RGB32F, RGBA32F, …) after image load.
 */
typedef struct IWImageFormat {
    uint16_t       componentCount;               /**< Number of active components (<= IW_MAX_COMPONENTS) */
    uint16_t       bitsPerPixel;                 /**< Total bits consumed by one pixel                   */
    IWComponentDef components[IW_MAX_COMPONENTS]; /**< Component descriptors, ascending bitOffset order   */
} IWImageFormat;

/* -----------------------------------------------------------------------
 * Transfer function / colour space encoding (part of the plugin ABI)
 * ----------------------------------------------------------------------- */

typedef enum ImageColorSpace {
    IMAGE_COLOR_SPACE_SRGB    = 0,   /* Standard sRGB (gamma ~2.2)       */
    IMAGE_COLOR_SPACE_LINEAR  = 1,   /* Linear light, no gamma encoding  */
    IMAGE_COLOR_SPACE_HDR_PQ  = 2,   /* SMPTE ST.2084 PQ (HDR10)         */
    IMAGE_COLOR_SPACE_HDR_HLG = 3,   /* Hybrid Log-Gamma (HLG)           */
    IMAGE_COLOR_SPACE_UNKNOWN = 255  /* Unrecognised / not signalled      */
} ImageColorSpace;

/* -----------------------------------------------------------------------
 * Host-internal canonical pixel layout types
 *
 * These are NOT part of the plugin format descriptor (IWImageFormat).
 * They describe the uniform, byte-aligned formats used by the host
 * renderer after normalising the plugin-provided IWImageFormat.
 * ----------------------------------------------------------------------- */

/** Storage precision of a single channel in the canonical host layout. */
typedef enum ImagePixelFormat {
    IMAGE_PIXEL_FORMAT_U8  = 0,  /* 8-bit unsigned integer per channel  */
    IMAGE_PIXEL_FORMAT_U16 = 1,  /* 16-bit unsigned integer per channel */
    IMAGE_PIXEL_FORMAT_F32 = 2   /* 32-bit IEEE 754 float per channel   */
} ImagePixelFormat;

/** In-memory channel ordering of the canonical host layout. */
typedef enum ImageChannelOrder {
    IMAGE_CHANNEL_ORDER_RGB        = 0,
    IMAGE_CHANNEL_ORDER_RGBA       = 1,
    IMAGE_CHANNEL_ORDER_BGR        = 2,
    IMAGE_CHANNEL_ORDER_BGRA       = 3,
    IMAGE_CHANNEL_ORDER_GRAY       = 4,
    IMAGE_CHANNEL_ORDER_GRAY_ALPHA = 5
} ImageChannelOrder;

/** Returns the byte size of one channel value in the canonical host layout. */
static inline int ImagePixelFormatBytesPerChannel(ImagePixelFormat fmt)
{
    switch (fmt) {
        case IMAGE_PIXEL_FORMAT_U16: return 2;
        case IMAGE_PIXEL_FORMAT_F32: return 4;
        default:                     return 1;
    }
}

/* -----------------------------------------------------------------------
 * Plugin data structures
 * ----------------------------------------------------------------------- */

struct ImagePluginData {
    int      width;   /* image width in pixels                              */
    int      height;  /* image height in pixels                             */
    int      stride;  /* row stride in bytes (>= width * bitsPerPixel / 8) */
    size_t   size;    /* total pixel buffer size in bytes                   */
    uint8_t* data;    /* raw pixel buffer                                   */

    ImageColorSpace colorSpace; /* transfer function / colour space encoding */
    IWImageFormat   format;     /* per-component layout descriptor           */
};

struct ImagePluginResult {
    ImagePluginResultCode code;
    ImagePluginData*      data;
};

#ifdef _WIN32
typedef const wchar_t* ImagePluginPath;
#else
typedef const char* ImagePluginPath;
#endif

typedef ImagePluginResult (*LoadImageFromFileFunc)(ImagePluginPath);
typedef void              (*FreeImageDataFunc)(ImagePluginData*);
typedef const char*       (*ImagePluginGetLastErrorFunc)(void);

#ifdef __cplusplus
}
#endif
