#include <algorithm>
#include <format>
#include <memory>
#include <vector>
#include <iostream>
#include <cstdint>
#include <format>
#include <thread>
#include <filesystem>
#include <cstdio>

#define GLFW_INCLUDE_NONE
#include <glad/glad.h>
#include <GLFW/glfw3.h>

import RawImage;
import UserInterface;

template <class T>
Image* ProcessInput(int argc, T* argv[])
{
	if (argc <= 0) {
		[[unlikely]]
		throw std::runtime_error("Argc cannot be less than zero");
	}

	std::filesystem::path path(argv[argc-1]);
	return Image::FromFile(path);
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
	Window* window;
	std::unique_ptr<GLTexturePool> texturePool;
	std::unique_ptr<Image> image;

	#pragma omp parallel num_threads(2)
	{
		#pragma omp master
		{
			#pragma omp task
			try
			{
				Image* imageLocal = ProcessInput(argc, argv);
				image.reset(imageLocal);
			}
			catch (std::exception& ex)
			{
				std::cerr << "Failed to process the request : " << ex.what() << std::endl;
			}

			window = Window::Initialize();
		}
	}
	if (window == nullptr) {
		[[unlikely]]
		return EXIT_FAILURE;
	}
	glEnable(GL_TEXTURE_2D);

	GLTexturePool *pool = window->MakeTexturesFromRaw(
		*image,
		Window::PreferredTextureWidth,
		Window::PreferredTextureHeight,
		Window::PreferredRedundantBorder);

	image.reset();

	window->Main(*pool);

	delete pool;
	Window::Deinitialize();
	return 0;
}
