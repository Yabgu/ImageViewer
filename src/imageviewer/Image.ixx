module;

#include <cstdio>
#include <stdexcept>
#include <cstring>
#include <cstdint>
#include <cmath>
#include <algorithm>
#include <filesystem>
#include <regex>
#include <format>
#include "ImagePluginDef.h"

export module Image;

import PluginManager;

// ─── Canonical pixel-format (internal to the rendering pipeline) ─────────────
// These enums describe the uniform per-channel storage type used after the
// normalization step.  Plugins describe their layout via IWImageFormat instead.
export enum ImagePixelFormat {
    IMAGE_PIXEL_FORMAT_U8  = 0,  /* 8-bit unsigned integer per channel  */
    IMAGE_PIXEL_FORMAT_U16 = 1,  /* 16-bit unsigned integer per channel */
    IMAGE_PIXEL_FORMAT_F32 = 2   /* 32-bit IEEE 754 float per channel   */
};

// ─── Canonical channel order (internal to the rendering pipeline) ─────────────
export enum ImageChannelOrder {
    IMAGE_CHANNEL_ORDER_RGB        = 0,
    IMAGE_CHANNEL_ORDER_RGBA       = 1,
    IMAGE_CHANNEL_ORDER_BGR        = 2,
    IMAGE_CHANNEL_ORDER_BGRA       = 3,
    IMAGE_CHANNEL_ORDER_GRAY       = 4,
    IMAGE_CHANNEL_ORDER_GRAY_ALPHA = 5
};

export int ImagePixelFormatBytesPerChannel(ImagePixelFormat fmt) noexcept
{
    switch (fmt) {
        case IMAGE_PIXEL_FORMAT_U16: return 2;
        case IMAGE_PIXEL_FORMAT_F32: return 4;
        default:                     return 1;
    }
}

// ─── Internal normalization helpers ───────────────────────────────────────────
// All functions below are module-private (not exported).

// Decode an IEEE 754 half-precision float to single precision.
// Handles zero, subnormals, normals, infinity, and NaN correctly.
static float HalfToFloat(uint16_t h) noexcept
{
    const uint32_t sign     = static_cast<uint32_t>(h & 0x8000u) << 16u;
    const uint32_t exponent = static_cast<uint32_t>(h & 0x7C00u);
    const uint32_t mantissa = static_cast<uint32_t>(h & 0x03FFu);

    uint32_t f;
    if (exponent == 0u) {
        if (mantissa == 0u) {
            f = sign;                                               // ±zero
        } else {
            // Subnormal half: normalise by counting the leading zeros in mantissa.
            // Start from the float-biased exponent that would represent 2^(-14),
            // which is the effective exponent for a subnormal half (biased as 0).
            // float32 biased exponent for 2^(-14) = -14 + 127 = 113 = 0x71.
            // Each left shift of mantissa decreases the effective exponent by 1.
            uint32_t m = mantissa;
            uint32_t e = 0x38800000u;                              // = 113 << 23
            while (!(m & 0x400u)) { m <<= 1u; e -= 0x800000u; }   // find leading 1
            m &= 0x3FFu;                                           // remove implicit 1
            f = sign | e | (m << 13u);
        }
    } else if (exponent == 0x7C00u) {
        f = sign | 0x7F800000u | (mantissa << 13u);                // Inf / NaN
    } else {
        // Normal: rebias exponent from 15 to 127 (difference = 112).
        f = sign | (((exponent >> 10u) + 112u) << 23u) | (mantissa << 13u);
    }

    float result;
    std::memcpy(&result, &f, sizeof(result));
    return result;
}

// Extract a single component from a packed pixel and return it as float.
// Handles UINT, SINT, UNORM, SNORM, and FLOAT (16- or 32-bit) component classes.
static float ExtractComponentAsFloat(const uint8_t* pixelBytes, const IWComponentDef& comp) noexcept
{
    // Gather up to 5 bytes to safely cover any 32-bit field that starts on a
    // non-byte boundary (worst case: bitOffset%8==7, bitWidth==32 → 33 bits over 5 bytes).
    uint64_t word = 0;
    const int byteStart  = comp.bitOffset / 8;
    const int bitStart   = comp.bitOffset % 8;
    const int bytesNeeded = (bitStart + comp.bitWidth + 7) / 8;
    for (int i = 0; i < bytesNeeded && i < 5; ++i)
        word |= (uint64_t)pixelBytes[byteStart + i] << (i * 8);

    word >>= bitStart;
    uint64_t mask = (comp.bitWidth < 64u) ? ((uint64_t(1) << comp.bitWidth) - 1u) : ~uint64_t(0);
    uint64_t raw  = word & mask;

    switch (comp.componentClass) {
    case IW_COMPONENT_CLASS_FLOAT:
        if (comp.bitWidth == 16u) {
            return HalfToFloat(static_cast<uint16_t>(raw));
        }
        if (comp.bitWidth == 32u) {
            float f; uint32_t u32 = static_cast<uint32_t>(raw);
            std::memcpy(&f, &u32, sizeof(f));
            return f;
        }
        return static_cast<float>(raw);

    case IW_COMPONENT_CLASS_UNORM: {
        uint64_t maxVal = (comp.bitWidth < 64u) ? ((uint64_t(1) << comp.bitWidth) - 1u) : ~uint64_t(0);
        return static_cast<float>(raw) / static_cast<float>(maxVal);
    }
    case IW_COMPONENT_CLASS_SNORM: {
        uint64_t halfRange = uint64_t(1) << (comp.bitWidth - 1u);
        int64_t  s = (raw >= halfRange) ? (int64_t(raw) - int64_t(uint64_t(1) << comp.bitWidth))
                                        : int64_t(raw);
        return std::max(-1.0f, static_cast<float>(s) / static_cast<float>(halfRange - 1u));
    }
    case IW_COMPONENT_CLASS_SINT: {
        uint64_t halfRange = uint64_t(1) << (comp.bitWidth - 1u);
        int64_t  s = (raw >= halfRange) ? (int64_t(raw) - int64_t(uint64_t(1) << comp.bitWidth))
                                        : int64_t(raw);
        return static_cast<float>(s);
    }
    default: /* IW_COMPONENT_CLASS_UINT */
        return static_cast<float>(raw);
    }
}

// Derive a canonical ImageChannelOrder from the component semantic list.
static ImageChannelOrder InferChannelOrder(const IWComponentDef* comps, uint16_t n) noexcept
{
    if (n == 1) {
        return (comps[0].semantic == IW_COMPONENT_SEMANTIC_GRAY ||
                comps[0].semantic == IW_COMPONENT_SEMANTIC_R)
               ? IMAGE_CHANNEL_ORDER_GRAY : IMAGE_CHANNEL_ORDER_GRAY;
    }
    if (n == 2) {
        if (comps[0].semantic == IW_COMPONENT_SEMANTIC_GRAY &&
            comps[1].semantic == IW_COMPONENT_SEMANTIC_A)
            return IMAGE_CHANNEL_ORDER_GRAY_ALPHA;
    }
    if (n == 3) {
        if (comps[0].semantic == IW_COMPONENT_SEMANTIC_R &&
            comps[1].semantic == IW_COMPONENT_SEMANTIC_G &&
            comps[2].semantic == IW_COMPONENT_SEMANTIC_B)
            return IMAGE_CHANNEL_ORDER_RGB;
        if (comps[0].semantic == IW_COMPONENT_SEMANTIC_B &&
            comps[1].semantic == IW_COMPONENT_SEMANTIC_G &&
            comps[2].semantic == IW_COMPONENT_SEMANTIC_R)
            return IMAGE_CHANNEL_ORDER_BGR;
    }
    if (n == 4) {
        if (comps[0].semantic == IW_COMPONENT_SEMANTIC_R &&
            comps[1].semantic == IW_COMPONENT_SEMANTIC_G &&
            comps[2].semantic == IW_COMPONENT_SEMANTIC_B &&
            comps[3].semantic == IW_COMPONENT_SEMANTIC_A)
            return IMAGE_CHANNEL_ORDER_RGBA;
        if (comps[0].semantic == IW_COMPONENT_SEMANTIC_B &&
            comps[1].semantic == IW_COMPONENT_SEMANTIC_G &&
            comps[2].semantic == IW_COMPONENT_SEMANTIC_R &&
            comps[3].semantic == IW_COMPONENT_SEMANTIC_A)
            return IMAGE_CHANNEL_ORDER_BGRA;
    }
    // Generic fallback by component count
    switch (n) {
    case 1:  return IMAGE_CHANNEL_ORDER_GRAY;
    case 2:  return IMAGE_CHANNEL_ORDER_GRAY_ALPHA;
    case 4:  return IMAGE_CHANNEL_ORDER_RGBA;
    default: return IMAGE_CHANNEL_ORDER_RGB;
    }
}

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

	static std::filesystem::path PluginFileName(const char* baseName)
	{
#ifdef _WIN32
#ifdef __MINGW32__
		return std::format("lib{}.dll", baseName);
#else
		return std::format("{}.dll", baseName);
#endif
#elif defined(__APPLE__)
		return std::format("lib{}.dylib", baseName);
#else
		return std::format("lib{}.so", baseName);
#endif
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

	// ── Normalization boundary ────────────────────────────────────────────────
	// Convert pixel-by-pixel to a canonical RGBA/RGB/GRAY F32 image.
	// Called only for non-trivial (mixed-precision or unsupported) layouts.
	static Image ConvertToCanonicalF32(const ImagePluginData& pd)
	{
		const IWImageFormat& fmt = pd.format;
		const uint16_t       n   = fmt.componentCount;

		bool hasAlpha = false;
		for (uint16_t i = 0; i < n; ++i)
			if (fmt.components[i].semantic == IW_COMPONENT_SEMANTIC_A)
				hasAlpha = true;

		ImageChannelOrder outOrder;
		int               outN;
		if (n <= 1)        { outOrder = IMAGE_CHANNEL_ORDER_GRAY;       outN = 1; }
		else if (n == 2)   { outOrder = IMAGE_CHANNEL_ORDER_GRAY_ALPHA; outN = 2; }
		else if (hasAlpha) { outOrder = IMAGE_CHANNEL_ORDER_RGBA;       outN = 4; }
		else               { outOrder = IMAGE_CHANNEL_ORDER_RGB;        outN = 3; }

		Image img(pd.width, pd.height, outN, IMAGE_PIXEL_FORMAT_F32, pd.colorSpace, outOrder);

		const int bytesPerPixel = (fmt.bitsPerPixel + 7) / 8;
		float* dst = reinterpret_cast<float*>(img.data);

		for (int y = 0; y < pd.height; ++y) {
			const uint8_t* row = pd.data + static_cast<size_t>(y) * pd.stride;
			for (int x = 0; x < pd.width; ++x) {
				const uint8_t* pixel = row + x * bytesPerPixel;
				float values[IW_MAX_COMPONENTS] = {};
				for (uint16_t c = 0; c < n; ++c)
					values[c] = ExtractComponentAsFloat(pixel, fmt.components[c]);
				float* outPixel = dst + (static_cast<size_t>(y) * pd.width + x) * outN;
				for (int c = 0; c < outN; ++c)
					outPixel[c] = (c < n) ? values[c] : 0.0f;
			}
		}

		return img;
	}

	// Normalise an ImagePluginData block into a canonical Image.
	//
	// For formats where all components share the same class and bit-width, and
	// that width is 8, 16, or 32 bits, the raw bytes are copied verbatim into a
	// canonical U8/U16/F32 image (no pixel-by-pixel work).  All other layouts —
	// including mixed-precision formats such as HSL with a float16 L channel —
	// are converted component-by-component to a F32 image so the renderer always
	// receives one of its supported canonical formats.
	static Image NormalizeFromPlugin(const ImagePluginData& pd)
	{
		const IWImageFormat& fmt = pd.format;
		const uint16_t       n   = fmt.componentCount;

		if (n == 0)
			throw std::runtime_error("Image format has zero components");

		ImageChannelOrder channelOrder = InferChannelOrder(fmt.components, n);

		// Check whether all components share the same class and bit-width.
		bool             uniform = true;
		IWComponentClass cls     = fmt.components[0].componentClass;
		uint16_t         bw      = fmt.components[0].bitWidth;
		for (uint16_t i = 1; i < n; ++i) {
			if (fmt.components[i].componentClass != cls || fmt.components[i].bitWidth != bw) {
				uniform = false;
				break;
			}
		}

		if (uniform) {
			// Float32: direct copy.
			if (cls == IW_COMPONENT_CLASS_FLOAT && bw == 32u) {
				Image img(pd.width, pd.height, n, IMAGE_PIXEL_FORMAT_F32, pd.colorSpace, channelOrder);
				std::memcpy(img.data, pd.data, pd.size);
				return img;
			}
			// 16-bit integer (any integer class): direct copy.
			if (cls != IW_COMPONENT_CLASS_FLOAT && bw == 16u) {
				Image img(pd.width, pd.height, n, IMAGE_PIXEL_FORMAT_U16, pd.colorSpace, channelOrder);
				std::memcpy(img.data, pd.data, pd.size);
				return img;
			}
			// 8-bit integer (any integer class): direct copy.
			if (cls != IW_COMPONENT_CLASS_FLOAT && bw == 8u) {
				Image img(pd.width, pd.height, n, IMAGE_PIXEL_FORMAT_U8, pd.colorSpace, channelOrder);
				std::memcpy(img.data, pd.data, pd.size);
				return img;
			}
		}

		// Non-trivial layout (mixed precision, float16, etc.): convert to F32.
		return ConvertToCanonicalF32(pd);
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
			pluginPath = PluginFileName("ImageLoaderJpeg");
		}
		else if (std::regex_match(extension, std::regex("\\.png", std::regex::icase)))
		{
			pluginPath = PluginFileName("ImageLoaderPng");
		}
		else if (std::regex_match(extension, std::regex("\\.webp", std::regex::icase)))
		{
			pluginPath = PluginFileName("ImageLoaderWebp");
		}
		else if (std::regex_match(extension, std::regex("\\.tiff|\\.tif", std::regex::icase)))
		{
			pluginPath = PluginFileName("ImageLoaderTiff");
		}
		else if (std::regex_match(extension, std::regex("\\.bmp|\\.tga|\\.gif|\\.hdr|\\.pic|\\.ppm|\\.pgm", std::regex::icase)))
		{
			pluginPath = PluginFileName("ImageLoaderStb");
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

		// Normalisation boundary: convert the plugin's per-component layout into
		// a canonical Image (U8/U16/F32 with a simple channel order) before
		// handing the image to the rendering pipeline.
		Image result = NormalizeFromPlugin(*pluginResult.data);

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
