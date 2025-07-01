module;

#include <cstdio>
#include <stdexcept>
#include <cstring>
#include <cstdint>
#include <jpeglib.h>
#include <setjmp.h>
#include <cstdio>
#include <filesystem>
#include <regex>
#include <png.h>

struct my_error_mgr
{
	struct jpeg_error_mgr pub;
	jmp_buf setjmp_buffer;
};

EXTERN(void)
my_error_exit(j_common_ptr cinfo)
{
	/* cinfo->err really points to a my_error_mgr struct, so coerce pointer */
	struct my_error_mgr* myerr = (struct my_error_mgr*)cinfo->err;

	/* Always display the message. */
	/* We could postpone this until after returning, if we chose. */
	(*cinfo->err->output_message)(cinfo);

	/* Return control to the setjmp point */
	longjmp(myerr->setjmp_buffer, 1);
}

export module Image;
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

	static FILE* OpenFile(const std::filesystem::path& filePath)
	{
		FILE* file = nullptr;
#ifdef _WIN32
		auto err = _wfopen_s(&file, filePath.native().c_str(), L"rb");
#else
		file = std::fopen((const char *)filePath.u8string().c_str(), "rb");
#endif
		if (file == nullptr)
		{
			[[unlikely]] throw std::runtime_error(
				std::format("can't open file: {}", filePath.string()));
		}
		return file;
	}

	static Image FromFileJpeg(const std::filesystem::path& filePath)
	{
		std::shared_ptr<FILE> infile(OpenFile(filePath), ::fclose);

		struct my_error_mgr jerr = {
			.pub = {
				.error_exit = my_error_exit} };

		struct jpeg_decompress_struct cinfo = {
			.err = jpeg_std_error(&jerr.pub) };

		if (setjmp(jerr.setjmp_buffer))
		{
			jpeg_destroy_decompress(&cinfo);
			throw std::runtime_error("Jpeg decode failed");
		}

		jpeg_create_decompress(&cinfo);
		jpeg_stdio_src(&cinfo, infile.get());

		jpeg_read_header(&cinfo, TRUE);

		(void)jpeg_start_decompress(&cinfo);
		int row_stride = cinfo.output_width * cinfo.output_components;

		constexpr int MaxLinesToRead = 1;

		JSAMPARRAY buffer = (*cinfo.mem->alloc_sarray)((j_common_ptr)&cinfo, JPOOL_IMAGE, row_stride, MaxLinesToRead);

		Image res(cinfo.output_width, cinfo.output_height, cinfo.output_components);

		while (cinfo.output_scanline < cinfo.output_height)
		{
			auto current_scanline = cinfo.output_scanline;
			auto readlines = jpeg_read_scanlines(&cinfo, buffer, MaxLinesToRead);
			if (readlines == 0)
			{
				[[unlikely]]
				// TODO: Handle the error!
				break;
			}
			std::memcpy(&res.data[current_scanline * row_stride], buffer[0], row_stride * readlines);
		}

		(void)jpeg_finish_decompress(&cinfo);
		jpeg_destroy_decompress(&cinfo);
		return res;
	}

	static Image FromFilePng(const std::filesystem::path& filePath)
	{
		std::shared_ptr<FILE> infile(OpenFile(filePath), ::fclose);

		png_structp png_ptr = png_create_read_struct(PNG_LIBPNG_VER_STRING, nullptr, nullptr, nullptr);
		if (!png_ptr)
		{
			throw std::runtime_error("Failed to create PNG read struct");
		}

		png_infop info_ptr = png_create_info_struct(png_ptr);
		if (!info_ptr)
		{
			png_destroy_read_struct(&png_ptr, nullptr, nullptr);
			throw std::runtime_error("Failed to create PNG info struct");
		}

		if (setjmp(png_jmpbuf(png_ptr)))
		{
			png_destroy_read_struct(&png_ptr, &info_ptr, nullptr);
			throw std::runtime_error("PNG decode failed");
		}

		png_init_io(png_ptr, infile.get());
		png_read_info(png_ptr, info_ptr);

		int width = png_get_image_width(png_ptr, info_ptr);
		int height = png_get_image_height(png_ptr, info_ptr);
		int color_type = png_get_color_type(png_ptr, info_ptr);
		int bit_depth = png_get_bit_depth(png_ptr, info_ptr);

		if (bit_depth == 16)
		{
			png_set_strip_16(png_ptr);
		}

		if (color_type == PNG_COLOR_TYPE_PALETTE)
		{
			png_set_palette_to_rgb(png_ptr);
		}

		if (color_type == PNG_COLOR_TYPE_GRAY && bit_depth < 8)
		{
			png_set_expand_gray_1_2_4_to_8(png_ptr);
		}

		if (png_get_valid(png_ptr, info_ptr, PNG_INFO_tRNS))
		{
			png_set_tRNS_to_alpha(png_ptr);
		}

		png_read_update_info(png_ptr, info_ptr);

		int componentsPerPixel = png_get_channels(png_ptr, info_ptr);
		Image res(width, height, componentsPerPixel);

		for (int y = 0; y < height; ++y)
		{
			png_bytep row = res.data + y * res.stride;
			png_read_rows(png_ptr, &row, nullptr, 1);
		}

		png_destroy_read_struct(&png_ptr, &info_ptr, nullptr);
		return res;
	}

	static Image FromFile(const std::filesystem::path& path)
	{
		auto extension = path.extension().string();
		if (std::regex_match(extension, std::regex("\\.jpeg|\\.jpg|\\.jfif", std::regex::icase)))
		{
			return FromFileJpeg(path);
		}
		else if (std::regex_match(extension, std::regex("\\.png", std::regex::icase)))
		{
			return FromFilePng(path);
		}
		else
		{
			[[unlikely]] throw std::runtime_error("Unknown file extension");
		}
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
