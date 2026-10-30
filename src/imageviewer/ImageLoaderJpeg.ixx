module;

#include <cstdio>
#include <stdexcept>
#include <cstring>
#include <cstdint>
#include <memory>
#include <jpeglib.h>
#include <setjmp.h>
#include <filesystem>
#include <format>
#include "ImagePluginDef.h"
#include "ExifOrientation.hpp"

namespace {
struct my_error_mgr
{
	struct jpeg_error_mgr pub;
	jmp_buf setjmp_buffer;
};

/* Owns everything the decode allocates: the pixel storage first, then the
 * descriptor handed back to the caller on success.  libjpeg reports fatal
 * errors by longjmp()ing out of the failing call, so this holder is created
 * *before* setjmp() and is not replaced while a libjpeg call can still
 * longjmp; an automatic variable written between setjmp() and longjmp() would
 * be indeterminate on the error path and leak or double-free.  The upright
 * buffer replaces the decode buffer only after the last libjpeg call. */
struct OwnedImagePluginData
{
	std::unique_ptr<uint8_t[]>       pixels;
	std::unique_ptr<ImagePluginData> info;
};
}

export module ImageLoaderJpeg;

EXTERN(void)
my_error_exit(j_common_ptr cinfo)
{
	struct my_error_mgr* myerr = (struct my_error_mgr*)cinfo->err;
	(*cinfo->err->output_message)(cinfo);
	longjmp(myerr->setjmp_buffer, 1);
}

static thread_local char lastError[256] = "";

export extern "C" IMAGEPLUGIN_API ImagePluginResult LoadImageFromFile(const ImagePluginPath filePath)
{
    ImagePluginResult result = { .code = IMAGE_PLUGIN_UNKNOWN_ERROR, .data = nullptr };
    lastError[0] = '\0';
    FILE* file = nullptr;
#ifdef _WIN32
    auto err = _wfopen_s(&file, filePath, L"rb");
#else
    file = std::fopen(filePath, "rb");
#endif
    if (file == nullptr)
    {
        snprintf(lastError, sizeof(lastError), "Failed to open file");
        result.code = IMAGE_PLUGIN_FILE_ERROR;
        return result;
    }

    struct my_error_mgr jerr = {
        .pub = {
            .error_exit = my_error_exit} };

    struct jpeg_decompress_struct cinfo = {
        .err = jpeg_std_error(&jerr.pub) };

    const auto owned = std::make_unique<OwnedImagePluginData>();

    if (setjmp(jerr.setjmp_buffer))
    {
        jpeg_destroy_decompress(&cinfo);
        fclose(file);
        snprintf(lastError, sizeof(lastError), "JPEG decode failed");
        result.code = IMAGE_PLUGIN_DECODE_ERROR;
        return result;   /* owned releases whatever was allocated so far */
    }

    jpeg_create_decompress(&cinfo);
    jpeg_stdio_src(&cinfo, file);

    /* Keep EXIF APP1 segments: libjpeg discards markers unless asked, and the
     * Orientation tag lives in one.  0xFFFF is the largest a marker can be. */
    jpeg_save_markers(&cinfo, JPEG_APP0 + 1, 0xFFFF);

    jpeg_read_header(&cinfo, TRUE);

    /* EXIF Orientation (tag 0x0112).  XMP also uses APP1, so match the
     * six-byte "Exif\0\0" identifier before handing the TIFF block to the
     * parser.  Anything missing or malformed yields 1 (normal): no rotation. */
    int orientation = 1;
    for (jpeg_saved_marker_ptr marker = cinfo.marker_list; marker != nullptr;
         marker = marker->next)
    {
        if (marker->marker != JPEG_APP0 + 1 || marker->data_length < 6) continue;
        if (std::memcmp(marker->data, "Exif\0\0", 6) != 0) continue;
        orientation = exif::ParseOrientation(marker->data + 6,
                                             marker->data_length - 6);
        break;
    }

    /* ── Component-count policy ──────────────────────────────────────────
     * The number of components is a property of the file, not a preference,
     * so it decides everything below.  Exactly two shapes are supported:
     *   1 component  -- grayscale, decoded as JCS_GRAYSCALE
     *   3 components -- YCbCr or RGB, decoded as JCS_RGB
     * Anything else is rejected instead of described.  The previous code
     * assumed three components unconditionally: a four-component CMYK/YCCK
     * file got componentCount = 4 and bitsPerPixel = 32 with only three
     * component definitions filled, leaving components[3] at bitWidth 0, so
     * ValidateImageFormat() rejected the descriptor while the buffer held
     * four bytes per pixel and the K sample went undescribed.  A
     * two-component file passed validation but was described as R+G.
     *
     * TODO(cmyk): accept 4-component files as a follow-up by setting
     * JCS_CMYK and converting to RGB here.  Adobe CMYK JPEGs store inverted
     * samples, so the conversion has to detect the Adobe APP14 marker and
     * un-invert before converting; getting that wrong is worse than
     * rejecting, which is why it is deferred rather than guessed at. */
    const int numComponents = cinfo.num_components;
    if (numComponents != 1 && numComponents != 3)
    {
        jpeg_destroy_decompress(&cinfo);
        fclose(file);
        snprintf(lastError, sizeof(lastError),
                 "JPEG has %d components; only grayscale and YCbCr are supported.",
                 numComponents);
        result.code = IMAGE_PLUGIN_DECODE_ERROR;
        return result;
    }

    /* Ask libjpeg for exactly the layout decided above.  Without this the
     * default output space for some files is not RGB (a 4-component file
     * defaults to JCS_CMYK), so the decoded buffer would not match the
     * descriptor built below. */
    cinfo.out_color_space = (numComponents == 1) ? JCS_GRAYSCALE : JCS_RGB;

    jpeg_start_decompress(&cinfo);

    /* Single source of truth for the decoded buffer's shape.  componentCount,
     * bitsPerPixel, stride and pixelDataSize are all derived from this one
     * value, so the descriptor and the buffer cannot drift apart. */
    const int channelCount = (numComponents == 1) ? 1 : 3;

    /* libjpeg must have honoured the out_color_space request.  If it did not,
     * the buffer would not match the descriptor about to be built, so fail
     * rather than hand the caller a self-inconsistent image. */
    if (cinfo.output_components != channelCount)
    {
        jpeg_destroy_decompress(&cinfo);
        fclose(file);
        snprintf(lastError, sizeof(lastError),
                 "JPEG output component mismatch: expected %d, got %d",
                 channelCount, cinfo.output_components);
        result.code = IMAGE_PLUGIN_DECODE_ERROR;
        return result;
    }

    constexpr int kBitsPerComponent = 8;
    const int bytesPerPixel = channelCount * (kBitsPerComponent / 8);

    /* Snapshot the stored geometry now: orientation can swap width and height,
     * and cinfo is destroyed before the descriptor is built. */
    const int    storedWidth  = static_cast<int>(cinfo.output_width);
    const int    storedHeight = static_cast<int>(cinfo.output_height);
    const int    rowStride    = storedWidth * bytesPerPixel;
    const size_t dataSize     = static_cast<size_t>(rowStride) * storedHeight;

    constexpr int MaxLinesToRead = 1;

    JSAMPARRAY buffer = (*cinfo.mem->alloc_sarray)((j_common_ptr)&cinfo, JPOOL_IMAGE, rowStride, MaxLinesToRead);

    IWImageFormat fmt = {};
    fmt.componentCount = static_cast<uint16_t>(channelCount);
    fmt.bitsPerPixel   = static_cast<uint16_t>(channelCount * kBitsPerComponent);
    if (channelCount == 1) {
        fmt.components[0] = { IW_COMPONENT_SEMANTIC_GRAY, IW_COMPONENT_CLASS_UNORM, 0, kBitsPerComponent };
    } else {
        fmt.components[0] = { IW_COMPONENT_SEMANTIC_R, IW_COMPONENT_CLASS_UNORM,  0, kBitsPerComponent };
        fmt.components[1] = { IW_COMPONENT_SEMANTIC_G, IW_COMPONENT_CLASS_UNORM,  8, kBitsPerComponent };
        fmt.components[2] = { IW_COMPONENT_SEMANTIC_B, IW_COMPONENT_CLASS_UNORM, 16, kBitsPerComponent };
    }

    owned->pixels = std::make_unique<uint8_t[]>(dataSize);
    if (!owned->pixels) {
        jpeg_destroy_decompress(&cinfo);
        fclose(file);
        snprintf(lastError, sizeof(lastError), "Failed to allocate image buffer");
        result.code = IMAGE_PLUGIN_ALLOC_ERROR;
        return result;
    }

    while (cinfo.output_scanline < cinfo.output_height)
    {
        auto current_scanline = cinfo.output_scanline;
        auto readlines = jpeg_read_scanlines(&cinfo, buffer, MaxLinesToRead);
        if (readlines == 0)
        {
            jpeg_destroy_decompress(&cinfo);
            fclose(file);
            snprintf(lastError, sizeof(lastError), "JPEG read error");
            result.code = IMAGE_PLUGIN_DECODE_ERROR;
            return result;
        }
        std::memcpy(owned->pixels.get() + current_scanline * rowStride, buffer[0], rowStride * readlines);
    }

    jpeg_finish_decompress(&cinfo);
    /* No libjpeg call below this point can longjmp, so replacing the pixel
     * holder is safe even though it was created before setjmp(). */
    jpeg_destroy_decompress(&cinfo);
    fclose(file);

    /* ── Upright the raster ────────────────────────────────────────────────
     * The decode buffer is in storage order; EXIF Orientation says how to
     * rotate or mirror it for display.  A phone portrait shot is stored as a
     * landscape raster plus Orientation=6, so without this the image opens
     * sideways and reports swapped width/height.  Turning it here means every
     * consumer — viewer, iwconv, tests — receives upright pixels and upright
     * dimensions instead of each having to interpret the tag. */
    int    finalWidth  = storedWidth;
    int    finalHeight = storedHeight;
    int    finalStride = rowStride;
    size_t finalSize   = dataSize;

    if (orientation != 1)
    {
        finalWidth  = exif::OrientedWidth(storedWidth, storedHeight, orientation);
        finalHeight = exif::OrientedHeight(storedWidth, storedHeight, orientation);
        finalStride = finalWidth * bytesPerPixel;
        finalSize   = static_cast<size_t>(finalStride) * finalHeight;

        auto upright = std::make_unique<uint8_t[]>(finalSize);
        if (!upright) {
            snprintf(lastError, sizeof(lastError), "Failed to allocate image buffer");
            result.code = IMAGE_PLUGIN_ALLOC_ERROR;
            return result;   /* owned releases the decoded buffer */
        }
        exif::OrientPixels(owned->pixels.get(), storedWidth, storedHeight,
                           bytesPerPixel, orientation, upright.get());
        owned->pixels = std::move(upright);
    }

    owned->info = std::make_unique<ImagePluginData>(ImagePluginData{
        .width      = finalWidth,
        .height     = finalHeight,
        .stride     = finalStride,
        .colorSpace = IMAGE_COLOR_SPACE_SRGB,
        .size       = finalSize,
        .data       = owned->pixels.get(),
        .format     = fmt
    });

    /* Success: transfer ownership of the descriptor and its pixel buffer to
     * the caller, which releases them through FreeImageData(). */
    result.code = IMAGE_PLUGIN_OK;
    result.data = owned->info.release();
    owned->pixels.release();
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
