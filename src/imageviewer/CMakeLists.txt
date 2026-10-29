# OpenMP is deliberately not enabled.  Nothing in this project uses it: there
# is no `#pragma omp` and no `omp.h` include under src/.
#
# It used to be enabled unconditionally here, and appending the flags to
# CMAKE_CXX_FLAGS applied them to every target in this directory -- including
# all the plugin SHARED libraries.  On MSVC /openmp:llvm therefore made every
# plugin import the OpenMP runtime (libomp140.x86_64.dll), which is not on
# PATH on a stock machine, so LoadLibrary failed with ERROR_MOD_NOT_FOUND and
# the shipped viewer would not start on a clean Windows install.  Defining
# _OPENMP for the modules but not for their consumers also produced C5050
# "possible incompatible environment" warnings.  clang64 does not find OpenMP
# at all, which is why the MSYS2 build never showed either symptom.
#
# If OpenMP is adopted later, give the flags to the targets that actually use
# it via target_compile_options()/target_link_libraries() rather than to
# CMAKE_CXX_FLAGS, and apply identical flags to every module importer.

if(NOT IMAGEVIEWER_BUILD_VIEWER)
    message(STATUS "IMAGEVIEWER_BUILD_VIEWER=OFF — skipping GUI viewer build")
    return()
endif()

if(IMAGEVIEWER_USE_VCPKG)
    find_package(glfw3 CONFIG REQUIRED)
    set(_GLFW_TARGET glfw)
else()
    list(APPEND CMAKE_MODULE_PATH "${CMAKE_SOURCE_DIR}/cmake")
    find_package(GLFW3 REQUIRED)
    set(_GLFW_TARGET glfw3::glfw3)
endif()

# ── Per-plugin optional dependencies (QUIET — skip plugin if not found) ────────

find_package(JPEG QUIET)
if(JPEG_FOUND)
    set(IMAGEVIEWER_JPEG_TARGET JPEG::JPEG)
    message(STATUS "Plugin: ImageLoaderJpeg enabled (libjpeg-turbo found)")
else()
    message(STATUS "Plugin: ImageLoaderJpeg skipped (libjpeg-turbo not found)")
endif()

find_package(WebP CONFIG QUIET)
if(WebP_FOUND)
    message(STATUS "Plugin: ImageLoaderWebp enabled (libwebp found)")
else()
    message(STATUS "Plugin: ImageLoaderWebp skipped (libwebp not found)")
endif()

find_package(Stb QUIET)
if(Stb_FOUND)
    message(STATUS "Plugin: ImageLoaderStb enabled (stb found)")
else()
    message(STATUS "Plugin: ImageLoaderStb skipped (stb not found)")
endif()

if(CMAKE_CROSSCOMPILING)
    if(DEFINED VCPKG_INSTALLED_DIR AND NOT "${VCPKG_INSTALLED_DIR}" STREQUAL "")
        set(_VCPKG_INSTALLED_DIR "${VCPKG_INSTALLED_DIR}")
    else()
        set(_VCPKG_INSTALLED_DIR "${CMAKE_BINARY_DIR}/vcpkg_installed")
    endif()
    set(_TIFF_INCLUDE_DIR "${_VCPKG_INSTALLED_DIR}/${VCPKG_TARGET_TRIPLET}/include")
    find_library(_TIFF_LIBRARY
        NAMES tiff libtiff
        PATHS "${_VCPKG_INSTALLED_DIR}/${VCPKG_TARGET_TRIPLET}/lib"
        NO_DEFAULT_PATH
    )
    if(_TIFF_LIBRARY)
        add_library(TIFF::TIFF UNKNOWN IMPORTED)
        set_target_properties(TIFF::TIFF PROPERTIES
            IMPORTED_LOCATION "${_TIFF_LIBRARY}"
            INTERFACE_INCLUDE_DIRECTORIES "${_TIFF_INCLUDE_DIR}"
        )
        set(TIFF_FOUND TRUE)
        message(STATUS "Plugin: ImageLoaderTiff enabled (tiff found at ${_TIFF_LIBRARY})")
    else()
        message(STATUS "Plugin: ImageLoaderTiff skipped (tiff not found for triplet ${VCPKG_TARGET_TRIPLET})")
    endif()
else()
    find_package(TIFF QUIET)
    if(TIFF_FOUND)
        message(STATUS "Plugin: ImageLoaderTiff enabled (tiff found)")
    else()
        message(STATUS "Plugin: ImageLoaderTiff skipped (tiff not found)")
    endif()
endif()

# ── pixman ────────────────────────────────────────────────────────────────────
# The vcpkg pixman port ships only pkg-config files, not a CMake CONFIG package.
# When cross-compiling, locate the library directly from vcpkg_installed like we
# do for TIFF; on native builds fall back to system pkg-config.
if(CMAKE_CROSSCOMPILING)
    if(NOT DEFINED _VCPKG_INSTALLED_DIR)
        if(DEFINED VCPKG_INSTALLED_DIR AND NOT "${VCPKG_INSTALLED_DIR}" STREQUAL "")
            set(_VCPKG_INSTALLED_DIR "${VCPKG_INSTALLED_DIR}")
        else()
            set(_VCPKG_INSTALLED_DIR "${CMAKE_BINARY_DIR}/vcpkg_installed")
        endif()
    endif()
    find_library(_PIXMAN_LIBRARY
        NAMES pixman-1 libpixman-1
        PATHS "${_VCPKG_INSTALLED_DIR}/${VCPKG_TARGET_TRIPLET}/lib"
        NO_DEFAULT_PATH
    )
    if(_PIXMAN_LIBRARY)
        add_library(pixman_vcpkg UNKNOWN IMPORTED)
        set_target_properties(pixman_vcpkg PROPERTIES
            IMPORTED_LOCATION "${_PIXMAN_LIBRARY}"
            INTERFACE_INCLUDE_DIRECTORIES "${_VCPKG_INSTALLED_DIR}/${VCPKG_TARGET_TRIPLET}/include/pixman-1"
        )
        set(PIXMAN_TARGET pixman_vcpkg)
    endif()
else()
    find_package(unofficial-pixman CONFIG QUIET)
    if(unofficial-pixman_FOUND)
        set(PIXMAN_TARGET unofficial::pixman::pixman)
    else()
        find_package(PkgConfig QUIET)
        if(PkgConfig_FOUND)
            pkg_check_modules(PIXMAN IMPORTED_TARGET pixman-1)
            if(PIXMAN_FOUND)
                set(PIXMAN_TARGET PkgConfig::PIXMAN)
            endif()
        endif()
        if(NOT DEFINED PIXMAN_TARGET)
            # vcpkg's pixman port ships no CMake config, and a plain MSVC/Ninja
            # install has no pkg-config either, so the plugin used to be skipped
            # silently on Windows.  Locate the library and headers directly so
            # FilterPixman (and therefore --filter=pwm) is actually built.
            find_path(PIXMAN_INCLUDE_DIR NAMES pixman.h PATH_SUFFIXES pixman-1)
            find_library(PIXMAN_LIBRARY NAMES pixman-1 libpixman-1)
            if(PIXMAN_INCLUDE_DIR AND PIXMAN_LIBRARY)
                add_library(pixman_vcpkg UNKNOWN IMPORTED)
                set_target_properties(pixman_vcpkg PROPERTIES
                    IMPORTED_LOCATION "${PIXMAN_LIBRARY}"
                    INTERFACE_INCLUDE_DIRECTORIES "${PIXMAN_INCLUDE_DIR}")
                set(PIXMAN_TARGET pixman_vcpkg)
            endif()
        endif()
    endif()
endif()

if(DEFINED PIXMAN_TARGET)
    message(STATUS "Plugin: FilterPixman enabled (pixman found)")
else()
    message(STATUS "Plugin: FilterPixman skipped (pixman not found)")
endif()

# ── Vulkan headers + volk (third-party/volk submodule) ───────────────────────
# volk is the only Vulkan binding: every entry point is a function pointer loaded
# from the platform loader at runtime, so nothing links against vulkan-1.lib and
# no GL loader (glad) is needed any more.
find_path(VULKAN_INCLUDE_DIR
    NAMES vulkan/vulkan.h
    HINTS ENV VULKAN_SDK
    PATH_SUFFIXES Include include
)
if(NOT VULKAN_INCLUDE_DIR)
    message(FATAL_ERROR
        "Vulkan headers not found (<vulkan/vulkan.h>). Install the Vulkan SDK and "
        "set VULKAN_SDK, or add the vcpkg 'vulkan-headers' port to the viewer feature.")
endif()
message(STATUS "Vulkan headers: ${VULKAN_INCLUDE_DIR}")

set(VOLK_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../../third-party/volk")
if(NOT EXISTS "${VOLK_DIR}/volk.c")
    message(FATAL_ERROR
        "third-party/volk is missing. Run: git submodule update --init --recursive")
endif()

add_library(volk STATIC ${VOLK_DIR}/volk.c)
target_include_directories(volk PUBLIC ${VOLK_DIR} ${VULKAN_INCLUDE_DIR})
target_compile_definitions(volk PUBLIC VK_NO_PROTOTYPES)

# ── Dear ImGui (third-party/imgui submodule, pinned at v1.93.0) ──────────────
# ImGui is compiled as its own static library and linked into the viewer.  The
# OpenGL3 backend is replaced by the Vulkan backend, built with
# IMGUI_IMPL_VULKAN_USE_VOLK so it shares volk's function pointers with the rest
# of the executable: exactly one Vulkan loader, and no GL loader at all.
set(IMGUI_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../../third-party/imgui")

add_library(imgui STATIC
    ${IMGUI_DIR}/imgui.cpp
    ${IMGUI_DIR}/imgui_draw.cpp
    ${IMGUI_DIR}/imgui_tables.cpp
    ${IMGUI_DIR}/imgui_widgets.cpp
    ${IMGUI_DIR}/backends/imgui_impl_glfw.cpp
    ${IMGUI_DIR}/backends/imgui_impl_vulkan.cpp
)
target_include_directories(imgui PUBLIC
    ${IMGUI_DIR}
    ${IMGUI_DIR}/backends
)
target_compile_definitions(imgui PRIVATE
    GLFW_INCLUDE_NONE
)
target_compile_definitions(imgui PUBLIC
    IMGUI_IMPL_VULKAN_USE_VOLK
)
target_link_libraries(imgui PUBLIC
    ${_GLFW_TARGET}
    volk
)
set_property(TARGET imgui PROPERTY CXX_STANDARD 17)

# Main executable
if(WIN32)
    add_executable(iview WIN32)
else()
    add_executable(iview)
endif()


add_library(imageviewer)

target_sources(imageviewer PUBLIC
    FILE_SET imageviewer TYPE CXX_MODULES
    FILES
        Window.ixx
        VulkanContext.ixx
        VulkanRenderer.ixx
        Image.ixx
        HotkeysHandler.ixx
        PluginManager.ixx
        FilterPlugin.ixx
)

if(WIN32)
    target_compile_definitions(iview PRIVATE
        WIN32_LEAN_AND_MEAN
        NOMINMAX
        UNICODE
        _UNICODE
    )
    target_compile_definitions(imageviewer PUBLIC
        WIN32_LEAN_AND_MEAN
        NOMINMAX
        UNICODE
        _UNICODE
    )
    if(CMAKE_CXX_COMPILER_ID MATCHES "Clang|GNU")
        target_link_options(iview PRIVATE -municode)
    endif()
    target_sources(iview PRIVATE
        main_windows.cpp
    )
else()
    target_sources(iview PRIVATE
        main_unix.cpp
    )
endif()


set_property(TARGET imageviewer PROPERTY CXX_STANDARD 23)
set_property(TARGET iview PROPERTY CXX_STANDARD 23)

find_package(PNG REQUIRED)

target_link_libraries(imageviewer PRIVATE
    ${_GLFW_TARGET}
    imgui
)

target_link_libraries(iview PRIVATE
    ${_GLFW_TARGET}
    imageviewer
)

# Copying runtime DLLs next to iview is a convenience for launching the binary
# directly on a Windows desktop; the build itself does not need it, because the
# tests and the app are run with the toolchain's own bin directory on PATH.
# It is also not always possible: CMake's Ninja generator implements POST_BUILD
# by nesting `cmd.exe /C` inside another `cmd.exe /C`, and Wine's cmd cannot
# parse that, so the link step of a target carrying a POST_BUILD command fails
# under LinSYS2 even though the binary itself linked correctly.  CI therefore
# sets this to OFF and packages DLLs explicitly.
option(IMAGEVIEWER_COPY_RUNTIME_DLLS
    "Copy compiler/runtime DLLs next to iview after linking (Windows desktop convenience)" ON)

# vcpkg's applocal handles dependency DLLs from vcpkg, but not compiler runtime
# DLLs such as libc++.dll/libunwind.dll used by llvm-mingw toolchains.
if(WIN32 AND IMAGEVIEWER_COPY_RUNTIME_DLLS)
    get_filename_component(_CXX_COMPILER_DIR "${CMAKE_CXX_COMPILER}" DIRECTORY)
    set(_MINGW_RUNTIME_DLL_CANDIDATES
        "${_CXX_COMPILER_DIR}/libc++.dll"
        "${_CXX_COMPILER_DIR}/libunwind.dll"
        "${_CXX_COMPILER_DIR}/libomp.dll"
        "${_CXX_COMPILER_DIR}/libwinpthread-1.dll"
        "${_CXX_COMPILER_DIR}/libgcc_s_seh-1.dll"
        "${_CXX_COMPILER_DIR}/libstdc++-6.dll"
        "${_CXX_COMPILER_DIR}/../x86_64-w64-mingw32/bin/libc++.dll"
        "${_CXX_COMPILER_DIR}/../x86_64-w64-mingw32/bin/libunwind.dll"
        "${_CXX_COMPILER_DIR}/../x86_64-w64-mingw32/bin/libomp.dll"
        "/opt/llvm-mingw/x86_64-w64-mingw32/bin/libomp.dll"
        "${_CXX_COMPILER_DIR}/../x86_64-w64-mingw32/bin/libwinpthread-1.dll"
        "${_CXX_COMPILER_DIR}/../x86_64-w64-mingw32/bin/libgcc_s_seh-1.dll"
        "${_CXX_COMPILER_DIR}/../x86_64-w64-mingw32/bin/libstdc++-6.dll"
    )

    set(_MINGW_RUNTIME_DLLS "")
    foreach(_dll IN LISTS _MINGW_RUNTIME_DLL_CANDIDATES)
        if(EXISTS "${_dll}")
            list(APPEND _MINGW_RUNTIME_DLLS "${_dll}")
        endif()
    endforeach()
    list(REMOVE_DUPLICATES _MINGW_RUNTIME_DLLS)

    if(_MINGW_RUNTIME_DLLS)
        add_custom_command(TARGET iview POST_BUILD
            COMMAND ${CMAKE_COMMAND} -E copy_if_different
                ${_MINGW_RUNTIME_DLLS}
                $<TARGET_FILE_DIR:iview>
            COMMAND_EXPAND_LISTS
            VERBATIM
        )
    endif()
endif()

# JPEG Plugin DLL — only when libjpeg-turbo is available
if(IMAGEVIEWER_JPEG_TARGET)
    add_library(ImageLoaderJpeg SHARED)
    list(APPEND _PLUGIN_TARGETS ImageLoaderJpeg)
    target_sources(ImageLoaderJpeg PUBLIC
        FILE_SET jpegloader TYPE CXX_MODULES
        FILES
            ImageLoaderJpeg.ixx
    )
    set_property(TARGET ImageLoaderJpeg PROPERTY CXX_STANDARD 23)
    target_link_libraries(ImageLoaderJpeg PRIVATE
        ${IMAGEVIEWER_JPEG_TARGET}
    )
endif()

# PNG Plugin DLL — always built (libpng is a default vcpkg dep)
add_library(ImageLoaderPng SHARED)
list(APPEND _PLUGIN_TARGETS ImageLoaderPng)
target_sources(ImageLoaderPng PUBLIC
    FILE_SET pngloader TYPE CXX_MODULES
    FILES
        ImageLoaderPng.ixx
)
set_property(TARGET ImageLoaderPng PROPERTY CXX_STANDARD 23)
target_include_directories(ImageLoaderPng PRIVATE
    ${PNG_INCLUDE_DIRS}
)
target_link_libraries(ImageLoaderPng PRIVATE
    PNG::PNG
)

# WebP Plugin DLL — only when libwebp is available
if(WebP_FOUND)
    add_library(ImageLoaderWebp SHARED)
    list(APPEND _PLUGIN_TARGETS ImageLoaderWebp)
    target_sources(ImageLoaderWebp PUBLIC
        FILE_SET webploader TYPE CXX_MODULES
        FILES
            ImageLoaderWebp.ixx
    )
    set_property(TARGET ImageLoaderWebp PROPERTY CXX_STANDARD 23)
    target_link_libraries(ImageLoaderWebp PRIVATE
        WebP::webp
        WebP::webpdecoder
    )
endif()

# TIFF Plugin DLL — only when libtiff is available
if(TIFF_FOUND)
    add_library(ImageLoaderTiff SHARED)
    list(APPEND _PLUGIN_TARGETS ImageLoaderTiff)
    target_sources(ImageLoaderTiff PUBLIC
        FILE_SET tiffloader TYPE CXX_MODULES
        FILES
            ImageLoaderTiff.ixx
    )
    set_property(TARGET ImageLoaderTiff PROPERTY CXX_STANDARD 23)
    target_link_libraries(ImageLoaderTiff PRIVATE
        TIFF::TIFF
    )
endif()

# Stb Plugin DLL — only when stb is available
if(Stb_FOUND)
    add_library(ImageLoaderStb SHARED)
    list(APPEND _PLUGIN_TARGETS ImageLoaderStb)
    target_sources(ImageLoaderStb PUBLIC
        FILE_SET stbloader TYPE CXX_MODULES
        FILES
            ImageLoaderStb.ixx
    )
    set_property(TARGET ImageLoaderStb PROPERTY CXX_STANDARD 23)
    target_include_directories(ImageLoaderStb PRIVATE ${Stb_INCLUDE_DIR})
endif()

# FilterPixman Plugin DLL — only when pixman is available
if(DEFINED PIXMAN_TARGET AND PIXMAN_TARGET)
    add_library(FilterPixman SHARED)
    list(APPEND _PLUGIN_TARGETS FilterPixman)
    target_sources(FilterPixman PUBLIC
        FILE_SET filterpixman TYPE CXX_MODULES
        FILES
            FilterPluginPixman.ixx
    )
    set_property(TARGET FilterPixman PROPERTY CXX_STANDARD 23)
    target_include_directories(FilterPixman PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}
    )
    target_link_libraries(FilterPixman PRIVATE ${PIXMAN_TARGET})

endif()
# ── Ship the plugins with the viewer ────────────────────────────────────────
# The plugin libraries are built into the same directory as iview by default, but
# nothing in the target graph said so: building only the iview target skipped the
# plugins entirely, which left a Release folder with no loadable image loaders or
# filter.  Make the dependency explicit and copy every plugin next to the
# executable, so any build (single- or multi-config) ships a runnable viewer.
if(_PLUGIN_TARGETS)
    add_dependencies(iview ${_PLUGIN_TARGETS})
    set(_PLUGIN_FILES "")
    foreach(_plugin IN LISTS _PLUGIN_TARGETS)
        if(TARGET ${_plugin})
            list(APPEND _PLUGIN_FILES "$<TARGET_FILE:${_plugin}>")
        endif()
    endforeach()
    add_custom_command(TARGET iview POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
            ${_PLUGIN_FILES}
            $<TARGET_FILE_DIR:iview>
        COMMAND_EXPAND_LISTS
        VERBATIM
    )
    message(STATUS "Plugins shipped with iview: ${_PLUGIN_TARGETS}")
endif()

# Packaging path: `cmake --install` produces a folder containing iview.exe and
# every plugin (the runtime DLLs beside them come from vcpkg's applocal step).
install(TARGETS iview RUNTIME DESTINATION .)
if(_PLUGIN_TARGETS)
    install(TARGETS ${_PLUGIN_TARGETS} RUNTIME DESTINATION .)
endif()
