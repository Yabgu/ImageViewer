// Minimal C++ glue to load and run a WASM module using WasmEdge
#include <wasmedge/wasmedge.h>
#include <iostream>
#include <vector>
#include <cstdio>

int main(int argc, char* argv[]) {
    if (argc < 2) {
        std::cerr << "Usage: " << argv[0] << " <module.wasm>" << std::endl;
        return 1;
    }
    // Create WasmEdge configuration context
    WasmEdge_ConfigureContext* Conf = WasmEdge_ConfigureCreate();
    WasmEdge_VMContext* VM = WasmEdge_VMCreate(Conf, nullptr);

    // Load WASM module from file
    FILE* file = fopen(argv[1], "rb");
    if (!file) {
        std::cerr << "Failed to open WASM file: " << argv[1] << std::endl;
        return 1;
    }
    fseek(file, 0, SEEK_END);
    size_t size = ftell(file);
    fseek(file, 0, SEEK_SET);
    std::vector<uint8_t> wasm(size);
    fread(wasm.data(), 1, size, file);
    fclose(file);

    WasmEdge_Result res = WasmEdge_VMRegisterModuleFromBuffer(VM, "mod", wasm.data(), wasm.size());
    if (!WasmEdge_ResultOK(res)) {
        std::cerr << "Failed to load module." << std::endl;
        WasmEdge_VMDelete(VM);
        WasmEdge_ConfigureDelete(Conf);
        return 1;
    }
    std::cout << "WASM module loaded successfully with WasmEdge." << std::endl;
    WasmEdge_VMDelete(VM);
    WasmEdge_ConfigureDelete(Conf);
    return 0;
}
