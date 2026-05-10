cmake_minimum_required(VERSION 3.30)
project(ImageViewer)

set(CMAKE_EXPORT_COMPILE_COMMANDS ON)
set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

enable_testing()

# Use vcpkg for dependency management (default: OFF, use system libraries).
# Set to ON for MinGW cross-compilation or when system libraries are unavailable.
option(IMAGEVIEWER_USE_VCPKG
    "Use vcpkg for dependency management instead of system libraries"
    OFF)

# Build the GUI viewer and its image-loader plugins.
# Set IMAGEVIEWER_BUILD_VIEWER=OFF to compile only the format library and tests
# (e.g. for native Linux CI that lacks GLFW/GLAD and X11 headers).
option(IMAGEVIEWER_BUILD_VIEWER
    "Build the ImageViewer GUI application and image-loader plugins (requires GLFW, GLAD)"
    ON)

# Only configure vcpkg if explicitly enabled.
if(IMAGEVIEWER_USE_VCPKG)
    # Activate the 'viewer' vcpkg feature and all per-plugin features when the GUI
    # is requested.  Without this, vcpkg will not install glad/glfw3/plugin libs.
    if(IMAGEVIEWER_BUILD_VIEWER)
        set(_IW_FEATURES "viewer;plugin-jpeg;plugin-webp;plugin-tiff;plugin-stb;plugin-pixman")
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
