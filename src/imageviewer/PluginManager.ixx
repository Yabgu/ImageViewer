module;
#include <filesystem>
#include <map>
#include <memory>
#include <string>
#include "ImagePluginDef.h"
#ifdef _WIN32
#include <libloaderapi.h>
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
     * A PluginEntry* would instead point into the map itself. */
    std::shared_ptr<PluginEntry> getPlugin(const std::filesystem::path& pluginPath) {
        const std::string key = pluginPath.string();

        auto it = plugins.find(key);
        if (it != plugins.end()) return it->second;

        PluginHandle handle = findAndLoadModule(pluginPath);
        if (!handle) return nullptr;

        auto entry = std::make_shared<PluginEntry>();
        if (!getLoadAndFreeProc(handle, *entry))
            return nullptr;

        plugins[key] = entry;
        return entry;
    }

private:
    PluginHandle findAndLoadModule(const std::filesystem::path& pluginPath) {
#ifdef _WIN32
        HMODULE hModule = LoadLibraryW(pluginPath.c_str());
        return hModule;
#else
        void* handle = dlopen(pluginPath.c_str(), RTLD_LAZY);
        if (!handle) {
            // Try loading from the current executable's directory
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
            handle = dlopen(altPath.c_str(), RTLD_LAZY);
        }
        return handle;
#endif
    }

    /* Fills entry on success.  On failure the module is unloaded again before
     * returning false, so nothing is left registered in plugins. */
    bool getLoadAndFreeProc(PluginHandle handle, PluginEntry& entry) {
        entry.handle = handle;
#ifdef _WIN32
        entry.loadFunc = (LoadImageFromFileFunc)GetProcAddress(handle, "LoadImageFromFile");
        if (!entry.loadFunc) {
            FreeLibrary(handle);
            return false;
        }
        entry.freeFunc = (FreeImageDataFunc)GetProcAddress(handle, "FreeImageData");
        if (!entry.freeFunc) {
            FreeLibrary(handle);
            return false;
        }
#else
        entry.loadFunc = (LoadImageFromFileFunc)dlsym(handle, "LoadImageFromFile");
        if (!entry.loadFunc) {
            dlclose(handle);
            return false;
        }
        entry.freeFunc = (FreeImageDataFunc)dlsym(handle, "FreeImageData");
        if (!entry.freeFunc) {
            dlclose(handle);
            return false;
        }
#endif
        return true;
    }
private:
    std::map<std::string, std::shared_ptr<PluginEntry>> plugins;
};
