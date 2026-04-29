module;

#include <cstdio>
#include <stdexcept>
#include <cstring>
#include <cstdint>
#include <filesystem>
#include <regex>
#include <format>
#include "ImagePluginDef.h"

#ifdef _WIN32
#include <windows.h>
#else
#include <dlfcn.h>
#endif

import PluginManager;

export module Image;

// Plugin interface structure
// struct ImageData
// {
// 	int width;
// 	int height;
// 	int componentsPerPixel;
// 	int stride;
// 	size_t size;
// 	uint8_t* data;
// };

// typedef ImageData* (*LoadImageFromFileFunc)(const wchar_t*);
// typedef void (*FreeImageDataFunc)(ImageData*);

export class Image
{
private:
	static size_t CalculateSize(int width, int height, int componentsPerPixel, ImagePixelFormat pixelFormat)
	{
		if (width <= 0 || height <= 0 || componentsPerPixel <= 0)
		{
			[[unlikely]] throw std::runtime_error("Size cannot be zero or negative");
		}
		return static_cast<size_t>(width) * height * componentsPerPixel
		       * ImagePixelFormatBytesPerChannel(pixelFormat);
	}

	static ImagePluginResult LoadImageViaPlugin(PluginManager& manager, const std::filesystem::path& pluginPath, const std::filesystem::path& imagePath)
	{
		auto* entry = manager.getPlugin(pluginPath);
		if (!entry || !entry->loadFunc) {
			throw std::runtime_error("Failed to load or resolve plugin: " + pluginPath.string());
		}
		return entry->loadFunc(imagePath.c_str());
	}

	static void FreeImageResultViaPlugin(PluginManager& manager, const std::filesystem::path& pluginPath, ImagePluginResult& result)
	{
		auto* entry = manager.getPlugin(pluginPath);
		if (!entry || !entry->freeFunc) {
			throw std::runtime_error("Failed to load or resolve plugin: " + pluginPath.string());
		}
		entry->freeFunc(result.data);
		result.data = nullptr;
	}

public:
	const int width;
	const int height;
	const int componentsPerPixel;
	const int stride;
	const size_t size;
	uint8_t* const data;
	const ImagePixelFormat  pixelFormat;
	const ImageColorSpace   colorSpace;
	const ImageChannelOrder channelOrder;

	int BytesPerChannel() const noexcept
	{
		return ImagePixelFormatBytesPerChannel(pixelFormat);
	}

	Image(Image&& o)
		: width(o.width),
		height(o.height),
		componentsPerPixel(o.componentsPerPixel),
		stride(o.stride),
		size(o.size),
		data(o.data),
		pixelFormat(o.pixelFormat),
		colorSpace(o.colorSpace),
		channelOrder(o.channelOrder)
	{
		const_cast<uint8_t*&>(o.data) = nullptr;
	}

	Image(const Image& o)
		: width(o.width),
		height(o.height),
		componentsPerPixel(o.componentsPerPixel),
		stride(o.stride),
		size(o.size),
		data(new uint8_t[o.size]),
		pixelFormat(o.pixelFormat),
		colorSpace(o.colorSpace),
		channelOrder(o.channelOrder)
	{
		std::copy(o.data, o.data + o.size, data);
	}

	Image(int width, int height, int componentsPerPixel,
	      ImagePixelFormat  pixelFormat  = IMAGE_PIXEL_FORMAT_U8,
	      ImageColorSpace   colorSpace   = IMAGE_COLOR_SPACE_SRGB,
	      ImageChannelOrder channelOrder = IMAGE_CHANNEL_ORDER_RGB)
		: width(width),
		height(height),
		componentsPerPixel(componentsPerPixel),
		stride(width * componentsPerPixel * ImagePixelFormatBytesPerChannel(pixelFormat)),
		size(CalculateSize(width, height, componentsPerPixel, pixelFormat)),
		data(new uint8_t[size]),
		pixelFormat(pixelFormat),
		colorSpace(colorSpace),
		channelOrder(channelOrder)
	{
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
		{
#ifdef _WIN32
			pluginPath = "ImageLoaderJpeg.dll";
#else
			pluginPath = "libImageLoaderJpeg.so";
#endif
		}
		else if (std::regex_match(extension, std::regex("\\.png", std::regex::icase)))
		{
#ifdef _WIN32
			pluginPath = "ImageLoaderPng.dll";
#else
			pluginPath = "libImageLoaderPng.so";
#endif
		}
		else if (std::regex_match(extension, std::regex("\\.webp", std::regex::icase)))
		{
#ifdef _WIN32
			pluginPath = "ImageLoaderWebp.dll";
#else
			pluginPath = "libImageLoaderWebp.so";
#endif
		}
		else if (std::regex_match(extension, std::regex("\\.tiff|\\.tif", std::regex::icase)))
		{
#ifdef _WIN32
			pluginPath = "ImageLoaderTiff.dll";
#else
			pluginPath = "libImageLoaderTiff.so";
#endif
		}
		else if (std::regex_match(extension, std::regex("\\.bmp|\\.tga|\\.gif|\\.hdr|\\.pic|\\.ppm|\\.pgm", std::regex::icase)))
		{
#ifdef _WIN32
			pluginPath = "ImageLoaderStb.dll";
#else
			pluginPath = "libImageLoaderStb.so";
#endif
		}
		else
		{
			[[unlikely]] throw std::runtime_error("Unknown file extension");
		}

		ImagePluginResult pluginResult = LoadImageViaPlugin(pluginManager, pluginPath, path);
		if (pluginResult.code != IMAGE_PLUGIN_OK || !pluginResult.data)
		{
			std::string errMsg = "Failed to load image via plugin. Error code: " + std::to_string(pluginResult.code);
			throw std::runtime_error(errMsg);
		}

		Image result(
			pluginResult.data->width,
			pluginResult.data->height,
			pluginResult.data->componentsPerPixel,
			pluginResult.data->pixelFormat,
			pluginResult.data->colorSpace,
			pluginResult.data->channelOrder
		);
		std::memcpy(result.data, pluginResult.data->data, pluginResult.data->size);

		FreeImageResultViaPlugin(pluginManager, pluginPath, pluginResult);

		return result;
	}

	Image* Subsection(const int x, const int y, const int width, const int height) const
	{
		if (width < 0 || height < 0)
		{
			[[unlikely]] throw std::runtime_error("argument error, width and height cannot be negative");
		}

		Image* res = new Image(width, height, componentsPerPixel, pixelFormat, colorSpace, channelOrder);
		std::fill_n(res->data, res->size, 0u);

		if (-x >= width || x >= this->width || -y >= height || y >= this->height)
		{
			[[unlikely]] return res;
		}

		const int srcX       = std::max<int>(x, 0);
		const int srcY       = std::max<int>(y, 0);
		const int destX      = std::max<int>(-x, 0);
		const int destY      = std::max<int>(-y, 0);
		const int bpc        = BytesPerChannel();
		const int copyWidth  = std::min<int>(width - destX, this->width - srcX);
		const int copyStride = copyWidth * componentsPerPixel * bpc;
		const int copyHeight = std::min<int>(height - destY, this->height - srcY);

		for (int py = 0; py < copyHeight; ++py)
		{
			std::memcpy(
				(void*)&res->data[(destY + py) * res->stride + destX * componentsPerPixel * bpc],
				(void*)&this->data[(srcY + py) * this->stride + srcX * componentsPerPixel * bpc],
				copyStride);
		}

		return res;
	}
};
