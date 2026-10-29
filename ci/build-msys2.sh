#!/bin/bash
# ------------------------------------------------------------------------------
# build-msys2.sh — build and test ImageViewer for Windows with the real MSYS2
# toolchain, executed through LinSYS2 (MSYS2 packages running under Wine).
#
# This is the replacement for prepare_mingw32.sh, which used llvm-mingw (a Linux
# port of the MinGW toolchain) plus vcpkg.  Here the compiler, CMake, Ninja and
# all libraries are the actual Windows MSYS2 packages, so the build behaves the
# same way it does on Windows.
#
# Requirements (provided by ci/Containerfile.linsys2):
#   * linsys2 + a prepared clang64 environment
#   * GLAD_INCLUDE_DIR / GLAD_LIBRARY exported by the image, because MSYS2 has
#     no glad package and FindGlad's generate-at-configure fallback cannot run
#     under Wine (Windows Python's ssl module fails to initialise).
#
# Tests are executed, not just built: the binaries are Windows PE files and run
# under Wine via `linsys2 run`.
# ------------------------------------------------------------------------------
set -euo pipefail

if [ -z "${GITHUB_WORKSPACE:-}" ]; then
    GITHUB_WORKSPACE=$(dirname "$(realpath "$0")")/..
fi
GITHUB_WORKSPACE=$(realpath "$GITHUB_WORKSPACE")

ENV_NAME="${LINSYS2_ENV:-clang64}"
BUILD_DIR_REL="build-msys2"

# The environment's Windows-visible path.  MSYS2 tools normally learn this from
# the MSYS2 shell profile; under `linsys2 run` there is no such profile, so it
# is passed to CMake explicitly.  Without it pkg-config and CMake's default
# search path look in the wrong roots.
ENV_ROOT="$HOME/.local/share/linsys2/${ENV_NAME}/${ENV_NAME}"
ENV_WIN=$(WINEPREFIX="$HOME/.local/share/linsys2/${ENV_NAME}/wine" \
          winepath -w "$ENV_ROOT")

if [ -z "${GLAD_INCLUDE_DIR:-}" ] || [ -z "${GLAD_LIBRARY:-}" ]; then
    echo "Error: GLAD_INCLUDE_DIR and GLAD_LIBRARY must be set." >&2
    echo "       They are baked into ci/Containerfile.linsys2." >&2
    exit 1
fi

cd "$GITHUB_WORKSPACE"

echo "== configure =="
linsys2 run --env "$ENV_NAME" -- cmake -S . -B "$BUILD_DIR_REL" -G Ninja \
    -DIMAGEVIEWER_USE_VCPKG=OFF \
    -DIMAGEVIEWER_BUILD_VIEWER=ON \
    -DCMAKE_BUILD_TYPE=Release \
    -DCMAKE_PREFIX_PATH="$ENV_WIN" \
    -DGLAD_INCLUDE_DIR="$GLAD_INCLUDE_DIR" \
    -DGLAD_LIBRARY="$GLAD_LIBRARY" \
    -DIMAGEVIEWER_COPY_RUNTIME_DLLS=OFF

echo "== build =="
linsys2 run --env "$ENV_NAME" -- cmake --build "$BUILD_DIR_REL" -j "$(nproc)"

# Running the PE binaries through `linsys2 run` puts the environment's bin
# directory on WINEPATH, so the DLLs next to neither the exe nor the build tree
# need to exist.  That is why the build above disables the post-build DLL copy.
echo "== test (iview_tests) =="
linsys2 run --env "$ENV_NAME" -- "$BUILD_DIR_REL/src/tests/iview_tests.exe"

echo "== test (iview_abi_tests) =="
linsys2 run --env "$ENV_NAME" -- "$BUILD_DIR_REL/src/tests/abi/iview_abi_tests.exe"

# iview_format_tests needs the gradient fixtures.  They come from a submodule,
# so run it only when they are actually present rather than failing the job on
# a missing checkout.
if [ -d "third-party/gradient/test_sequences" ]; then
    echo "== test (iview_format_tests) =="
    linsys2 run --env "$ENV_NAME" -- \
        "$BUILD_DIR_REL/src/tests/format/iview_format_tests.exe" --no-intro
else
    echo "== test (iview_format_tests) SKIPPED: third-party/gradient not checked out =="
fi
