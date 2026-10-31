module;

#include <cstdio>
#include <stdexcept>
#include <cstring>
#include <cstdint>
#include <algorithm>
#include <filesystem>
#include <regex>
#include <format>
#include <exception>
#include <future>
#include <memory>
#include <mutex>
#include <optional>
#include <utility>
#include "ImagePluginDef.h"

export module Image;

import PluginManager;

/* Returned by PreviewFromFile().  `image` is the reduced-resolution raster;
 * `fullWidth`/`fullHeight` are the upright dimensions of the full-resolution
 * image (0 when the loader cannot report them).  `fullResolution` says the
 * preview already is the final image, so no second decode is needed. */
export struct ImagePreview;

/* Hand-off between the decode worker and the UI thread.  Defined after Image
 * because it stores Image by value. */
export struct ProgressiveImageLoad;

// ─── Image class ─────────────────────────────────────────────────────────────
//
// Image stores raw pixel data exactly as the plugin (or caller) provides it.
// The format field (IWImageFormat) carries per-component descriptors so that
// any layout — RGB8, RGBA16, HSL mixed-precision, single-bit masks, planar
// infrared+visible pairs, etc. — can be represented without adding new enums.
//
// The rendering pipeline (TexturePool) converts from IWImageFormat to
// whatever the GPU requires at upload time.

export class Image
{
private:
	/* Stride of a single row in bytes.
	 * Interleaved: width × ⌈bitsPerPixel/8⌉
	 * Planar:      width × ⌈(bitsPerPixel/componentCount)/8⌉
	 *              (each plane has this stride; total size = stride × height × componentCount) */
	static int ComputeStride(int width, const IWImageFormat& fmt) noexcept
	{
		if (fmt.storageLayout == IW_STORAGE_PLANAR && fmt.componentCount > 0u) {
			// Planar: stride is the byte-width of one row of a single component plane.
			// bitsPerPixel is the total across all components; divide to get per-component.
			// Integer division rounds down — if components have unequal widths the caller
			// should set bitsPerPixel to the maximum component width instead.
			const uint16_t bitsPerSample =
			    static_cast<uint16_t>(fmt.bitsPerPixel / fmt.componentCount);
			return width * static_cast<int>((bitsPerSample + 7u) / 8u);
		}
		return width * static_cast<int>((fmt.bitsPerPixel + 7u) / 8u);
	}

	static size_t ComputeSize(int height, int stride, const IWImageFormat& fmt) noexcept
	{
		if (fmt.storageLayout == IW_STORAGE_PLANAR)
			return static_cast<size_t>(height) * stride * fmt.componentCount;
		return static_cast<size_t>(height) * stride;
	}

	static ImagePluginResult LoadImageViaPlugin(PluginManager& manager,
	                                             const std::filesystem::path& pluginPath,
	                                             const std::filesystem::path& imagePath)
	{
		auto entry = manager.getPlugin(pluginPath);
		if (!entry || !entry->loadFunc)
		{
			const std::string detail = manager.GetLastErrorText();
			throw std::runtime_error("Failed to load or resolve plugin: " + pluginPath.string()
			                         + (detail.empty() ? "" : " (" + detail + ")"));
		}
		return entry->loadFunc(imagePath.c_str());
	}

	static std::filesystem::path PluginFileName(const char* baseName)
	{
#ifdef _WIN32
#  ifdef __MINGW32__
		return std::format("lib{}.dll", baseName);
#  else
		return std::format("{}.dll", baseName);
#  endif
#elif defined(__APPLE__)
		return std::format("lib{}.dylib", baseName);
#else
		return std::format("lib{}.so", baseName);
#endif
	}

	/* One manager per process: FromFile() and PreviewFromFile() must share the
	 * loaded plugin handles and their optional exports. */
	static PluginManager& Manager()
	{
		static PluginManager manager;
		return manager;
	}

	static std::filesystem::path PluginPathFor(const std::filesystem::path& path)
	{
		auto extension = path.extension().string();
		if (std::regex_match(extension, std::regex("\\.jpeg|\\.jpg|\\.jfif", std::regex::icase)))
			return PluginFileName("ImageLoaderJpeg");
		if (std::regex_match(extension, std::regex("\\.png", std::regex::icase)))
			return PluginFileName("ImageLoaderPng");
		if (std::regex_match(extension, std::regex("\\.webp", std::regex::icase)))
			return PluginFileName("ImageLoaderWebp");
		if (std::regex_match(extension, std::regex("\\.tiff|\\.tif", std::regex::icase)))
			return PluginFileName("ImageLoaderTiff");
		if (std::regex_match(extension, std::regex("\\.bmp|\\.tga|\\.gif|\\.hdr|\\.pic|\\.ppm|\\.pgm", std::regex::icase)))
			return PluginFileName("ImageLoaderStb");
		[[unlikely]] throw std::runtime_error("Unknown file extension");
	}

	/* Copy a plugin-owned raster into an Image.  The plugin keeps ownership of
	 * its descriptor and buffer; the caller releases them with freeFunc. */
	static Image CopyFromPluginData(const ImagePluginData& pd)
	{
		Image result(pd.width, pd.height, pd.format, pd.colorSpace);
		const int pluginStride = pd.stride;
		if (pluginStride == result.stride) {
			std::memcpy(result.data, pd.data, pd.size);
		} else {
			const int copyBytes = std::min(pluginStride, result.stride);
			for (int y = 0; y < pd.height; ++y) {
				std::memcpy(result.data + static_cast<ptrdiff_t>(y) * result.stride,
				            pd.data    + static_cast<ptrdiff_t>(y) * pluginStride,
				            copyBytes);
			}
		}
		return result;
	}

	static void FreeImageResultViaPlugin(PluginManager& manager,
	                                      const std::filesystem::path& pluginPath,
	                                      ImagePluginResult& result)
	{
		auto entry = manager.getPlugin(pluginPath);
		if (!entry || !entry->freeFunc)
		{
			const std::string detail = manager.GetLastErrorText();
			throw std::runtime_error("Failed to load or resolve plugin: " + pluginPath.string()
			                         + (detail.empty() ? "" : " (" + detail + ")"));
		}
		entry->freeFunc(result.data);
		result.data = nullptr;
	}

public:
	const int           width;
	const int           height;
	const IWImageFormat format;      /* per-component layout descriptor                          */
	const int           stride;      /* row stride in bytes (see ComputeStride)                  */
	const size_t        size;        /* total buffer size in bytes (see ComputeSize)             */
	uint8_t* const      data;        /* raw pixel buffer                                         */
	const ImageColorSpace colorSpace; /* transfer function / colour space                        */

	/* Bytes consumed by one pixel (interleaved) or one sample (planar). */
	int BytesPerPixel() const noexcept
	{
		return static_cast<int>((format.bitsPerPixel + 7u) / 8u);
	}

	/* Number of components per pixel. */
	int ComponentCount() const noexcept
	{
		return static_cast<int>(format.componentCount);
	}

	Image(Image&& o) noexcept
		: width(o.width), height(o.height), format(o.format),
		  stride(o.stride), size(o.size), data(o.data), colorSpace(o.colorSpace)
	{
		const_cast<uint8_t*&>(o.data) = nullptr;
	}

	Image(const Image& o)
		: width(o.width), height(o.height), format(o.format),
		  stride(o.stride), size(o.size), data(new uint8_t[o.size]), colorSpace(o.colorSpace)
	{
		std::copy(o.data, o.data + o.size, data);
	}

	/* Primary constructor.  Takes ownership of storage; allocates internally if
	 * pixelData is null. */
	Image(int w, int h, IWImageFormat fmt,
	      ImageColorSpace cs = IMAGE_COLOR_SPACE_UNKNOWN,
	      uint8_t* pixelData = nullptr)
		: width(w), height(h), format(fmt),
		  stride(ComputeStride(w, fmt)),
		  size(ComputeSize(h, ComputeStride(w, fmt), fmt)),
		  data(pixelData ? pixelData : new uint8_t[ComputeSize(h, ComputeStride(w, fmt), fmt)]),
		  colorSpace(cs)
	{
		if (w <= 0 || h <= 0)
			[[unlikely]] throw std::runtime_error("Image dimensions must be positive");
	}

	~Image()
	{
		delete[] data;
	}

	static Image FromFile(const std::filesystem::path& path)
	{
		auto& manager = Manager();
		const std::filesystem::path pluginPath = PluginPathFor(path);

		ImagePluginResult pluginResult = LoadImageViaPlugin(manager, pluginPath, path);
		if (pluginResult.code != IMAGE_PLUGIN_OK || !pluginResult.data)
			throw std::runtime_error("Failed to load image via plugin. Error code: "
			                         + std::to_string(pluginResult.code));

		/* Allocate host Image with the exact format the plugin reported.
		 * No normalization is performed here — the format is preserved verbatim.
		 * The rendering layer (TexturePool) converts to a GPU-compatible layout
		 * at upload time. */
		try
		{
			Image result = CopyFromPluginData(*pluginResult.data);
			FreeImageResultViaPlugin(manager, pluginPath, pluginResult);
			return result;
		}
		catch (...)
		{
			FreeImageResultViaPlugin(manager, pluginPath, pluginResult);
			throw;
		}
	}

	/* Decode a reduced-resolution raster through the plugin's optional
	 * "LoadImagePreviewFromFile" export.  Plugins without that export (or that
	 * fail the preview decode) fall back to a normal full decode, and the result
	 * is marked fullResolution so the caller does not decode the file twice. */
	static ImagePreview PreviewFromFile(const std::filesystem::path& path,
	                                    uint32_t maxDimension);

	/* Start a background preview -> full load.  The worker publishes the preview
	 * first, then the full raster, through ProgressiveImageLoad::state; the UI
	 * thread polls that state and uploads each stage as it lands.  Both stages
	 * are upright and carry the plugin's real format descriptor. */
	static ProgressiveImageLoad LoadProgressive(const std::filesystem::path& path,
	                                            uint32_t maxDimension);

	/* Extract a rectangular sub-region.  The caller owns the returned pointer.
	 * Only interleaved formats are currently supported for sub-region extraction.
	 * Out-of-bounds regions are filled with zeroed bytes. */
	Image* Subsection(const int x, const int y, const int width, const int height) const
	{
		if (width < 0 || height < 0)
			[[unlikely]] throw std::runtime_error(
				"argument error, width and height cannot be negative");

		Image* res = new Image(width, height, format, colorSpace);
		std::fill_n(res->data, res->size, uint8_t{0});

		if (-x >= width || x >= this->width || -y >= height || y >= this->height)
			[[unlikely]] return res;

		const int srcX       = std::max<int>(x, 0);
		const int srcY       = std::max<int>(y, 0);
		const int destX      = std::max<int>(-x, 0);
		const int destY      = std::max<int>(-y, 0);
		const int bpp        = BytesPerPixel();
		const int copyWidth  = std::min<int>(width - destX, this->width - srcX);
		const int copyStride = copyWidth * bpp;
		const int copyHeight = std::min<int>(height - destY, this->height - srcY);

		for (int py = 0; py < copyHeight; ++py) {
			std::memcpy(
				res->data  + static_cast<ptrdiff_t>(destY + py) * res->stride + destX * bpp,
				this->data + static_cast<ptrdiff_t>(srcY  + py) * this->stride + srcX * bpp,
				copyStride);
		}

		return res;
	}
};

/* ── Progressive loading ─────────────────────────────────────────────────────
 *
 * A single worker decodes the preview first and the full raster second.  The
 * two results are published through a mutex-guarded state object rather than a
 * future, because the UI consumes them as two independent stages: it must be
 * able to put the preview on screen minutes before the full decode finishes,
 * and it must still be able to see the preview if the full decode fails.
 */
export struct ImagePreview
{
	Image image;
	int   fullWidth      = 0;   /* upright full-resolution dimensions, 0 = unknown */
	int   fullHeight     = 0;
	bool  fullResolution = false; /* preview already is the final raster            */

	ImagePreview(Image&& previewImage, int width, int height, bool isFullResolution)
		: image(std::move(previewImage)),
		  fullWidth(width),
		  fullHeight(height),
		  fullResolution(isFullResolution)
	{
	}
};

export struct ProgressiveImageState
{
	std::mutex mutex;
	std::optional<Image> preview;   /* low-resolution stage, published first       */
	std::optional<Image> full;      /* full-resolution stage, absent when no second
	                                 * decode is needed or the full decode failed   */
	bool preview_ready = false;
	bool preview_done  = false;   /* preview worker finished (success or failure) */
	bool done          = false;   /* full worker finished                       */
	bool full_expected = false;
	int  full_width    = 0;
	int  full_height   = 0;
	std::exception_ptr error;
};

export struct ProgressiveImageLoad
{
	std::shared_ptr<ProgressiveImageState> state;
	/* Present only when the loader has a cheap preview; both workers are
	 * independent and publish into the same state under its mutex. */
	std::future<void> preview_worker;
	std::future<void> worker;          /* full decode, or the only decode */
};

inline ImagePreview Image::PreviewFromFile(const std::filesystem::path& path,
                                           const uint32_t maxDimension)
{
	auto& manager = Manager();
	const std::filesystem::path pluginPath = PluginPathFor(path);

	auto entry = manager.getPlugin(pluginPath);
	if (!entry || !entry->loadFunc)
	{
		const std::string detail = manager.GetLastErrorText();
		throw std::runtime_error("Failed to load or resolve plugin: " + pluginPath.string()
		                         + (detail.empty() ? "" : " (" + detail + ")"));
	}

	if (entry->previewFunc)
	{
		int width = 0;
		int height = 0;
		ImagePluginResult previewResult =
			entry->previewFunc(path.c_str(), maxDimension, &width, &height);

		if (previewResult.code == IMAGE_PLUGIN_OK && previewResult.data)
		{
			try
			{
				Image preview = CopyFromPluginData(*previewResult.data);
				const int previewWidth  = preview.width;
				const int previewHeight = preview.height;
				entry->freeFunc(previewResult.data);

				/* When the loader reports the full dimensions, equality means the
				 * preview is already final.  Without them, a preview no larger
				 * than the requested bound can only be final when that bound is
				 * the full size.  Otherwise assume a sharper stage is coming. */
				const bool dimsKnown = (width > 0 && height > 0);
				const bool fullResolution = dimsKnown
					? (previewWidth == width && previewHeight == height)
					: (maxDimension != 0u &&
					   std::max(previewWidth, previewHeight) <= static_cast<int>(maxDimension));

				return ImagePreview(std::move(preview), width, height, fullResolution);
			}
			catch (...)
			{
				entry->freeFunc(previewResult.data);
				throw;
			}
		}

		if (previewResult.data)
			entry->freeFunc(previewResult.data);
		/* Preview failed: a full decode is still a correct answer. */
	}

	Image fullImage = FromFile(path);
	const int width  = fullImage.width;
	const int height = fullImage.height;
	return ImagePreview(std::move(fullImage), width, height, true);
}

inline ProgressiveImageLoad Image::LoadProgressive(const std::filesystem::path& path,
                                                   const uint32_t maxDimension)
{
	auto state = std::make_shared<ProgressiveImageState>();

	/* Resolving the plugin here is cheap (the handle is cached) and tells us
	 * whether there is a separate preview decode worth starting at all.  A
	 * loader without the optional export gets one worker, which decodes the full
	 * raster and publishes it as the only stage. */
	bool havePreview = false;
	try
	{
		auto& manager = Manager();
		auto entry = manager.getPlugin(PluginPathFor(path));
		havePreview = (entry && entry->previewFunc != nullptr);
	}
	catch (...)
	{
		havePreview = false;
	}

	if (!havePreview)
	{
		auto worker = std::async(std::launch::async, [state, path] {
			try
			{
				Image fullImage = FromFile(path);
				const int width  = fullImage.width;
				const int height = fullImage.height;
				std::lock_guard<std::mutex> lock(state->mutex);
				state->preview.emplace(std::move(fullImage));
				state->preview_ready = true;
				state->preview_done  = true;
				state->full_width    = width;
				state->full_height   = height;
				state->full_expected = false;
				state->done          = true;
			}
			catch (...)
			{
				std::lock_guard<std::mutex> lock(state->mutex);
				state->error         = std::current_exception();
				state->preview_ready = true;
				state->preview_done  = true;
				state->full_expected = false;
				state->done          = true;
			}
		});
		return ProgressiveImageLoad{ std::move(state), std::future<void>{}, std::move(worker) };
	}

	/* Preview and full decodes run concurrently.  Entropy decoding dominates
	 * both, so on a multi-core machine the full raster finishes much sooner than
	 * "preview first, full afterwards", while the preview is still early enough
	 * to overlap window construction.  The UI takes whichever stage is ready. */
	auto previewWorker = std::async(std::launch::async, [state, path, maxDimension] {
		try
		{
			ImagePreview preview = PreviewFromFile(path, maxDimension);
			std::lock_guard<std::mutex> lock(state->mutex);
			state->preview.emplace(std::move(preview.image));
			state->preview_ready = true;
			state->preview_done  = true;
			state->full_width    = preview.fullWidth;
			state->full_height   = preview.fullHeight;
			state->full_expected = !preview.fullResolution;
		}
		catch (...)
		{
			std::lock_guard<std::mutex> lock(state->mutex);
			if (!state->error)
				state->error = std::current_exception();
			state->preview_ready = true;   /* unblock the consumer */
			state->preview_done  = true;
		}
	});

	auto fullWorker = std::async(std::launch::async, [state, path] {
		try
		{
			Image fullImage = FromFile(path);
			std::lock_guard<std::mutex> lock(state->mutex);
			state->full.emplace(std::move(fullImage));
			state->done = true;
		}
		catch (...)
		{
			std::lock_guard<std::mutex> lock(state->mutex);
			if (!state->error)
				state->error = std::current_exception();
			state->done = true;
		}
	});

	return ProgressiveImageLoad{ std::move(state),
	                              std::move(previewWorker),
	                              std::move(fullWorker) };
}
