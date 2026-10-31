module;

#include <cstdio>
#include <stdexcept>
#include <cstring>
#include <cstdint>
#include <algorithm>
#include <memory>
#include <vector>
#include <fstream>
#include <filesystem>
#include <jpeglib.h>
#include <setjmp.h>
#include <format>
#include "ImagePluginDef.h"
#include "ExifOrientation.hpp"

export module ImageLoaderJpeg;

namespace {
struct my_error_mgr
{
	struct jpeg_error_mgr pub;
	jmp_buf setjmp_buffer;
};
} // namespace

/* Named, not anonymous: GCC 14 rejects a template instantiation whose argument
 * has internal linkage inside a module ("references internal linkage entity"
 * from std::make_unique below).  Inside a module purview a named namespace is
 * still module-local and unexported, so linkage does not change. */
namespace jpeg_detail {

/* Owns everything the decode allocates: the compressed file bytes first, then
 * the pixel storage, then the descriptor handed back to the caller on success.
 * libjpeg reports fatal errors by longjmp()ing out of the failing call, so this
 * holder is created *before* setjmp() and is not replaced while a libjpeg call
 * can still longjmp; an automatic variable written between setjmp() and
 * longjmp() would be indeterminate on the error path and leak.  The upright
 * buffer replaces the decode buffer only after the last libjpeg call.
 *
 * The compressed bytes are read through std::ifstream (the plugin's own C++
 * runtime) and fed to libjpeg with jpeg_mem_src().  Passing a FILE* from this
 * DLL into libjpeg would cross a CRT boundary: in a Debug build the plugin uses
 * the debug CRT while the release libjpeg uses the release CRT, and fread() on
 * the foreign FILE* fails with FAST_FAIL_INVALID_ARG.  A memory source has no
 * such shared handle.
 */
struct OwnedImagePluginData
{
	std::unique_ptr<std::vector<uint8_t>> file;
	std::unique_ptr<uint8_t[]>            pixels;
	std::unique_ptr<ImagePluginData>      info;
};

} // namespace jpeg_detail

namespace {

/* libjpeg writes straight into the destination rows below, so a larger batch
 * only reduces call overhead.  16 rows keeps the stack array small while
 * avoiding one call per scanline. */
constexpr int kMaxScanlinesPerRead = 16;

/* Largest DCT scale step libjpeg-turbo implements (1/8). */
constexpr int kMaxScaleDenom = 8;

/* The smallest denominator (largest image) whose longest side still fits in
 * maxDimension.  Returns 1 when the full image is already small enough, which
 * lets the caller skip a second decode entirely; returns 1/8 when even that is
 * larger than maxDimension (libjpeg's largest scale step).  maxDimension == 0
 * asks for the fastest preview. */
int ChoosePreviewDenom(int width, int height, uint32_t maxDimension) noexcept
{
	if (maxDimension == 0u)
		return kMaxScaleDenom;

	const long long longest = std::max(width, height);
	for (const int denom : { 1, 2, 4, 8 })
	{
		if (static_cast<long long>(maxDimension) * denom >= longest)
			return denom;
	}
	return kMaxScaleDenom;
}

bool ReadWholeFile(ImagePluginPath filePath, std::vector<uint8_t>& out)
{
	try
	{
		std::ifstream in(std::filesystem::path(filePath),
		                 std::ios::binary | std::ios::ate);
		if (!in)
			return false;

		const std::streamoff size = in.tellg();
		/* jpeg_mem_src() takes an unsigned long, which is 32 bits on Windows.
		 * Nothing this viewer opens is near that limit, so reject instead of
		 * silently truncating. */
		if (size <= 0 || static_cast<unsigned long long>(size) > 0x7FFFFFFFull)
			return false;

		out.resize(static_cast<size_t>(size));
		in.seekg(0, std::ios::beg);
		in.read(reinterpret_cast<char*>(out.data()), size);
		return in.gcount() == size;
	}
	catch (...)
	{
		return false;
	}
}

} // namespace

EXTERN(void)
my_error_exit(j_common_ptr cinfo)
{
	struct my_error_mgr* myerr = (struct my_error_mgr*)cinfo->err;
	(*cinfo->err->output_message)(cinfo);
	longjmp(myerr->setjmp_buffer, 1);
}

static thread_local char lastError[256] = "";

/* Shared implementation for the full and preview entry points.
 *
 * preview == false always decodes at 1/1.  preview == true picks the largest
 * DCT scale step that fits maxDimension, so a 24 MP file can produce a
 * 1/4-resolution preview without decoding every output pixel through the IDCT.
 * Entropy decoding still has to walk all coefficients, which is why the win is
 * roughly 35-45 % rather than 8x -- but that still moves the first frame
 * substantially earlier.
 *
 * Both paths write the upright full-resolution dimensions into fullWidth and
 * fullHeight when those pointers are non-null. */
static ImagePluginResult DecodeJpeg(ImagePluginPath filePath,
                                    bool preview,
                                    uint32_t maxDimension,
                                    int* fullWidth,
                                    int* fullHeight)
{
	ImagePluginResult result = { .code = IMAGE_PLUGIN_UNKNOWN_ERROR, .data = nullptr };
	lastError[0] = '\0';
	if (fullWidth)  *fullWidth = 0;
	if (fullHeight) *fullHeight = 0;

	/* Created before setjmp so the longjmp error path can still release it. */
	const auto owned = std::make_unique<jpeg_detail::OwnedImagePluginData>();
	owned->file = std::make_unique<std::vector<uint8_t>>();

	if (!ReadWholeFile(filePath, *owned->file))
	{
		snprintf(lastError, sizeof(lastError), "Failed to read file");
		result.code = IMAGE_PLUGIN_FILE_ERROR;
		return result;
	}

	struct my_error_mgr jerr = {
		.pub = {
			.error_exit = my_error_exit } };

	struct jpeg_decompress_struct cinfo = {
		.err = jpeg_std_error(&jerr.pub) };

	if (setjmp(jerr.setjmp_buffer))
	{
		jpeg_destroy_decompress(&cinfo);
		snprintf(lastError, sizeof(lastError), "JPEG decode failed");
		result.code = IMAGE_PLUGIN_DECODE_ERROR;
		return result;   /* owned releases whatever was allocated so far */
	}

	jpeg_create_decompress(&cinfo);
	jpeg_mem_src(&cinfo, owned->file->data(),
	             static_cast<unsigned long>(owned->file->size()));

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
	 * Anything else is rejected instead of described.
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

	/* Full-resolution geometry, in storage order, before scaling. */
	const int storedFullWidth  = static_cast<int>(cinfo.image_width);
	const int storedFullHeight = static_cast<int>(cinfo.image_height);

	if (fullWidth)
		*fullWidth  = exif::OrientedWidth(storedFullWidth, storedFullHeight, orientation);
	if (fullHeight)
		*fullHeight = exif::OrientedHeight(storedFullWidth, storedFullHeight, orientation);

	/* DCT scaling is applied between header parsing and start_decompress.
	 * Fancy (triangle-filter) chroma upsampling is the quality default for the
	 * final image; a preview is upscaled by the GPU anyway, so the cheaper
	 * box filter is the right trade there. */
	const int scaleDenom = preview
		? ChoosePreviewDenom(storedFullWidth, storedFullHeight, maxDimension)
		: 1;
	cinfo.scale_num       = 1;
	cinfo.scale_denom     = scaleDenom;
	cinfo.do_fancy_upsampling = (scaleDenom == 1) ? TRUE : FALSE;

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
		snprintf(lastError, sizeof(lastError),
		         "JPEG output component mismatch: expected %d, got %d",
		         channelCount, cinfo.output_components);
		result.code = IMAGE_PLUGIN_DECODE_ERROR;
		return result;
	}

	constexpr int kBitsPerComponent = 8;
	const int bytesPerPixel = channelCount * (kBitsPerComponent / 8);

	/* Scaled output geometry (== full geometry when scaleDenom == 1). */
	const int    storedWidth  = static_cast<int>(cinfo.output_width);
	const int    storedHeight = static_cast<int>(cinfo.output_height);
	const int    rowStride    = storedWidth * bytesPerPixel;
	const size_t dataSize     = static_cast<size_t>(rowStride) * storedHeight;

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
		snprintf(lastError, sizeof(lastError), "Failed to allocate image buffer");
		result.code = IMAGE_PLUGIN_ALLOC_ERROR;
		return result;
	}

	/* Decode straight into the destination instead of into a libjpeg sample
	 * buffer and then memcpy()ing every row.  On a 24 MP image that removes one
	 * full 72 MB pass over memory. */
	while (cinfo.output_scanline < cinfo.output_height)
	{
		const JDIMENSION first = cinfo.output_scanline;
		const JDIMENSION want  = std::min<JDIMENSION>(
			kMaxScanlinesPerRead, cinfo.output_height - first);

		JSAMPROW rows[kMaxScanlinesPerRead];
		for (JDIMENSION i = 0; i < want; ++i)
			rows[i] = owned->pixels.get() +
			          static_cast<size_t>(first + i) * rowStride;

		const JDIMENSION got = jpeg_read_scanlines(&cinfo, rows, want);
		if (got == 0)
		{
			jpeg_destroy_decompress(&cinfo);
			snprintf(lastError, sizeof(lastError), "JPEG read error");
			result.code = IMAGE_PLUGIN_DECODE_ERROR;
			return result;
		}
	}

	jpeg_finish_decompress(&cinfo);
	/* No libjpeg call below this point can longjmp, so replacing the pixel
	 * holder is safe even though it was created before setjmp(). */
	jpeg_destroy_decompress(&cinfo);

	/* The compressed bytes are no longer needed.  Release them before the caller
	 * copies the raster into its own Image, so the file and the copied pixels do
	 * not overlap in memory any longer than the decode itself. */
	owned->file.reset();

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

export extern "C" IMAGEPLUGIN_API ImagePluginResult LoadImageFromFile(const ImagePluginPath filePath)
{
	return DecodeJpeg(filePath, false, 0u, nullptr, nullptr);
}

export extern "C" IMAGEPLUGIN_API ImagePluginResult LoadImagePreviewFromFile(
	const ImagePluginPath filePath,
	const uint32_t maxDimension,
	int* fullWidth,
	int* fullHeight)
{
	return DecodeJpeg(filePath, true, maxDimension, fullWidth, fullHeight);
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
