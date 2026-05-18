module;

#include <algorithm>
#include <array>
#include <bit>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iterator>
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

export module WasmPluginManager;

namespace {

typedef char wasm_byte_t;

typedef struct wasm_engine_t wasm_engine_t;
typedef struct wasm_store_t wasm_store_t;
typedef struct wasm_module_t wasm_module_t;
typedef struct wasm_instance_t wasm_instance_t;
typedef struct wasm_extern_t wasm_extern_t;
typedef struct wasm_func_t wasm_func_t;
typedef struct wasm_memory_t wasm_memory_t;
typedef struct wasm_trap_t wasm_trap_t;
typedef struct wasm_exporttype_t wasm_exporttype_t;

typedef struct wasm_byte_vec_t {
    size_t size;
    wasm_byte_t* data;
} wasm_byte_vec_t;
typedef wasm_byte_vec_t wasm_name_t;

typedef struct wasm_extern_vec_t {
    size_t size;
    wasm_extern_t** data;
} wasm_extern_vec_t;

typedef struct wasm_exporttype_vec_t {
    size_t size;
    wasm_exporttype_t** data;
} wasm_exporttype_vec_t;

typedef uint8_t wasm_valkind_t;
enum wasm_valkind_enum : uint8_t {
    WASM_I32 = 0
};

typedef struct wasm_val_t {
    wasm_valkind_t kind;
    union {
        int32_t i32;
        int64_t i64;
        float f32;
        double f64;
        void* ref;
    } of;
} wasm_val_t;

typedef struct wasm_val_vec_t {
    size_t size;
    wasm_val_t* data;
} wasm_val_vec_t;

typedef uint8_t wasm_externkind_t;
enum wasm_externkind_enum : uint8_t {
    WASM_EXTERN_FUNC = 0,
    WASM_EXTERN_GLOBAL = 1,
    WASM_EXTERN_TABLE = 2,
    WASM_EXTERN_MEMORY = 3
};

struct WasmApi {
    using engine_new_fn = wasm_engine_t* (*)();
    using engine_delete_fn = void (*)(wasm_engine_t*);
    using store_new_fn = wasm_store_t* (*)(wasm_engine_t*);
    using store_delete_fn = void (*)(wasm_store_t*);
    using byte_vec_new_uninitialized_fn = void (*)(wasm_byte_vec_t*, size_t);
    using byte_vec_delete_fn = void (*)(wasm_byte_vec_t*);
    using module_new_fn = wasm_module_t* (*)(wasm_store_t*, const wasm_byte_vec_t*);
    using module_delete_fn = void (*)(wasm_module_t*);
    using module_exports_fn = void (*)(const wasm_module_t*, wasm_exporttype_vec_t*);
    using exporttype_vec_delete_fn = void (*)(wasm_exporttype_vec_t*);
    using exporttype_name_fn = const wasm_name_t* (*)(const wasm_exporttype_t*);
    using instance_new_fn = wasm_instance_t* (*)(wasm_store_t*, const wasm_module_t*, const wasm_extern_vec_t*, wasm_trap_t**);
    using instance_delete_fn = void (*)(wasm_instance_t*);
    using instance_exports_fn = void (*)(const wasm_instance_t*, wasm_extern_vec_t*);
    using extern_vec_delete_fn = void (*)(wasm_extern_vec_t*);
    using extern_kind_fn = wasm_externkind_t (*)(const wasm_extern_t*);
    using extern_as_func_fn = wasm_func_t* (*)(wasm_extern_t*);
    using extern_as_memory_fn = wasm_memory_t* (*)(wasm_extern_t*);
    using func_call_fn = wasm_trap_t* (*)(const wasm_func_t*, const wasm_val_vec_t*, wasm_val_vec_t*);
    using trap_delete_fn = void (*)(wasm_trap_t*);
    using trap_message_fn = void (*)(const wasm_trap_t*, wasm_name_t*);
    using memory_data_fn = wasm_byte_t* (*)(wasm_memory_t*);
    using memory_data_size_fn = size_t (*)(const wasm_memory_t*);

    engine_new_fn engine_new = nullptr;
    engine_delete_fn engine_delete = nullptr;
    store_new_fn store_new = nullptr;
    store_delete_fn store_delete = nullptr;
    byte_vec_new_uninitialized_fn byte_vec_new_uninitialized = nullptr;
    byte_vec_delete_fn byte_vec_delete = nullptr;
    module_new_fn module_new = nullptr;
    module_delete_fn module_delete = nullptr;
    module_exports_fn module_exports = nullptr;
    exporttype_vec_delete_fn exporttype_vec_delete = nullptr;
    exporttype_name_fn exporttype_name = nullptr;
    instance_new_fn instance_new = nullptr;
    instance_delete_fn instance_delete = nullptr;
    instance_exports_fn instance_exports = nullptr;
    extern_vec_delete_fn extern_vec_delete = nullptr;
    extern_kind_fn extern_kind = nullptr;
    extern_as_func_fn extern_as_func = nullptr;
    extern_as_memory_fn extern_as_memory = nullptr;
    func_call_fn func_call = nullptr;
    trap_delete_fn trap_delete = nullptr;
    trap_message_fn trap_message = nullptr;
    memory_data_fn memory_data = nullptr;
    memory_data_size_fn memory_data_size = nullptr;
};

class WasmRuntime {
public:
#ifdef _WIN32
    using LibHandle = HMODULE;
#else
    using LibHandle = void*;
#endif

    WasmRuntime() = default;

    ~WasmRuntime() {
#ifdef _WIN32
        if (handle_) FreeLibrary(handle_);
#else
        if (handle_) dlclose(handle_);
#endif
    }

    static std::shared_ptr<WasmRuntime> Load() {
        auto runtime = std::make_shared<WasmRuntime>();
        runtime->openLibrary();
        runtime->loadSymbols();
        return runtime;
    }

    const WasmApi& api() const noexcept { return api_; }

private:
    LibHandle handle_ = nullptr;
    WasmApi api_;

    static std::filesystem::path exeDir() noexcept {
#ifdef _WIN32
        wchar_t buf[MAX_PATH];
        DWORD len = GetModuleFileNameW(nullptr, buf, MAX_PATH);
        if (len > 0) return std::filesystem::path(buf).parent_path();
        return {};
#elif defined(__linux__)
        try {
            return std::filesystem::canonical("/proc/self/exe").parent_path();
        } catch (...) { return {}; }
#elif defined(__APPLE__)
        char buf[PATH_MAX];
        uint32_t sz = sizeof(buf);
        if (_NSGetExecutablePath(buf, &sz) == 0) {
            try { return std::filesystem::canonical(buf).parent_path(); } catch (...) {}
        }
        return {};
#elif defined(__FreeBSD__)
        char buf[PATH_MAX];
        size_t sz = sizeof(buf);
        int mib[4] = {CTL_KERN, KERN_PROC, KERN_PROC_PATHNAME, -1};
        if (sysctl(mib, 4, buf, &sz, nullptr, 0) == 0) {
            try { return std::filesystem::canonical(buf).parent_path(); } catch (...) {}
        }
        return {};
#else
        return {};
#endif
    }

    void openLibrary() {
        std::vector<std::filesystem::path> names;
#ifdef _WIN32
        names.emplace_back("wasmtime.dll");
#elif defined(__APPLE__)
        names.emplace_back("libwasmtime.dylib");
#else
        names.emplace_back("libwasmtime.so");
        names.emplace_back("libwasmtime.so.0");
#endif

        std::vector<std::filesystem::path> candidates = names;
        const auto base = exeDir();
        if (!base.empty()) {
            for (const auto& name : names) {
                candidates.emplace_back(base / name);
            }
        }

        for (const auto& candidate : candidates) {
#ifdef _WIN32
            handle_ = LoadLibraryW(candidate.c_str());
#else
            handle_ = dlopen(candidate.c_str(), RTLD_LAZY);
#endif
            if (handle_) break;
        }
        if (!handle_) throw std::runtime_error("Failed to load Wasmtime runtime library");
    }

    template <typename Fn>
    Fn loadSymbol(const char* symbol) {
#ifdef _WIN32
        auto* raw = GetProcAddress(handle_, symbol);
#else
        auto* raw = dlsym(handle_, symbol);
#endif
        if (!raw) throw std::runtime_error(std::string("Missing Wasmtime symbol: ") + symbol);
        return reinterpret_cast<Fn>(raw);
    }

    void loadSymbols() {
        api_.engine_new = loadSymbol<WasmApi::engine_new_fn>("wasm_engine_new");
        api_.engine_delete = loadSymbol<WasmApi::engine_delete_fn>("wasm_engine_delete");
        api_.store_new = loadSymbol<WasmApi::store_new_fn>("wasm_store_new");
        api_.store_delete = loadSymbol<WasmApi::store_delete_fn>("wasm_store_delete");
        api_.byte_vec_new_uninitialized = loadSymbol<WasmApi::byte_vec_new_uninitialized_fn>("wasm_byte_vec_new_uninitialized");
        api_.byte_vec_delete = loadSymbol<WasmApi::byte_vec_delete_fn>("wasm_byte_vec_delete");
        api_.module_new = loadSymbol<WasmApi::module_new_fn>("wasm_module_new");
        api_.module_delete = loadSymbol<WasmApi::module_delete_fn>("wasm_module_delete");
        api_.module_exports = loadSymbol<WasmApi::module_exports_fn>("wasm_module_exports");
        api_.exporttype_vec_delete = loadSymbol<WasmApi::exporttype_vec_delete_fn>("wasm_exporttype_vec_delete");
        api_.exporttype_name = loadSymbol<WasmApi::exporttype_name_fn>("wasm_exporttype_name");
        api_.instance_new = loadSymbol<WasmApi::instance_new_fn>("wasm_instance_new");
        api_.instance_delete = loadSymbol<WasmApi::instance_delete_fn>("wasm_instance_delete");
        api_.instance_exports = loadSymbol<WasmApi::instance_exports_fn>("wasm_instance_exports");
        api_.extern_vec_delete = loadSymbol<WasmApi::extern_vec_delete_fn>("wasm_extern_vec_delete");
        api_.extern_kind = loadSymbol<WasmApi::extern_kind_fn>("wasm_extern_kind");
        api_.extern_as_func = loadSymbol<WasmApi::extern_as_func_fn>("wasm_extern_as_func");
        api_.extern_as_memory = loadSymbol<WasmApi::extern_as_memory_fn>("wasm_extern_as_memory");
        api_.func_call = loadSymbol<WasmApi::func_call_fn>("wasm_func_call");
        api_.trap_delete = loadSymbol<WasmApi::trap_delete_fn>("wasm_trap_delete");
        api_.trap_message = loadSymbol<WasmApi::trap_message_fn>("wasm_trap_message");
        api_.memory_data = loadSymbol<WasmApi::memory_data_fn>("wasm_memory_data");
        api_.memory_data_size = loadSymbol<WasmApi::memory_data_size_fn>("wasm_memory_data_size");
    }
};

inline uint16_t read_u16_le(const uint8_t* p) noexcept {
    return static_cast<uint16_t>(p[0] | (static_cast<uint16_t>(p[1]) << 8));
}

inline uint32_t read_u32_le(const uint8_t* p) noexcept {
    return static_cast<uint32_t>(p[0])
         | (static_cast<uint32_t>(p[1]) << 8)
         | (static_cast<uint32_t>(p[2]) << 16)
         | (static_cast<uint32_t>(p[3]) << 24);
}

inline int32_t read_i32_le(const uint8_t* p) noexcept {
    return static_cast<int32_t>(read_u32_le(p));
}

} // namespace

export class WasmImagePlugin {
public:
    explicit WasmImagePlugin(std::shared_ptr<WasmRuntime> runtime, const std::filesystem::path& path)
        : runtime_(std::move(runtime)) {
        init(path);
    }

    ~WasmImagePlugin() {
        const auto& api = runtime_->api();
        if (instance_) api.instance_delete(instance_);
        if (module_) api.module_delete(module_);
        if (store_) api.store_delete(store_);
        if (engine_) api.engine_delete(engine_);
    }

    ImagePluginResult LoadImageFromFile(const std::filesystem::path& imagePath) {
        if (std::endian::native != std::endian::little) {
            throw std::runtime_error("Wasm image plugin host requires little-endian system");
        }

        const auto pathU8 = imagePath.u8string();
        const std::string pathUtf8(
            reinterpret_cast<const char*>(pathU8.data()),
            pathU8.size());
        const int32_t pathLen = static_cast<int32_t>(pathUtf8.size());
        const int32_t allocSize = pathLen + 1;
        const int32_t pathPtr = call_i32_1(allocFunc_, allocSize);
        if (pathPtr <= 0) {
            throw std::runtime_error("Wasm plugin allocation failed for path buffer");
        }

        write_memory(pathPtr, reinterpret_cast<const uint8_t*>(pathUtf8.data()), static_cast<size_t>(pathLen));
        uint8_t zero = 0;
        write_memory(pathPtr + pathLen, &zero, 1);

        int32_t resultPtr = 0;
        try {
            resultPtr = call_i32_2(loadFunc_, pathPtr, pathLen);
            call_void_2(freeFunc_, pathPtr, allocSize);
        } catch (...) {
            try { call_void_2(freeFunc_, pathPtr, allocSize); } catch (...) {}
            throw;
        }

        if (resultPtr <= 0) {
            throw std::runtime_error("Wasm plugin returned null/invalid result pointer");
        }

        auto result = read_and_copy_result(resultPtr);
        call_void_1(freeResultFunc_, resultPtr);
        return result;
    }

    void FreeImageData(ImagePluginData* data) noexcept {
        if (!data) return;
        delete[] data->data;
        delete data;
    }

private:
    std::shared_ptr<WasmRuntime> runtime_;
    wasm_engine_t* engine_ = nullptr;
    wasm_store_t* store_ = nullptr;
    wasm_module_t* module_ = nullptr;
    wasm_instance_t* instance_ = nullptr;
    wasm_memory_t* memory_ = nullptr;
    wasm_func_t* allocFunc_ = nullptr;
    wasm_func_t* freeFunc_ = nullptr;
    wasm_func_t* loadFunc_ = nullptr;
    wasm_func_t* freeResultFunc_ = nullptr;

    void init(const std::filesystem::path& path) {
        const auto& api = runtime_->api();
        engine_ = api.engine_new();
        if (!engine_) throw std::runtime_error("Failed to create Wasm engine");
        store_ = api.store_new(engine_);
        if (!store_) throw std::runtime_error("Failed to create Wasm store");

        std::ifstream f(path, std::ios::binary);
        if (!f) throw std::runtime_error("Failed to open wasm plugin: " + path.string());
        std::vector<char> bytes((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
        if (bytes.empty()) throw std::runtime_error("Wasm plugin is empty: " + path.string());

        wasm_byte_vec_t moduleBytes{};
        api.byte_vec_new_uninitialized(&moduleBytes, bytes.size());
        std::memcpy(moduleBytes.data, bytes.data(), bytes.size());
        module_ = api.module_new(store_, &moduleBytes);
        api.byte_vec_delete(&moduleBytes);
        if (!module_) throw std::runtime_error("Failed to compile wasm plugin module: " + path.string());

        wasm_extern_vec_t imports{};
        wasm_trap_t* trap = nullptr;
        instance_ = api.instance_new(store_, module_, &imports, &trap);
        if (!instance_) {
            std::string trapMsg = "Failed to instantiate wasm plugin";
            if (trap) {
                wasm_name_t msg{};
                api.trap_message(trap, &msg);
                trapMsg.assign(msg.data, msg.size);
                api.byte_vec_delete(&msg);
                api.trap_delete(trap);
            }
            throw std::runtime_error(trapMsg);
        }
        resolve_exports();
    }

    void resolve_exports() {
        const auto& api = runtime_->api();
        wasm_exporttype_vec_t moduleExportTypes{};
        wasm_extern_vec_t instanceExports{};
        api.module_exports(module_, &moduleExportTypes);
        api.instance_exports(instance_, &instanceExports);

        if (moduleExportTypes.size != instanceExports.size) {
            api.exporttype_vec_delete(&moduleExportTypes);
            api.extern_vec_delete(&instanceExports);
            throw std::runtime_error("Wasm module export metadata mismatch");
        }

        for (size_t i = 0; i < moduleExportTypes.size; ++i) {
            const wasm_name_t* name = api.exporttype_name(moduleExportTypes.data[i]);
            const std::string exportName(name && name->data ? std::string(name->data, name->size) : std::string{});
            wasm_extern_t* ext = instanceExports.data[i];

            const auto kind = api.extern_kind(ext);
            if (kind == WASM_EXTERN_MEMORY && exportName == "memory") {
                memory_ = api.extern_as_memory(ext);
            } else if (kind == WASM_EXTERN_FUNC) {
                wasm_func_t* fn = api.extern_as_func(ext);
                if (exportName == "iw_alloc") allocFunc_ = fn;
                else if (exportName == "iw_free") freeFunc_ = fn;
                else if (exportName == "iw_load_image_from_file") loadFunc_ = fn;
                else if (exportName == "iw_free_image_result") freeResultFunc_ = fn;
            }
        }

        api.exporttype_vec_delete(&moduleExportTypes);
        api.extern_vec_delete(&instanceExports);

        if (!memory_ || !allocFunc_ || !freeFunc_ || !loadFunc_ || !freeResultFunc_) {
            throw std::runtime_error(
                "Wasm plugin missing required exports: memory, iw_alloc, iw_free, "
                "iw_load_image_from_file, iw_free_image_result");
        }
    }

    uint8_t* memory_data(size_t* outSize = nullptr) const {
        const auto& api = runtime_->api();
        size_t sz = api.memory_data_size(memory_);
        if (outSize) *outSize = sz;
        return reinterpret_cast<uint8_t*>(api.memory_data(memory_));
    }

    void write_memory(int32_t ptr, const uint8_t* src, size_t n) const {
        size_t memSize = 0;
        uint8_t* mem = memory_data(&memSize);
        if (ptr < 0 || static_cast<size_t>(ptr) + n > memSize) {
            throw std::runtime_error("Wasm memory write out of bounds");
        }
        std::memcpy(mem + ptr, src, n);
    }

    ImagePluginResult read_and_copy_result(int32_t resultPtr) {
        constexpr size_t kResultSize = 132;
        constexpr size_t kFmtOffset = 28;
        constexpr size_t kComponentsOffset = 36;
        constexpr size_t kCompSize = 12;

        size_t memSize = 0;
        uint8_t* mem = memory_data(&memSize);
        if (resultPtr < 0 || static_cast<size_t>(resultPtr) + kResultSize > memSize) {
            throw std::runtime_error("Wasm image result struct out of bounds");
        }
        const uint8_t* base = mem + resultPtr;

        const uint32_t code = read_u32_le(base + 0);
        const int32_t width = read_i32_le(base + 4);
        const int32_t height = read_i32_le(base + 8);
        const int32_t stride = read_i32_le(base + 12);
        const uint32_t colorSpace = read_u32_le(base + 16);
        const uint32_t byteSize = read_u32_le(base + 20);
        const uint32_t dataPtr = read_u32_le(base + 24);

        if (static_cast<size_t>(dataPtr) + byteSize > memSize) {
            throw std::runtime_error("Wasm image pixel buffer out of bounds");
        }

        if (code != IMAGE_PLUGIN_OK) {
            ImagePluginResult ret{};
            ret.code = static_cast<ImagePluginResultCode>(code);
            ret.data = nullptr;
            return ret;
        }

        auto* out = new ImagePluginData{};
        out->width = width;
        out->height = height;
        out->stride = stride;
        out->colorSpace = static_cast<ImageColorSpace>(colorSpace);
        out->size = byteSize;
        out->data = new uint8_t[byteSize];
        std::memcpy(out->data, mem + dataPtr, byteSize);

        IWImageFormat fmt{};
        fmt.componentCount = read_u16_le(base + kFmtOffset + 0);
        fmt.bitsPerPixel = read_u16_le(base + kFmtOffset + 2);
        fmt.storageLayout = read_u32_le(base + kFmtOffset + 4);

        for (size_t i = 0; i < IW_MAX_COMPONENTS; ++i) {
            const size_t off = kComponentsOffset + i * kCompSize;
            fmt.components[i].semantic = read_u32_le(base + off + 0);
            fmt.components[i].componentClass = read_u32_le(base + off + 4);
            fmt.components[i].bitOffset = read_u16_le(base + off + 8);
            fmt.components[i].bitWidth = read_u16_le(base + off + 10);
        }
        out->format = fmt;

        ImagePluginResult ret{};
        ret.code = static_cast<ImagePluginResultCode>(code);
        ret.data = out;
        return ret;
    }

    int32_t call_i32_1(wasm_func_t* fn, int32_t a0) const {
        wasm_val_t argsBuf[1] = {};
        wasm_val_t resultsBuf[1] = {};
        argsBuf[0].kind = WASM_I32;
        argsBuf[0].of.i32 = a0;
        wasm_val_vec_t args{1, argsBuf};
        wasm_val_vec_t results{1, resultsBuf};
        auto* trap = runtime_->api().func_call(fn, &args, &results);
        if (trap) throw_trap(trap);
        return resultsBuf[0].of.i32;
    }

    int32_t call_i32_2(wasm_func_t* fn, int32_t a0, int32_t a1) const {
        wasm_val_t argsBuf[2] = {};
        wasm_val_t resultsBuf[1] = {};
        argsBuf[0].kind = WASM_I32; argsBuf[0].of.i32 = a0;
        argsBuf[1].kind = WASM_I32; argsBuf[1].of.i32 = a1;
        wasm_val_vec_t args{2, argsBuf};
        wasm_val_vec_t results{1, resultsBuf};
        auto* trap = runtime_->api().func_call(fn, &args, &results);
        if (trap) throw_trap(trap);
        return resultsBuf[0].of.i32;
    }

    void call_void_1(wasm_func_t* fn, int32_t a0) const {
        wasm_val_t argsBuf[1] = {};
        argsBuf[0].kind = WASM_I32;
        argsBuf[0].of.i32 = a0;
        wasm_val_vec_t args{1, argsBuf};
        wasm_val_vec_t results{0, nullptr};
        auto* trap = runtime_->api().func_call(fn, &args, &results);
        if (trap) throw_trap(trap);
    }

    void call_void_2(wasm_func_t* fn, int32_t a0, int32_t a1) const {
        wasm_val_t argsBuf[2] = {};
        argsBuf[0].kind = WASM_I32; argsBuf[0].of.i32 = a0;
        argsBuf[1].kind = WASM_I32; argsBuf[1].of.i32 = a1;
        wasm_val_vec_t args{2, argsBuf};
        wasm_val_vec_t results{0, nullptr};
        auto* trap = runtime_->api().func_call(fn, &args, &results);
        if (trap) throw_trap(trap);
    }

    [[noreturn]] void throw_trap(wasm_trap_t* trap) const {
        wasm_name_t msg{};
        runtime_->api().trap_message(trap, &msg);
        std::string message(msg.data ? std::string(msg.data, msg.size) : std::string("Wasm trap"));
        runtime_->api().byte_vec_delete(&msg);
        runtime_->api().trap_delete(trap);
        throw std::runtime_error(message);
    }
};

export class WasmPluginManager {
public:
    WasmPluginManager() = default;

    std::unique_ptr<WasmImagePlugin> load(const std::filesystem::path& pluginPath) {
        if (!runtime_) runtime_ = WasmRuntime::Load();
        auto resolved = resolve_plugin_path(pluginPath);
        return std::make_unique<WasmImagePlugin>(runtime_, resolved);
    }

private:
    std::shared_ptr<WasmRuntime> runtime_;

    static std::filesystem::path resolve_plugin_path(const std::filesystem::path& pluginPath) {
        if (std::filesystem::exists(pluginPath)) return pluginPath;

#if defined(_WIN32)
        wchar_t buf[MAX_PATH];
        DWORD len = GetModuleFileNameW(nullptr, buf, MAX_PATH);
        if (len > 0) {
            auto candidate = std::filesystem::path(buf).parent_path() / pluginPath.filename();
            if (std::filesystem::exists(candidate)) return candidate;
        }
#elif defined(__linux__)
        try {
            auto candidate = std::filesystem::canonical("/proc/self/exe").parent_path() / pluginPath.filename();
            if (std::filesystem::exists(candidate)) return candidate;
        } catch (...) {}
#elif defined(__APPLE__)
        char buf[PATH_MAX];
        uint32_t sz = sizeof(buf);
        if (_NSGetExecutablePath(buf, &sz) == 0) {
            try {
                auto candidate = std::filesystem::canonical(buf).parent_path() / pluginPath.filename();
                if (std::filesystem::exists(candidate)) return candidate;
            } catch (...) {}
        }
#elif defined(__FreeBSD__)
        char buf[PATH_MAX];
        size_t sz = sizeof(buf);
        int mib[4] = {CTL_KERN, KERN_PROC, KERN_PROC_PATHNAME, -1};
        if (sysctl(mib, 4, buf, &sz, nullptr, 0) == 0) {
            try {
                auto candidate = std::filesystem::canonical(buf).parent_path() / pluginPath.filename();
                if (std::filesystem::exists(candidate)) return candidate;
            } catch (...) {}
        }
#endif
        throw std::runtime_error("Wasm plugin not found: " + pluginPath.string());
    }
};
