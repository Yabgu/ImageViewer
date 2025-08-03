module;

#include <cstdio>
#include <cstring>
#include <cstdint>
#include <jpeglib.h>
#include <setjmp.h>
#include "ImagePluginDef.h"

struct my_error_mgr
{
	struct jpeg_error_mgr pub;
	jmp_buf setjmp_buffer;
};

EXTERN(void)
my_error_exit(j_common_ptr cinfo)
{
	struct my_error_mgr* myerr = (struct my_error_mgr*)cinfo->err;
	(*cinfo->err->output_message)(cinfo);
	longjmp(myerr->setjmp_buffer, 1);
}

export module ImageLoaderJpeg;

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

    if (setjmp(jerr.setjmp_buffer))
    {
        jpeg_destroy_decompress(&cinfo);
        fclose(file);
        snprintf(lastError, sizeof(lastError), "JPEG decode failed");
        result.code = IMAGE_PLUGIN_DECODE_ERROR;
        return result;
    }

    jpeg_create_decompress(&cinfo);
    jpeg_stdio_src(&cinfo, file);
    jpeg_read_header(&cinfo, TRUE);
    jpeg_start_decompress(&cinfo);

    int row_stride = cinfo.output_width * cinfo.output_components;
    constexpr int MaxLinesToRead = 1;

    JSAMPARRAY buffer = (*cinfo.mem->alloc_sarray)((j_common_ptr)&cinfo, JPOOL_IMAGE, row_stride, MaxLinesToRead);

    ImagePluginData* data = new ImagePluginData{
        .width = static_cast<int>(cinfo.output_width),
        .height = static_cast<int>(cinfo.output_height),
        .componentsPerPixel = cinfo.output_components,
        .stride = static_cast<int>(cinfo.output_width * cinfo.output_components),
        .size = static_cast<size_t>(cinfo.output_width * cinfo.output_height * cinfo.output_components),
        .data = new uint8_t[cinfo.output_width * cinfo.output_height * cinfo.output_components]
    };
    if (!data->data) {
        jpeg_destroy_decompress(&cinfo);
        fclose(file);
        delete data;
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
            delete[] data->data;
            delete data;
            snprintf(lastError, sizeof(lastError), "JPEG read error");
            result.code = IMAGE_PLUGIN_DECODE_ERROR;
            return result;
        }
        std::memcpy(&data->data[current_scanline * row_stride], buffer[0], row_stride * readlines);
    }

    jpeg_finish_decompress(&cinfo);
    jpeg_destroy_decompress(&cinfo);
    fclose(file);
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
