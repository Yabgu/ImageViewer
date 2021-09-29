#include <format>
#include <memory>
#include <vector>
#include <iostream>
#include <cstdint>
#include <setjmp.h>
#include <cassert>

#define GLFW_INCLUDE_NONE
#include <glad/glad.h>
#include <GLFW/glfw3.h>
#include <jpeglib.h>

void window_size_callback(GLFWwindow* window, int width, int height)
{
	glViewport(0, 0, width, height);
}

GLFWwindow* InitializeUI()
{
	int width = 640;
	int height = 640;

	if (!glfwInit()) {
		[[unlikely]]
		std::cerr << "glfwInit failed" << std::endl;
		return nullptr;
	}

	// Check that the window was successfully created
	glfwWindowHint(GLFW_CONTEXT_VERSION_MAJOR, 1);
    glfwWindowHint(GLFW_CONTEXT_VERSION_MINOR, 1);
 
    GLFWwindow* window = glfwCreateWindow(width, height, "ImageViewer", NULL, NULL);
	
	glfwMakeContextCurrent( window );

	glfwSetWindowSizeCallback(window, window_size_callback);
	if (!gladLoadGLLoader((GLADloadproc)glfwGetProcAddress))
	{
		[[unlikely]]
		std::cerr << "gladLoadGLLoader failed" << std::endl;
		return nullptr;
	}
	return window;
}

struct my_error_mgr {
	struct jpeg_error_mgr pub;
	jmp_buf setjmp_buffer;
};

typedef struct my_error_mgr* my_error_ptr;


METHODDEF(void) my_error_exit(j_common_ptr cinfo)
{
	/* cinfo->err really points to a my_error_mgr struct, so coerce pointer */
	my_error_ptr myerr = (my_error_ptr)cinfo->err;

	/* Always display the message. */
	/* We could postpone this until after returning, if we chose. */
	(*cinfo->err->output_message) (cinfo);

	/* Return control to the setjmp point */
	longjmp(myerr->setjmp_buffer, 1);
}

#pragma pack(push)
#pragma pack(1)
class RawImageData
{
protected:
	static void* operator new(size_t size, unsigned int width, unsigned int height, unsigned int components_per_pixel)
	{
		return ::operator new(size + static_cast<size_t>(width) * height * components_per_pixel);
	}

	RawImageData(unsigned int width, unsigned int height, unsigned int components_per_pixel)
		: width(width), height(height), components_per_pixel(components_per_pixel)
	{
	}
public:
	unsigned int width;
	unsigned int height;
	unsigned int components_per_pixel;
	uint8_t data[sizeof(width)];

	static RawImageData* FromFile(FILE* infile);
};
#pragma pack(pop)

RawImageData* RawImageData::FromFile(FILE* infile)
{
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
		return 0;
	}

	jpeg_create_decompress(&cinfo);
	jpeg_stdio_src(&cinfo, infile);

	(void)jpeg_read_header(&cinfo, TRUE);
	(void)jpeg_start_decompress(&cinfo);
	int row_stride = cinfo.output_width * cinfo.output_components;

	constexpr int MaxLinesToRead = 2;

	JSAMPARRAY buffer = (*cinfo.mem->alloc_sarray)((j_common_ptr)&cinfo, JPOOL_IMAGE, row_stride, MaxLinesToRead);

	RawImageData *image = new((unsigned)cinfo.output_width, (unsigned)cinfo.output_height, cinfo.output_components)
		RawImageData(cinfo.output_width, cinfo.output_height, cinfo.output_components);


	while (cinfo.output_scanline < cinfo.output_height) {
		auto current_scanline = cinfo.output_scanline;
		auto readlines = jpeg_read_scanlines(&cinfo, buffer, MaxLinesToRead);
		if (readlines == 0)
		{
			[[unlikely]]
			// TODO: Handle the error!
			break;
		}
		memcpy(&image->data[current_scanline * row_stride], buffer[0], row_stride * readlines);
	}

	(void)jpeg_finish_decompress(&cinfo);
	jpeg_destroy_decompress(&cinfo);
	return image;
}

template <class T>
RawImageData* ProcessInput(int argc, T* argv[])
{
	if (argc <= 0) {
		return 0;
	}
	FILE* infile;
#ifdef WIN32
	if ((infile = _wfopen(argv[argc - 1], L"rb")) == nullptr) {
		fwprintf(stderr, L"can't open %s\n", argv[1]);
#else
	if ((infile = fopen(argv[argc - 1], "rb")) == nullptr) {
		fprintf(stderr, "can't open %s\n", argv[1]);
#endif
		return 0;
	}
	RawImageData* image = RawImageData::FromFile(infile);
	fclose(infile);
	return image;
}

GLuint MakeTexture(RawImageData* image)
{
	GLuint texture;
	glGenTextures(1, &texture);
	glBindTexture(GL_TEXTURE_2D, texture);
	// set the texture wrapping/filtering options (on the currently bound texture object)
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_REPEAT);	
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_REPEAT);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
	// load and generate the texture

	glTexImage2D(GL_TEXTURE_2D, 0, GL_RGB, image->width, image->height, 0, GL_RGB, GL_UNSIGNED_BYTE, image->data);
	return texture;
}

void draw(GLFWwindow* window)
{
 	glClearColor(0.2f, 0.3f, 0.3f, 1.0f);
	glClear(GL_COLOR_BUFFER_BIT);
	glLoadIdentity();

	glBegin(GL_TRIANGLE_STRIP);
	glTexCoord2f(0.0f, 0.0f); glVertex3f(-1.0f,  1.0f, 0.0f);
	glTexCoord2f(1.0f, 0.0f); glVertex3f( 1.0f,  1.0f, 0.0f);
	glTexCoord2f(0.0f, 1.0f); glVertex3f(-1.0f, -1.0f, 0.0f);
	glTexCoord2f(1.0f, 1.0f); glVertex3f( 1.0f, -1.0f, 0.0f);
	glEnd();
	glfwSwapBuffers(window);
}

void framebuffer_size_callback(GLFWwindow* window, int width, int height)
{
    glViewport(0, 0, width, height);
    draw(window);
}

#if defined(_WIN32)
#include <Windows.h>
int WINAPI wWinMain(HINSTANCE hInstance, HINSTANCE hPrevInstance, PWSTR pCmdLine, int nCmdShow)
{
	LPWSTR *argv;
	int argc;

	argv = CommandLineToArgvW(pCmdLine, &argc);
	if(argv == nullptr)
	{
		return EXIT_FAILURE;
	}
#else
int main(int argc, char* argv[])
{
#endif
	static GLFWwindow* window = nullptr;
	static RawImageData* image = nullptr;
	static GLuint texture = 0;

	#pragma omp parallel num_threads(2)
	{
		#pragma omp master
		{
			#pragma omp task
			{
				image = ProcessInput(argc, argv);
			}
			window = InitializeUI();
		}
	}
	if (window == nullptr) {
		[[unlikely]]
		if (image != nullptr)
		{
			delete image;
		}
		exit(EXIT_FAILURE);
	}
	glEnable(GL_TEXTURE_2D);
	if (image != nullptr)
	{
		texture = MakeTexture(image);
		delete image;
		image = nullptr;
	}

	glfwSetFramebufferSizeCallback(window, framebuffer_size_callback);

	while (!glfwWindowShouldClose(window))
	{
		glfwWaitEventsTimeout(1.0);
		[[likely]]
		draw(window);
	}
	glfwTerminate();
	return 0;
}
