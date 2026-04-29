#pragma once
#include <cstdint>
#include <cstddef>

#ifdef __cplusplus
extern "C" {
#endif

// Export macro for cross-platform symbol visibility
#ifndef IMAGEPLUGIN_API
#ifdef _WIN32
#define IMAGEPLUGIN_API __declspec(dllexport)
#else
#define IMAGEPLUGIN_API __attribute__((visibility("default")))
#endif
#endif

typedef enum ImagePluginResultCode {
    IMAGE_PLUGIN_OK = 0,
    IMAGE_PLUGIN_FILE_ERROR = 1,
    IMAGE_PLUGIN_DECODE_ERROR = 2,
    IMAGE_PLUGIN_ALLOC_ERROR = 3,
    IMAGE_PLUGIN_UNKNOWN_ERROR = 255
} ImagePluginResultCode;

/* Precision: storage size per channel */
typedef enum ImagePixelFormat {
    IMAGE_PIXEL_FORMAT_U8  = 0,  /* 8-bit unsigned integer per channel  */
    IMAGE_PIXEL_FORMAT_U16 = 1,  /* 16-bit unsigned integer per channel */
    IMAGE_PIXEL_FORMAT_F32 = 2   /* 32-bit IEEE 754 float per channel   */
} ImagePixelFormat;

/* Transfer function / colour space encoding */
typedef enum ImageColorSpace {
    IMAGE_COLOR_SPACE_SRGB    = 0,   /* Standard sRGB (gamma ~2.2)       */
    IMAGE_COLOR_SPACE_LINEAR  = 1,   /* Linear light, no gamma encoding  */
    IMAGE_COLOR_SPACE_HDR_PQ  = 2,   /* SMPTE ST.2084 PQ (HDR10)         */
    IMAGE_COLOR_SPACE_HDR_HLG = 3,   /* Hybrid Log-Gamma (HLG)           */
    IMAGE_COLOR_SPACE_UNKNOWN = 255  /* Unrecognised / not signalled      */
} ImageColorSpace;

/* In-memory byte order of colour channels */
typedef enum ImageChannelOrder {
    IMAGE_CHANNEL_ORDER_RGB        = 0,
    IMAGE_CHANNEL_ORDER_RGBA       = 1,
    IMAGE_CHANNEL_ORDER_BGR        = 2,
    IMAGE_CHANNEL_ORDER_BGRA       = 3,
    IMAGE_CHANNEL_ORDER_GRAY       = 4,
    IMAGE_CHANNEL_ORDER_GRAY_ALPHA = 5
} ImageChannelOrder;

/* Returns the number of bytes occupied by one channel value */
static inline int ImagePixelFormatBytesPerChannel(ImagePixelFormat fmt)
{
    switch (fmt) {
        case IMAGE_PIXEL_FORMAT_U16: return 2;
        case IMAGE_PIXEL_FORMAT_F32: return 4;
        default:                     return 1;
    }
}

struct ImagePluginData {
    int width;
    int height;
    int componentsPerPixel;
    int stride;        /* row stride in bytes                        */
    size_t size;       /* total buffer size in bytes                 */
    uint8_t* data;     /* raw pixel buffer                           */
    /* Precision and colour metadata */
    ImagePixelFormat  pixelFormat;   /* storage precision per channel    */
    ImageColorSpace   colorSpace;    /* transfer function                */
    ImageChannelOrder channelOrder;  /* in-memory channel ordering       */
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
