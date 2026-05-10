#!/bin/bash

set -e

if [ -z "$GITHUB_WORKSPACE" ]; then
    GITHUB_WORKSPACE=$(dirname "$(realpath "$0")")
fi

if [ ! -d "$GITHUB_WORKSPACE/third-party/vcpkg" ]; then
    "$GITHUB_WORKSPACE/prepare.sh"
fi

LLVM_MINGW_BIN=/opt/llvm-mingw/bin

if ! command -v ninja >/dev/null 2>&1; then
    echo "Error: Ninja is required but was not found in PATH." >&2
    exit 1
fi

if [ ! -x "$LLVM_MINGW_BIN/x86_64-w64-mingw32-clang" ]; then
    echo "Error: clang toolchain not found at $LLVM_MINGW_BIN" >&2
    exit 1
fi

# Ensure vcpkg ports and nested CMake invocations use clang, not gcc.
export PATH="$LLVM_MINGW_BIN:$PATH"


cmake -S "$GITHUB_WORKSPACE" -B "$GITHUB_WORKSPACE/build-mingw32" -G Ninja \
    -DCMAKE_SYSTEM_NAME=Windows \
    -DIMAGEVIEWER_USE_VCPKG=ON \
    -DVCPKG_TARGET_TRIPLET=x64-mingw-dynamic \
    -DVCPKG_HOST_TRIPLET=x64-linux \
    -DZ_VCPKG_POWERSHELL_PATH=pwsh \
    -DCMAKE_BUILD_TYPE=Release \
    -DCMAKE_PREFIX_PATH="$GITHUB_WORKSPACE/build-mingw32/vcpkg_installed/x64-mingw-dynamic" \
    -DVCPKG_CHAINLOAD_TOOLCHAIN_FILE="$GITHUB_WORKSPACE/cmake/toolchains/x86_64-w64-mingw32.cmake" \
    -DVCPKG_MANIFEST_FEATURES="viewer" \
    -DCMAKE_TOOLCHAIN_FILE="$GITHUB_WORKSPACE/third-party/vcpkg/scripts/buildsystems/vcpkg.cmake"
