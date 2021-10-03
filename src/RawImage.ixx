#include <stdexcept>
#include <cstdint>
#include <jpeglib.h>
#include <setjmp.h>
#include <cstdio>
#include <filesystem>

export module RawImage;
struct my_error_mgr {
	struct jpeg_error_mgr pub;
	jmp_buf setjmp_buffer;
};

METHODDEF(void) my_error_exit(j_common_ptr cinfo)
{
	/* cinfo->err really points to a my_error_mgr struct, so coerce pointer */
	struct my_error_mgr* myerr = (struct my_error_mgr*)cinfo->err;

	/* Always display the message. */
	/* We could postpone this until after returning, if we chose. */
	(*cinfo->err->output_message) (cinfo);

	/* Return control to the setjmp point */
	longjmp(myerr->setjmp_buffer, 1);
}

export class Image
{
private:
	static inline int CalculateSize(int width, int height, int componentsPerPixel)
	{
		if (width <= 0 || height <= 0 || componentsPerPixel <= 0)
		{
			[[unlikely]]
			throw std::runtime_error("Size cannot be zero or negative");
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

	Image(Image&&) = default;

	Image(const Image&) = delete;

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

	static Image* FromFile(const std::filesystem::path& filePath)
	{
		#ifdef WIN32
		FILE* infile = nullptr;
		auto err = _wfopen_s(&infile, filePath.native().c_str(), L"rb");
		#else
		FILE* infile = ::fopen((const char*)filePath.u8string().c_str(), "rb");
		#endif
		if (infile == nullptr) {
			fprintf(stderr, "can't open %s\n", filePath.u8string().c_str());
			[[unlikely]]
			throw std::runtime_error("Cannot open file");
		}

		struct my_error_mgr jerr = {
			.pub = {
				.error_exit = my_error_exit
			}
		};

		struct jpeg_decompress_struct cinfo = {
			.err = jpeg_std_error(&jerr.pub)
		};

		if (setjmp(jerr.setjmp_buffer)) {
			jpeg_destroy_decompress(&cinfo);
			fclose(infile);
			throw std::runtime_error("Jpeg decode failed");
		}

		jpeg_create_decompress(&cinfo);
		jpeg_stdio_src(&cinfo, infile);

		(void)jpeg_read_header(&cinfo, TRUE);
		(void)jpeg_start_decompress(&cinfo);
		int row_stride = cinfo.output_width * cinfo.output_components;

		constexpr int MaxLinesToRead = 1;

		JSAMPARRAY buffer = (*cinfo.mem->alloc_sarray)((j_common_ptr)&cinfo, JPOOL_IMAGE, row_stride, MaxLinesToRead);

		Image* res = new Image(cinfo.output_width, cinfo.output_height, cinfo.output_components);

		while (cinfo.output_scanline < cinfo.output_height) {
			auto current_scanline = cinfo.output_scanline;
			auto readlines = jpeg_read_scanlines(&cinfo, buffer, MaxLinesToRead);
			if (readlines == 0)
			{
				[[unlikely]]
				// TODO: Handle the error!
				break;
			}
			::memcpy(&res->data[current_scanline * row_stride], buffer[0], row_stride * readlines);
		}

		(void)jpeg_finish_decompress(&cinfo);
		jpeg_destroy_decompress(&cinfo);
		fclose(infile);
		return res;
	}

	Image* Subsection(const int x, const int y, const int width, const int height) const
	{
		if (width < 0 || height < 0)
		{
			[[unlikely]]
			throw std::runtime_error("");
		}

		Image* res = new Image(width, height, componentsPerPixel);
		std::fill_n(res->data, res->size, 0u);

		if (-x >= width || x >= this->width || -y >= height || y >= this->height)
		{
			[[unlikely]]
			return res;
		}

		const int destX = std::max<int>(-x, 0);
		const int destY = std::max<int>(-y, 0);
		const int destWidth = std::min<int>(width, this->width - x);
		const int destStride = destWidth * componentsPerPixel;
		const int destHeight = std::min<int>(height, this->height - y);

		for (int py = 0; py < destHeight; ++py)
		{
			memcpy(&res->data[py * res->stride], &this->data[x * componentsPerPixel + (y + py) * this->stride], destStride);
		}

		return res;
	}
};
