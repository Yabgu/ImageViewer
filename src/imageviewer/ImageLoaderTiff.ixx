module;

#include <cstdio>
#include <cstring>
#include <cstdint>
#include <filesystem>
#include "ImagePluginDef.h"
#include <tiffio.h>

export module ImageLoaderTiff;

static thread_local char lastError[256] = "";

export extern "C" IMAGEPLUGIN_API ImagePluginResult LoadImageFromFile(const ImagePluginPath filePath)
{
    ImagePluginResult result = { .code = IMAGE_PLUGIN_UNKNOWN_ERROR, .data = nullptr };
    lastError[0] = '\0';

#ifdef _WIN32
    TIFF* tif = TIFFOpenW(filePath, "r");
#else
    TIFF* tif = TIFFOpen(filePath, "r");
#endif
    if (!tif)
    {
        snprintf(lastError, sizeof(lastError), "Failed to open TIFF file");
        result.code = IMAGE_PLUGIN_FILE_ERROR;
        return result;
    }

    uint32_t width = 0;
    uint32_t height = 0;
    TIFFGetField(tif, TIFFTAG_IMAGEWIDTH, &width);
    TIFFGetField(tif, TIFFTAG_IMAGELENGTH, &height);

    if (width == 0 || height == 0)
    {
        TIFFClose(tif);
        snprintf(lastError, sizeof(lastError), "Invalid TIFF dimensions");
        result.code = IMAGE_PLUGIN_DECODE_ERROR;
        return result;
    }

    size_t pixelCount = static_cast<size_t>(width) * height;
    uint32_t* raster = static_cast<uint32_t*>(_TIFFmalloc(pixelCount * sizeof(uint32_t)));
    if (!raster)
    {
        TIFFClose(tif);
        snprintf(lastError, sizeof(lastError), "Failed to allocate raster buffer");
        result.code = IMAGE_PLUGIN_ALLOC_ERROR;
        return result;
    }

    if (!TIFFReadRGBAImageOriented(tif, width, height, raster, ORIENTATION_TOPLEFT, 0))
    {
        _TIFFfree(raster);
        TIFFClose(tif);
        snprintf(lastError, sizeof(lastError), "TIFF decode failed");
        result.code = IMAGE_PLUGIN_DECODE_ERROR;
        return result;
    }
    TIFFClose(tif);

    constexpr int components = 4;
    ImagePluginData* data = new ImagePluginData{
        .width = static_cast<int>(width),
        .height = static_cast<int>(height),
        .componentsPerPixel = components,
        .stride = static_cast<int>(width) * components,
        .size = pixelCount * components,
        .data = new uint8_t[pixelCount * components]
    };
    if (!data->data)
    {
        _TIFFfree(raster);
        delete data;
        snprintf(lastError, sizeof(lastError), "Failed to allocate image buffer");
        result.code = IMAGE_PLUGIN_ALLOC_ERROR;
        return result;
    }

    // TIFFReadRGBAImageOriented stores each pixel as a uint32_t with R in the low bits and
    // A in the high bits (TIFFGetR masks bits 0-7, TIFFGetA masks bits 24-31). On
    // little-endian systems the in-memory byte order is therefore R, G, B, A.
    std::memcpy(data->data, raster, pixelCount * components);
    _TIFFfree(raster);

    result.code = IMAGE_PLUGIN_OK;
    result.data = data;
    return result;
}

export extern "C" IMAGEPLUGIN_API void FreeImageData(ImagePluginData* imageData)
{
    if (imageData)
    {
        delete[] imageData->data;
        delete imageData;
    }
}

export extern "C" IMAGEPLUGIN_API const char* ImagePluginGetLastError()
{
    return lastError;
}
