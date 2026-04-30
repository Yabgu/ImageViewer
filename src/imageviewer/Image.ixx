module;

#include <cstdio>
#include <stdexcept>
#include <cstring>
#include <cstdint>
#include <algorithm>
#include <filesystem>
#include <regex>
#include <format>
#include "ImagePluginDef.h"

export module Image;

import PluginManager;

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
			const uint16_t bitsPerSample = fmt.bitsPerPixel / fmt.componentCount;
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
		auto* entry = manager.getPlugin(pluginPath);
		if (!entry || !entry->loadFunc)
			throw std::runtime_error("Failed to load or resolve plugin: " + pluginPath.string());
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

	static void FreeImageResultViaPlugin(PluginManager& manager,
	                                      const std::filesystem::path& pluginPath,
	                                      ImagePluginResult& result)
	{
		auto* entry = manager.getPlugin(pluginPath);
		if (!entry || !entry->freeFunc)
			throw std::runtime_error("Failed to load or resolve plugin: " + pluginPath.string());
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
		static PluginManager pluginManager;
		auto extension = path.extension().string();
		std::filesystem::path pluginPath;
		if (std::regex_match(extension, std::regex("\\.jpeg|\\.jpg|\\.jfif", std::regex::icase)))
			pluginPath = PluginFileName("ImageLoaderJpeg");
		else if (std::regex_match(extension, std::regex("\\.png", std::regex::icase)))
			pluginPath = PluginFileName("ImageLoaderPng");
		else if (std::regex_match(extension, std::regex("\\.webp", std::regex::icase)))
			pluginPath = PluginFileName("ImageLoaderWebp");
		else if (std::regex_match(extension, std::regex("\\.tiff|\\.tif", std::regex::icase)))
			pluginPath = PluginFileName("ImageLoaderTiff");
		else if (std::regex_match(extension, std::regex("\\.bmp|\\.tga|\\.gif|\\.hdr|\\.pic|\\.ppm|\\.pgm", std::regex::icase)))
			pluginPath = PluginFileName("ImageLoaderStb");
		else
			[[unlikely]] throw std::runtime_error("Unknown file extension");

		ImagePluginResult pluginResult = LoadImageViaPlugin(pluginManager, pluginPath, path);
		if (pluginResult.code != IMAGE_PLUGIN_OK || !pluginResult.data)
			throw std::runtime_error("Failed to load image via plugin. Error code: "
			                         + std::to_string(pluginResult.code));

		const ImagePluginData& pd = *pluginResult.data;

		// Allocate host Image with the exact format the plugin reported.
		// No normalization is performed here — the format is preserved verbatim.
		// The rendering layer (TexturePool) converts to a GPU-compatible layout
		// at upload time.
		Image result(pd.width, pd.height, pd.format, pd.colorSpace);

		// Copy pixel data, accounting for possible plugin-side stride padding.
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

		FreeImageResultViaPlugin(pluginManager, pluginPath, pluginResult);
		return result;
	}

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
