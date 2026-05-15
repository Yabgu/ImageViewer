find_package(OpenMP)
# For cross-compilation targets, skip OpenMP since module metadata is strict about compile flags
# and inconsistent OpenMP flags between modules cause precompilation metadata mismatches
if (OPENMP_FOUND AND NOT CMAKE_CROSSCOMPILING)
    set (CMAKE_C_FLAGS "${CMAKE_C_FLAGS} ${OpenMP_C_FLAGS}")
    set (CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} ${OpenMP_CXX_FLAGS}")

    if(CMAKE_CXX_COMPILER_ID MATCHES "MSVC")
        set (CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} /openmp:llvm /openmp:experimental")
        set (OpenMP_RUNTIME_MSVC llvm CACHE STRING "Specify the OpenMP Runtime for MSVC")
    endif()

    set (CMAKE_EXE_LINKER_FLAGS "${CMAKE_EXE_LINKER_FLAGS} ${OpenMP_EXE_LINKER_FLAGS}")
endif()

if(NOT IMAGEVIEWER_BUILD_VIEWER)
    message(STATUS "IMAGEVIEWER_BUILD_VIEWER=OFF — skipping GUI viewer build")
    return()
endif()

if(IMAGEVIEWER_USE_VCPKG)
    find_package(glfw3 CONFIG REQUIRED)
    find_package(glad CONFIG REQUIRED)
    set(_GLFW_TARGET glfw)
    set(_GLAD_TARGET glad::glad)
else()
    list(APPEND CMAKE_MODULE_PATH "${CMAKE_SOURCE_DIR}/cmake")
    find_package(GLFW3 REQUIRED)
    find_package(Glad REQUIRED)
    set(_GLFW_TARGET glfw3::glfw3)
    set(_GLAD_TARGET glad::glad)
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
    endif()
endif()

if(DEFINED PIXMAN_TARGET)
    message(STATUS "Plugin: FilterPixman enabled (pixman found)")
else()
    message(STATUS "Plugin: FilterPixman skipped (pixman not found)")
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
        TexturePool.ixx
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
    ${_GLAD_TARGET}
)

target_link_libraries(iview PRIVATE
    ${_GLFW_TARGET}
    ${_GLAD_TARGET}
    imageviewer
)

if(EMSCRIPTEN)
    target_link_options(iview PRIVATE "-sMAIN_MODULE=2")
endif()

function(iw_configure_wasm_plugin target_name)
    if(EMSCRIPTEN)
        target_link_options(${target_name} PRIVATE "-sSIDE_MODULE=2")
        set_target_properties(${target_name} PROPERTIES
            PREFIX ""
            SUFFIX ".wasm"
        )
    endif()
endfunction()

# vcpkg's applocal handles dependency DLLs from vcpkg, but not compiler runtime
# DLLs such as libc++.dll/libunwind.dll used by llvm-mingw toolchains.
if(WIN32)
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
    target_sources(ImageLoaderJpeg PUBLIC
        FILE_SET jpegloader TYPE CXX_MODULES
        FILES
            ImageLoaderJpeg.ixx
    )
    set_property(TARGET ImageLoaderJpeg PROPERTY CXX_STANDARD 23)
    target_link_libraries(ImageLoaderJpeg PRIVATE
        ${IMAGEVIEWER_JPEG_TARGET}
    )
    iw_configure_wasm_plugin(ImageLoaderJpeg)
endif()

# PNG Plugin DLL — always built (libpng is a default vcpkg dep)
add_library(ImageLoaderPng SHARED)
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
iw_configure_wasm_plugin(ImageLoaderPng)

# WebP Plugin DLL — only when libwebp is available
if(WebP_FOUND)
    add_library(ImageLoaderWebp SHARED)
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
    iw_configure_wasm_plugin(ImageLoaderWebp)
endif()

# TIFF Plugin DLL — only when libtiff is available
if(TIFF_FOUND)
    add_library(ImageLoaderTiff SHARED)
    target_sources(ImageLoaderTiff PUBLIC
        FILE_SET tiffloader TYPE CXX_MODULES
        FILES
            ImageLoaderTiff.ixx
    )
    set_property(TARGET ImageLoaderTiff PROPERTY CXX_STANDARD 23)
    target_link_libraries(ImageLoaderTiff PRIVATE
        TIFF::TIFF
    )
    iw_configure_wasm_plugin(ImageLoaderTiff)
endif()

# Stb Plugin DLL — only when stb is available
if(Stb_FOUND)
    add_library(ImageLoaderStb SHARED)
    target_sources(ImageLoaderStb PUBLIC
        FILE_SET stbloader TYPE CXX_MODULES
        FILES
            ImageLoaderStb.ixx
    )
    set_property(TARGET ImageLoaderStb PROPERTY CXX_STANDARD 23)
    target_include_directories(ImageLoaderStb PRIVATE ${Stb_INCLUDE_DIR})
    iw_configure_wasm_plugin(ImageLoaderStb)
endif()

# FilterPixman Plugin DLL — only when pixman is available
if(DEFINED PIXMAN_TARGET AND PIXMAN_TARGET)
    add_library(FilterPixman SHARED)
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
    iw_configure_wasm_plugin(FilterPixman)

    # Copy FilterPixman DLL next to iview on Windows (same logic as runtime DLLs)
    if(WIN32)
        add_custom_command(TARGET iview POST_BUILD
            COMMAND ${CMAKE_COMMAND} -E copy_if_different
                $<TARGET_FILE:FilterPixman>
                $<TARGET_FILE_DIR:iview>
            VERBATIM
        )
    endif()
endif()
