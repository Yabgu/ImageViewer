#!/bin/bash

set -e

if [ ! -d third-party/vcpkg ]; then
    echo "Cloning vcpkg..."
    mkdir -p third-party
    pushd third-party
    git clone https://github.com/microsoft/vcpkg
    pushd vcpkg
    ./bootstrap-vcpkg.sh --disableMetrics
    popd
    popd
else
    echo "vcpkg source already present in third-party/vcpkg"
fi

# Optional: clone Wasmtime (upstream) so users can build/use it if vcpkg doesn't provide it
pushd third-party
if [ ! -d wasmtime ]; then
	echo "Cloning Wasmtime (bytecodealliance/wasmtime)..."
	git clone https://github.com/bytecodealliance/wasmtime --recurse-submodules
else
	echo "Wasmtime source already present in third-party/wasmtime"
fi

# Try a best-effort build of the Wasmtime C API if cargo is available
if command -v cargo >/dev/null 2>&1; then
	echo "Cargo detected — attempting to build Wasmtime C API (release)..."
	pushd wasmtime
	# Try the C API package name; if that fails try a plain release build
	cargo build -p wasmtime-c-api --release
    cmake -S crates/c-api -B target/c-api --install-prefix "$(pwd)/artifacts"
    popd
else
	echo "Cargo not found — skipping automatic Wasmtime build. Install Rust/Cargo to enable building Wasmtime locally."
fi

