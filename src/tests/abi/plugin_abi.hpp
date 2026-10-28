#pragma once
/*
 * plugin_abi.hpp — helpers for tests that drive ImageViewer plugins through
 * their public C ABI.
 *
 * The plugins are built as separate shared libraries (libImageLoaderJpeg.so,
 * ImageLoaderJpeg.dll, …) and are only reachable at runtime by loading the
 * artefact and resolving its exported symbols.  That is exactly what iview
 * does, so these tests do the same rather than re-implementing plugin logic.
 *
 * Everything here is C++17 and header-only: it needs ImagePluginDef.h for the
 * ABI types and Utils.hpp for ValidateImageFormat, but no GL, no modules and
 * no project-internal headers.
 */

#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <stdexcept>
#include <string>
#include <utility>

#include "ImagePluginDef.h"
#include "FilterPluginDef.h"
#include "Utils.hpp"

#ifdef _WIN32
#  ifndef WIN32_LEAN_AND_MEAN
#    define WIN32_LEAN_AND_MEAN
#  endif
#  ifndef NOMINMAX
#    define NOMINMAX
#  endif
#  include <windows.h>
#else
#  include <dlfcn.h>
#endif

namespace iwabi {

// ─── Shared-library loader ───────────────────────────────────────────────────

/*
 * Minimal RAII wrapper around dlopen/LoadLibrary.  Errors are thrown with the
 * library path included, because "the plugin test failed" is useless without
 * knowing which artefact was being loaded.
 */
class SharedLibrary
{
public:
#ifdef _WIN32
    using Handle = HMODULE;
#else
    using Handle = void*;
#endif

    explicit SharedLibrary(const std::filesystem::path& path)
    {
#ifdef _WIN32
        // LOAD_WITH_ALTERED_SEARCH_PATH, not plain LoadLibraryW: the plugins
        // import runtime DLLs that sit next to them (vcpkg's applocal step
        // deploys jpeg62.dll, libpng16.dll, ... into the same directory), and
        // LoadLibraryW given an absolute path searches the *executable's*
        // directory, the system directories and PATH -- but not the directory
        // of the module being loaded.
        handle_ = LoadLibraryExW(path.wstring().c_str(), nullptr,
                                 LOAD_WITH_ALTERED_SEARCH_PATH);
        if (!handle_) {
            // Capture the code immediately.  Building the message below
            // allocates and may call Win32 functions that overwrite the
            // thread's last-error value, which makes the reported code
            // unreliable if it is read inside the concatenation.
            const unsigned long err = GetLastError();
            throw std::runtime_error(
                "cannot load plugin library: " + path.string() +
                " (GetLastError=" + std::to_string(err) + ")");
        }
#else
        // RTLD_NOW so a missing symbol fails here, not at first call.
        handle_ = ::dlopen(path.c_str(), RTLD_NOW | RTLD_LOCAL);
        if (!handle_) {
            const char* err = ::dlerror();
            throw std::runtime_error(
                "cannot load plugin library: " + path.string() +
                (err ? std::string(" (") + err + ")" : std::string()));
        }
#endif
    }

    ~SharedLibrary()
    {
        if (!handle_) return;
#ifdef _WIN32
        FreeLibrary(handle_);
#else
        ::dlclose(handle_);
#endif
    }

    SharedLibrary(const SharedLibrary&)            = delete;
    SharedLibrary& operator=(const SharedLibrary&) = delete;

    SharedLibrary(SharedLibrary&& other) noexcept : handle_(other.handle_)
    {
        other.handle_ = nullptr;
    }

    /* Resolve an exported symbol; throws if absent. */
    void* symbol(const char* name) const
    {
#ifdef _WIN32
        void* p = reinterpret_cast<void*>(GetProcAddress(handle_, name));
#else
        void* p = ::dlsym(handle_, name);
#endif
        if (!p)
            throw std::runtime_error(std::string("plugin is missing export: ") + name);
        return p;
    }

    template <class Fn>
    Fn function(const char* name) const
    {
        return reinterpret_cast<Fn>(symbol(name));
    }

private:
    Handle handle_ = nullptr;
};

// ─── Image-loader plugin facade ─────────────────────────────────────────────

/*
 * The three exports every image-loader plugin provides
 * (see ImagePluginDef.h): LoadImageFromFile, FreeImageData and the optional
 * ImagePluginGetLastError.
 */
class ImageLoaderPlugin
{
public:
    explicit ImageLoaderPlugin(const std::filesystem::path& path)
        : lib_(path)
    {
        load_  = lib_.function<LoadImageFromFileFunc>("LoadImageFromFile");
        free_  = lib_.function<FreeImageDataFunc>("FreeImageData");
        error_ = lib_.function<ImagePluginGetLastErrorFunc>("ImagePluginGetLastError");
    }

    ImagePluginResult load(const std::filesystem::path& image) const
    {
#ifdef _WIN32
        return load_(image.wstring().c_str());
#else
        return load_(image.c_str());
#endif
    }

    void free(ImagePluginData* data) const
    {
        if (data) free_(data);
    }

    /* Last error string.  The ABI declares this as non-null, but be defensive:
     * a plugin that returns nullptr here must not crash the test. */
    std::string lastError() const
    {
        const char* s = error_ ? error_() : nullptr;
        return s ? std::string(s) : std::string();
    }

private:
    SharedLibrary      lib_;
    LoadImageFromFileFunc        load_  = nullptr;
    FreeImageDataFunc            free_  = nullptr;
    ImagePluginGetLastErrorFunc  error_ = nullptr;
};

// ─── Descriptor consistency ─────────────────────────────────────────────────

/*
 * Check that a loader's descriptor and the buffer it describes agree.
 *
 * The contract these tests enforce is the one the renderer relies on:
 *
 *   • the format descriptor passes ValidateImageFormat();
 *   • width and height are positive;
 *   • bitsPerPixel describes whole bytes per pixel for the 8-bit formats
 *     these loaders emit, and equals componentCount * 8;
 *   • stride is large enough for one row of pixels;
 *   • size is exactly stride * height — these loaders emit one tightly packed
 *     buffer with no trailing slack, so `size` is the total byte length of
 *     stride-length rows, not width*height*bpp of unpadded data.
 *
 * Returns an empty string when consistent, otherwise a human-readable reason.
 * Returning a reason (rather than asserting) keeps the failure message useful
 * inside a doctest CHECK.
 */
inline std::string DescriptorProblem(const ImagePluginData& d)
{
    const char* fmtError = nullptr;
    if (!ValidateImageFormat(d.format, &fmtError)) {
        return std::string("ValidateImageFormat rejected the descriptor: ") +
               (fmtError ? fmtError : "unknown reason");
    }

    if (d.width <= 0 || d.height <= 0)
        return "non-positive dimensions";
    if (d.data == nullptr)
        return "null pixel buffer";

    const int bytesPerPixel = static_cast<int>((d.format.bitsPerPixel + 7u) / 8u);
    if (bytesPerPixel <= 0)
        return "bitsPerPixel does not describe a whole number of bytes";

    if (static_cast<int>(d.format.bitsPerPixel) !=
        static_cast<int>(d.format.componentCount) * 8)
    {
        return "bitsPerPixel != componentCount * 8";
    }

    if (d.stride < d.width * bytesPerPixel)
        return "stride is too small for one row";

    const size_t expected = static_cast<size_t>(d.stride) *
                            static_cast<size_t>(d.height);
    if (d.size != expected)
        return "size != stride * height (expected " + std::to_string(expected) +
               ", got " + std::to_string(d.size) + ")";

    return {};
}

// ─── Filter plugin facade ───────────────────────────────────────────────────

/*
 * The three exports a filter plugin provides (see FilterPluginDef.h):
 * FilterImage, FreeFilterImageSet and the optional FilterPluginGetLastError.
 */
class FilterPlugin
{
public:
    explicit FilterPlugin(const std::filesystem::path& path)
        : lib_(path)
    {
        filter_ = lib_.function<FilterImageFunc>("FilterImage");
        free_   = lib_.function<FreeFilterImageSetFunc>("FreeFilterImageSet");
        error_  = lib_.function<FilterPluginGetLastErrorFunc>("FilterPluginGetLastError");
    }

    IWFilterImageSet* Filter(const ImagePluginData* src,
                             const IWScreenInfo*    screen,
                             const IWFilterOptions* options) const
    {
        return filter_(src, screen, options);
    }

    void Free(IWFilterImageSet* set) const
    {
        if (set) free_(set);
    }

    std::string lastError() const
    {
        const char* s = error_ ? error_() : nullptr;
        return s ? std::string(s) : std::string();
    }

private:
    SharedLibrary               lib_;
    FilterImageFunc             filter_ = nullptr;
    FreeFilterImageSetFunc      free_   = nullptr;
    FilterPluginGetLastErrorFunc error_ = nullptr;
};

// ─── Pixel access ──────────────────────────────────────────────────────────

/*
 * Read component `index` of the pixel at (x, y) as a fraction of that
 * component's declared maximum, so 8-bit and 10-bit targets are comparable.
 * Uses the descriptor's own bitOffset/bitWidth: the packed 10-bit formats this
 * plugin emits do not align to byte boundaries.
 */
inline double ComponentFraction(const ImagePluginData& d, int index, int x, int y)
{
    const IWComponentDef& c = d.format.components[index];
    const int bpp = static_cast<int>((d.format.bitsPerPixel + 7u) / 8u);
    const uint8_t* pixel = d.data + static_cast<std::ptrdiff_t>(y) * d.stride +
                           static_cast<std::ptrdiff_t>(x) * bpp;

    const int byteStart = c.bitOffset / 8;
    const int bitStart  = c.bitOffset % 8;
    const int need      = (bitStart + c.bitWidth + 7) / 8;

    uint64_t word = 0;
    for (int i = 0; i < need && i < 5; ++i)
        word |= static_cast<uint64_t>(pixel[byteStart + i]) << (i * 8);
    word >>= bitStart;

    const uint64_t mask = (c.bitWidth < 64u)
                          ? ((uint64_t{1} << c.bitWidth) - 1u) : ~uint64_t{0};
    return static_cast<double>(word & mask) / static_cast<double>(mask);
}

} // namespace iwabi
