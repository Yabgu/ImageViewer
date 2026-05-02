module;

#include <filesystem>
#include <stdexcept>
#include <string>
#include <format>
#include "FilterPluginDef.h"

#ifdef _WIN32
#  include <libloaderapi.h>
#else
#  include <dlfcn.h>
#  if defined(__linux__)
#    include <unistd.h>
#  elif defined(__APPLE__)
#    include <mach-o/dyld.h>
#    include <climits>
#  elif defined(__FreeBSD__)
#    include <sys/types.h>
#    include <sys/sysctl.h>
#    include <climits>
#  endif
#endif

export module FilterPlugin;

/*
 * FilterPlugin — host-side loader for a filter plugin DLL.
 *
 * Mirrors the structure of PluginManager but manages a single plugin
 * instead of a keyed map.  The plugin exposes three C symbols:
 *   FilterImage            — perform conversion / dithering
 *   FreeFilterImageSet     — release memory returned by FilterImage
 *   FilterPluginGetLastError — retrieve the last error string (optional)
 *
 * Usage:
 *   FilterPlugin fp;
 *   fp.Load("libFilterPixman.so");           // throws on failure
 *   IWFilterImageSet* set = fp.Filter(...);
 *   fp.Free(set);
 */
export class FilterPlugin {
public:

#ifdef _WIN32
    using Handle = HMODULE;
#else
    using Handle = void*;
#endif

private:
    Handle                       handle_     = nullptr;
    FilterImageFunc              filterFunc_ = nullptr;
    FreeFilterImageSetFunc       freeFunc_   = nullptr;
    FilterPluginGetLastErrorFunc errorFunc_  = nullptr;

    /* ── Platform DLL helpers ──────────────────────────────────────────── */

    static Handle OpenLib(const std::filesystem::path& p) noexcept
    {
#ifdef _WIN32
        return LoadLibraryW(p.c_str());
#else
        return dlopen(p.c_str(), RTLD_LAZY);
#endif
    }

    static void* Sym(Handle h, const char* name) noexcept
    {
#ifdef _WIN32
        return reinterpret_cast<void*>(GetProcAddress(h, name));
#else
        return dlsym(h, name);
#endif
    }

    static void CloseLib(Handle h) noexcept
    {
#ifdef _WIN32
        FreeLibrary(h);
#else
        dlclose(h);
#endif
    }

    /* Locate the running executable's directory (Linux / macOS / FreeBSD). */
    static std::filesystem::path ExeDir() noexcept
    {
#if defined(_WIN32)
        wchar_t buf[MAX_PATH];
        DWORD len = GetModuleFileNameW(nullptr, buf, MAX_PATH);
        if (len > 0)
            return std::filesystem::path(buf).parent_path();
        return {};
#elif defined(__linux__)
        try {
            return std::filesystem::canonical("/proc/self/exe").parent_path();
        } catch (...) { return {}; }
#elif defined(__APPLE__)
        char buf[PATH_MAX];
        uint32_t sz = sizeof(buf);
        if (_NSGetExecutablePath(buf, &sz) == 0) {
            try {
                return std::filesystem::canonical(buf).parent_path();
            } catch (...) {}
        }
        return {};
#elif defined(__FreeBSD__)
        char buf[PATH_MAX];
        size_t sz = sizeof(buf);
        int mib[4] = {CTL_KERN, KERN_PROC, KERN_PROC_PATHNAME, -1};
        if (sysctl(mib, 4, buf, &sz, nullptr, 0) == 0) {
            try {
                return std::filesystem::canonical(buf).parent_path();
            } catch (...) {}
        }
        return {};
#else
        return {};
#endif
    }

    Handle FindAndLoad(const std::filesystem::path& pluginPath) noexcept
    {
        /* Try as-is first (absolute path, or found via LD_LIBRARY_PATH). */
        Handle h = OpenLib(pluginPath);
        if (h) return h;

        /* Try exe-directory / filename. */
        auto alt = ExeDir() / pluginPath.filename();
        return OpenLib(alt);
    }

public:
    FilterPlugin() noexcept = default;

    ~FilterPlugin() noexcept {
        if (handle_) CloseLib(handle_);
    }

    FilterPlugin(const FilterPlugin&)            = delete;
    FilterPlugin& operator=(const FilterPlugin&) = delete;

    FilterPlugin(FilterPlugin&& o) noexcept
        : handle_(o.handle_),
          filterFunc_(o.filterFunc_),
          freeFunc_(o.freeFunc_),
          errorFunc_(o.errorFunc_)
    {
        o.handle_     = nullptr;
        o.filterFunc_ = nullptr;
        o.freeFunc_   = nullptr;
        o.errorFunc_  = nullptr;
    }

    FilterPlugin& operator=(FilterPlugin&& o) noexcept
    {
        if (this != &o) {
            if (handle_) CloseLib(handle_);
            handle_     = o.handle_;
            filterFunc_ = o.filterFunc_;
            freeFunc_   = o.freeFunc_;
            errorFunc_  = o.errorFunc_;
            o.handle_     = nullptr;
            o.filterFunc_ = nullptr;
            o.freeFunc_   = nullptr;
            o.errorFunc_  = nullptr;
        }
        return *this;
    }

    bool IsLoaded() const noexcept { return handle_ != nullptr; }

    /* Load plugin from path; throws std::runtime_error on failure. */
    void Load(const std::filesystem::path& path)
    {
        if (handle_) { CloseLib(handle_); handle_ = nullptr; }

        Handle h = FindAndLoad(path);
        if (!h)
            throw std::runtime_error(
                "FilterPlugin: failed to open \"" + path.string() + "\"");

        filterFunc_ = reinterpret_cast<FilterImageFunc>(Sym(h, "FilterImage"));
        freeFunc_   = reinterpret_cast<FreeFilterImageSetFunc>(
            Sym(h, "FreeFilterImageSet"));
        errorFunc_  = reinterpret_cast<FilterPluginGetLastErrorFunc>(
            Sym(h, "FilterPluginGetLastError"));

        if (!filterFunc_ || !freeFunc_) {
            CloseLib(h);
            throw std::runtime_error(
                "FilterPlugin: missing required exports in \"" +
                path.string() + "\"");
        }
        handle_ = h;
    }

    /* Return the platform-appropriate filename for the default pixman plugin. */
    static std::filesystem::path DefaultPluginPath() noexcept
    {
#ifdef _WIN32
#  ifdef __MINGW32__
        return "libFilterPixman.dll";
#  else
        return "FilterPixman.dll";
#  endif
#elif defined(__APPLE__)
        return "libFilterPixman.dylib";
#else
        return "libFilterPixman.so";
#endif
    }

    /*
     * Call FilterImage in the loaded plugin.
     * Returns nullptr on allocation failure; use GetLastError() for details.
     * Throws if no plugin is loaded.
     */
    IWFilterImageSet* Filter(const ImagePluginData* src,
                              const IWScreenInfo*    screen,
                              const IWFilterOptions* opts)
    {
        if (!filterFunc_)
            throw std::runtime_error("FilterPlugin: plugin not loaded");
        return filterFunc_(src, screen, opts);
    }

    /* Release a set returned by Filter(). Safe to call with nullptr. */
    void Free(IWFilterImageSet* set) noexcept
    {
        if (freeFunc_ && set) freeFunc_(set);
    }

    /* Returns the last error string from the plugin, or nullptr. */
    const char* GetLastError() const noexcept
    {
        return errorFunc_ ? errorFunc_() : nullptr;
    }
};
