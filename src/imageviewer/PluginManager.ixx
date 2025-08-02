module;
#include <filesystem>
#include <map>
#include <string>
#include "ImagePluginDef.h"
#ifdef _WIN32
#include <windows.h>
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
    };

    PluginManager() = default;
    ~PluginManager() {
        for (auto& [_, entry] : plugins) {
#ifdef _WIN32
            if (entry.handle) FreeLibrary(entry.handle);
#else
            if (entry.handle) dlclose(entry.handle);
#endif
        }
    }

    PluginEntry* getPlugin(const std::filesystem::path& pluginPath) {
        auto it = plugins.find(pluginPath.string());
        if (it != plugins.end()) return &it->second;
#ifdef _WIN32
        HMODULE hModule = LoadLibraryW(pluginPath.c_str());
        if (!hModule) return nullptr;
        auto loadFunc = (LoadImageFromFileFunc)GetProcAddress(hModule, "LoadImageFromFile");
        if (!loadFunc) {
            FreeLibrary(hModule);
            return nullptr;
        }
        plugins[pluginPath.string()] = { hModule, loadFunc };
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
            if (!handle) return nullptr;
        }
        auto loadFunc = (LoadImageFromFileFunc)dlsym(handle, "LoadImageFromFile");
        if (!loadFunc) {
            dlclose(handle);
            return nullptr;
        }
        plugins[pluginPath.string()] = { handle, loadFunc };
#endif
        return &plugins[pluginPath.string()];
    }
private:
    std::map<std::string, PluginEntry> plugins;
};
