#include <algorithm>
#include <format>
#include <memory>
#include <vector>
#include <iostream>
#include <cstdint>
#include <cassert>
#include <format>
#include <thread>
#include <filesystem>
#include <cstdio>

#define GLFW_INCLUDE_NONE
#include <glad/glad.h>
#include <GLFW/glfw3.h>

#include <boost/lockfree/spsc_queue.hpp>

export module UserInterface;
import RawImage;

export struct GLTexturePool
{
private:
	size_t size;
	size_t capacity;
	GLuint* textures;

public:
	GLTexturePool(size_t capacity)
		: size(0), capacity(capacity), textures(new GLuint[capacity])
	{
		glGenTextures(capacity, textures);
	}
	
	GLTexturePool(GLTexturePool&&) = default;
	GLTexturePool(const GLTexturePool&) = delete;

	~GLTexturePool()
	{
		glDeleteTextures(capacity, textures);
		delete[] textures;
	}

	void Bind(int index)
	{
		if (index < 0)
		{
			[[unlikely]]
			throw std::runtime_error("negative index");
		}
		glBindTexture(GL_TEXTURE_2D, textures[index]);
	}

	GLuint AddTexture(const Image& image)
	{
		if (size >= capacity)
		{
			[[unlikely]]
			throw std::runtime_error("Max capacity reached for texture pool");
		}

		glBindTexture(GL_TEXTURE_2D, textures[size]);

		glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP);	
		glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP);
		glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
		glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);

		glTexImage2D(GL_TEXTURE_2D, 0, GL_RGB, image.width, image.height, 0, GL_RGB, GL_UNSIGNED_BYTE, image.data);
		return textures[size++];
	}
};

export class Window
{
protected:
	static void window_size_callback(GLFWwindow* window, int width, int height)
	{
		glViewport(0, 0, width, height);
		glMatrixMode(GL_PROJECTION);
		glLoadIdentity();
		glOrtho(0, width, height, 0, -1, 1);

		glMatrixMode(GL_TEXTURE);
		glLoadIdentity();
		glScalef(1.0/256, 1.0/256, 1.0);

		glMatrixMode(GL_MODELVIEW);
	}

	static void framebuffer_size_callback(GLFWwindow* window, int width, int height)
	{
		window_size_callback(window, width, height);
		//draw(window);
	}


	static void scroll_callback(GLFWwindow* window, double xoffset, double yoffset)
	{
		Window* win = (Window*)glfwGetWindowUserPointer(window);
		win->scroll = std::min(std::max(win->scroll - (int)(yoffset * 16), -75), 400);
		win->zoom = 100.0 / (100 + win->scroll);
	}

private:
	static Window* gWindow;

	GLFWwindow* const window;
	int scroll;
	double zoom;

	GLFWwindow* InitializeWindow(int width, int height)
	{
		if (!glfwInit()) {
			[[unlikely]]
			throw std::runtime_error("glfwInit failed");
			return nullptr;
		}

		// Check that the window was successfully created
		glfwWindowHint(GLFW_CONTEXT_VERSION_MAJOR, 1);
		glfwWindowHint(GLFW_CONTEXT_VERSION_MINOR, 1);

		GLFWwindow* window = glfwCreateWindow(width, height, "ImageViewer", NULL, NULL);

		glfwMakeContextCurrent(window);

		glfwSetWindowSizeCallback(window, window_size_callback);
		glfwSetFramebufferSizeCallback(window, framebuffer_size_callback);
		glfwSetScrollCallback(window, scroll_callback);

		if (!gladLoadGLLoader((GLADloadproc)glfwGetProcAddress))
		{
			[[unlikely]]
			throw std::runtime_error("gladLoadGLLoader failed");
		}

		// Resize the window
		window_size_callback(window, width, height);
		return window;
	}


	Window(int width, int height)
		: window(InitializeWindow(width, height)), scroll(0), zoom(1.0)
	{
		glfwSetWindowUserPointer(window, (void*)this);
	}

public:
	static constexpr int PreferredTextureWidth = 256;
	static constexpr int PreferredTextureHeight = 256;
	static constexpr int PreferredRedundantBorder = 2;

	static Window* Initialize()
	{
		if (gWindow != nullptr)
		{
			[[unlikely]]
			throw std::runtime_error("Already initialized");
		}
		gWindow = new Window(640, 640);
	}

	static void Deinitialize()
	{
		delete gWindow;
		gWindow = nullptr;
		glfwTerminate();
	}

	int CalculateNumberOfSlices(const unsigned int size, const unsigned int segment, const unsigned int redundantBorder)
	{
		unsigned int slices = (size + segment + redundantBorder - 1) / segment;
		while (slices * (segment - redundantBorder) < size)
		{
			++slices;
		}
		return slices;
	}

	GLTexturePool* MakeTexturesFromRaw(
		const Image& image,
		const unsigned int width,
		const unsigned int height,
		const unsigned int redundantBorder)
	{
		if (width == 0 || height == 0)
		{
			[[unlikely]]
			throw std::runtime_error("width and height cannot be zero");
		}
		const unsigned int numberOfVerticalSlices = CalculateNumberOfSlices(image.width, width, redundantBorder);
		const unsigned int numberOfHorizontalSlices = CalculateNumberOfSlices(image.height, height, redundantBorder);
		const size_t numberOfTextures = numberOfHorizontalSlices * numberOfVerticalSlices;

		GLTexturePool* result = new GLTexturePool(numberOfTextures);
		auto indexHash = [numberOfVerticalSlices](int x, int y) -> size_t { return x + y * numberOfVerticalSlices; };
		std::vector<Image*> subsection(numberOfHorizontalSlices * numberOfVerticalSlices);
		boost::lockfree::spsc_queue<size_t> finishedIndexes(subsection.size());


		#pragma omp parallel shared(subsection, finishedIndexes, image)
		{
			#pragma omp master
			{
				#pragma omp task
				{
					#pragma omp parallel for ordered collapse(2)
					for (auto y = 0; y < numberOfHorizontalSlices; ++y)
					{
						for (auto x = 0; x < numberOfVerticalSlices; ++x)
						{
							auto i = indexHash(x, y);
							subsection[i] = image.Subsection(x * (width - redundantBorder), y * (height - redundantBorder), width, height);
							#pragma omp ordered
							finishedIndexes.push(i);
						}
					}
				}
				for (auto y = 0; y < subsection.size(); ++y)
				{
					while (finishedIndexes.empty())
					{
						std::this_thread::yield();
					}
					auto i = finishedIndexes.front();
					finishedIndexes.pop();
					result->AddTexture(*subsection[i]);
					delete subsection[i];
				}
			}
		}
		return result;
	}

	void Draw(GLTexturePool& pool)
	{
		glClearColor(0.2f, 0.3f, 0.3f, 1.0f);
		glClear(GL_COLOR_BUFFER_BIT);

		for (auto y = 0; y < 7; ++y)
		{
			for (auto x = 0; x < 4; ++x)
			{
				glLoadIdentity();
				glScaled(zoom, zoom, 0);
				glTranslated((double)x * (PreferredTextureWidth - PreferredRedundantBorder), (double)y * (PreferredTextureHeight - PreferredRedundantBorder), 0.0);
				pool.Bind(x + y * 4);
				glBegin(GL_TRIANGLE_STRIP);
				glTexCoord2i(1, 255); glVertex2i(1, PreferredTextureHeight-1);
				glTexCoord2i(255, 255); glVertex2i(PreferredTextureWidth-1, PreferredTextureHeight-1);
				glTexCoord2i(1, 1); glVertex2i(1, 1);
				glTexCoord2i(255, 1); glVertex2i(PreferredTextureWidth-1, 1);
				glEnd();
			}
		}
		glfwSwapBuffers(window);
	}
	
	void Main(GLTexturePool &pool)
	{
		while (!glfwWindowShouldClose(window))
		{
			glfwWaitEventsTimeout(1.0 / 60);
			[[likely]]
			Draw(pool);
		}
	}
};

Window* Window::gWindow = nullptr;
