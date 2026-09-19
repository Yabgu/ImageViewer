#pragma once

#include <filesystem>
#include <future>
#include <iostream>
#include <memory>
#include <stdexcept>
#include "FilterPluginDef.h"

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

    /* Decode the image on a worker thread so that file I/O and decoder work
     * overlaps window creation on this (main) thread.  The future is the only
     * hand-off between the two: the worker writes the shared state, and
     * get() below publishes it to this thread, so `image` is never touched
     * concurrently.  std::launch::async guarantees a real thread (the default
     * launch policy would silently run ProcessInput inline on this thread). */
    std::future<Image> imageFuture = std::async(std::launch::async, [argc, argv] {
        return ProcessInput(argc, argv);
    });

    /* Window creation stays on the main thread: GLFW expects window and event
     * handling on the thread that owns the context. */
    Window::Initialize();
    window = std::make_unique<Window>(640, 640);

    /* Blocks until the worker has finished (or rethrows its exception).  This
     * is the happens-before edge: everything the loader wrote is visible from
     * here on, and the Image is moved out of the future's shared state. */
    try
    {
        image = std::make_unique<Image>(imageFuture.get());
    }
    catch (std::exception& ex)
    {
#ifdef _WIN32
        MessageBoxA(NULL, ex.what(), "Error", MB_OK | MB_ICONERROR);
#else
        std::cerr << "Failed to process the request : " << ex.what() << std::endl;
#endif
    }

    if (image != nullptr)
    {
        static constexpr int PreferredTextureWidth = 256;
        static constexpr int PreferredTextureHeight = 256;
        static constexpr int PreferredRedundantBorder = 2;

        /* Default filter options: no arguments → plugin uses its built-in
         * defaults (typically a pass-through conversion to screen precision).
         * To configure dithering, pass named IWFilterArg entries, e.g.:
         *   IWFilterArg modeArg{ "mode", IW_FILTER_ARG_UINT32, {.u32=3} }; // PWM
         *   IWFilterArg bitsArg{ "pwm_bits", IW_FILTER_ARG_UINT32, {.u32=1} };
         *   IWFilterOptions opts{ IW_FILTER_OPTIONS_VERSION, 2, args }; */
        static constexpr IWFilterOptions kDefaultFilterOpts = {
            IW_FILTER_OPTIONS_VERSION,
            0u,
            nullptr
        };

        window->LoadTextures(
            *image,
            PreferredTextureWidth,
            PreferredTextureHeight,
            PreferredRedundantBorder,
            kDefaultFilterOpts);

        image.reset();
    }

    window->Main();
    window.reset();
    Window::Deinitialize();
    return 0;
}
