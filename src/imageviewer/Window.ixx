#include <memory>
#include <vector>
#include <iostream>
#include <cstdint>
#include <cassert>
#include <format>
#include <thread>
#include <filesystem>

#define GLFW_INCLUDE_NONE
#include <glad/glad.h>
#include <GLFW/glfw3.h>

import TexturePool;
import Image;
export module UserInterface;

export class Window
{
protected:
	static void window_size_callback(GLFWwindow* window, int width, int height)
	{
		Window* win = (Window*)glfwGetWindowUserPointer(window);
		if (win != nullptr)
		{
			win->width = width;
			win->height = height;
		}

		glViewport(0, 0, width, height);
		glMatrixMode(GL_PROJECTION);
		glLoadIdentity();
		glOrtho(0, width, height, 0, -1, 1);

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
	}

private:
	GLFWwindow* const glfwWindow;
	int width;
	int height;
	int scroll;
	std::unique_ptr<TextureCollection> textureCollection;

	static GLFWwindow* InitializeWindow(int width, int height, Window* containerWindow)
	{
		// Check that the window was successfully created
		glfwWindowHint(GLFW_CONTEXT_VERSION_MAJOR, 3);
		glfwWindowHint(GLFW_CONTEXT_VERSION_MINOR, 1);

		GLFWwindow* glfwWindow = glfwCreateWindow(width, height, "ImageViewer", NULL, NULL);

		glfwSetWindowUserPointer(glfwWindow, (void*)containerWindow);

		glfwMakeContextCurrent(glfwWindow);

		glfwSetWindowSizeCallback(glfwWindow, window_size_callback);
		glfwSetFramebufferSizeCallback(glfwWindow, framebuffer_size_callback);
		glfwSetScrollCallback(glfwWindow, scroll_callback);

		if (!gladLoadGLLoader((GLADloadproc)glfwGetProcAddress))
		{
			[[unlikely]] throw std::runtime_error("gladLoadGLLoader failed");
		}

		glEnable(GL_TEXTURE_2D);

		// Resize the window
		window_size_callback(glfwWindow, width, height);
		return glfwWindow;
	}

public:
	static void Initialize()
	{
		if (!glfwInit())
		{
			[[unlikely]] throw std::runtime_error("glfwInit failed");
		}
	}

	static void Deinitialize()
	{
		glfwTerminate();
	}

	Window(int width, int height)
		: glfwWindow(InitializeWindow(width, height, this)), width(width), height(height), scroll(0)
	{
	}

	void LoadTextures(
		const Image& image,
		const int segmentWidth,
		const int segmentHeight,
		const int redundantBorderSize)
	{
		this->textureCollection.reset(new TextureCollection(image, segmentWidth, segmentHeight, redundantBorderSize));
		double zoom = std::min<double>((double)width / textureCollection->width, (double)height / textureCollection->height);
		this->scroll = 100.0 / zoom - 100;
	}

	void Draw()
	{
		glClearColor(0.0f, 0.0f, 0.0f, 1.0f);
		glClear(GL_COLOR_BUFFER_BIT);
		glLoadIdentity();
		if (textureCollection != nullptr)
		{
			double zoom = 100.0 / (100 + this->scroll);
			glTranslated((width - textureCollection->width * zoom) * 0.5, (height - textureCollection->height * zoom) * 0.5, 0);
			glScaled(zoom, zoom, 0);
			textureCollection->Draw();
		}
		glfwSwapBuffers(glfwWindow);
	}

	void Main()
	{
		glfwMakeContextCurrent(this->glfwWindow);
		while (!glfwWindowShouldClose(this->glfwWindow))
		{
			[[likely]] glfwWaitEventsTimeout(1.0 / 60);
			Draw();
		}
	}
};
