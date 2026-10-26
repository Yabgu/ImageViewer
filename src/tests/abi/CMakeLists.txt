# ABI conformance tests.
#
# These load the *built* plugin binaries through their exported C ABI
# (dlopen / LoadLibrary) and drive them exactly as iview does at runtime.
#
# Why a separate target rather than more cases in src/tests/test_iwimageformat.cpp:
# that suite is header-only -- it links ImagePluginDef.h, Utils.hpp and
# FilterCore.hpp and never touches a plugin.  Descriptor/buffer mismatches that
# live inside a plugin (for example the JPEG loader describing a 4-component
# CMYK file as RGB) are structurally invisible to it.  Loading the real
# artefact is the only way to see them.
#
# Availability: the plugins are only built when IMAGEVIEWER_BUILD_VIEWER=ON
# (src/imageviewer/CMakeLists.txt returns early otherwise), so this directory
# contributes nothing when the viewer is disabled.  That includes the primary
# Linux CI job (.github/workflows/build.yml, "Build (Linux / system libraries,
# no vcpkg)"), which configures with -DIMAGEVIEWER_BUILD_VIEWER=OFF.  These
# tests therefore run in builds that produce plugins: the MinGW and MSVC
# Windows jobs, and any local build with the viewer enabled.
#
# Note for sanitizer builds: this target dlopen()s a C++ shared library, and
# loading libImageLoaderJpeg.so leaves ~104 bytes allocated by that DSO's own
# static initialisation which dlclose() cannot reclaim.  LeakSanitizer reports
# it against this binary.  It is not ImageViewer code: a ten-line program that
# only calls dlopen()+dlclose() on the plugin reproduces it, with identical
# size whether the plugin itself was built with ASan or not.  The plugin is
# loaded once per binary to keep the artefact to a single instance.

if(NOT TARGET ImageLoaderJpeg)
    message(STATUS "ABI tests: ImageLoaderJpeg not built — skipping JPEG ABI tests "
                   "(enable IMAGEVIEWER_BUILD_VIEWER and the JPEG plugin to run them)")
    return()
endif()

find_package(doctest REQUIRED)

add_executable(iview_abi_tests
    test_imageloader_jpeg.cpp
)
set_property(TARGET iview_abi_tests PROPERTY CXX_STANDARD 17)

target_include_directories(iview_abi_tests PRIVATE
    # ImagePluginDef.h and Utils.hpp (ValidateImageFormat)
    ${CMAKE_CURRENT_SOURCE_DIR}/../../imageviewer
    ${CMAKE_CURRENT_SOURCE_DIR}
)

# Paths are baked in at configure time so the test does not depend on the
# working directory ctest happens to use.
target_compile_definitions(iview_abi_tests PRIVATE
    IW_JPEG_PLUGIN_PATH="$<TARGET_FILE:ImageLoaderJpeg>"
    IW_ABI_FIXTURE_DIR="${CMAKE_CURRENT_SOURCE_DIR}/fixtures"
)

# The plugin must exist before the test binary is built (the path is a genex,
# which does not itself create an ordering edge).
add_dependencies(iview_abi_tests ImageLoaderJpeg)

target_link_libraries(iview_abi_tests PRIVATE doctest::doctest)

# dlopen lives in libdl on Linux; macOS and Windows need nothing extra.
if(UNIX AND NOT APPLE)
    target_link_libraries(iview_abi_tests PRIVATE ${CMAKE_DL_LIBS})
endif()

add_test(NAME iview_abi_tests COMMAND iview_abi_tests)
