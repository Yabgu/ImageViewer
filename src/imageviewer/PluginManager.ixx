module;
#include <filesystem>
#include <map>
#include <memory>
#include <stdexcept>
#include <string>
#include <vector>
#include "ImagePluginDef.h"
#ifdef _WIN32
#include <libloaderapi.h>
#else
#include <dlfcn.h>
#if defined(__linux__)
#include <unistd.h>
#elif defined(__APPLE__)
#include <mach-o/dyld.h>
#include <climits>
#elif defined(__FreeBSD__)
#include <sys/types.h>
#include <sys/sysctl.h>
#include <climits>
#endif
#endif

export module PluginManager;

import WasmPluginManager;

export class PluginManager {
public:
#ifdef _WIN32
    using PluginHandle = HMODULE;
#else
    using PluginHandle = void*;
#endif

    PluginManager() = default;

    ~PluginManager() {
        for (auto& [_, entry] : plugins) {
#ifdef _WIN32
            if (entry.nativeHandle) FreeLibrary(entry.nativeHandle);
#else
            if (entry.nativeHandle) dlclose(entry.nativeHandle);
#endif
        }
    }

    ImagePluginResult LoadImage(const std::filesystem::path& pluginPath,
                                const std::filesystem::path& imagePath) {
        PluginEntry& entry = getOrCreatePlugin(pluginPath);
        if (entry.isWasm) {
            return entry.wasmPlugin->LoadImageFromFile(imagePath);
        }
        if (!entry.loadFunc) {
            throw std::runtime_error("Failed to resolve plugin load function: " + pluginPath.string());
        }
        return entry.loadFunc(imagePath.c_str());
    }

    void FreeImageData(const std::filesystem::path& pluginPath, ImagePluginData* data) {
        if (!data) return;
        PluginEntry& entry = getOrCreatePlugin(pluginPath);
        if (entry.isWasm) {
            entry.wasmPlugin->FreeImageData(data);
            return;
        }
        if (!entry.freeFunc) {
            throw std::runtime_error("Failed to resolve plugin free function: " + pluginPath.string());
        }
        entry.freeFunc(data);
    }

private:
    struct PluginEntry {
        LoadImageFromFileFunc loadFunc = nullptr;
        FreeImageDataFunc freeFunc = nullptr;
        PluginHandle nativeHandle = nullptr;
        bool isWasm = false;
        std::unique_ptr<WasmImagePlugin> wasmPlugin;
    };

    PluginEntry& getOrCreatePlugin(const std::filesystem::path& pluginPath) {
        const std::string key = pluginPath.string();
        auto it = plugins.find(key);
        if (it != plugins.end()) return it->second;

        PluginEntry entry;
        if (pluginPath.extension() == ".wasm") {
            entry.isWasm = true;
            entry.wasmPlugin = wasmPlugins.load(pluginPath);
            if (!entry.wasmPlugin) {
                throw std::runtime_error("Failed to load wasm image plugin: " + key);
            }
        } else {
            entry.nativeHandle = findAndLoadNativeModule(pluginPath);
            if (entry.nativeHandle) {
                resolveNativeEntryPoints(entry.nativeHandle, key, entry);
            } else {
                bool wasmLoaded = false;
                for (const auto& wasmCandidate : buildWasmFallbackCandidates(pluginPath)) {
                    try {
                        entry.isWasm = true;
                        entry.wasmPlugin = wasmPlugins.load(wasmCandidate);
                        wasmLoaded = true;
                        break;
                    } catch (...) {
                    }
                }
                if (!wasmLoaded) {
                    throw std::runtime_error("Failed to load native image plugin and wasm fallback: " + key);
                }
            }
        }
        auto [inserted, _] = plugins.emplace(key, std::move(entry));
        return inserted->second;
    }

    PluginHandle findAndLoadNativeModule(const std::filesystem::path& pluginPath) {
#ifdef _WIN32
        HMODULE hModule = LoadLibraryW(pluginPath.c_str());
        return hModule;
#else
        void* handle = dlopen(pluginPath.c_str(), RTLD_LAZY);
        if (!handle) {
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
                if (sysctl(mib, 4, exePath, &size, nullptr, 0) == 0) {
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

    static std::vector<std::filesystem::path> buildWasmFallbackCandidates(
        const std::filesystem::path& pluginPath) {
        std::vector<std::filesystem::path> out;
        const std::string stem = pluginPath.stem().string();
        out.emplace_back(pluginPath.parent_path() / (stem + ".wasm"));
        if (stem.rfind("lib", 0) == 0 && stem.size() > 3) {
            out.emplace_back(pluginPath.parent_path() / (stem.substr(3) + ".wasm"));
        }
        return out;
    }

    static void resolveNativeEntryPoints(PluginHandle handle,
                                         const std::string& key,
                                         PluginEntry& out) {
#ifdef _WIN32
        auto loadFunc = (LoadImageFromFileFunc)GetProcAddress(handle, "LoadImageFromFile");
        auto freeFunc = (FreeImageDataFunc)GetProcAddress(handle, "FreeImageData");
        if (!loadFunc || !freeFunc) {
            FreeLibrary(handle);
            throw std::runtime_error("Missing required exports in native plugin: " + key);
        }
#else
        auto loadFunc = (LoadImageFromFileFunc)dlsym(handle, "LoadImageFromFile");
        auto freeFunc = (FreeImageDataFunc)dlsym(handle, "FreeImageData");
        if (!loadFunc || !freeFunc) {
            dlclose(handle);
            throw std::runtime_error("Missing required exports in native plugin: " + key);
        }
#endif
        out.loadFunc = loadFunc;
        out.freeFunc = freeFunc;
    }

    std::map<std::string, PluginEntry> plugins;
    WasmPluginManager wasmPlugins;
};
