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
#include <regex>

#define GLFW_INCLUDE_NONE
#include <glad/glad.h>
#include <GLFW/glfw3.h>

import Image;
import UserInterface;

template <class T>
Image* ProcessInput(int argc, T* argv[])
{
	if (argc <= 0)
	{
		[[unlikely]] throw std::runtime_error("Argc cannot be less than zero");
	}

	std::filesystem::path path(argv[argc - 1]);
	auto extension = path.extension().string();
	if (!std::regex_match(extension, std::regex("\\.jpeg|\\.jpg|\\.jfif", std::regex::icase)))
	{
		[[unlikely]] throw std::runtime_error("Only jpeg format is supported now");
	}

	return Image::FromFile(path);
}

#if defined(_WIN32)
#include <Windows.h>
int WINAPI wWinMain(HINSTANCE hInstance, HINSTANCE hPrevInstance, PWSTR pCmdLine, int nCmdShow)
{
#else
int main(int argc, char* argv[])
{
#endif
	std::unique_ptr<Window> window;
	std::unique_ptr<Image> image;

#pragma omp parallel num_threads(2)
	{
#pragma omp master
		{
#pragma omp task
			try
			{
#if defined(_WIN32)
				LPWSTR* argv;
				int argc;

				argv = CommandLineToArgvW(pCmdLine, &argc);
				if (argv == nullptr)
				{
					argc = 0;
				}
#endif

				Image* imageLocal = ProcessInput(argc, argv);
				if (imageLocal != nullptr)
				{
					image.reset(imageLocal);
				}
			}
			catch (std::exception& ex)
			{
				std::cerr << "Failed to process the request : " << ex.what() << std::endl;
			}

			Window::Initialize();
			window.reset(new Window(640, 640));
		}
	}

	if (image != nullptr)
	{
		static constexpr int PreferredTextureWidth = 256;
		static constexpr int PreferredTextureHeight = 256;
		static constexpr int PreferredRedundantBorder = 2;
		window->LoadTextures(
			*image,
			PreferredTextureWidth,
			PreferredTextureHeight,
			PreferredRedundantBorder);

		image.reset();
	}

	window->Main();
	window.reset();
	Window::Deinitialize();
	return 0;
}
