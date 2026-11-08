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

# ── Image-loader and filter plugins ─────────────────────────────────────────
#
# The loaders and the filter need their own format libraries and nothing
# else.  They do NOT need GLFW, GLAD or imgui, so they are gated on
# IMAGEVIEWER_BUILD_PLUGINS rather than on the GUI: a machine with no
# display stack can still build and test every plugin, which is what makes
# the plugin ABI testable in CI.

find_package(PNG REQUIRED)

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

# giflib.  CMake ships FindGIF, so no local module is needed and there is no
# vcpkg-config-only failure mode like the one Stb has (see docs/build-msys2.md
# §9.2).  QUIET for the same reason the others are: the plugin is optional and
# a build without libgif simply has no GIF loader rather than failing.
find_package(GIF QUIET)
if(GIF_FOUND)
    message(STATUS "Plugin: ImageLoaderGif enabled (giflib found)")
else()
    message(STATUS "Plugin: ImageLoaderGif skipped (giflib not found)")
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

# ── Plugin host ─────────────────────────────────────────────────────────────
#
# PluginManager owns dlopen/LoadLibrary and the cached handle table.  It is
# a target of its own so that a headless consumer can import it without
# inheriting the GLFW/GLAD/imgui requirements the imageviewer archive
# carries into anything that links it.

add_library(iw_plugin_host STATIC)

target_sources(iw_plugin_host PUBLIC
    FILE_SET plugin_host TYPE CXX_MODULES
    FILES
        PluginManager.ixx
)

target_include_directories(iw_plugin_host PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}
)

set_property(TARGET iw_plugin_host PROPERTY CXX_STANDARD 23)

# ── Plugin DSOs ─────────────────────────────────────────────────────────────
set(_PLUGIN_TARGETS "")
if(IMAGEVIEWER_BUILD_PLUGINS)

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

# GIF Plugin DLL — only when giflib is available
if(GIF_FOUND)
    add_library(ImageLoaderGif SHARED)
    list(APPEND _PLUGIN_TARGETS ImageLoaderGif)
    target_sources(ImageLoaderGif PUBLIC
        FILE_SET gifloader TYPE CXX_MODULES
        FILES
            ImageLoaderGif.ixx
    )
    set_property(TARGET ImageLoaderGif PROPERTY CXX_STANDARD 23)
    target_link_libraries(ImageLoaderGif PRIVATE GIF::GIF)
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

# Packaging path for the plugins: `cmake --install` collects them next to
# iview.exe.  Installed here rather than with the viewer so a headless
# build installs them too.
if(_PLUGIN_TARGETS)
    install(TARGETS ${_PLUGIN_TARGETS} RUNTIME DESTINATION .)
endif()

else()
    message(STATUS "IMAGEVIEWER_BUILD_PLUGINS=OFF — skipping image-loader and filter plugins")
endif()

# ── GUI viewer ──────────────────────────────────────────────────────────────
# Everything below needs GLFW, GLAD and imgui.

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

# ── GL loader (glad) ────────────────────────────────────────────────────────
# Exactly one GL loader in the process: glad.  ImGui's OpenGL3 backend is built
# with IMGUI_IMPL_OPENGL_LOADER_CUSTOM and force-includes glad/glad.h, so it
# resolves the same symbols the Window module uses.
if(IMAGEVIEWER_USE_VCPKG)
    find_package(glad CONFIG REQUIRED)
    set(_GLAD_TARGET glad::glad)
else()
    find_package(Glad REQUIRED)
    set(_GLAD_TARGET glad::glad)
endif()

# ── Dear ImGui (third-party/imgui submodule, pinned at v1.93.0) ──────────────
set(IMGUI_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../../third-party/imgui")

add_library(imgui STATIC
    ${IMGUI_DIR}/imgui.cpp
    ${IMGUI_DIR}/imgui_draw.cpp
    ${IMGUI_DIR}/imgui_tables.cpp
    ${IMGUI_DIR}/imgui_widgets.cpp
    ${IMGUI_DIR}/backends/imgui_impl_glfw.cpp
    ${IMGUI_DIR}/backends/imgui_impl_opengl3.cpp
)
target_include_directories(imgui PUBLIC
    ${IMGUI_DIR}
    ${IMGUI_DIR}/backends
)
target_compile_definitions(imgui PRIVATE
    GLFW_INCLUDE_NONE
    IMGUI_IMPL_OPENGL_LOADER_CUSTOM
)
target_link_libraries(imgui PUBLIC
    ${_GLFW_TARGET}
    ${_GLAD_TARGET}
)
set_property(TARGET imgui PROPERTY CXX_STANDARD 17)

if(MSVC)
    set_source_files_properties(${IMGUI_DIR}/backends/imgui_impl_opengl3.cpp
        PROPERTIES COMPILE_OPTIONS "/FIglad/glad.h")
else()
    set_source_files_properties(${IMGUI_DIR}/backends/imgui_impl_opengl3.cpp
        PROPERTIES COMPILE_OPTIONS "-include;glad/glad.h")
endif()

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
        Image.ixx
        HotkeysHandler.ixx
        FilterPlugin.ixx
)


option(IMAGEVIEWER_GENERATE_ICON
    "Generate the Windows .ico from assets/icon/iview.svg at build time (needs Python 3 and rsvg-convert)"
    ON)

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

    # ── Application icon ────────────────────────────────────────────────────
    # resources/iview.rc embeds the .ico as the executable's icon (plus the
    # named GLFW_ICON copy GLFW's Win32 backend loads for its window class).
    #
    # The .ico is normally generated from assets/icon/iview.svg at build time,
    # so editing the SVG is all it takes to change the icon in the exe.  That
    # step needs Python 3 and rsvg-convert; when either is unavailable the
    # build falls back to the checked-in resources/iview.ico, which is also
    # what keeps a source tarball buildable without librsvg.
    #
    # The .rc is rendered into the build tree with the icon's absolute path
    # substituted.  That is deliberate: rc.exe searches the .rc file's own
    # directory before its /I paths, so with a relative name a stale copy
    # sitting beside the source .rc would win without any diagnostic.  An
    # absolute path leaves the toolchains no room to disagree -- rc.exe,
    # windres and llvm-windres all then read the same file.
    #
    # The .rc is produced by a build step (cmake/WriteIconRc.cmake) rather than
    # configure_file(), so it is an ordinary build output that a cleaned build
    # directory regenerates on its own.
    set(_IVIEW_ICON_SVG "${CMAKE_SOURCE_DIR}/assets/icon/iview.svg")
    set(_IVIEW_ICON_SCRIPT "${CMAKE_SOURCE_DIR}/src/tools/make_icon.py")
    set(_IVIEW_ICON_FALLBACK "${CMAKE_CURRENT_SOURCE_DIR}/resources/iview.ico")
    set(_IVIEW_ICON_DIR "${CMAKE_CURRENT_BINARY_DIR}/icon")
    set(_IVIEW_ICON_GENERATED "${_IVIEW_ICON_DIR}/iview.ico")

    set(IMAGEVIEWER_RSVG_CONVERT "" CACHE FILEPATH
        "rsvg-convert used to rasterize the icon SVG (empty: search PATH)")

    set(_IVIEW_ICON_ENV)
    if(IMAGEVIEWER_RSVG_CONVERT)
        set(_IVIEW_ICON_ENV "${CMAKE_COMMAND}" -E env
            "RSVG_CONVERT=${IMAGEVIEWER_RSVG_CONVERT}")
    endif()

    set(_IVIEW_ICON_CAN_GENERATE OFF)
    if(IMAGEVIEWER_GENERATE_ICON AND EXISTS "${_IVIEW_ICON_SVG}")
        find_package(Python3 COMPONENTS Interpreter QUIET)
        if(Python3_Interpreter_FOUND)
            execute_process(
                COMMAND ${_IVIEW_ICON_ENV} "${Python3_EXECUTABLE}"
                        "${_IVIEW_ICON_SCRIPT}" --check
                RESULT_VARIABLE _IVIEW_ICON_PROBE
                OUTPUT_VARIABLE _IVIEW_ICON_RSVG
                ERROR_QUIET
                OUTPUT_STRIP_TRAILING_WHITESPACE)
            if(_IVIEW_ICON_PROBE EQUAL 0)
                set(_IVIEW_ICON_CAN_GENERATE ON)
            endif()
        endif()
    endif()

    if(_IVIEW_ICON_CAN_GENERATE)
        add_custom_command(
            OUTPUT "${_IVIEW_ICON_GENERATED}"
            COMMAND "${CMAKE_COMMAND}" -E make_directory "${_IVIEW_ICON_DIR}"
            COMMAND ${_IVIEW_ICON_ENV} "${Python3_EXECUTABLE}"
                    "${_IVIEW_ICON_SCRIPT}"
                    --svg "${_IVIEW_ICON_SVG}"
                    --out "${_IVIEW_ICON_GENERATED}"
            DEPENDS "${_IVIEW_ICON_SVG}" "${_IVIEW_ICON_SCRIPT}"
            COMMENT "Generating iview.ico from assets/icon/iview.svg"
            VERBATIM)
        set(_IVIEW_ICON_FILE "${_IVIEW_ICON_GENERATED}")
        message(STATUS "Icon: generating iview.ico with ${_IVIEW_ICON_RSVG}")
    else()
        set(_IVIEW_ICON_FILE "${_IVIEW_ICON_FALLBACK}")
        message(STATUS
            "Icon: using checked-in resources/iview.ico "
            "(generation disabled or Python 3 / rsvg-convert not found)")
    endif()

    # Forward slashes, not native separators: rc.exe unescapes C-style
    # sequences inside resource filenames, so a path containing \r (as in
    # "...\resources\...") would be read as a carriage return and truncated.
    # Forward slashes are accepted by rc.exe, windres and llvm-windres alike
    # and never take part in that escape processing.
    set(_IVIEW_ICON_RC "${_IVIEW_ICON_DIR}/iview.rc")
    add_custom_command(
        OUTPUT "${_IVIEW_ICON_RC}"
        COMMAND "${CMAKE_COMMAND}" -E make_directory "${_IVIEW_ICON_DIR}"
        COMMAND "${CMAKE_COMMAND}"
                "-DIVIEW_ICON_FILE=${_IVIEW_ICON_FILE}"
                "-DIVIEW_ICON_RC_TEMPLATE=${CMAKE_CURRENT_SOURCE_DIR}/resources/iview.rc"
                "-DIVIEW_ICON_RC_OUTPUT=${_IVIEW_ICON_RC}"
                -P "${CMAKE_SOURCE_DIR}/cmake/WriteIconRc.cmake"
        DEPENDS
            "${CMAKE_CURRENT_SOURCE_DIR}/resources/iview.rc"
            "${CMAKE_SOURCE_DIR}/cmake/WriteIconRc.cmake"
        COMMENT "Writing iview.rc for the application icon"
        VERBATIM)

    target_sources(iview PRIVATE
        main_windows.cpp
        "${_IVIEW_ICON_RC}"
    )
    # Header search for ResourceIds.h; the .ico itself needs no include path.
    target_include_directories(iview PRIVATE
        "${CMAKE_CURRENT_SOURCE_DIR}/resources"
    )

    # rc.exe emits no depfile, so list the resource inputs explicitly: replacing
    # the .ico or changing the shared icon ID must rebuild and relink the exe.
    set_source_files_properties("${_IVIEW_ICON_RC}" PROPERTIES
        OBJECT_DEPENDS
            "${_IVIEW_ICON_FILE};${_IVIEW_ICON_RC};${CMAKE_CURRENT_SOURCE_DIR}/resources/ResourceIds.h"
    )
else()
    target_sources(iview PRIVATE
        main_unix.cpp
    )
endif()

set_property(TARGET imageviewer PROPERTY CXX_STANDARD 23)
set_property(TARGET iview PROPERTY CXX_STANDARD 23)

target_link_libraries(imageviewer PRIVATE
    ${_GLFW_TARGET}
    ${_GLAD_TARGET}
    imgui
)

# PluginManager lives in its own archive; the viewer imports it.
target_link_libraries(imageviewer PRIVATE iw_plugin_host)

target_link_libraries(iview PRIVATE
    ${_GLFW_TARGET}
    ${_GLAD_TARGET}
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

# ── Ship the plugins with the viewer ────────────────────────────────────────
# Nothing in the target graph used to say that iview needs the plugins, so
# building only the iview target skipped them entirely and left a folder with no
# loadable image loaders and no filter.  Make that dependency explicit.
#
# No copy step is needed: every plugin is add_library(SHARED) in this same
# CMakeLists, so its runtime output directory is iview's, in every generator
# and configuration (single-config: src/imageviewer; multi-config:
# src/imageviewer/<CONFIG>).  A POST_BUILD copy here would copy each DLL onto
# itself -- and, worse, CMake implements POST_BUILD by nesting `cmd.exe /C`
# inside the link rule's own `cmd.exe /C`, which Wine's cmd cannot parse, so it
# broke the LinSYS2 job's link step for no benefit.  `cmake --install` still
# collects both via the install() rules below, and ci/package-msys2.sh packages
# this same directory.
if(TARGET iview AND _PLUGIN_TARGETS)
    add_dependencies(iview ${_PLUGIN_TARGETS})
    message(STATUS "Plugins shipped with iview: ${_PLUGIN_TARGETS}")
endif()

# Packaging path: `cmake --install` produces a folder containing iview.exe and
# every plugin (the runtime DLLs beside them come from vcpkg's applocal step).
install(TARGETS iview RUNTIME DESTINATION .)
