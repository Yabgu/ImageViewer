#!/bin/bash

cmake -S . -B build-mingw32 -G Ninja \
    -DCMAKE_SYSTEM_NAME=Windows \
    -DVCPKG_TARGET_TRIPLET=x64-mingw-dynamic \
    -DVCPKG_HOST_TRIPLET=x64-linux \
    -DZ_VCPKG_POWERSHELL_PATH=pwsh \
    -DCMAKE_C_COMPILER=x86_64-w64-mingw32-gcc \
    -DCMAKE_CXX_COMPILER=x86_64-w64-mingw32-g++ \
    -DCMAKE_RC_COMPILER=x86_64-w64-mingw32-windres \
    -DCMAKE_BUILD_TYPE=Release \
    -DCMAKE_TOOLCHAIN_FILE="$GITHUB_WORKSPACE/third-party/vcpkg/scripts/buildsystems/vcpkg.cmake"
