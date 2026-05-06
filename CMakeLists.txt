cmake_minimum_required(VERSION 3.30)
project(ImageViewer)

set(CMAKE_EXPORT_COMPILE_COMMANDS ON)
set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

enable_testing()

# Build the GUI viewer and its image-loader plugins.
# Set IMAGEVIEWER_BUILD_VIEWER=OFF to compile only the format library and tests
# (e.g. for native Linux CI that lacks GLFW/GLAD and X11 headers).
option(IMAGEVIEWER_BUILD_VIEWER
    "Build the ImageViewer GUI application and image-loader plugins (requires GLFW, GLAD)"
    ON)

# Activate the 'viewer' vcpkg feature only when the GUI is requested.
# Without this, vcpkg will not install glad/glfw3/etc. and the CI runners that
# lack X11 headers (ubuntu-latest) will not fail on those packages.
if(IMAGEVIEWER_BUILD_VIEWER)
    if(NOT DEFINED VCPKG_MANIFEST_FEATURES)
        set(VCPKG_MANIFEST_FEATURES "viewer" CACHE STRING "Vcpkg manifest features to install")
    else()
        list(APPEND VCPKG_MANIFEST_FEATURES "viewer")
        set(VCPKG_MANIFEST_FEATURES "${VCPKG_MANIFEST_FEATURES}" CACHE STRING "Vcpkg manifest features to install" FORCE)
    endif()
endif()

include(third-party/vcpkg/scripts/buildsystems/vcpkg.cmake)

add_subdirectory(src)
