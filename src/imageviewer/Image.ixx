module;

#include <stdexcept>
#include <cstring>
#include <cstdint>
#include <jpeglib.h>
#include <setjmp.h>
#include <cstdio>
#include <filesystem>
#include <regex>
#include <utility>

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
#ifdef WIN32
		FILE* file = nullptr;
		auto err = _wfopen_s(&file, filePath.native().c_str(), L"rb");
#else
		FILE* file = ::fopen((const char*)filePath.u8string().c_str(), "rb");
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
		return Image(0, 0, 0);
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
