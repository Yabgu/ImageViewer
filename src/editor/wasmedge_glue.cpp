// Minimal C++ glue to load a WASM module using the C++ Wasmtime wrapper
#include <wasmtime.hh>
#include <iostream>
#include <vector>
#include <fstream>

int main(int argc, char* argv[]) {
    if (argc < 2) {
        std::cerr << "Usage: " << argv[0] << " <module.wasm>" << std::endl;
        return 1;
    }

    // Read WASM file into memory using C++ streams
    std::ifstream in(argv[1], std::ios::binary | std::ios::ate);
    if (!in) {
        std::cerr << "Failed to open WASM file: " << argv[1] << std::endl;
        return 1;
    }
    std::streamsize size = in.tellg();
    if (size <= 0) {
        std::cerr << "Empty or invalid WASM file: " << argv[1] << std::endl;
        return 1;
    }
    in.seekg(0, std::ios::beg);
    std::vector<uint8_t> wasm(static_cast<size_t>(size));
    if (!in.read(reinterpret_cast<char*>(wasm.data()), size)) {
        std::cerr << "Failed to read WASM file: " << argv[1] << std::endl;
        return 1;
    }

    try {
        // Create engine and store (RAII)
        wasmtime::Engine engine{};
        wasmtime::Store store{engine};

        // Compile module from bytes
        auto module_res = wasmtime::Module::compile(engine, wasm);
        if (!module_res) {
            std::cerr << "Failed to compile module: " << module_res.err().message() << std::endl;
            return 1;
        }
        auto module = module_res.ok();

        // Instantiate the module (no imports provided)
        auto inst_res = wasmtime::Instance::create(store.context(), module, std::vector<wasmtime::Extern>{});
        if (!inst_res) {
            std::cerr << "Failed to instantiate module: " << inst_res.err().message() << std::endl;
            return 1;
        }
        auto instance = inst_res.ok();

        std::cout << "WASM module loaded successfully with Wasmtime (C++ API)." << std::endl;
    } catch (const std::exception& e) {
        std::cerr << "Wasmtime error: " << e.what() << std::endl;
        return 1;
    }

    return 0;
}
