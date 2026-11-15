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
#   * build-windows-msys2 (Windows / MSYS2 clang64 / LinSYS2)
#   * build-windows-msvc  (Windows / MSVC / vcpkg)
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

# The memory-span TU compiles against whatever subset of loaders exists: each
# of its cases is behind the same IW_<FMT>_PLUGIN_PATH define as the per-format
# file, so a build with two loaders still links and still tests those two.
set(_abi_sources main.cpp test_imageloader_memory.cpp test_plugin_ownership.cpp)
set(_abi_plugin_targets "")

macro(_abi_add_loader TARGET_ID SOURCE EXTRA_MESSAGE)
    if(TARGET ${TARGET_ID})
        list(APPEND _abi_sources ${SOURCE})
        list(APPEND _abi_plugin_targets ${TARGET_ID})
    else()
        message(STATUS "ABI tests: ${TARGET_ID} not built — skipping its cases "
                       "(${EXTRA_MESSAGE})")
    endif()
endmacro()

_abi_add_loader(ImageLoaderJpeg test_imageloader_jpeg.cpp
                "enable IMAGEVIEWER_BUILD_PLUGINS and find libjpeg-turbo")
_abi_add_loader(ImageLoaderPng  test_imageloader_png.cpp
                "enable IMAGEVIEWER_BUILD_PLUGINS and find libpng")
_abi_add_loader(ImageLoaderIWI  test_imageloader_iwi.cpp
                "enable IMAGEVIEWER_BUILD_PLUGINS (IWI has no external dependency)")
_abi_add_loader(ImageLoaderWebp test_imageloader_webp.cpp
                "enable IMAGEVIEWER_BUILD_PLUGINS and find libwebp")
_abi_add_loader(ImageLoaderTiff test_imageloader_tiff.cpp
                "enable IMAGEVIEWER_BUILD_PLUGINS and find libtiff")
_abi_add_loader(ImageLoaderGif  test_imageloader_gif.cpp
                "enable IMAGEVIEWER_BUILD_PLUGINS and find giflib")
_abi_add_loader(ImageLoaderStb  test_imageloader_stb.cpp
                "enable IMAGEVIEWER_BUILD_PLUGINS and find stb")
_abi_add_loader(FilterPixman    test_filterpixman.cpp
                "enable IMAGEVIEWER_BUILD_PLUGINS and find pixman")

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
#
# The fixture directory is unconditional: every per-format case file needs it,
# including whichever ones exist in this build.
target_compile_definitions(iview_abi_tests PRIVATE
    IW_ABI_FIXTURE_DIR="${CMAKE_CURRENT_SOURCE_DIR}/fixtures"
)

if(TARGET ImageLoaderJpeg)
    target_compile_definitions(iview_abi_tests PRIVATE
        IW_JPEG_PLUGIN_PATH="$<TARGET_FILE:ImageLoaderJpeg>")
endif()
if(TARGET ImageLoaderPng)
    target_compile_definitions(iview_abi_tests PRIVATE
        IW_PNG_PLUGIN_PATH="$<TARGET_FILE:ImageLoaderPng>")
endif()
if(TARGET ImageLoaderIWI)
    target_compile_definitions(iview_abi_tests PRIVATE
        IW_IWI_PLUGIN_PATH="$<TARGET_FILE:ImageLoaderIWI>")
endif()
if(TARGET ImageLoaderWebp)
    target_compile_definitions(iview_abi_tests PRIVATE
        IW_WEBP_PLUGIN_PATH="$<TARGET_FILE:ImageLoaderWebp>")
endif()
if(TARGET ImageLoaderTiff)
    target_compile_definitions(iview_abi_tests PRIVATE
        IW_TIFF_PLUGIN_PATH="$<TARGET_FILE:ImageLoaderTiff>")
endif()
if(TARGET ImageLoaderGif)
    target_compile_definitions(iview_abi_tests PRIVATE
        IW_GIF_PLUGIN_PATH="$<TARGET_FILE:ImageLoaderGif>")
endif()
if(TARGET ImageLoaderStb)
    target_compile_definitions(iview_abi_tests PRIVATE
        IW_STB_PLUGIN_PATH="$<TARGET_FILE:ImageLoaderStb>")
endif()
if(TARGET FilterPixman)
    target_compile_definitions(iview_abi_tests PRIVATE
        IW_FILTERPIXMAN_PLUGIN_PATH="$<TARGET_FILE:FilterPixman>")
endif()

# The plugins must exist before the test binary links (the paths above are
# genexes, which do not themselves create an ordering edge).
add_dependencies(iview_abi_tests ${_abi_plugin_targets})

# Windows ships iview.exe with the plugin DLLs beside it, and PluginManager
# loads them with a plain LoadLibraryW, which resolves a plugin's own imports
# (jpeg62.dll, libwebp.dll, ...) from the executable's directory.  This test
# binary lives in a different output directory from the plugins, so copy the
# plugins and their runtime DLLs next to it and load them from there: the tests
# then exercise the shipped layout and the shipped loader call.
#
# The DLL list comes from the plugin's own output directory rather than from
# $<TARGET_RUNTIME_DLLS>: that genex only follows SHARED imported targets, and
# this project's JPEG, PNG and TIFF dependencies are UNKNOWN imported targets,
# so it yields no jpeg62.dll and the load fails with 126.  See
# copy_plugin_dlls.cmake.
#
# Gated on IMAGEVIEWER_COPY_RUNTIME_DLLS because the LinSYS2/MSYS2 build sets
# it OFF: a POST_BUILD command there runs through Wine's cmd, which cannot run
# the nested command the Ninja generator emits.  That build resolves the
# plugins' imports from the MSYS2 prefix instead, so it keeps loading the
# build-tree artefacts in place.
if(WIN32 AND IMAGEVIEWER_COPY_RUNTIME_DLLS)
    #
    # Deploy the plugins, and the runtime DLLs they import, next to the ABI test
    # binary.  Three things this has to get right:
    #
    #   1. run after the plugins are built.  A stamped custom command with
    #      `DEPENDS $<TARGET_FILE:...>` looked cleaner but the Visual Studio
    #      generator normalizes that path and then reports MSB8064 ("specified
    #      dependency ... does not exist"), so the copy could run before the
    #      plugins existed and the test then failed with ERROR_MOD_NOT_FOUND.
    #      A target ordered by add_dependencies is generator-independent.
    #
    #   2. run again when a plugin changes.  The original POST_BUILD step hung
    #      off iview_abi_tests, so a plugin-only edit relinked the plugin,
    #      nothing re-ran the copy, and the suite validated the previous DLL.
    #      A custom target with no OUTPUT runs on every build; the work is a
    #      handful of file copies.
    #
    #   3. copy for each plugin separately.  The copy script globs the plugin's
    #      whole output directory (vcpkg's applocal step puts the import DLLs
    #      there, and the target graph cannot list them), so two concurrent
    #      copies into the same destination collide.  The COMMANDs of one target
    #      run in order.
    #
    add_custom_target(deploy_abi_plugins
        COMMENT "Deploying plugin DLLs next to the ABI tests"
        VERBATIM)

    foreach(_plugin IN LISTS _abi_plugin_targets)
        add_custom_command(TARGET deploy_abi_plugins
            COMMAND ${CMAKE_COMMAND}
                "-DPLUGIN_DIR=$<TARGET_FILE_DIR:${_plugin}>"
                "-DDEST_DIR=$<TARGET_FILE_DIR:iview_abi_tests>"
                -P "${CMAKE_CURRENT_SOURCE_DIR}/copy_plugin_dlls.cmake"
            VERBATIM)
    endforeach()

    add_dependencies(deploy_abi_plugins ${_abi_plugin_targets})
    add_dependencies(iview_abi_tests deploy_abi_plugins)

    target_compile_definitions(iview_abi_tests PRIVATE IW_ABI_PLUGINS_DEPLOYED=1)
endif()
target_link_libraries(iview_abi_tests PRIVATE doctest::doctest)

# dlopen lives in libdl on Linux; macOS and Windows need nothing extra.
if(UNIX AND NOT APPLE)
    target_link_libraries(iview_abi_tests PRIVATE ${CMAKE_DL_LIBS})
endif()

add_test(NAME iview_abi_tests COMMAND iview_abi_tests)
