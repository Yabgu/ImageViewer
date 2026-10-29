#pragma once

#include <cstring>
#include <filesystem>
#include <future>
#include <iostream>
#include <memory>
#include <stdexcept>
#include <string>
#include <type_traits>
#include <vector>
#include "Args.hpp"
#include "FilterPluginDef.h"

import Image;
import UserInterface;

inline Image ProcessInput(const std::filesystem::path& path)
{
    return Image::FromFile(path);
}

#if defined(_MSC_VER)
#define IVIEW_FORCEINLINE __forceinline
#elif defined(__GNUC__) || defined(__clang__)
#define IVIEW_FORCEINLINE __attribute__((always_inline)) inline
#else
#define IVIEW_FORCEINLINE inline
#endif

/*
 * Build the IWFilterOptions handed to the filter plugin from the parsed
 * command line.  `mode` is always sent explicitly so that mode=none is a
 * deliberate request rather than an absent argument, and pwm_bits is only sent
 * for PWM because the plugin treats it as PWM-specific.
 */
inline IWFilterOptions MakeFilterOptions(const ViewerArgs& parsed,
                                         IWFilterArg (&storage)[2]) noexcept
{
    std::memset(storage, 0, sizeof(IWFilterArg) * 2u);
    uint32_t count = 0;

    std::strncpy(storage[count].name, "mode", IW_FILTER_ARG_NAME_MAX - 1u);
    storage[count].type      = IW_FILTER_ARG_UINT32;
    storage[count].value.u32 = static_cast<uint32_t>(parsed.mode);
    ++count;

    if (parsed.mode == kModePWM && parsed.pwmBits != 0u) {
        std::strncpy(storage[count].name, "pwm_bits", IW_FILTER_ARG_NAME_MAX - 1u);
        storage[count].type      = IW_FILTER_ARG_UINT32;
        storage[count].value.u32 = parsed.pwmBits;
        ++count;
    }

    IWFilterOptions opts{};
    opts.version = IW_FILTER_OPTIONS_VERSION;
    opts.argc    = count;
    opts.argv    = storage;
    return opts;
}

template <typename T>
IVIEW_FORCEINLINE int real_main(int argc, T* argv[])
{
    const ViewerArgs parsed = ParseArgs(argc, argv);

    if (parsed.helpRequested) {
        std::cout << UsageText();
        return 0;
    }

    if (!parsed.parseError.empty()) {
        std::cerr << "iview: " << parsed.parseError << "\n\n" << UsageText();
        return 2;
    }

    std::unique_ptr<Window> window;
    std::unique_ptr<Image> image;

    /* Decode the image on a worker thread so that file I/O and decoder work
     * overlaps window creation on this (main) thread.  The future is the only
     * hand-off between the two: the worker writes the shared state, and
     * get() below publishes it to this thread, so `image` is never touched
     * concurrently.  std::launch::async guarantees a real thread (the default
     * launch policy would silently run ProcessInput inline on this thread).
     * The path is captured by value because `parsed` is not shared. */
    const std::filesystem::path imagePath = PathFromUtf8(parsed.imagePath);
    std::future<Image> imageFuture = std::async(std::launch::async, [imagePath] {
        return ProcessInput(imagePath);
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

        /* Filter options come from the command line, so dithering modes are
         * reachable from the shipped binary: `iview --filter=pwm photo.jpg`.
         * See ParseViewerArgs() in Args.hpp for the accepted flags. */
        IWFilterArg filterArgs[2];
        const IWFilterOptions filterOpts = MakeFilterOptions(parsed, filterArgs);

        window->LoadTextures(
            *image,
            PreferredTextureWidth,
            PreferredTextureHeight,
            PreferredRedundantBorder,
            filterOpts);

        image.reset();
    }

    window->Main();
    window.reset();
    Window::Deinitialize();
    return 0;
}
