module;

#include <cstdio>
#include <stdexcept>
#include <cstring>
#include <cstdint>
#include <filesystem>
#include <regex>
#include <format>
#include <algorithm>
#include "ImagePluginDef.h"

export module Image;

import PluginManager;

/* -----------------------------------------------------------------------
 * Normalization helpers (module-private)
 *
 * These functions map the plugin-provided IWImageFormat to one of the
 * host's canonical internal layouts:
 *   GRAY8 / GRAY16 / GRAY32F
 *   GRAY_ALPHA8 / GRAY_ALPHA16 / GRAY_ALPHA32F
 *   RGB8  / RGB16  / RGB32F
 *   RGBA8 / RGBA16 / RGBA32F
 *   BGR8  / BGR16  / BGR32F
 *   BGRA8 / BGRA16 / BGRA32F
 *
 * For mixed-precision or non-byte-aligned formats (e.g. HSL with a
 * 6-bit UNORM S and a float16 L), all components are unpacked and the
 * image is converted to RGBA F32 (the most general canonical format).
 * ----------------------------------------------------------------------- */

struct CanonicalLayout {
    ImagePixelFormat  pixelFormat;
    ImageChannelOrder channelOrder;
    int               componentsPerPixel;
};

/* IEEE-754 half-precision (float16) to single-precision conversion. */
static float HalfToFloat(uint16_t h) noexcept
{
    const uint32_t sign = (uint32_t)(h >> 15u) & 1u;
    const uint32_t exp  = (uint32_t)(h >> 10u) & 0x1Fu;
    const uint32_t mant = (uint32_t)(h)         & 0x3FFu;
    uint32_t result;

    if (exp == 0u) {
        if (mant == 0u) {
            /* ±zero */
            result = sign << 31;
        } else {
            /* Denormalized half → normalized float32.
             * Bias difference: float32 uses 127, float16 uses 15.
             * Smallest normalized half has unbiased exp = 1-15 = -14,
             * which maps to float32 biased exp = -14+127 = 113.
             * We start there and shift left until the hidden bit appears. */
            int32_t e32 = 113;
            uint32_t m  = mant;
            while (!(m & 0x400u)) { m <<= 1; --e32; }
            m &= 0x3FFu;
            result = (sign << 31) | ((uint32_t)e32 << 23) | (m << 13);
        }
    } else if (exp == 31u) {
        /* Infinity or NaN */
        result = (sign << 31) | 0x7F800000u | (mant << 13);
    } else {
        /* Normalized: rebias exponent from 15 to 127 (delta = 112). */
        result = (sign << 31) | ((exp + 112u) << 23) | (mant << 13);
    }

    float f;
    std::memcpy(&f, &result, sizeof(f));
    return f;
}

/**
 * Extracts `bitWidth` bits starting at bit position `bitOffset` (LSB=0)
 * from the raw byte sequence `pixelBytes`.
 */
static uint64_t ExtractBits(const uint8_t* pixelBytes,
                             uint16_t bitOffset,
                             uint16_t bitWidth) noexcept
{
    const int byteStart  = bitOffset / 8;
    const int bitShift   = bitOffset % 8;
    const int bitsNeeded = bitShift + bitWidth;
    const int bytesNeeded = (bitsNeeded + 7) / 8;

    uint64_t raw = 0;
    for (int i = 0; i < bytesNeeded && i < 8; ++i)
        raw |= (uint64_t)pixelBytes[byteStart + i] << (i * 8);

    raw >>= bitShift;
    if (bitWidth < 64u)
        raw &= (1ULL << bitWidth) - 1ULL;
    return raw;
}

/** Converts extracted component bits to a float according to its class. */
static float ComponentToFloat(uint64_t bits,
                               IWComponentClass cls,
                               uint16_t         bitWidth) noexcept
{
    switch (cls) {
    case IW_COMPONENT_CLASS_UNORM:
        if (bitWidth == 0u || bitWidth >= 64u) return 0.0f;
        return (float)bits / (float)((1ULL << bitWidth) - 1ULL);

    case IW_COMPONENT_CLASS_SNORM: {
        /* Sign-extend from bitWidth to 64 bits. */
        int64_t sv = (int64_t)bits;
        if (bitWidth > 0u && bitWidth < 64u && (bits >> (bitWidth - 1u)))
            sv = (int64_t)(bits | (~0ULL << bitWidth));
        const int64_t maxPos = (int64_t)((1ULL << (bitWidth - 1u)) - 1ULL);
        const float norm = (maxPos > 0) ? (float)sv / (float)maxPos : 0.0f;
        return std::max(-1.0f, norm);
    }

    case IW_COMPONENT_CLASS_FLOAT:
        if (bitWidth == 16u) return HalfToFloat((uint16_t)bits);
        if (bitWidth == 32u) {
            float f;
            uint32_t b32 = (uint32_t)bits;
            std::memcpy(&f, &b32, sizeof(f));
            return f;
        }
        return (float)bits;

    case IW_COMPONENT_CLASS_UINT:
    case IW_COMPONENT_CLASS_SINT:
    default:
        return (float)bits;
    }
}

/**
 * Attempts to map fmt to a canonical (uniform bit-width, byte-aligned)
 * layout without rewriting the pixel data.
 *
 * Returns true on success and fills `out`.  Returns false for formats
 * that require a pixel-data conversion (e.g. float16, mixed precision,
 * non-byte-aligned components, or unknown semantic ordering).
 */
static bool TryCanonicalLayout(const IWImageFormat& fmt, CanonicalLayout& out) noexcept
{
    const uint16_t n = fmt.componentCount;
    if (n == 0u || n > 4u) return false;

    /* All components must share the same bit width, class, and must be
     * packed consecutively from offset 0. */
    const uint16_t bw  = fmt.components[0].bitWidth;
    const auto     cls = fmt.components[0].componentClass;

    if (bw != 8u && bw != 16u && bw != 32u) return false;

    for (uint16_t i = 0u; i < n; ++i) {
        const IWComponentDef& c = fmt.components[i];
        if (c.bitWidth        != bw)       return false;
        if (c.componentClass  != cls)      return false;
        if (c.bitOffset       != i * bw)   return false;
    }

    /* Determine canonical pixel format. */
    if (cls == IW_COMPONENT_CLASS_FLOAT) {
        if (bw != 32u) return false;  /* float16 needs conversion */
        out.pixelFormat = IMAGE_PIXEL_FORMAT_F32;
    } else {
        out.pixelFormat = (bw == 16u) ? IMAGE_PIXEL_FORMAT_U16
                                      : IMAGE_PIXEL_FORMAT_U8;
    }
    out.componentsPerPixel = (int)n;

    /* Determine channel order from component semantics. */
    auto s = [&](int i) { return fmt.components[i].semantic; };

    if (n == 1 && s(0) == IW_COMPONENT_SEMANTIC_GRAY) {
        out.channelOrder = IMAGE_CHANNEL_ORDER_GRAY;
    } else if (n == 2 &&
               s(0) == IW_COMPONENT_SEMANTIC_GRAY &&
               s(1) == IW_COMPONENT_SEMANTIC_A) {
        out.channelOrder = IMAGE_CHANNEL_ORDER_GRAY_ALPHA;
    } else if (n == 3 &&
               s(0) == IW_COMPONENT_SEMANTIC_R &&
               s(1) == IW_COMPONENT_SEMANTIC_G &&
               s(2) == IW_COMPONENT_SEMANTIC_B) {
        out.channelOrder = IMAGE_CHANNEL_ORDER_RGB;
    } else if (n == 3 &&
               s(0) == IW_COMPONENT_SEMANTIC_B &&
               s(1) == IW_COMPONENT_SEMANTIC_G &&
               s(2) == IW_COMPONENT_SEMANTIC_R) {
        out.channelOrder = IMAGE_CHANNEL_ORDER_BGR;
    } else if (n == 4 &&
               s(0) == IW_COMPONENT_SEMANTIC_R &&
               s(1) == IW_COMPONENT_SEMANTIC_G &&
               s(2) == IW_COMPONENT_SEMANTIC_B &&
               s(3) == IW_COMPONENT_SEMANTIC_A) {
        out.channelOrder = IMAGE_CHANNEL_ORDER_RGBA;
    } else if (n == 4 &&
               s(0) == IW_COMPONENT_SEMANTIC_B &&
               s(1) == IW_COMPONENT_SEMANTIC_G &&
               s(2) == IW_COMPONENT_SEMANTIC_R &&
               s(3) == IW_COMPONENT_SEMANTIC_A) {
        out.channelOrder = IMAGE_CHANNEL_ORDER_BGRA;
    } else {
        return false;  /* Non-standard or unknown semantic ordering. */
    }

    return true;
}

/**
 * Converts any IWImageFormat pixel buffer to RGBA F32.
 *
 * Used as the fallback normalisation path for mixed-precision, packed,
 * or non-standard channel layouts (e.g. HSL with a 6-bit S UNORM and a
 * float16 L).  Each component is unpacked bit-by-bit and converted to a
 * float in [0,1] (or the native float range for FLOAT components), then
 * placed in the appropriate RGBA slot.  Non-RGBA semantics (H, S, L,
 * GRAY, UNKNOWN) are mapped into R, G, B in component order.
 *
 * Returns a heap-allocated buffer of width*height*4*sizeof(float) bytes.
 * The caller is responsible for delete[]-ing it.
 * `out` is always set to { F32, RGBA, 4 }.
 */
static uint8_t* ConvertToRGBAF32(const IWImageFormat& fmt,
                                  const uint8_t*       srcData,
                                  int                  width,
                                  int                  height,
                                  CanonicalLayout&     out)
{
    out.pixelFormat        = IMAGE_PIXEL_FORMAT_F32;
    out.channelOrder       = IMAGE_CHANNEL_ORDER_RGBA;
    out.componentsPerPixel = 4;

    const size_t dstSize = (size_t)width * height * 4u * sizeof(float);
    uint8_t* dst = new uint8_t[dstSize];

    const int srcBytesPerPixel = (fmt.bitsPerPixel + 7) / 8;
    float*    dstF             = reinterpret_cast<float*>(dst);

    for (int py = 0; py < height; ++py) {
        for (int px = 0; px < width; ++px) {
            const uint8_t* srcPixel = srcData + ((size_t)py * width + px) * srcBytesPerPixel;
            float rgba[4] = { 0.0f, 0.0f, 0.0f, 1.0f };

            /* Running index for mapping non-RGBA semantics into R/G/B slots. */
            int genericSlot = 0;

            for (uint16_t ci = 0; ci < fmt.componentCount; ++ci) {
                const IWComponentDef& comp = fmt.components[ci];
                const uint64_t bits  = ExtractBits(srcPixel, comp.bitOffset, comp.bitWidth);
                const float    value = ComponentToFloat(bits, comp.componentClass, comp.bitWidth);

                switch (comp.semantic) {
                case IW_COMPONENT_SEMANTIC_R:    rgba[0] = value; break;
                case IW_COMPONENT_SEMANTIC_G:    rgba[1] = value; break;
                case IW_COMPONENT_SEMANTIC_B:    rgba[2] = value; break;
                case IW_COMPONENT_SEMANTIC_A:    rgba[3] = value; break;
                case IW_COMPONENT_SEMANTIC_GRAY:
                    rgba[0] = rgba[1] = rgba[2] = value;
                    break;
                default:
                    /* H, S, L, UNKNOWN: map sequentially into R, G, B. */
                    if (genericSlot < 3) rgba[genericSlot] = value;
                    ++genericSlot;
                    break;
                }
            }

            float* dstPixel = dstF + ((size_t)py * width + px) * 4u;
            dstPixel[0] = rgba[0];
            dstPixel[1] = rgba[1];
            dstPixel[2] = rgba[2];
            dstPixel[3] = rgba[3];
        }
    }

    return dst;
}

/* -----------------------------------------------------------------------
 * Image class
 * ----------------------------------------------------------------------- */

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

		/* ------------------------------------------------------------------
		 * Normalisation boundary: convert the plugin-provided IWImageFormat
		 * into one of the host's canonical internal layouts.
		 * ------------------------------------------------------------------ */
		const ImagePluginData& pd = *pluginResult.data;
		CanonicalLayout layout{};

		if (TryCanonicalLayout(pd.format, layout)) {
			/* Fast path: pixel data is already in a canonical byte layout.
			 * Copy it directly into the Image buffer. */
			Image result(pd.width, pd.height, layout.componentsPerPixel,
			             layout.pixelFormat, pd.colorSpace, layout.channelOrder);
			std::memcpy(result.data, pd.data, result.size);
			FreeImageResultViaPlugin(pluginManager, pluginPath, pluginResult);
			return result;
		} else {
			/* Slow path: unpack and convert every component to RGBA F32. */
			uint8_t* converted = ConvertToRGBAF32(pd.format, pd.data,
			                                       pd.width, pd.height, layout);
			Image result(pd.width, pd.height, layout.componentsPerPixel,
			             layout.pixelFormat, pd.colorSpace, layout.channelOrder);
			std::memcpy(result.data, converted, result.size);
			delete[] converted;
			FreeImageResultViaPlugin(pluginManager, pluginPath, pluginResult);
			return result;
		}
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
