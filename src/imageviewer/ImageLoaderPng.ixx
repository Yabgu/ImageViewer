module;

#include <cstdio>
#include <stdexcept>
#include <cstring>
#include <cstdint>
#include <new>
#include <png.h>
#include <filesystem>
#include <format>
#include "ImagePluginDef.h"

export module ImageLoaderPng;

static thread_local char lastError[256] = "";

namespace {

/*
 * Where the compressed bytes come from.  Exactly one is set.
 *
 * Unlike JPEG and WebP this loader never buffered the file: it handed a FILE*
 * straight to libpng.  The memory path therefore needs a read callback instead
 * of an io pointer, and the file path keeps png_init_io so its behaviour is
 * unchanged.
 */
struct Source
{
    const uint8_t*  span = nullptr;
    size_t          size = 0;
    ImagePluginPath path = nullptr;
};

/* Read cursor handed to libpng through png_set_read_fn. */
struct ReadCtx
{
    const uint8_t* p   = nullptr;
    size_t         n   = 0;
    size_t         off = 0;
};

/*
 * libpng read callback over a caller-owned span.
 *
 * A short read must fail through the png_jmpbuf path, not return fewer bytes:
 * libpng treats a read callback that fills less than asked as an error only if
 * the callback *reports* it.  png_error() longjmp()s to the setjmp in
 * DecodePng, which is the error path this loader already has.
 */
void SpanRead(png_structp png, png_bytep out, png_size_t count)
{
    auto* ctx = static_cast<ReadCtx*>(png_get_io_ptr(png));
    if (ctx == nullptr)
    {
        png_error(png, "PNG read callback has no context");
        return;
    }
    if (ctx->off > ctx->n || count > ctx->n - ctx->off)
    {
        png_error(png, "Truncated PNG data");
        return;
    }
    std::memcpy(out, ctx->p + ctx->off, count);
    ctx->off += count;
}

ImagePluginResult DecodePng(const Source& src)
{
    ImagePluginResult result = { IMAGE_PLUGIN_UNKNOWN_ERROR, nullptr };
    lastError[0] = '\0';

    FILE* file = nullptr;
    if (src.span == nullptr)
    {
#ifdef _WIN32
        auto err = _wfopen_s(&file, src.path, L"rb");
        (void)err;
#else
        file = std::fopen(src.path, "rb");
#endif
        if (file == nullptr)
        {
            snprintf(lastError, sizeof(lastError), "Failed to open file");
            result.code = IMAGE_PLUGIN_FILE_ERROR;
            return result;
        }
    }
    else if (src.size == 0)
    {
        snprintf(lastError, sizeof(lastError), "Empty input");
        result.code = IMAGE_PLUGIN_FILE_ERROR;
        return result;
    }

    png_structp png_ptr = png_create_read_struct(PNG_LIBPNG_VER_STRING, nullptr, nullptr, nullptr);
    if (!png_ptr)
    {
        if (file) fclose(file);
        snprintf(lastError, sizeof(lastError), "Failed to create PNG read struct");
        result.code = IMAGE_PLUGIN_ALLOC_ERROR;
        return result;
    }

    png_infop info_ptr = png_create_info_struct(png_ptr);
    if (!info_ptr)
    {
        png_destroy_read_struct(&png_ptr, nullptr, nullptr);
        if (file) fclose(file);
        snprintf(lastError, sizeof(lastError), "Failed to create PNG info struct");
        result.code = IMAGE_PLUGIN_ALLOC_ERROR;
        return result;
    }

    if (setjmp(png_jmpbuf(png_ptr)))
    {
        /* libpng has already reported through png_error/png_warning.  The
         * read context lives on this frame, so nothing else to release. */
        png_destroy_read_struct(&png_ptr, &info_ptr, nullptr);
        if (file) fclose(file);
        snprintf(lastError, sizeof(lastError), "PNG decode failed");
        result.code = IMAGE_PLUGIN_DECODE_ERROR;
        return result;
    }

    /* Declared here, not inside the else branch below: libpng stores the
     * pointer and uses it for the rest of the decode, so the cursor has to live
     * until the read struct is destroyed.  Scoped inside the branch it would be
     * a dangling pointer for every png_read_* call after it. */
    ReadCtx ctx{ src.span, src.size, 0 };

    if (file)
    {
        png_init_io(png_ptr, file);
    }
    else
    {
        png_set_read_fn(png_ptr, &ctx, SpanRead);
    }

    png_read_info(png_ptr, info_ptr);

    int width      = png_get_image_width(png_ptr, info_ptr);
    int height     = png_get_image_height(png_ptr, info_ptr);
    int color_type = png_get_color_type(png_ptr, info_ptr);
    int bit_depth  = png_get_bit_depth(png_ptr, info_ptr);

    /* libpng is fed untrusted files, so validate what it reports instead of
     * asserting on it: assert() aborts a debug build and lets a release build
     * continue with a zero-sized / bogus allocation. */
    if (width <= 0 || height <= 0 || bit_depth <= 0)
    {
        png_destroy_read_struct(&png_ptr, &info_ptr, nullptr);
        if (file) fclose(file);
        snprintf(lastError, sizeof(lastError),
                 "Invalid PNG header: width=%d height=%d bit depth=%d",
                 width, height, bit_depth);
        result.code = IMAGE_PLUGIN_DECODE_ERROR;
        return result;
    }

    // Expand palette to RGB
    if (color_type == PNG_COLOR_TYPE_PALETTE)
        png_set_palette_to_rgb(png_ptr);
    // Expand low-bit-depth gray to 8-bit
    if (color_type == PNG_COLOR_TYPE_GRAY && bit_depth < 8)
        png_set_expand_gray_1_2_4_to_8(png_ptr);
    // Expand tRNS transparency chunk to a full alpha channel
    if (png_get_valid(png_ptr, info_ptr, PNG_INFO_tRNS))
        png_set_tRNS_to_alpha(png_ptr);
    // For 16-bit data swap from big-endian (PNG native) to host byte order
    if (bit_depth == 16)
        png_set_swap(png_ptr);

    png_read_update_info(png_ptr, info_ptr);

    // Re-read metadata after transforms
    bit_depth  = png_get_bit_depth(png_ptr, info_ptr);
    color_type = png_get_color_type(png_ptr, info_ptr);
    int componentsPerPixel = png_get_channels(png_ptr, info_ptr);

    // --- Bytes per channel and colour space ---
    int bpc = (bit_depth == 16) ? 2 : 1;

    // --- Colour space: prefer explicit PNG metadata over heuristics ---
    ImageColorSpace colorSpace;
    {
        // Gamma value for linear light (1.0) expressed in the same scale as
        // the value returned by png_get_gAMA (which already gives a double).
        constexpr double LINEAR_GAMMA_LOWER = 0.99;
        constexpr double LINEAR_GAMMA_UPPER = 1.01;
        int srgb_intent = -1;
        double gamma_val = 0.0;
        if (png_get_sRGB(png_ptr, info_ptr, &srgb_intent) == PNG_INFO_sRGB) {
            colorSpace = IMAGE_COLOR_SPACE_SRGB;
        } else if (png_get_gAMA(png_ptr, info_ptr, &gamma_val) == PNG_INFO_gAMA
                   && gamma_val > LINEAR_GAMMA_LOWER && gamma_val < LINEAR_GAMMA_UPPER) {
            colorSpace = IMAGE_COLOR_SPACE_LINEAR;
        } else {
            // Convention: 16-bit PNG is most commonly linear-light encoded
            colorSpace = (bit_depth == 16) ? IMAGE_COLOR_SPACE_LINEAR : IMAGE_COLOR_SPACE_SRGB;
        }
    }

    // --- Per-component format descriptor ---
    IWImageFormat fmt = {};
    {
        const uint16_t bw = static_cast<uint16_t>(bit_depth);

        fmt.componentCount = static_cast<uint16_t>(componentsPerPixel);
        fmt.bitsPerPixel   = static_cast<uint16_t>(componentsPerPixel * bit_depth);

        switch (color_type) {
        case PNG_COLOR_TYPE_GRAY:
            fmt.components[0] = { IW_COMPONENT_SEMANTIC_GRAY, IW_COMPONENT_CLASS_UNORM, 0, bw };
            break;
        case PNG_COLOR_TYPE_GRAY_ALPHA:
            fmt.components[0] = { IW_COMPONENT_SEMANTIC_GRAY, IW_COMPONENT_CLASS_UNORM, 0,  bw };
            fmt.components[1] = { IW_COMPONENT_SEMANTIC_A,    IW_COMPONENT_CLASS_UNORM, bw, bw };
            break;
        case PNG_COLOR_TYPE_RGBA:
            fmt.components[0] = { IW_COMPONENT_SEMANTIC_R, IW_COMPONENT_CLASS_UNORM, static_cast<uint16_t>(0 * bw), bw };
            fmt.components[1] = { IW_COMPONENT_SEMANTIC_G, IW_COMPONENT_CLASS_UNORM, static_cast<uint16_t>(1 * bw), bw };
            fmt.components[2] = { IW_COMPONENT_SEMANTIC_B, IW_COMPONENT_CLASS_UNORM, static_cast<uint16_t>(2 * bw), bw };
            fmt.components[3] = { IW_COMPONENT_SEMANTIC_A, IW_COMPONENT_CLASS_UNORM, static_cast<uint16_t>(3 * bw), bw };
            break;
        default: /* PNG_COLOR_TYPE_RGB and palette-expanded RGB */
            fmt.components[0] = { IW_COMPONENT_SEMANTIC_R, IW_COMPONENT_CLASS_UNORM, static_cast<uint16_t>(0 * bw), bw };
            fmt.components[1] = { IW_COMPONENT_SEMANTIC_G, IW_COMPONENT_CLASS_UNORM, static_cast<uint16_t>(1 * bw), bw };
            fmt.components[2] = { IW_COMPONENT_SEMANTIC_B, IW_COMPONENT_CLASS_UNORM, static_cast<uint16_t>(2 * bw), bw };
            break;
        }
    }

    int    strideBytes = width * componentsPerPixel * bpc;
    size_t dataSize    = static_cast<size_t>(height) * strideBytes;

    /* Allocated before the descriptor is populated: `new` throws on failure,
     * so a null return is not a test that can succeed, and a throw here must
     * not leak the read struct. */
    uint8_t* pixels = nullptr;
    try
    {
        pixels = new uint8_t[dataSize];
    }
    catch (const std::bad_alloc&)
    {
        png_destroy_read_struct(&png_ptr, &info_ptr, nullptr);
        if (file) fclose(file);
        snprintf(lastError, sizeof(lastError), "Failed to allocate image buffer");
        result.code = IMAGE_PLUGIN_ALLOC_ERROR;
        return result;
    }

    ImagePluginData* data = nullptr;
    try
    {
        data = new ImagePluginData{
            .width             = width,
            .height            = height,
            .stride            = strideBytes,
            .colorSpace        = colorSpace,
            .size              = dataSize,
            .data              = pixels,
            .format            = fmt
        };
    }
    catch (const std::bad_alloc&)
    {
        delete[] pixels;
        png_destroy_read_struct(&png_ptr, &info_ptr, nullptr);
        if (file) fclose(file);
        snprintf(lastError, sizeof(lastError), "Failed to allocate image buffer");
        result.code = IMAGE_PLUGIN_ALLOC_ERROR;
        return result;
    }

    for (size_t y = 0; y < static_cast<size_t>(height); ++y)
    {
        png_bytep row = data->data + y * static_cast<size_t>(strideBytes);
        png_read_rows(png_ptr, &row, nullptr, 1);
    }

    png_destroy_read_struct(&png_ptr, &info_ptr, nullptr);
    if (file) fclose(file);
    result.code = IMAGE_PLUGIN_OK;
    result.data = data;
    return result;
}

} // namespace

/* Exceptions may not cross this boundary; see the note in ImageLoaderWebp. */
export extern "C" IMAGEPLUGIN_API ImagePluginResult LoadImageFromFile(const ImagePluginPath filePath)
{
    try
    {
        Source src;
        src.path = filePath;
        return DecodePng(src);
    }
    catch (const std::bad_alloc&)
    {
        snprintf(lastError, sizeof(lastError), "Out of memory");
        return ImagePluginResult{ IMAGE_PLUGIN_ALLOC_ERROR, nullptr };
    }
    catch (...)
    {
        snprintf(lastError, sizeof(lastError), "Unexpected failure");
        return ImagePluginResult{ IMAGE_PLUGIN_UNKNOWN_ERROR, nullptr };
    }
}

export extern "C" IMAGEPLUGIN_API ImagePluginResult LoadImageFromMemory(const uint8_t* data, size_t size)
{
    /* A null span is not a path and must not be treated as one: this entry
     * point has no filesystem fallback, so null is rejected like empty input. */
    if (data == nullptr || size == 0)
    {
        snprintf(lastError, sizeof(lastError), "Empty input");
        return ImagePluginResult{ IMAGE_PLUGIN_FILE_ERROR, nullptr };
    }
    try
    {
        Source src;
        src.span = data;
        src.size = size;
        return DecodePng(src);
    }
    catch (const std::bad_alloc&)
    {
        snprintf(lastError, sizeof(lastError), "Out of memory");
        return ImagePluginResult{ IMAGE_PLUGIN_ALLOC_ERROR, nullptr };
    }
    catch (...)
    {
        snprintf(lastError, sizeof(lastError), "Unexpected failure");
        return ImagePluginResult{ IMAGE_PLUGIN_UNKNOWN_ERROR, nullptr };
    }
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
