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

namespace {
struct my_error_mgr
{
	struct jpeg_error_mgr pub;
	jmp_buf setjmp_buffer;
};

/* Owns everything the decode allocates: the pixel storage first, then the
 * descriptor handed back to the caller on success.  libjpeg reports fatal
 * errors by longjmp()ing out of the failing call, so this holder is created
 * *before* setjmp() and never reassigned afterwards; an automatic variable
 * written between setjmp() and longjmp() would be indeterminate on the error
 * path and leak or double-free. */
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
    jpeg_read_header(&cinfo, TRUE);
    jpeg_start_decompress(&cinfo);

    int row_stride = cinfo.output_width * cinfo.output_components;
    constexpr int MaxLinesToRead = 1;

    JSAMPARRAY buffer = (*cinfo.mem->alloc_sarray)((j_common_ptr)&cinfo, JPOOL_IMAGE, row_stride, MaxLinesToRead);

    int components = cinfo.output_components;
    size_t dataSize = static_cast<size_t>(cinfo.output_width) * cinfo.output_height * components;

    IWImageFormat fmt = {};
    fmt.componentCount = static_cast<uint16_t>(components);
    fmt.bitsPerPixel   = static_cast<uint16_t>(components * 8);
    if (components == 1) {
        fmt.components[0] = { IW_COMPONENT_SEMANTIC_GRAY, IW_COMPONENT_CLASS_UINT, 0, 8 };
    } else {
        fmt.components[0] = { IW_COMPONENT_SEMANTIC_R, IW_COMPONENT_CLASS_UINT,  0, 8 };
        fmt.components[1] = { IW_COMPONENT_SEMANTIC_G, IW_COMPONENT_CLASS_UINT,  8, 8 };
        fmt.components[2] = { IW_COMPONENT_SEMANTIC_B, IW_COMPONENT_CLASS_UINT, 16, 8 };
    }

    owned->pixels = std::make_unique<uint8_t[]>(dataSize);
    if (!owned->pixels) {
        jpeg_destroy_decompress(&cinfo);
        fclose(file);
        snprintf(lastError, sizeof(lastError), "Failed to allocate image buffer");
        result.code = IMAGE_PLUGIN_ALLOC_ERROR;
        return result;
    }

    owned->info = std::make_unique<ImagePluginData>(ImagePluginData{
        .width      = static_cast<int>(cinfo.output_width),
        .height     = static_cast<int>(cinfo.output_height),
        .stride     = static_cast<int>(cinfo.output_width) * components,
        .colorSpace = IMAGE_COLOR_SPACE_SRGB,
        .size       = dataSize,
        .data       = owned->pixels.get(),
        .format     = fmt
    });

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
        std::memcpy(owned->pixels.get() + current_scanline * row_stride, buffer[0], row_stride * readlines);
    }

    jpeg_finish_decompress(&cinfo);
    jpeg_destroy_decompress(&cinfo);
    fclose(file);

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
