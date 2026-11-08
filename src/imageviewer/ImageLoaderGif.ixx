module;

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cstdint>
#include <memory>
#include <new>
#include <stdexcept>
#include <string>
#include <vector>
#include "ImagePluginDef.h"
#include <gif_lib.h>

export module ImageLoaderGif;

static thread_local char lastError[256] = "";

namespace {

/*
 * Largest raster this loader will allocate: 16384 x 16384 pixels.
 *
 * GIF dimensions are 16-bit fields, so a legitimate canvas is at most
 * 65535 x 65535 -- but the canvas is the *logical screen*, which a malformed
 * file can state independently of the frames inside it.  1 << 28 pixels is
 * generous for real content (a 4K GIF is 8.3 M) and keeps width * height * 4
 * well inside size_t on every target, so the multiply below cannot wrap.
 */
constexpr uint64_t kMaxPixels = 1ull << 28;

/*
 * Where the compressed bytes come from.  Exactly one field is set: `span` for
 * LoadImageFromMemory, `path` for LoadImageFromFile.
 *
 * The file path is read into memory and then fed through the same span
 * decoder.  That is deliberate and it is the one place this loader differs in
 * shape from the PNG one (which hands libpng a FILE*):
 *
 *   - DGifOpenFileName takes a *narrow* char* and giflib opens it with
 *     fopen().  On Windows that is the ANSI code page, not UTF-16, so a wide
 *     path handed to it would be mis-decoded for any non-ASCII character.
 *     Reading the file ourselves through std::filesystem/_wfopen keeps the
 *     plugin's existing wide-path contract instead of introducing a second,
 *     narrower one.
 *   - DGifOpenFileHandle takes an fd and giflib *closes* it in DGifCloseFile,
 *     which makes an fopen/_fileno hand-off a double-close hazard.
 *
 * So there is exactly one decoder path here: GifMemRead over a span.  GIFs are
 * small, and JPEG, WebP and Stb already pay the same read-then-decode cost.
 */
struct Source
{
    const uint8_t*  span = nullptr;
    size_t          size = 0;
    ImagePluginPath path = nullptr;
};

/* Read cursor behind giflib's InputFunc. */
struct MemSource
{
    const uint8_t* data = nullptr;
    size_t         size = 0;
    size_t         off  = 0;
};

/*
 * giflib read callback over a caller-owned span.
 *
 * Returns the number of bytes actually copied, and 0 at end of input.  A short
 * read is reported as short -- never padded -- so giflib surfaces truncated
 * data as D_GIF_ERR_EOF_TOO_SOON / D_GIF_ERR_IMAGE_DEFECT rather than walking
 * off the end of the buffer.  This is the same discipline as the PNG loader's
 * SpanRead.
 */
int GifMemRead(GifFileType* gif, GifByteType* dest, int count)
{
    auto* src = static_cast<MemSource*>(gif->UserData);
    if (src == nullptr || dest == nullptr || count <= 0) return 0;
    if (src->off >= src->size) return 0;

    const size_t want  = static_cast<size_t>(count);
    const size_t avail = src->size - src->off;
    const size_t take  = (want < avail) ? want : avail;
    std::memcpy(dest, src->data + src->off, take);
    src->off += take;
    return static_cast<int>(take);
}

/*
 * Owns the giflib handle so every exit path closes it exactly once, including
 * the throw paths (Pass 1's TIFF precedent: the exception barrier turns a leak
 * into a permanent one).
 */
struct GifHandle
{
    GifFileType* gif = nullptr;
    ~GifHandle()
    {
        if (gif != nullptr)
        {
            int closeError = D_GIF_SUCCEEDED;
            DGifCloseFile(gif, &closeError);
        }
    }
    GifHandle(const GifHandle&)            = delete;
    GifHandle& operator=(const GifHandle&) = delete;
    GifHandle() = default;
};

/* giflib returns const char* that may be null for an unknown code. */
void SetErrorFromCode(int code)
{
    const char* text = GifErrorString(code);
    snprintf(lastError, sizeof(lastError), "GIF decode failed: %s",
             text != nullptr ? text : "unknown giflib error");
}

/* Read a whole file into `out`.  Returns false and sets lastError on failure. */
bool ReadWholeFile(ImagePluginPath path, std::vector<uint8_t>& out)
{
    FILE* file = nullptr;
#ifdef _WIN32
    auto err = _wfopen_s(&file, path, L"rb");
    (void)err;
#else
    file = std::fopen(path, "rb");
#endif
    if (file == nullptr)
    {
        snprintf(lastError, sizeof(lastError), "Failed to open file");
        return false;
    }

    std::fseek(file, 0, SEEK_END);
    const long fileSize = std::ftell(file);
    std::fseek(file, 0, SEEK_SET);
    if (fileSize <= 0)
    {
        std::fclose(file);
        snprintf(lastError, sizeof(lastError), "Invalid file size");
        return false;
    }

    out.resize(static_cast<size_t>(fileSize));
    const size_t got = std::fread(out.data(), 1, out.size(), file);
    std::fclose(file);
    if (got != out.size())
    {
        snprintf(lastError, sizeof(lastError), "Failed to read file");
        return false;
    }
    return true;
}

/*
 * Composite one frame onto a canvas of the logical screen and return RGBA8.
 *
 * The ABI carries a single ImagePluginData, so this produces one image, not an
 * animation.  It mirrors what the stb path returned: the first frame, drawn in
 * its own rectangle on the logical screen.
 *
 * The canvas starts fully transparent (all zeroes).  Pixels the frame does not
 * cover stay transparent, and so do pixels whose palette index is the graphic
 * control extension's transparent index.  Everything else is opaque.
 */
ImagePluginData* CompositeFirstFrame(GifFileType* gif)
{
    const int canvasW = static_cast<int>(gif->SWidth);
    const int canvasH = static_cast<int>(gif->SHeight);
    if (canvasW <= 0 || canvasH <= 0) return nullptr;
    if (static_cast<uint64_t>(canvasW) * static_cast<uint64_t>(canvasH) > kMaxPixels)
        return nullptr;
    if (gif->ImageCount < 1 || gif->SavedImages == nullptr) return nullptr;

    const SavedImage&    frame = gif->SavedImages[0];
    const GifImageDesc&  desc  = frame.ImageDesc;
    const ColorMapObject* cmap = desc.ColorMap != nullptr ? desc.ColorMap
                                                          : gif->SColorMap;
    if (cmap == nullptr || cmap->Colors == nullptr || cmap->ColorCount <= 0)
        return nullptr;
    if (frame.RasterBits == nullptr) return nullptr;
    if (desc.Width <= 0 || desc.Height <= 0) return nullptr;
    if (desc.Left < 0 || desc.Top < 0) return nullptr;

    /* The graphic control extension carries the transparent palette index.
     * -1 (NO_TRANSPARENT_COLOR) means the frame is fully opaque. */
    int transparent = NO_TRANSPARENT_COLOR;
    GraphicsControlBlock gcb{};
    if (DGifSavedExtensionToGCB(gif, 0, &gcb) == GIF_OK)
        transparent = gcb.TransparentColor;

    const size_t canvasBytes =
        static_cast<size_t>(canvasW) * static_cast<size_t>(canvasH) * 4u;

    /* Value-initialised, so every pixel starts at (0,0,0,0): transparent.  The
     * guard exists because the descriptor allocation below can throw, and
     * without it a bad_alloc there would leak the raster into the caller's
     * catch.  `new` throws rather than returning null, so there is no null
     * check to make here. */
    std::unique_ptr<uint8_t[]> rgbaGuard(new uint8_t[canvasBytes]());
    uint8_t* rgba = rgbaGuard.get();

    for (int fy = 0; fy < static_cast<int>(desc.Height); ++fy)
    {
        const int cy = static_cast<int>(desc.Top) + fy;
        if (cy >= canvasH) break;

        for (int fx = 0; fx < static_cast<int>(desc.Width); ++fx)
        {
            const int cx = static_cast<int>(desc.Left) + fx;
            if (cx >= canvasW) break;

            const GifPixelType index =
                frame.RasterBits[static_cast<size_t>(fy) * static_cast<size_t>(desc.Width) +
                                 static_cast<size_t>(fx)];
            if (transparent >= 0 && static_cast<int>(index) == transparent) continue;
            if (static_cast<int>(index) >= cmap->ColorCount) continue;

            const GifColorType& c = cmap->Colors[index];
            uint8_t* out = rgba + (static_cast<size_t>(cy) * static_cast<size_t>(canvasW) +
                                   static_cast<size_t>(cx)) * 4u;
            out[0] = c.Red;
            out[1] = c.Green;
            out[2] = c.Blue;
            out[3] = 255u;
        }
    }

    IWImageFormat fmt = {};
    fmt.componentCount = 4;
    fmt.bitsPerPixel   = 32;
    fmt.storageLayout  = IW_STORAGE_INTERLEAVED;
    fmt.components[0]  = { IW_COMPONENT_SEMANTIC_R, IW_COMPONENT_CLASS_UNORM,  0, 8 };
    fmt.components[1]  = { IW_COMPONENT_SEMANTIC_G, IW_COMPONENT_CLASS_UNORM,  8, 8 };
    fmt.components[2]  = { IW_COMPONENT_SEMANTIC_B, IW_COMPONENT_CLASS_UNORM, 16, 8 };
    fmt.components[3]  = { IW_COMPONENT_SEMANTIC_A, IW_COMPONENT_CLASS_UNORM, 24, 8 };

    ImagePluginData* data = new ImagePluginData{
        .width      = canvasW,
        .height     = canvasH,
        .stride     = canvasW * 4,
        .colorSpace = IMAGE_COLOR_SPACE_SRGB,
        .size       = canvasBytes,
        .data       = rgbaGuard.release(),   /* ownership moves to the descriptor */
        .format     = fmt
    };
    return data;
}

ImagePluginResult DecodeGif(const Source& src)
{
    ImagePluginResult result = { IMAGE_PLUGIN_UNKNOWN_ERROR, nullptr };
    lastError[0] = '\0';

    std::vector<uint8_t> owned;
    const uint8_t* bytes = src.span;
    size_t         count = src.size;

    if (bytes == nullptr)
    {
        if (!ReadWholeFile(src.path, owned))
        {
            result.code = IMAGE_PLUGIN_FILE_ERROR;
            return result;
        }
        bytes = owned.data();
        count = owned.size();
    }
    else if (count == 0)
    {
        snprintf(lastError, sizeof(lastError), "Empty input");
        result.code = IMAGE_PLUGIN_FILE_ERROR;
        return result;
    }

    /* giflib's InputFunc returns int, so a span larger than INT_MAX cannot be
     * described to it.  GIFs are orders of magnitude smaller; reject rather
     * than truncate. */
    if (count > static_cast<size_t>(0x7FFFFFFF))
    {
        snprintf(lastError, sizeof(lastError), "Input too large for giflib");
        result.code = IMAGE_PLUGIN_FILE_ERROR;
        return result;
    }

    MemSource mem{ bytes, count, 0 };

    int openError = D_GIF_SUCCEEDED;
    GifHandle handle;
    handle.gif = DGifOpen(&mem, GifMemRead, &openError);
    if (handle.gif == nullptr)
    {
        SetErrorFromCode(openError);
        result.code = IMAGE_PLUGIN_DECODE_ERROR;
        return result;
    }

    /* One call decodes every frame.  Only the first is composited below. */
    if (DGifSlurp(handle.gif) != GIF_OK)
    {
        const int slurpError = handle.gif->Error;
        SetErrorFromCode(slurpError);
        result.code = IMAGE_PLUGIN_DECODE_ERROR;
        return result;   /* handle closes on the way out */
    }

    ImagePluginData* data = CompositeFirstFrame(handle.gif);
    if (data == nullptr)
    {
        snprintf(lastError, sizeof(lastError),
                 "GIF has no usable first frame (canvas %dx%d, %d frame(s))",
                 static_cast<int>(handle.gif->SWidth),
                 static_cast<int>(handle.gif->SHeight),
                 handle.gif->ImageCount);
        result.code = IMAGE_PLUGIN_DECODE_ERROR;
        return result;
    }

    result.code = IMAGE_PLUGIN_OK;
    result.data = data;
    return result;
}

} // namespace

/*
 * Exceptions may not cross this boundary; see the note in ImageLoaderWebp.
 *
 * No preview export: giflib has no cheap reduced-resolution decode.  The
 * preview ABI is optional, and a preview that decoded the full image would be
 * a second full decode rather than a cheaper one, so the honest answer is to
 * omit it.  PluginManager resolves the symbol optionally and the viewer falls
 * back to the full decode for GIF.
 */
export extern "C" IMAGEPLUGIN_API ImagePluginResult LoadImageFromFile(const ImagePluginPath filePath)
{
    try
    {
        Source src;
        src.path = filePath;
        return DecodeGif(src);
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
        return DecodeGif(src);
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
