#!/usr/bin/env bash
#
# Compile the exported .iwi and plugin headers as C11, and fail if they have
# regressed to C++-only declarations.
#
# Why this exists
# ---------------
# ImagePluginDef.h and IwiFile.h are C headers.  Tension binds the format
# through FFI and a C game engine would consume it the same way, so a C
# compiler accepting them is the actual contract - not a nicety.  When five
# `enum X : uint8_t` declarations appeared in ImagePluginDef.h the whole project
# still built, because the project is C++ and nothing was compiling the header
# as C.  Those declarations are C23; the consumers are C11.
#
# The translation unit is generated into a temporary directory, never into the
# repository: the project keeps a "no .c source files" rule and this gate is not
# an exception to it.
#
# The authoritative check, including _Static_asserts that pin every C-side
# constant, is the `iwi_c_header_gate` ctest test defined in
# src/tests/CMakeLists.txt (it branches on the compiler so it works with MSVC's
# /TC too).  This script is a thin wrapper so CI and a developer can run the
# gate alone, without configuring the project.
#
# Usage:
#   ci/check-iwi-c-header.sh [build-dir]
#
# Environment:
#   CC        compiler to use (default: clang, then cc)
#   CMAKE     cmake to use (default: cmake)

set -euo pipefail

root=$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)
build_dir=${1:-"${root}/build-c-gate"}
cmake_bin=${CMAKE:-cmake}

if ! command -v "${cmake_bin}" >/dev/null 2>&1; then
    echo "check-iwi-c-header: cmake not found (set CMAKE=...)" >&2
    exit 2
fi

echo "check-iwi-c-header: configuring ${build_dir}"
"${cmake_bin}" -S "${root}" -B "${build_dir}" -G Ninja >/dev/null

echo "check-iwi-c-header: building target iwi_c_header_gate"
if ! "${cmake_bin}" --build "${build_dir}" --target iwi_c_header_gate; then
    cat >&2 <<'EOF'

The exported headers no longer compile as C11.

Most likely causes:
  - an `enum Name : type` declaration (C23, not C11) outside an
    `#ifdef __cplusplus` guard
  - a signature mentioning a C++ type (std::, a class, a template)
  - a construct only legal in C++ such as default arguments or `//`-only
    features used in a C-parseable position

The header is a C contract.  Guard the C++ form or use the typedef +
#define form the C path already uses.
EOF
    exit 1
fi

echo "check-iwi-c-header: OK - the exported headers compile as C11"
