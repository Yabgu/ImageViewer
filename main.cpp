#include <format>
#include <memory>
#include <iostream>
#include <SDL.h>
#include <SDL_image.h>
#include <jpeglib.h>
#include <setjmp.h>

SDL_Window* CreateWindow()
{
	int initialXPos = SDL_WINDOWPOS_UNDEFINED;
	int initialYPos = SDL_WINDOWPOS_UNDEFINED;
	int width = 640;
	int height = 480;
	Uint32 flags = SDL_WINDOW_RESIZABLE;

	SDL_Window* window =
		SDL_CreateWindow(
			"ImageViewer",
			initialXPos,
			initialYPos,
			width,
			height,
			flags);

	// Check that the window was successfully created
	if (window == NULL) {
		// In the case that the window could not be made...
		throw std::runtime_error(SDL_GetError());
	}

	return window;
}

struct my_error_mgr {
	struct jpeg_error_mgr pub;    /* "public" fields */

	jmp_buf setjmp_buffer;        /* for return to caller */
};

typedef struct my_error_mgr* my_error_ptr;


METHODDEF(void)
my_error_exit(j_common_ptr cinfo)
{
	/* cinfo->err really points to a my_error_mgr struct, so coerce pointer */
	my_error_ptr myerr = (my_error_ptr)cinfo->err;

	/* Always display the message. */
	/* We could postpone this until after returning, if we chose. */
	(*cinfo->err->output_message) (cinfo);

	/* Return control to the setjmp point */
	longjmp(myerr->setjmp_buffer, 1);
}


SDL_Surface* LoadImage(const char* filename)
{
	struct jpeg_decompress_struct cinfo;

	struct my_error_mgr jerr;

	FILE* infile;                 /* source file */
	JSAMPARRAY buffer;            /* Output row buffer */
	int row_stride;               /* physical row width in output buffer */


	if ((infile = fopen(filename, "rb")) == NULL) {
		fprintf(stderr, "can't open %s\n", filename);
		return nullptr;
	}

	/* Step 1: allocate and initialize JPEG decompression object */

	/* We set up the normal JPEG error routines, then override error_exit. */
	cinfo.err = jpeg_std_error(&jerr.pub);
	jerr.pub.error_exit = my_error_exit;
	/* Establish the setjmp return context for my_error_exit to use. */
	if (setjmp(jerr.setjmp_buffer)) {
		/* If we get here, the JPEG code has signaled an error.
		 * We need to clean up the JPEG object, close the input file, and return.
		 */
		jpeg_destroy_decompress(&cinfo);
		fclose(infile);
		return nullptr;
	}

	jpeg_create_decompress(&cinfo);
	jpeg_stdio_src(&cinfo, infile);

	(void)jpeg_read_header(&cinfo, TRUE);
	(void)jpeg_start_decompress(&cinfo);
	row_stride = cinfo.output_width * cinfo.output_components;
	buffer = (*cinfo.mem->alloc_sarray)((j_common_ptr)&cinfo, JPOOL_IMAGE, row_stride, 1);

	    Uint32 rmask, gmask, bmask, amask;

    /* SDL interprets each pixel as a 32-bit number, so our masks must depend
       on the endianness (byte order) of the machine */
#if SDL_BYTEORDER == SDL_BIG_ENDIAN
    rmask = 0xff000000;
    gmask = 0x00ff0000;
    bmask = 0x0000ff00;
    amask = 0x000000ff;
#else
    rmask = 0x000000ff;
    gmask = 0x0000ff00;
    bmask = 0x00ff0000;
    amask = 0xff000000;
#endif
	
	uint8_t *pixels = new uint8_t[cinfo.output_height * row_stride];

	while (cinfo.output_scanline < cinfo.output_height) {
		/* jpeg_read_scanlines expects an array of pointers to scanlines.
		 * Here the array is only one element long, but you could ask for
		 * more than one scanline at a time if that's more convenient.
		 */
		(void)jpeg_read_scanlines(&cinfo, buffer, 1);
		/* Assume put_scanline_someplace wants a pointer and sample count. */
		memcpy(&pixels[(cinfo.output_scanline - 1) * row_stride], buffer[0], row_stride);
	}

	SDL_Surface* surface = SDL_CreateRGBSurfaceFrom(
		pixels,
		cinfo.output_width,
		cinfo.output_height,
		24,
		row_stride,
		rmask,
		gmask,
		bmask,
		amask);

	(void)jpeg_finish_decompress(&cinfo);
	jpeg_destroy_decompress(&cinfo);
	fclose(infile);
	return surface;
}


static SDL_Surface* OptimizeImage(SDL_Surface* img,  SDL_PixelFormat *format) {

	SDL_Surface* optimizedImg = SDL_ConvertSurface(img, format, 0);

	return optimizedImg;
}

SDL_Surface* ProcessInput(int argc, char* argv[])
{
	if (argc != 2) {
		return nullptr;
	}
	SDL_Surface* image = LoadImage(argv[1]);
	return image;
}

int main(int argc, char* argv[]) {
	static SDL_Window* window = nullptr;
	static SDL_Surface* image = nullptr;
	static SDL_Surface* optimized = nullptr;
	static SDL_Surface* screenSurface = nullptr;
	static SDL_Event event;
	static Uint32 windowID = 0;
	
	#pragma omp parallel num_threads(2)
	{
		#pragma omp master
		{
			#pragma omp task
			{
				image = ProcessInput(argc, argv);
			}

			SDL_Init(SDL_INIT_VIDEO | SDL_INIT_EVENTS);
			window = CreateWindow();
			screenSurface = SDL_GetWindowSurface(window);
			optimized = SDL_ConvertSurface(image, screenSurface->format, 0);
			SDL_FreeSurface(image);
			windowID = SDL_GetWindowID(window);

			do
			{
				[[likely]]
				if (SDL_WaitEvent(&event) == 0)
				{
					[[unlikely]]
					std::cerr << SDL_GetError() << std::endl;
					break;
				}
				else if (event.type == SDL_WINDOWEVENT && windowID == event.window.windowID)
				{
					if (event.window.event == SDL_WINDOWEVENT_EXPOSED)
					{
						screenSurface = SDL_GetWindowSurface(window);
						SDL_BlitSurface(optimized, NULL, screenSurface, NULL);
						SDL_UpdateWindowSurface(window);
					}
				}
			} while (event.type != SDL_QUIT);
			SDL_FreeSurface(optimized);
			SDL_DestroyWindow(window);
			SDL_Quit();
		}
	}

	return 0;
}
