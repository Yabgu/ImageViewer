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

struct ImagePluginData {
    int width;
    int height;
    int componentsPerPixel;
    int stride;
    size_t size;
    uint8_t* data;
};

struct ImagePluginResult {
    ImagePluginResultCode code;
    ImagePluginData* data;
};

#ifdef _WIN32
#define ImagePluginPath wchar_t*
#else
#define ImagePluginPath char*
#endif

typedef ImagePluginResult (*LoadImageFromFileFunc)(const ImagePluginPath);
typedef void (*FreeImageDataFunc)(ImagePluginData*);

typedef const char* (*GetLastErrorFunc)();

#ifdef __cplusplus
}
#endif
