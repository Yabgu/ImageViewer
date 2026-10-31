module;
#include <filesystem>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include "ImagePluginDef.h"
#ifdef _WIN32
#  ifndef WIN32_LEAN_AND_MEAN
#    define WIN32_LEAN_AND_MEAN
#  endif
#  ifndef NOMINMAX
#    define NOMINMAX
#  endif
#  include <windows.h>
#else
#include <dlfcn.h>
#endif

export module PluginManager;

export class PluginManager {
public:
#ifdef _WIN32
    using PluginHandle = HMODULE;
#else
    using PluginHandle = void*;
#endif
    struct PluginEntry {
        PluginHandle handle;
        LoadImageFromFileFunc loadFunc;
        FreeImageDataFunc freeFunc;
    };

    PluginManager() = default;
    ~PluginManager() {
        std::lock_guard<std::mutex> lock(mutex_);
        for (auto& [_, entry] : plugins) {
#ifdef _WIN32
            if (entry->handle) FreeLibrary(entry->handle);
#else
            if (entry->handle) dlclose(entry->handle);
#endif
        }
    }

    /* Returns a handle that stays valid for as long as the caller holds it,
     * independently of any later insertion into (or erasure from) the map.
     * A PluginEntry* would instead point into the map itself.
     *
     * Coarse lock: plugin loading is rare, and the map plus the OS loader state
     * must be serialized because Image::FromFile() can run on decode workers
     * concurrently with a drag/drop load on the main thread. */
    std::shared_ptr<PluginEntry> getPlugin(const std::filesystem::path& pluginPath) {
        std::lock_guard<std::mutex> lock(mutex_);
        lastError_.clear();

        const std::string key = pluginPath.string();
        auto it = plugins.find(key);
        if (it != plugins.end()) return it->second;

        PluginHandle handle = findAndLoadModule(pluginPath);
        if (!handle) {
            if (lastError_.empty()) lastError_ = "unknown loader failure";
            return nullptr;
        }

        auto entry = std::make_shared<PluginEntry>();
        if (!getLoadAndFreeProc(handle, *entry)) {
            if (lastError_.empty()) lastError_ = "plugin is missing required exports";
            return nullptr;
        }

        plugins[key] = entry;
        return entry;
    }

    /* Human-readable detail for the most recent getPlugin() failure.  This is
     * intentionally a copy so callers can throw/log it after the lock is gone. */
    std::string GetLastErrorText() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return lastError_;
    }

private:
    static std::string TrimTrailing(std::string text) {
        while (!text.empty()) {
            const char c = text.back();
            if (c != '\r' && c != '\n' && c != ' ' && c != '\t') break;
            text.pop_back();
        }
        return text;
    }

#ifdef _WIN32
    static std::string Win32ErrorText(DWORD code) {
        char* buffer = nullptr;
        const DWORD length = FormatMessageA(
            FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM |
                FORMAT_MESSAGE_IGNORE_INSERTS,
            nullptr, code, MAKELANGID(LANG_NEUTRAL, SUBLANG_DEFAULT),
            reinterpret_cast<LPSTR>(&buffer), 0, nullptr);
        std::string text = (length != 0 && buffer != nullptr)
            ? std::string(buffer, length)
            : std::string();
        if (buffer != nullptr) LocalFree(buffer);
        return TrimTrailing(text);
    }

    static std::string Win32Error(const std::string& operation, DWORD code) {
        const std::string detail = Win32ErrorText(code);
        std::string text = operation + " failed (Win32 error " +
                           std::to_string(code) + ")";
        if (!detail.empty()) text += ": " + detail;
        return text;
    }
#endif

    PluginHandle findAndLoadModule(const std::filesystem::path& pluginPath) {
#ifdef _WIN32
        SetLastError(0);
        HMODULE hModule = LoadLibraryW(pluginPath.c_str());
        if (!hModule)
        {
            const DWORD code = GetLastError();
            lastError_ = Win32Error("LoadLibraryW(\"" + pluginPath.string() + "\")", code);
        }
        return hModule;
#else
        dlerror();
        void* handle = dlopen(pluginPath.c_str(), RTLD_LAZY);
        if (handle) return handle;

        const char* firstError = dlerror();
        std::string firstText = firstError ? firstError : "unknown dlopen failure";

        // Try loading from the current executable's directory.
        std::filesystem::path exeDir;
#if defined(__linux__)
        exeDir = std::filesystem::canonical("/proc/self/exe").parent_path();
#elif defined(__APPLE__)
        {
            char exePath[PATH_MAX];
            uint32_t size = sizeof(exePath);
            if (_NSGetExecutablePath(exePath, &size) == 0) {
                exeDir = std::filesystem::canonical(exePath).parent_path();
            }
        }
#elif defined(__FreeBSD__)
        {
            char exePath[PATH_MAX];
            size_t size = sizeof(exePath);
            int mib[4] = {CTL_KERN, KERN_PROC, KERN_PROC_PATHNAME, -1};
            if (sysctl(mib, 4, exePath, &size, NULL, 0) == 0) {
                exeDir = std::filesystem::canonical(exePath).parent_path();
            }
        }
#endif
        auto altPath = exeDir / pluginPath.filename();
        dlerror();
        handle = dlopen(altPath.c_str(), RTLD_LAZY);
        if (handle) return handle;

        const char* secondError = dlerror();
        lastError_ = "dlopen(\"" + pluginPath.string() + "\") failed: " + firstText;
        if (secondError)
            lastError_ += "; dlopen(\"" + altPath.string() + "\") failed: " + secondError;
        return nullptr;
#endif
    }

    /* Fills entry on success.  On failure the module is unloaded again before
     * returning false, so nothing is left registered in plugins. */
    bool getLoadAndFreeProc(PluginHandle handle, PluginEntry& entry) {
        entry.handle = handle;
#ifdef _WIN32
        SetLastError(0);
        entry.loadFunc = (LoadImageFromFileFunc)GetProcAddress(handle, "LoadImageFromFile");
        if (!entry.loadFunc) {
            lastError_ = Win32Error("GetProcAddress(LoadImageFromFile)", GetLastError());
            FreeLibrary(handle);
            return false;
        }
        SetLastError(0);
        entry.freeFunc = (FreeImageDataFunc)GetProcAddress(handle, "FreeImageData");
        if (!entry.freeFunc) {
            lastError_ = Win32Error("GetProcAddress(FreeImageData)", GetLastError());
            FreeLibrary(handle);
            return false;
        }
#else
        dlerror();
        entry.loadFunc = (LoadImageFromFileFunc)dlsym(handle, "LoadImageFromFile");
        if (!entry.loadFunc) {
            const char* error = dlerror();
            lastError_ = std::string("dlsym(LoadImageFromFile) failed: ") +
                         (error ? error : "unknown");
            dlclose(handle);
            return false;
        }
        dlerror();
        entry.freeFunc = (FreeImageDataFunc)dlsym(handle, "FreeImageData");
        if (!entry.freeFunc) {
            const char* error = dlerror();
            lastError_ = std::string("dlsym(FreeImageData) failed: ") +
                         (error ? error : "unknown");
            dlclose(handle);
            return false;
        }
#endif
        return true;
    }
private:
    mutable std::mutex mutex_;
    std::string lastError_;
    std::map<std::string, std::shared_ptr<PluginEntry>> plugins;
};
