module;

#include <cstdlib>
#include <cstring>
#include <cstdint>
#include <new>
#include <stdexcept>
#include <string>
#include "ImagePluginDef.h"
#include <tiffio.h>

export module ImageLoaderTiff;

static thread_local std::string lastError;

export extern "C" IMAGEPLUGIN_API ImagePluginResult LoadImageFromFile(const ImagePluginPath filePath)
{
    ImagePluginResult result = { .code = IMAGE_PLUGIN_UNKNOWN_ERROR, .data = nullptr };
    lastError.clear();

    try
    {
#ifdef _WIN32
        TIFF* tif = TIFFOpenW(filePath, "r");
#else
        TIFF* tif = TIFFOpen(filePath, "r");
#endif
        if (!tif)
            throw std::runtime_error("Failed to open TIFF file");

        uint32_t width = 0;
        uint32_t height = 0;
        TIFFGetField(tif, TIFFTAG_IMAGEWIDTH, &width);
        TIFFGetField(tif, TIFFTAG_IMAGELENGTH, &height);

        if (width == 0 || height == 0)
        {
            TIFFClose(tif);
            throw std::runtime_error("Invalid TIFF dimensions");
        }

        size_t pixelCount = static_cast<size_t>(width) * height;
        uint32_t* raster = static_cast<uint32_t*>(_TIFFmalloc(pixelCount * sizeof(uint32_t)));
        if (!raster)
        {
            TIFFClose(tif);
            throw std::runtime_error("Failed to allocate raster buffer");
        }

        if (!TIFFReadRGBAImageOriented(tif, width, height, raster, ORIENTATION_TOPLEFT, 0))
        {
            _TIFFfree(raster);
            TIFFClose(tif);
            throw std::runtime_error("TIFF decode failed");
        }
        TIFFClose(tif);

        constexpr int components = 4;
        size_t dataSize = pixelCount * components;
        void* block = std::malloc(sizeof(ImagePluginData) + dataSize);
        if (!block)
        {
            _TIFFfree(raster);
            throw std::runtime_error("Failed to allocate image buffer");
        }

        ImagePluginData* data = ::new (block) ImagePluginData{
            .width = static_cast<int>(width),
            .height = static_cast<int>(height),
            .componentsPerPixel = components,
            .stride = static_cast<int>(width) * components,
            .size = dataSize,
            .data = static_cast<uint8_t*>(block) + sizeof(ImagePluginData)
        };

        // TIFFReadRGBAImageOriented stores each pixel as a uint32_t with R in the low bits and
        // A in the high bits (TIFFGetR masks bits 0-7, TIFFGetA masks bits 24-31). On
        // little-endian systems the in-memory byte order is therefore R, G, B, A.
        std::memcpy(data->data, raster, dataSize);
        _TIFFfree(raster);

        result.code = IMAGE_PLUGIN_OK;
        result.data = data;
    }
    catch (const std::runtime_error& e)
    {
        lastError = e.what();
    }

    return result;
}

export extern "C" IMAGEPLUGIN_API void FreeImageData(ImagePluginData* imageData)
{
    if (imageData)
        std::free(imageData);
}

export extern "C" IMAGEPLUGIN_API const char* ImagePluginGetLastError()
{
    return lastError.c_str();
}
