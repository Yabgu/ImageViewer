#pragma once
#include <cstdint>
#include <cstddef>

#ifdef __cplusplus
extern "C" {
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

typedef ImagePluginResult (*LoadImageFromFileFunc)(const wchar_t*);
typedef void (*FreeImageDataFunc)(ImagePluginData*);

typedef const char* (*GetLastErrorFunc)();

#ifdef __cplusplus
}
#endif
