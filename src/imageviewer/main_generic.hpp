#pragma once

#include <cstring>
#include <cstdio>
#include <chrono>
#include <filesystem>
#include <future>
#include <iostream>
#include <memory>
#include <stdexcept>
#include <string>
#include <type_traits>
#include <vector>
#include "Args.hpp"
#include "Config.hpp"
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
 * deliberate request rather than an absent argument.  The temporal bit count is
 * sent as "temporal_bits" (documented name) plus the legacy "pwm_bits" name,
 * and only for PWM; spatial_bits/seed are only meaningful for the spatial
 * modes.  The plugin clamps everything to its own budget.
 */
inline IWFilterOptions MakeFilterOptions(const ViewerArgs& parsed,
                                         IWFilterArg (&storage)[5]) noexcept
{
    std::memset(storage, 0, sizeof(IWFilterArg) * 5u);
    uint32_t count = 0;

    auto addU32 = [&](const char* name, uint32_t value) {
        std::strncpy(storage[count].name, name, IW_FILTER_ARG_NAME_MAX - 1u);
        storage[count].type      = IW_FILTER_ARG_UINT32;
        storage[count].value.u32 = value;
        ++count;
    };
    auto addU64 = [&](const char* name, uint64_t value) {
        std::strncpy(storage[count].name, name, IW_FILTER_ARG_NAME_MAX - 1u);
        storage[count].type      = IW_FILTER_ARG_UINT64;
        storage[count].value.u64 = value;
        ++count;
    };

    addU32("mode", static_cast<uint32_t>(parsed.mode));

    if (parsed.mode == kModePWM && parsed.pwmBits != 0u) {
        /* "temporal_bits" is the documented name; "pwm_bits" is sent as well so
         * a plugin built against the old vocabulary keeps working unchanged. */
        addU32("temporal_bits", parsed.pwmBits);
        addU32("pwm_bits",      parsed.pwmBits);
    }

    const bool spatialMode = (parsed.mode == kModeBayer4x4 ||
                              parsed.mode == kModeBayer8x8 ||
                              parsed.mode == kModeBlueNoise ||
                              parsed.mode == kModeSpatialTemporal);
    if (spatialMode) {
        addU32("spatial_bits", parsed.spatialBits);   /* 0 = matrix maximum */
        addU64("seed",         parsed.seed);
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

    /* N-phase temporal dithering is not implemented in the Vulkan renderer: it
     * only has the two offscreen phases A/B.  Refuse at startup — before the
     * window, the async decode and the filter plugin exist — instead of showing
     * a wrong image later. */
    if (parsed.pwmBits > 1u) {
        std::cerr << "iview: pwm_bits > 1 (" << parsed.pwmBits
                  << "-phase) is not implemented in this build\n";
        return 2;
    }

    std::unique_ptr<Window> window;

    /* Decode the image on a worker thread so that file I/O and decoder work
     * overlaps window creation on this (main) thread.  The future is the only
     * hand-off between the two: the worker writes the shared state, and Main()
     * is the single consumer on this thread, so the Image is never touched
     * concurrently.  std::launch::async guarantees a real thread (the default
     * launch policy would silently run ProcessInput inline on this thread).
     * The path is captured by value because `parsed` is not shared. */
    std::filesystem::path imagePath = PathFromUtf8(parsed.imagePath);
    std::future<Image> imageFuture;

    /* A directory argument opens its first image; the Window scans the folder
     * for prev/next navigation.  Done before the decode so the async launch
     * order below is unchanged.  --settings may run without an image. */
    if (!imagePath.empty())
    {
        std::error_code dirEc;
        if (std::filesystem::is_directory(imagePath, dirEc)) {
            try { imagePath = Window::FirstImageInFolder(imagePath); }
            catch (const std::exception& ex) {
                std::cerr << "iview: " << ex.what() << "\n";
                return 2;
            }
        }

        {
            char line[512];
            std::snprintf(line, sizeof(line), "initial decode submit path=%s",
                          imagePath.filename().string().c_str());
            IviewTrace(line);
        }
        imageFuture = std::async(std::launch::async, [imagePath] {
            IviewTrace("initial decode worker start");
            try
            {
                Image image = ProcessInput(imagePath);
                IviewTrace("initial decode worker done");
                return image;
            }
            catch (const std::exception& ex)
            {
                if (IviewTraceEnabled())
                {
                    char line[512];
                    std::snprintf(line, sizeof(line), "initial decode worker threw: %s", ex.what());
                    IviewTrace(line);
                }
                throw;
            }
            catch (...)
            {
                IviewTrace("initial decode worker threw (unknown)");
                throw;
            }
        });
        IviewTrace("async decode launched");
    }
    else
    {
        IviewTrace("no image path; starting with Settings");
    }

    /* Settings are read *after* the decode is running, so file I/O for the
     * config cannot delay the image.  Resolution order and validation live in
     * Config.hpp; this must stay well under a millisecond. */
    const auto configStart = std::chrono::steady_clock::now();
    Config::Settings settings = Config::Load(parsed.configPath, false);
    const double configMs = std::chrono::duration<double, std::milli>(
        std::chrono::steady_clock::now() - configStart).count();
    {
        char line[512];
        std::snprintf(line, sizeof(line), "config loaded in %.3f ms (%s)",
                      configMs,
                      settings.loadPath.empty() ? "builtin defaults" : settings.loadPath.c_str());
        IviewTrace(line);
    }

    /* Merge order: builtin defaults < config file < CLI flags. */
    if (parsed.filterSpecified) settings.filterMode = static_cast<uint32_t>(parsed.mode);
    if (parsed.pwmBits != 0u)   settings.pwmBits    = parsed.pwmBits;

    /* The same refusal as above, now covering a pwm_bits that came from the
     * config file. */
    if (settings.pwmBits > 1u) {
        std::cerr << "iview: pwm_bits > 1 (" << settings.pwmBits
                  << "-phase) is not implemented in this build\n";
        return 2;
    }

    /* Window creation stays on the main thread: GLFW expects window and event
     * handling on the thread that owns the context. */
    Window::Initialize();
    window = std::make_unique<Window>(settings.windowWidth, settings.windowHeight, settings);
    if (parsed.settingsRequested) window->SetSettingsOpen(true);
    IviewTrace("window + renderer constructed");

    /* The decode keeps running on its worker thread: Main polls the future
     * without blocking, uploads as soon as it is ready and normally only then
     * shows the window.  With --settings the window may be shown earlier for
     * Settings, so the image view stays blank until the decode lands.  The
     * filter options are copied into the Window, so `filterArgs` here does not
     * have to outlive Main(). */
    ViewerArgs effective = parsed;                    /* CLI, for spatial args */
    effective.mode    = static_cast<IWFilterMode>(settings.filterMode);
    effective.pwmBits = settings.pwmBits;
    IWFilterArg filterArgs[5];
    const IWFilterOptions filterOpts = MakeFilterOptions(effective, filterArgs);
    window->SetFilterOptions(filterOpts);

    window->Main(std::move(imageFuture), imagePath);
    window.reset();
    Window::Deinitialize();
    return 0;
}
