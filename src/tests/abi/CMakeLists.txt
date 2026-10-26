# ABI conformance tests.
#
# These load the *built* plugin binaries through their exported C ABI
# (dlopen / LoadLibrary) and drive them exactly as iview does at runtime.
#
# Why a separate target rather than more cases in src/tests/test_iwimageformat.cpp:
# that suite is header-only -- it links ImagePluginDef.h, Utils.hpp and
# FilterCore.hpp and never touches a plugin.  Behaviour that lives inside a
# plugin is structurally invisible to it: a JPEG loader describing a 4-component
# CMYK file as RGB, or a dither plugin adding a full-scale brightness lift, both
# pass a header-only test and fail at runtime.  Loading the real artefact is the
# only way to see them.
#
# Gating
# ------
# The plugins are only built when IMAGEVIEWER_BUILD_VIEWER=ON
# (src/imageviewer/CMakeLists.txt returns early otherwise), so this directory
# contributes no targets when the viewer is disabled.  That includes the primary
# Linux CI job in .github/workflows/build.yml -- "Build (Linux / system
# libraries, no vcpkg)" -- which configures with -DIMAGEVIEWER_BUILD_VIEWER=OFF.
#
# The cases that DO run are those built by the jobs that enable the viewer:
#   * build-mingw32      (Windows / MinGW-w64 / vcpkg)
#   * build-windows-msvc (Windows / MSVC / vcpkg)
# and any local build configured with -DIMAGEVIEWER_BUILD_VIEWER=ON.
#
# Each plugin's cases are added independently: a build with only some plugins
# available still tests the ones that exist, and says which ones it skipped.
#
# Note for sanitizer builds: this target dlopen()s C++ shared libraries.  With
# only the JPEG loader loaded, LeakSanitizer reports ~104 bytes allocated by that
# DSO's own static initialisation, which dlclose() cannot reclaim.  It is not
# ImageViewer code: a ten-line program calling only dlopen()+dlclose() on the
# plugin reproduces it, at the same size whether or not the plugin was itself
# built with ASan.  Measured caveat: it is NOT reported once several plugins are
# loaded (observed with JPEG + pixman), and pixman alone does not produce it, so
# the report depends on DSO registration order rather than on these tests.  Leak
# detection stays enabled and no suppression is added.

set(_abi_sources main.cpp)
set(_abi_plugin_targets "")

if(TARGET ImageLoaderJpeg)
    list(APPEND _abi_sources test_imageloader_jpeg.cpp)
    list(APPEND _abi_plugin_targets ImageLoaderJpeg)
    set(_abi_have_jpeg TRUE)
else()
    message(STATUS "ABI tests: ImageLoaderJpeg not built — skipping JPEG ABI cases "
                   "(enable IMAGEVIEWER_BUILD_VIEWER and the JPEG plugin to run them)")
endif()

if(TARGET FilterPixman)
    list(APPEND _abi_sources test_filterpixman.cpp)
    list(APPEND _abi_plugin_targets FilterPixman)
    set(_abi_have_pixman TRUE)
else()
    message(STATUS "ABI tests: FilterPixman not built — skipping FilterPixman ABI cases "
                   "(enable IMAGEVIEWER_BUILD_VIEWER and the pixman plugin to run them)")
endif()

if(NOT _abi_plugin_targets)
    message(STATUS "ABI tests: no plugins available — skipping the ABI test target")
    return()
endif()

find_package(doctest REQUIRED)

add_executable(iview_abi_tests ${_abi_sources})
set_property(TARGET iview_abi_tests PROPERTY CXX_STANDARD 17)

target_include_directories(iview_abi_tests PRIVATE
    # ImagePluginDef.h, FilterPluginDef.h and Utils.hpp (ValidateImageFormat)
    ${CMAKE_CURRENT_SOURCE_DIR}/../../imageviewer
    ${CMAKE_CURRENT_SOURCE_DIR}
)

# Paths are baked in at configure time so the tests do not depend on the
# working directory ctest happens to use.  Each definition is only added when
# the corresponding case file is compiled, because that file #errors without it.
if(_abi_have_jpeg)
    target_compile_definitions(iview_abi_tests PRIVATE
        IW_JPEG_PLUGIN_PATH="$<TARGET_FILE:ImageLoaderJpeg>"
        IW_ABI_FIXTURE_DIR="${CMAKE_CURRENT_SOURCE_DIR}/fixtures"
    )
endif()

if(_abi_have_pixman)
    target_compile_definitions(iview_abi_tests PRIVATE
        IW_FILTERPIXMAN_PLUGIN_PATH="$<TARGET_FILE:FilterPixman>"
    )
endif()

# The plugins must exist before the test binary links (the paths above are
# genexes, which do not themselves create an ordering edge).
add_dependencies(iview_abi_tests ${_abi_plugin_targets})

target_link_libraries(iview_abi_tests PRIVATE doctest::doctest)

# dlopen lives in libdl on Linux; macOS and Windows need nothing extra.
if(UNIX AND NOT APPLE)
    target_link_libraries(iview_abi_tests PRIVATE ${CMAKE_DL_LIBS})
endif()

add_test(NAME iview_abi_tests COMMAND iview_abi_tests)
