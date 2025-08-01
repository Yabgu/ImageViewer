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
	static inline int CalculateSize(int width, int height, int componentsPerPixel)
	{
		if (width <= 0 || height <= 0 || componentsPerPixel <= 0)
		{
			[[unlikely]] throw std::runtime_error("Size cannot be zero or negative");
		}
		return width * height * componentsPerPixel;
	}

	static ImagePluginResult LoadImageViaPlugin(const std::filesystem::path& pluginPath, const std::filesystem::path& imagePath)
	{
#ifdef _WIN32
		HMODULE hModule = LoadLibraryW(pluginPath.c_str());
		if (!hModule)
		{
			throw std::runtime_error("Failed to load plugin: " + pluginPath.string());
		}

		LoadImageFromFileFunc loadFunc = (LoadImageFromFileFunc)GetProcAddress(hModule, "LoadImageFromFile");
		if (!loadFunc)
		{
			FreeLibrary(hModule);
			throw std::runtime_error("Failed to find LoadImageFromFile function in plugin");
		}

		ImagePluginResult result = loadFunc(imagePath.c_str());
		FreeLibrary(hModule);
		return result;
#else
		void* handle = dlopen(pluginPath.c_str(), RTLD_LAZY);
		if (!handle)
		{
			throw std::runtime_error("Failed to load plugin: " + std::string(dlerror()));
		}

		LoadImageFromFileFunc loadFunc = (LoadImageFromFileFunc)dlsym(handle, "LoadImageFromFile");
		if (!loadFunc)
		{
			dlclose(handle);
			throw std::runtime_error("Failed to find LoadImageFromFile function in plugin");
		}

		ImagePluginResult result = loadFunc(imagePath.c_str());
		dlclose(handle);
		return result;
#endif
	}

public:
	const int width;
	const int height;
	const int componentsPerPixel;
	const int stride;
	const size_t size;
	uint8_t* const data;

	Image(Image&& o)
		: width(o.width),
		height(o.height),
		componentsPerPixel(o.componentsPerPixel),
		stride(o.stride),
		size(o.size),
		data{o.data}
	{
		const_cast<uint8_t*&>(o.data) = nullptr;
	}

	Image(const Image& o)
		: width(o.width),
		height(o.height),
		componentsPerPixel(o.componentsPerPixel),
		stride(o.stride),
		size(o.size),
		data(new uint8_t[o.size])
	{
		std::copy(o.data, o.data + o.size, data);
	}

	Image(const int width, const int height, const int componentsPerPixel)
		: width(width),
		height(height),
		componentsPerPixel(componentsPerPixel),
		stride(width* componentsPerPixel),
		size(CalculateSize(width, height, componentsPerPixel)),
		data(new uint8_t[size])
	{
	}

	~Image()
	{
		delete[] data;
	}

	static Image FromFile(const std::filesystem::path& path)
	{
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
		else
		{
			[[unlikely]] throw std::runtime_error("Unknown file extension");
		}

		ImagePluginResult pluginResult = LoadImageViaPlugin(pluginPath, path);
		if (pluginResult.code != IMAGE_PLUGIN_OK || !pluginResult.data)
		{
			throw std::runtime_error("Failed to load image via plugin");
		}

		Image result(pluginResult.data->width, pluginResult.data->height, pluginResult.data->componentsPerPixel);
		std::memcpy(result.data, pluginResult.data->data, pluginResult.data->size);

		delete[] pluginResult.data->data;
		delete pluginResult.data;

		return result;
	}

	Image* Subsection(const int x, const int y, const int width, const int height) const
	{
		if (width < 0 || height < 0)
		{
			[[unlikely]] throw std::runtime_error("argument error, width and height cannot be negative");
		}

		Image* res = new Image(width, height, componentsPerPixel);
		std::fill_n(res->data, res->size, 0u);

		if (-x >= width || x >= this->width || -y >= height || y >= this->height)
		{
			[[unlikely]] return res;
		}

		const int destX = std::max<int>(-x, 0);
		const int destY = std::max<int>(-y, 0);
		const int destWidth = std::min<int>(width, this->width - x);
		const int destStride = destWidth * componentsPerPixel;
		const int destHeight = std::min<int>(height, this->height - y);

		for (int py = 0; py < destHeight; ++py)
		{
			std::memcpy(
				(void*)&res->data[py * res->stride],
				(void*)&this->data[x * componentsPerPixel + (y + py) * this->stride],
				destStride);
		}

		return res;
	}
};
