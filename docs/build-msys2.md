# Building ImageViewer for Windows with LinSYS2 (MSYS2 on Wine)

**Status: evaluation only — not adopted.** The mingw32 + vcpkg path in
`ci/Containerfile` and `prepare_mingw32.sh` remains the supported Windows build
and must stay functional until the alternative is proven on real hardware.

This document records why the current path is painful, what LinSYS2 would
replace, exactly how to try it, and what is still unproven. Every step marked
**UNVERIFIED** has not been executed end-to-end for this project; treat those
as hypotheses, not instructions that are known to work.

---

## 1. Why consider replacing the current path

The Windows build currently uses a *Linux port* of the MinGW toolchain
(llvm-mingw, installed from a local `llvm-mingw-*.pkg.tar.zst` in
`ci/Containerfile`) plus vcpkg for every third-party library. Dependencies come
from a vcpkg manifest (`vcpkg.json`) resolved for the triplet
`x64-mingw-dynamic` with host triplet `x64-linux`, layered on top of a custom
toolchain file (`cmake/toolchains/x86_64-w64-mingw32.cmake`).

Two structural problems follow from that choice:

1. **The build process is not the Windows build process.** The compiler, its
   defaults, the dependency builds and the runtime layout all differ from what
   MSYS2 or MSVC produce on Windows. Divergence here is invisible until it
   breaks.
2. **vcpkg is doing the heaviest lifting.** Every library — including GLFW,
   glad, pixman and libtiff — is compiled from source by vcpkg during
   configure, for a cross target. That is a large, slow, network-dependent
   surface with its own PowerShell host-tooling requirements (hence the
   `pwsh` install in the Containerfile).

LinSYS2 takes the opposite approach: install the *actual* Windows MSYS2
packages, then run the Windows compiler through Wine.

---

## 2. What LinSYS2 is

From the upstream README (`wszqkzqk/LinSYS2`, GPL-2.0-or-later):

- A patched MSYS2 fork of **pacman**, rebuilt to run on Linux, pointed at the
  official MSYS2 repositories, installing into `~/.local/share/linsys2/`.
- A **Wine integration layer** (`linsys2 run`, `linsys2 shell`) that uses an
  isolated Wine prefix per environment and injects the environment's `bin`
  directory through `WINEPATH`.
- `linsys2-makepkg` (experimental) for building MSYS2 PKGBUILDs on Linux.

Environments:

| Env | Compiler(s) | `chost` | Default |
|---|---|---|---|
| `ucrt64` | GCC **and** Clang | `x86_64-w64-mingw32` | x86_64 |
| `clang64` | Clang only | `x86_64-w64-mingw32` | — |
| `clangarm64` | Clang only | `aarch64-w64-mingw32` | ARM64 |

**Use `clang64`, not the default `ucrt64`.** The current build compiles with
`x86_64-w64-mingw32-clang++` from llvm-mingw, which links **libc++**. `ucrt64`
uses GCC with **libstdc++** — a different standard library, different ABI
details, and different (weaker) C++20 module support. `clang64` is the
like-for-like replacement; `ucrt64` would be a second silent change on top of
the one we are already evaluating. Pass `--env clang64` to every command
below; it is not the default.

---

## 3. Install on Arch Linux

### 3.1 Runtime prerequisites

The AUR package depends on:

```
bash bubblewrap coreutils curl gawk gettext glibc gnupg gpgme grep \
libarchive openssl which bzip2 xz zstd wine python
```

`wine` is the largest addition (several hundred MB installed on Arch).
Everything else is likely already present.

`binfmt_misc` registration for `DOSWin` is **optional** for plain
`linsys2 run` (Wine is invoked explicitly) but required for
`linsys2-makepkg` and for running `.exe` files directly from a Linux shell:

```bash
# Check first — many systems (systemd) already register this.
ls /proc/sys/fs/binfmt_misc/DOSWin

# Only if missing:
echo ':DOSWin:M::MZ::/usr/bin/wine:F' | sudo tee /etc/binfmt.d/linsys2-wine.conf
sudo systemctl restart systemd-binfmt
```

### 3.2 Install the package

```bash
yay -S linsys2        # or: paru -S linsys2
```

Or build from source (needed if no AUR helper is installed):

```bash
git clone --recursive https://github.com/wszqkzqk/LinSYS2.git
cd LinSYS2
makepkg -si
```

The toolchain lands in its private prefix `/usr/lib/linsys2-pacman`; only
`linsys2`, `linsys2-pacman` and `linsys2-makepkg` go to `/usr/bin`.

> **Provenance note.** At the time of writing the AUR package reports
> version `r97.20260922.f9edf30-1`, 1 vote, popularity ~0.03, single
> maintainer, last upstream push 2026-09. It is a young project. Pinning the
> git commit you build (`makepkg` from a checkout) is wise.

---

## 4. Initialise pacman (step 1 of 3)

```bash
linsys2-pacman --env clang64 init
```

This creates the pacman config and imports the MSYS2 keyring. It does **not**
create the Wine prefix and does **not** touch any Windows `PATH` — that is
step 3.

---

## 5. Install the packages ImageViewer needs (step 2 of 3)

```bash
linsys2-pacman --env clang64 -Syu

linsys2-pacman --env clang64 -S \
    mingw-w64-clang-x86_64-clang \
    mingw-w64-clang-x86_64-cmake \
    mingw-w64-clang-x86_64-ninja \
    mingw-w64-clang-x86_64-pkgconf \
    mingw-w64-clang-x86_64-glfw \
    mingw-w64-clang-x86_64-libpng \
    mingw-w64-clang-x86_64-libjpeg-turbo \
    mingw-w64-clang-x86_64-libtiff \
    mingw-w64-clang-x86_64-libwebp \
    mingw-w64-clang-x86_64-pixman \
    mingw-w64-clang-x86_64-doctest \
    mingw-w64-clang-x86_64-stb \
    mingw-w64-clang-x86_64-zlib \
    mingw-w64-clang-x86_64-python-glad
```

Package contents confirmed against the live `clang64` repository database:

- `clang` 23.1.3 — includes `clang-scan-deps`, which CMake requires to build
  C++20 modules with Clang. Without it the `FILE_SET CXX_MODULES` targets in
  `src/imageviewer/CMakeLists.txt` cannot be configured.
- `cmake` 4.4.4, `ninja` 1.13.2 — Windows binaries; both run under Wine.
- `glfw` 3.5.1 ships `lib/cmake/glfw3/glfw3Config.cmake` **and**
  `lib/pkgconfig/glfw3.pc`.
- `libpng` 1.6.59, `libjpeg-turbo` 3.2.0, `libtiff` 4.7.2, `libwebp` 1.6.0,
  `pixman` 0.46.4, `doctest` 2.5.3, `zlib` 1.3.2, `stb` r2216 all present.
- `python-glad` 2.0.8 ships `bin/glad.exe` — see the glad gap in §9.1.
- OpenGL itself comes from `mingw-w64-clang-x86_64-crt` (`libopengl32.a`) and
  `mingw-w64-clang-x86_64-headers` (`GL/gl.h`, GL 1.1 only). That is fine here
  because the project loads modern GL through glad.

---

## 6. Initialise the Wine integration (step 3 of 3)

```bash
linsys2 init --env clang64
```

**Run this only after §5.** `linsys2 init` registers the environment's `bin`
directory into the Wine prefix's `HKEY_CURRENT_USER\Environment` `PATH`
**only if that directory already exists**. Run it before installing anything
and the registration is silently skipped, leaving a Wine prefix whose Windows
`PATH` does not contain the toolchain — which surfaces much later as
"cmake cannot find ninja" or "cannot find clang".

Verify it took effect:

```bash
linsys2 env --env clang64
```

Expect a `Wine PATH` listing that includes
`…\.local\share\linsys2\clang64\clang64\bin`, with
`Registration status: registered`.

> If you ever install packages *after* this step and see the registration
> missing, re-running `linsys2 init --env clang64` re-registers it (it is
> idempotent).

---

## 7. Build

Because CMake, Ninja and Clang are all *Windows* binaries, **no CMake
toolchain file and no `CMAKE_SYSTEM_NAME` override are needed.** CMake
naturally targets Windows because it *is* a Windows build. This is the main
structural improvement over the current path.

### 7.1 Configure

```bash
cd ~/work/ImageViewer

# Windows-side path of the MSYS2 environment. MSYS2 tools normally learn this
# from the MSYS2 shell profile; under LinSYS2 there is no such profile, so it
# must be passed explicitly.  (UNVERIFIED — see §10.1.)
ENV_WIN="$(WINEPREFIX="$HOME/.local/share/linsys2/clang64/wine" \
           winepath -w "$HOME/.local/share/linsys2/clang64/clang64")"

linsys2 run --env clang64 -- cmake -S . -B build-clang64 -G Ninja \
    -DIMAGEVIEWER_USE_VCPKG=OFF \
    -DIMAGEVIEWER_BUILD_VIEWER=ON \
    -DCMAKE_BUILD_TYPE=Release \
    -DCMAKE_PREFIX_PATH="$ENV_WIN"
```

`-DIMAGEVIEWER_USE_VCPKG=OFF` is correct and important: it selects the
project's own `FindGLFW3.cmake` / `FindGlad.cmake` / pkg-config discovery,
which is what MSYS2 satisfies. Nothing in `CMakeLists.txt` needs to change for
this to be attempted.

Use **relative** paths (`-S .`, `-B build-clang64`). Under Wine the source
tree is reachable as `Z:\home\...`; relative paths sidestep drive-letter
guessing entirely.

### 7.2 Build

```bash
linsys2 run --env clang64 -- cmake --build build-clang64
```

---

## 8. Test

The test binaries are Windows PE files. Either let Wine run them explicitly:

```bash
linsys2 run --env clang64 -- build-clang64/src/tests/iview_tests.exe
linsys2 run --env clang64 -- build-clang64/src/tests/format/iview_format_tests.exe --no-intro
```

…or, with `binfmt_misc DOSWin` registered, run them directly from bash. Prefer
the explicit `linsys2 run` form in scripts: it needs no host configuration.

`ctest` also exists as a Windows binary in the environment and can drive the
suite. `ctest` spawning tests is a Windows→Windows process spawn, which Wine
handles natively — that path does **not** need `binfmt_misc`.

A green run on the current branch means **137 test cases / 685 assertions**
for `iview_tests`, and a passing `iview_format_tests`.

---

## 9. Dependency gap analysis

Everything vcpkg currently supplies, and whether MSYS2 `clang64` covers it:

| Dependency | vcpkg manifest | MSYS2 clang64 | Discovery path used by this project |
|---|---|---|---|
| GLFW | `glfw3` (feature `viewer`) | **yes** | `FindGLFW3.cmake` via `glfw3.pc`, or `glfw3Config.cmake` |
| glad | `glad` (feature `viewer`) | **NO PACKAGE** | see §9.1 — the one real gap |
| libpng | `libpng` | **yes** | CMake's `FindPNG` + `libpng16.pc` |
| libjpeg-turbo | `libjpeg-turbo` | **yes** | CMake's `FindJPEG` |
| libtiff | `tiff` | **yes** | CMake's `FindTIFF` + `libtiff-4.pc` |
| libwebp | `libwebp` | **yes** | `WebPConfig.cmake` |
| pixman | `pixman` | **yes** | `PkgConfig::PIXMAN` via `pixman-1.pc` |
| doctest | `doctest` | **yes** | `doctestConfig.cmake` |
| stb | `stb` (feature `plugin-stb`) | headers only | **gap — see §9.2** |
| zlib | (transitive) | **yes** | `FindPNG`/`FindTIFF` dependency |

### 9.1 glad — the only blocker-class gap

There is no `glad` package in `clang64`. `cmake/FindGlad.cmake` has a
fallback: if no prebuilt glad is found, it looks for a `glad` program and
generates the loader at configure time into `glad-generated/`. MSYS2 does ship
`python-glad`, which installs `bin/glad.exe`, so `find_program(GLAD_GENERATOR
glad)` should succeed.

The catch: glad 2.x fetches the OpenGL XML registry **from the network at
generation time** unless handed a local `--spec`. That means `glad.exe`
(Windows Python under Wine) needs working TLS and a certificate store inside
the Wine prefix. Wine's certificate handling for Windows Python is a plausible
failure point and is **UNVERIFIED** here.

Options, in order of robustness:

1. **Pre-generate glad and commit the result** (or vendor it as a small
   third-party directory). Removes network and Windows-Python from configure
   entirely, and removes the `glad` dependency from `vcpkg.json` as a bonus.
   This is a source change and therefore out of scope for this branch.
2. Generate inside the MSYS2 environment once, then point `-DGLAD_INCLUDE_DIR`
   at the output and keep it in the build cache.
3. Let the fallback run and hope TLS works. Cheapest to try, least reliable.

### 9.2 stb — a soft gap

`find_package(Stb QUIET)` is used for the optional `ImageLoaderStb` plugin
(BMP/TGA/GIF/HDR/PPM). MSYS2 ships **headers only** — no `StbConfig.cmake`.
So `find_package(Stb)` fails and the plugin is silently skipped (the
CMakeLists prints a `STATUS` line rather than failing).

Consequence: an MSYS2 build produces a viewer **without** the stb-backed
formats, and CI would still pass. That is exactly the class of silent
degradation this project has been eliminating. Fix would be a small
`FindStb.cmake` (find `stb/stb_image.h`), or `-DSTB_INCLUDE_DIR=...` plumbing.

### 9.3 What is *not* needed

`OpenMP` is `find_package(OpenMP)` without `REQUIRED` and no source uses it,
so its absence is harmless. Mesa, freeglut and libepoxy are **not** required —
GLFW plus `opengl32` and the generated glad loader cover the project.

---

## 10. What is unverified, and why it matters

These are the steps that could not be executed without installing LinSYS2 and
building the project. Each is a plausible failure site; do not assume any of
them works.

1. **Prefix-relative path assumptions.** MSYS2 tools commonly assume their
   prefix is at `/clang64`. Under plain `linsys2 run` the environment lives at
   `~/.local/share/linsys2/clang64/clang64` (i.e. `Z:\home\...\clang64`), and
   only `WINEPATH` is injected. `linsys2-makepkg` solves this by bind-mounting
   the prefix at its canonical location inside a `bubblewrap` namespace — that
   mechanism is **not** active for `linsys2 run`. Concretely at risk:
   `pkgconf.exe` (compiled-in prefix) and CMake's default search path.
   Mitigation attempted above by passing `CMAKE_PREFIX_PATH` explicitly; a
   host bind-mount may be needed:
   `sudo mount --bind ~/.local/share/linsys2/clang64/clang64 /clang64`.
2. **glad generation under Wine** (§9.1) — network + TLS inside the prefix.
3. **C++23 modules.** The project builds `FILE_SET CXX_MODULES` targets with
   `CXX_STANDARD 23`. Clang 23 and `clang-scan-deps` are present, and CMake
   4.4.4 supports the flow, but a *Windows-hosted* CMake + Clang module build
   of this project has never been run. Modules are the single most
   version-sensitive part of the build.
4. **OpenGL under Wine for the actual viewer.** Wine maps `opengl32` onto the
   host's libGL. `iview.exe` needs a working display and GL driver. In a
   headless CI container there is no X/Wayland server, so **the viewer cannot
   be run in CI**; only the console tests can. This is a hard limit, not a
   configuration detail.
5. **Packaging.** The current `build-mingw32` CI job zips `iview.exe` plus every
   `*.dll` it finds next to it. With MSYS2 the runtime DLLs (`libc++.dll`,
   `libpng16.dll`, `glfw3.dll`, `libtiff.dll`, …) live in the *environment's*
   `bin`, not in the build directory. The packaging step needs to collect them
   explicitly, or link statically. Not addressed here.
6. **First-run cost.** `wineboot --init` on a cold prefix takes tens of
   seconds and may try to fetch wine-mono/gecko.

---

## 11. Recommendation

**Option C — LinSYS2 for local development only; CI unchanged for now.**

Rationale, in order:

- **The unknown is in the integration, not the concept.** MSYS2 has every
  library except glad, and package availability is not in doubt (§9). What is
  unproven is whether a *Windows-hosted* CMake+Clang driven through
  `linsys2 run` can configure this particular C++23-modules project without
  manual prefix plumbing (§10.1, §10.3). That question should be answered on
  a desk, not in CI.
- **The current path must keep working regardless** (project constraint). A
  local-first rollout preserves the Windows signal we have today while the
  replacement earns trust.
- **The project is young.** Single maintainer, 1 AUR vote, `r97`, and the
  author labels `linsys2-makepkg` experimental. Moving CI onto it before it
  has built this project once is how the last Windows setup broke.
- **CI cost is real but amortised.** The self-hosted runner uses a persistent
  volume (`gh-runner-data-mingw32`), so the multi-GB MSYS2 environment and the
  Wine prefix would survive between jobs. The recurring cost is mostly the
  first build; image size grows by roughly the `wine` package.

### Why not Option A (MSYS2 pacman + llvm-mingw)?

It keeps the compiler the user reports as unreliable and adds a hybrid
coupling: Windows-built, Wine-installed libraries consumed by a Linux
cross-compiler. Every failure then has two plausible causes instead of one. It
is the smallest diff on paper and the most confusing to debug in practice.
Rejected.

### Why not Option B (LinSYS2 in CI now)?

Not "never" — just not first. It is the right destination, and the sketch in
§12 is deliberately close to runnable so promotion is cheap. But promoting it
before §10.1–10.4 are settled would replace a known-fragile path with an
unknown-fragile one, and would lose the `build-mingw32` signal in the process.

### Promotion criteria (C → B)

Promote to CI only when all of these hold on a local machine:

- [ ] `linsys2 run -- cmake` configures with `-DIMAGEVIEWER_USE_VCPKG=OFF`
- [ ] The full module build succeeds (`iview`, all plugins)
- [ ] `iview_tests.exe` runs under Wine: 137 cases / 685 assertions pass
      (or a documented, understood difference)
- [ ] `iview_format_tests.exe` runs under Wine
- [ ] glad resolves without network access (ideally via committed generation)
- [ ] stb either builds, or its absence is explicit rather than silent
- [ ] Packaging produces an exe + DLL bundle that launches

Then add a *second*, non-required CI job rather than replacing
`build-mingw32`, run both for a while, and delete the old job only after the
new one has been green across several commits.

---

## 12. Experimental CI sketch (Option B, for later)

Not added to `ci/` on this branch, because Option C is the recommendation.
Kept here so the promotion work is mostly copy-and-test.

```dockerfile
# ci/Containerfile.linsys2  —  EXPERIMENTAL, DO NOT USE FOR RELEASES
# Builds ImageViewer with the real MSYS2 clang64 toolchain, executed via Wine.
#
# Differences from ci/Containerfile:
#   * no llvm-mingw .pkg.tar.zst, no pwsh, no vcpkg
#   * adds wine + linsys2, and a pre-warmed MSYS2 clang64 environment
#   * NOT covered by the release workflow
FROM archlinux:latest

RUN pacman-key --init && pacman-key --populate archlinux \
 && pacman -Syu --noconfirm --ask 4

# base-devel supplies the meson/gcc/patch needed to build LinSYS2 itself.
RUN pacman -S --noconfirm --needed --ask 4 \
    base-devel git meson cmake python \
    wine bubblewrap gpgme libarchive openssl gnupg \
    curl which gawk gettext xz zstd bzip2 ca-certificates

# LinSYS2 is AUR-only and has no binary repo; build it from source.
# NOTE: build as a non-root user (makepkg refuses to run as root).
RUN useradd -m builder
USER builder
WORKDIR /home/builder
RUN git clone --recursive https://github.com/wszqkzqk/LinSYS2.git \
 && cd LinSYS2 && makepkg -si --noconfirm

# Prepare the Windows toolchain inside the image so jobs do not pay for it.
# (Multi-GB; the runner's persistent volume makes this survive between jobs.)
# Order matters: pacman init -> install packages -> linsys2 init (see §6).
RUN linsys2-pacman --env clang64 init \
 && linsys2-pacman --env clang64 -Syu --noconfirm \
 && linsys2-pacman --env clang64 -S --noconfirm \
      mingw-w64-clang-x86_64-clang \
      mingw-w64-clang-x86_64-cmake \
      mingw-w64-clang-x86_64-ninja \
      mingw-w64-clang-x86_64-pkgconf \
      mingw-w64-clang-x86_64-glfw \
      mingw-w64-clang-x86_64-libpng \
      mingw-w64-clang-x86_64-libjpeg-turbo \
      mingw-w64-clang-x86_64-libtiff \
      mingw-w64-clang-x86_64-libwebp \
      mingw-w64-clang-x86_64-pixman \
      mingw-w64-clang-x86_64-doctest \
      mingw-w64-clang-x86_64-stb \
      mingw-w64-clang-x86_64-zlib \
      mingw-w64-clang-x86_64-python-glad \
 && linsys2 init --env clang64

# Build + test sequence a CI job would run:
#
#   ENV_WIN="$(WINEPREFIX=$HOME/.local/share/linsys2/clang64/wine \
#              winepath -w $HOME/.local/share/linsys2/clang64/clang64)"
#   linsys2 run --env clang64 -- cmake -S . -B build-clang64 -G Ninja \
#       -DIMAGEVIEWER_USE_VCPKG=OFF -DIMAGEVIEWER_BUILD_VIEWER=ON \
#       -DCMAKE_BUILD_TYPE=Release -DCMAKE_PREFIX_PATH="$ENV_WIN"
#   linsys2 run --env clang64 -- cmake --build build-clang64
#   linsys2 run --env clang64 -- build-clang64/src/tests/iview_tests.exe
#
# Caveats carried over from the main document:
#   * the GUI viewer cannot run headless (no X/Wayland in the container);
#     only console tests are runnable
#   * `binfmt_misc` must be registered on the *host* for direct .exe execution;
#     the explicit `linsys2 run --` form above avoids needing it
#   * packaging must collect runtime DLLs from the MSYS2 env, not the build dir
```

---

## 13. How to go back

Nothing on this branch alters the existing build. To remove all local
LinSYS2 state:

```bash
linsys2 unregister --env clang64   # only if you used `register` against ~/.wine
rm -rf ~/.local/share/linsys2 ~/.config/linsys2
sudo pacman -Rns linsys2           # keep `wine` if you use it elsewhere
```

---

## 14. Sources

- LinSYS2 README — <https://github.com/wszqkzqk/LinSYS2> (branch `master`)
- LinSYS2 CLI implementation — `python/linsys2/cli_linsys2.py`,
  `cli_pacman.py`, `common.py` (read for `init`/`run`/`shell` semantics, the
  `bin`-directory ordering requirement in `cmd_init`, and the environment
  table)
- AUR package metadata — <https://aur.archlinux.org/packages/linsys2>
- MSYS2 package availability — verified against the live `clang64` repository
  databases (`clang64.db`, `clang64.files` from `mirror.msys2.org`)
- DeepWiki pages — **could not be read**: `deepwiki.com` is behind a Vercel
  JavaScript challenge and returns HTTP 429 to non-browser clients. The
  equivalent information above was taken from the upstream source instead,
  which is what DeepWiki summarises.
