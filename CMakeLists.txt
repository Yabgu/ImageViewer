cmake_minimum_required(VERSION 3.30)
# C is enabled for the IWI container's C API (src/imageviewer/IwiFile.c).  It is
# a real C interface, not a C++ one wearing a .h name: Tension consumes the
# format and a game engine binds it through FFI, so it has to compile as C.
project(ImageViewer LANGUAGES C CXX)

set(CMAKE_EXPORT_COMPILE_COMMANDS ON)
set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

enable_testing()

# Use vcpkg for dependency management (default: OFF, use system libraries).
# Set to ON for MinGW cross-compilation or when system libraries are unavailable.
option(IMAGEVIEWER_USE_VCPKG
    "Use vcpkg for dependency management instead of system libraries"
    OFF)

# Build the GUI viewer.
# Set IMAGEVIEWER_BUILD_VIEWER=OFF to compile only the format library, the
# plugins and the tests (e.g. on a machine with no GLFW/GLAD/X11).
option(IMAGEVIEWER_BUILD_VIEWER
    "Build the ImageViewer GUI application (requires GLFW, GLAD)"
    ON)

# Build the image-loader and filter plugins.
#
# Independent of IMAGEVIEWER_BUILD_VIEWER: the plugins need their own format
# libraries (libjpeg, libpng, libwebp, libtiff, stb, pixman) and no GUI
# toolchain.  IWI is parsed inline and needs no library at all.  A headless
# build (-DIMAGEVIEWER_BUILD_VIEWER=OFF -DIMAGEVIEWER_BUILD_PLUGINS=ON)
# produces all seven DSOs and the ABI tests, which is what the Linux CI job runs.
option(IMAGEVIEWER_BUILD_PLUGINS
    "Build the image-loader and filter plugin libraries"
    ON)

# Only configure vcpkg if explicitly enabled.
if(IMAGEVIEWER_USE_VCPKG)
    # Activate the 'viewer' vcpkg feature and all per-plugin features when the GUI
    # is requested.  Without this, vcpkg will not install glad/glfw3/plugin libs.
    if(IMAGEVIEWER_BUILD_VIEWER)
        set(_IW_FEATURES "viewer;plugin-jpeg;plugin-webp;plugin-tiff;plugin-gif;plugin-stb;plugin-pixman")
        if(NOT DEFINED VCPKG_MANIFEST_FEATURES)
            set(VCPKG_MANIFEST_FEATURES "${_IW_FEATURES}" CACHE STRING "Vcpkg manifest features to install")
        else()
            list(APPEND VCPKG_MANIFEST_FEATURES ${_IW_FEATURES})
            set(VCPKG_MANIFEST_FEATURES "${VCPKG_MANIFEST_FEATURES}" CACHE STRING "Vcpkg manifest features to install" FORCE)
        endif()
    endif()
    include(third-party/vcpkg/scripts/buildsystems/vcpkg.cmake)
endif()

add_subdirectory(src)
