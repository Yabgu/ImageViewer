#pragma once

#include <filesystem>
#include <iostream>
#include <memory>
#include <stdexcept>

import Image;
import UserInterface;

template <typename T>
Image ProcessInput(int argc, T* argv[])
{
    if (argc <= 0)
    {
        [[unlikely]] throw std::runtime_error("Argc cannot be less than zero");
    }

    std::filesystem::path path(argv[argc - 1]);

    return Image::FromFile(path);
}

#if defined(_MSC_VER)
#define IVIEW_FORCEINLINE __forceinline
#elif defined(__GNUC__) || defined(__clang__)
#define IVIEW_FORCEINLINE __attribute__((always_inline)) inline
#else
#define IVIEW_FORCEINLINE inline
#endif

template <typename T>
IVIEW_FORCEINLINE int real_main(int argc, T* argv[])
{
    std::unique_ptr<Window> window;
    std::unique_ptr<Image> image;

#pragma omp parallel num_threads(2)
    {
#pragma omp master
        {
#pragma omp task
            try
            {
                Image imageLocal = ProcessInput(argc, argv);
                image.reset(new Image(imageLocal));
            }
            catch (std::exception& ex)
            {
#ifdef _WIN32
                MessageBoxA(NULL, ex.what(), "Error", MB_OK | MB_ICONERROR);
#else
                std::cerr << "Failed to process the request : " << ex.what() << std::endl;
#endif
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
