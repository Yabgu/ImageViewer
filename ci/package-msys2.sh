#!/bin/bash
# ------------------------------------------------------------------------------
# package-msys2.sh — assemble a runnable Windows bundle from a LinSYS2 build.
#
# iview.exe does not run on a bare Windows desktop by itself: the plugin DLLs
# sit next to it, and each of those imports MSYS2 runtime DLLs (libc++, libunwind,
# libwinpthread, and the library DLLs such as libpng16 / glfw3 / libjpeg-62).
#
# CMake normally copies those with IMAGEVIEWER_COPY_RUNTIME_DLLS, but the LinSYS2
# build disables it: with the Ninja generator CMake implements POST_BUILD by
# nesting `cmd.exe /C` inside another `cmd.exe /C`, and Wine's cmd cannot parse
# that.  So the DLLs are collected here, after the build, instead.
#
# Dependency resolution is a breadth-first walk over the PE import tables using
# the host's binutils `objdump`, which reads pei-x86-64 natively.  That avoids a
# Wine startup per DLL and does not require the Windows toolchain on PATH.
#
# Usage: ci/package-msys2.sh [build-dir] [out-zip]
# ------------------------------------------------------------------------------
set -euo pipefail

BUILD_DIR="${1:-build-msys2}"
OUT_ZIP="${2:-iview-msys2.zip}"

ENV_NAME="${LINSYS2_ENV:-clang64}"
ENV_ROOT="${LINSYS2_ENV_ROOT:-$HOME/.local/share/linsys2/${ENV_NAME}/${ENV_NAME}}"

BIN_DIR="$BUILD_DIR/src/imageviewer"

# Windows ships these itself; MSYS2 does not provide them and they must not be
# bundled (licensing and version skew).  The api-ms-win-* / ext-ms-* names are
# API-set forwarders resolved by the loader on any supported Windows.
SYSTEM_DLL_RE='^(api-ms-win-|ext-ms-)'
SYSTEM_DLL_RE+='|^(kernel32|kernelbase|user32|gdi32|advapi32|shell32|shlwapi'
SYSTEM_DLL_RE+='|ole32|oleaut32|comdlg32|comctl32|ws2_32|wsock32|mswsock'
SYSTEM_DLL_RE+='|opengl32|glu32|winmm|imm32|setupapi|version|bcrypt|secur32'
SYSTEM_DLL_RE+='|crypt32|ntdll|msvcrt|ucrtbase|dwmapi|uxtheme|hid|dinput8'
SYSTEM_DLL_RE+='|xinput|dbghelp|psapi|iphlpapi|userenv|rpcrt4|winspool)\.[dD][lL][lL]$'

if [ ! -f "$BIN_DIR/iview.exe" ]; then
    echo "Error: $BIN_DIR/iview.exe not found -- run the build first." >&2
    exit 1
fi
if [ ! -d "$ENV_ROOT/bin" ]; then
    echo "Error: MSYS2 prefix not found at $ENV_ROOT." >&2
    echo "       Set LINSYS2_ENV_ROOT to override." >&2
    exit 1
fi

stage="$(mktemp -d)"
trap 'rm -rf "$stage"' EXIT

# Executable plus the plugin DLLs built next to it (ImageLoader*.dll,
# FilterPixman.dll).  iwconv.exe is intentionally excluded: it is a build-time
# convenience tool, matching what the previous MinGW job shipped.
cp "$BIN_DIR/iview.exe" "$stage/"
find "$BIN_DIR" -maxdepth 1 -type f -name '*.dll' -exec cp -t "$stage" {} +

# Where the imported DLLs can live: the clang64 prefix, plus the MSYS2 runtime
# (msys-2.0.dll) which sits in the sibling usr/bin of the same environment.
search_dirs=(
    "$ENV_ROOT/bin"
    "$ENV_ROOT/usr/bin"
    "$ENV_ROOT/lib"
    "$(dirname "$ENV_ROOT")/usr/bin"
)

declare -A seen=()
pending=()
for pe in "$stage"/*.exe "$stage"/*.dll; do
    [ -f "$pe" ] && pending+=("$pe")
done

while [ "${#pending[@]}" -gt 0 ]; do
    next=()
    for pe in "${pending[@]}"; do
        while read -r name; do
            name="${name%$'\r'}"
            [ -n "$name" ] || continue
            key="${name,,}"
            [ -n "${seen[$key]:-}" ] && continue
            seen[$key]=1
            # Import tables store names in the casing the linker chose (KERNEL32.dll
            # next to kernel32.dll), so match the lowercased copy, not $name.
            [[ "$key" =~ $SYSTEM_DLL_RE ]] && continue

            found=""
            for dir in "${search_dirs[@]}"; do
                if [ -f "$dir/$name" ]; then
                    found="$dir/$name"
                    break
                fi
            done
            if [ -z "$found" ]; then
                echo "warning: $name (imported by $(basename "$pe")) is not in $ENV_ROOT" >&2
                continue
            fi
            cp -f "$found" "$stage/"
            next+=("$stage/$name")
        done < <(objdump -p "$pe" | awk '/DLL Name:/ {print $3}')
    done
    pending=("${next[@]}")
done

rm -f "$OUT_ZIP"
bsdtar --format zip -cf "$OUT_ZIP" -C "$stage" .

echo "== $OUT_ZIP =="
bsdtar -tf "$OUT_ZIP" | sort | sed 's/^/  /'
echo "packaged $(bsdtar -tf "$OUT_ZIP" | wc -l) files"
