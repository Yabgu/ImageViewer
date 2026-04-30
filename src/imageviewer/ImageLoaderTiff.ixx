module;

#include <cstdlib>
#include <cstring>
#include <cstdint>
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

        // Read image metadata
        uint16_t bitsPerSample  = 8;
        uint16_t samplesPerPixel = 3;
        uint16_t sampleFormat   = SAMPLEFORMAT_UINT;
        uint16_t photometric    = PHOTOMETRIC_RGB;
        uint16_t planarConfig   = PLANARCONFIG_CONTIG;
        TIFFGetFieldDefaulted(tif, TIFFTAG_BITSPERSAMPLE,   &bitsPerSample);
        TIFFGetFieldDefaulted(tif, TIFFTAG_SAMPLESPERPIXEL, &samplesPerPixel);
        TIFFGetFieldDefaulted(tif, TIFFTAG_SAMPLEFORMAT,    &sampleFormat);
        TIFFGetFieldDefaulted(tif, TIFFTAG_PHOTOMETRIC,     &photometric);
        TIFFGetFieldDefaulted(tif, TIFFTAG_PLANARCONFIG,    &planarConfig);

        // Check for an alpha (extra) sample
        uint16_t  extraSamples     = 0;
        uint16_t* extraSampleTypes = nullptr;
        TIFFGetField(tif, TIFFTAG_EXTRASAMPLES, &extraSamples, &extraSampleTypes);
        bool hasAlpha = (extraSamples > 0 && extraSampleTypes &&
                         (extraSampleTypes[0] == EXTRASAMPLE_ASSOCALPHA ||
                          extraSampleTypes[0] == EXTRASAMPLE_UNASSALPHA));

        // Determine pixel format from TIFF metadata
        IWComponentClass componentClass;
        uint16_t         componentBitWidth;
        if (bitsPerSample == 32 && sampleFormat == SAMPLEFORMAT_IEEEFP) {
            componentClass    = IW_COMPONENT_CLASS_FLOAT;
            componentBitWidth = 32;
        } else if (bitsPerSample == 16) {
            componentClass    = IW_COMPONENT_CLASS_UINT;
            componentBitWidth = 16;
        } else {
            componentClass    = IW_COMPONENT_CLASS_UINT;
            componentBitWidth = 8;
        }

        // Determine colour space (heuristic: float/16-bit TIFF files are
        // typically stored as linear light). A future filter layer can refine
        // this using the optional TIFFTAG_TRANSFERFUNCTION or ICC profile.
        ImageColorSpace colorSpace;
        if (componentBitWidth == 32 || componentBitWidth == 16)
            colorSpace = IMAGE_COLOR_SPACE_LINEAR;
        else
            colorSpace = IMAGE_COLOR_SPACE_SRGB;

        // Only expose native scanline data for photometrics whose samples
        // already match the viewer's gray/RGB channel-order expectations.
        const bool isGrayPhotometric =
            photometric == PHOTOMETRIC_MINISBLACK || photometric == PHOTOMETRIC_MINISWHITE;
        const bool isRgbPhotometric = photometric == PHOTOMETRIC_RGB;
        // Native scanline reading only works for contiguous planar config.
        const bool supportsNativeScanline =
            (isGrayPhotometric || isRgbPhotometric) && (planarConfig == PLANARCONFIG_CONTIG);

        // Determine channel order for native scanline decoding.
        // Build the IWImageFormat for the native-scanline path.
        // (The tiled/fallback path always produces 8-bit RGBA via libtiff.)
        auto BuildNativeFormat = [&](int nComponents, bool alpha) -> IWImageFormat {
            IWImageFormat f = {};
            f.componentCount = static_cast<uint16_t>(nComponents);
            f.bitsPerPixel   = static_cast<uint16_t>(nComponents * componentBitWidth);
            if (isGrayPhotometric) {
                f.components[0] = { IW_COMPONENT_SEMANTIC_GRAY, componentClass, 0, componentBitWidth };
                if (alpha)
                    f.components[1] = { IW_COMPONENT_SEMANTIC_A, componentClass, componentBitWidth, componentBitWidth };
            } else {
                f.components[0] = { IW_COMPONENT_SEMANTIC_R, componentClass, static_cast<uint16_t>(0 * componentBitWidth), componentBitWidth };
                f.components[1] = { IW_COMPONENT_SEMANTIC_G, componentClass, static_cast<uint16_t>(1 * componentBitWidth), componentBitWidth };
                f.components[2] = { IW_COMPONENT_SEMANTIC_B, componentClass, static_cast<uint16_t>(2 * componentBitWidth), componentBitWidth };
                if (alpha)
                    f.components[3] = { IW_COMPONENT_SEMANTIC_A, componentClass, static_cast<uint16_t>(3 * componentBitWidth), componentBitWidth };
            }
            return f;
        };

        // Tiled TIFFs and unsupported photometrics/planar configs: fall back to
        // the 8-bit RGBA convenience reader so libtiff performs any required
        // conversion (YCbCr→RGB, palette expansion, plane interleaving, etc.).
        if (TIFFIsTiled(tif) || !supportsNativeScanline)
        {
            size_t pixelCount = static_cast<size_t>(width) * height;
            uint32_t* raster = static_cast<uint32_t*>(_TIFFmalloc(pixelCount * sizeof(uint32_t)));
            if (!raster)
            {
                TIFFClose(tif);
                throw std::runtime_error("Failed to allocate raster buffer for tiled TIFF");
            }
            if (!TIFFReadRGBAImageOriented(tif, width, height, raster, ORIENTATION_TOPLEFT, 0))
            {
                _TIFFfree(raster);
                TIFFClose(tif);
                throw std::runtime_error("Tiled TIFF decode failed");
            }
            TIFFClose(tif);

            constexpr int components = 4;
            size_t dataSize = pixelCount * components;
            uint8_t* pixelData = static_cast<uint8_t*>(std::malloc(dataSize));
            if (!pixelData)
            {
                _TIFFfree(raster);
                throw std::runtime_error("Failed to allocate pixel buffer for TIFF raster decode");
            }
            std::memcpy(pixelData, raster, dataSize);
            _TIFFfree(raster);

            IWImageFormat tiledFmt = {};
            tiledFmt.componentCount = components;
            tiledFmt.bitsPerPixel   = components * 8;
            tiledFmt.components[0]  = { IW_COMPONENT_SEMANTIC_R, IW_COMPONENT_CLASS_UINT,  0, 8 };
            tiledFmt.components[1]  = { IW_COMPONENT_SEMANTIC_G, IW_COMPONENT_CLASS_UINT,  8, 8 };
            tiledFmt.components[2]  = { IW_COMPONENT_SEMANTIC_B, IW_COMPONENT_CLASS_UINT, 16, 8 };
            tiledFmt.components[3]  = { IW_COMPONENT_SEMANTIC_A, IW_COMPONENT_CLASS_UINT, 24, 8 };

            ImagePluginData* data = static_cast<ImagePluginData*>(std::malloc(sizeof(ImagePluginData)));
            if (!data)
            {
                std::free(pixelData);
                throw std::runtime_error("Failed to allocate ImagePluginData for TIFF raster decode");
            }
            std::memset(data, 0, sizeof(ImagePluginData));
            *data = ImagePluginData{
                .width      = static_cast<int>(width),
                .height     = static_cast<int>(height),
                .stride     = static_cast<int>(width) * components,
                .colorSpace = IMAGE_COLOR_SPACE_SRGB,
                .size       = dataSize,
                .data       = pixelData,
                .format     = tiledFmt
            };
            result.code = IMAGE_PLUGIN_OK;
            result.data = data;
            return result;
        }

        // Strip-based TIFF: read scanlines at native bit depth
        int bpc = componentBitWidth / 8;
        tmsize_t tiffRowBytes = TIFFScanlineSize(tif);
        size_t   packRowBytes = static_cast<size_t>(width) * samplesPerPixel * bpc;
        size_t   dataSize     = packRowBytes * height;
        uint8_t* pixelData    = static_cast<uint8_t*>(std::malloc(dataSize));
        if (!pixelData)
        {
            TIFFClose(tif);
            throw std::runtime_error("Failed to allocate pixel buffer for TIFF scanline decode");
        }

        // Temporary row buffer only needed when libtiff pads the scanline
        uint8_t* rowBuf = (tiffRowBytes > static_cast<tmsize_t>(packRowBytes))
                          ? static_cast<uint8_t*>(std::malloc(tiffRowBytes)) : nullptr;
        if (tiffRowBytes > static_cast<tmsize_t>(packRowBytes) && !rowBuf)
        {
            std::free(pixelData);
            TIFFClose(tif);
            throw std::runtime_error("Failed to allocate row buffer for TIFF scanline decode");
        }

        for (uint32_t row = 0; row < height; ++row)
        {
            uint8_t* dst = pixelData + row * packRowBytes;
            if (rowBuf)
            {
                if (TIFFReadScanline(tif, rowBuf, row, 0) < 0)
                {
                    std::free(rowBuf);
                    std::free(pixelData);
                    TIFFClose(tif);
                    throw std::runtime_error("TIFF scanline read failed");
                }
                std::memcpy(dst, rowBuf, packRowBytes);
            }
            else
            {
                if (TIFFReadScanline(tif, dst, row, 0) < 0)
                {
                    std::free(pixelData);
                    TIFFClose(tif);
                    throw std::runtime_error("TIFF scanline read failed");
                }
            }
        }
        std::free(rowBuf);
        TIFFClose(tif);

        ImagePluginData* data = static_cast<ImagePluginData*>(std::malloc(sizeof(ImagePluginData)));
        if (!data)
        {
            std::free(pixelData);
            throw std::runtime_error("Failed to allocate ImagePluginData for TIFF scanline decode");
        }
        std::memset(data, 0, sizeof(ImagePluginData));
        *data = ImagePluginData{
            .width      = static_cast<int>(width),
            .height     = static_cast<int>(height),
            .stride     = static_cast<int>(packRowBytes),
            .colorSpace = colorSpace,
            .size       = dataSize,
            .data       = pixelData,
            .format     = BuildNativeFormat(static_cast<int>(samplesPerPixel), hasAlpha)
        };
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
    {
        std::free(imageData->data);
        std::free(imageData);
    }
}

export extern "C" IMAGEPLUGIN_API const char* ImagePluginGetLastError()
{
    return lastError.c_str();
}
