if(NOT WIN32)
find_package(OpenMP)
if (OPENMP_FOUND)
    set (CMAKE_C_FLAGS "${CMAKE_C_FLAGS} ${OpenMP_C_FLAGS}")
    set (CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} ${OpenMP_CXX_FLAGS}")

    if(CMAKE_CXX_COMPILER_ID MATCHES "MSVC")
        set (CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} /openmp:llvm /openmp:experimental")
        set (OpenMP_RUNTIME_MSVC llvm CACHE STRING "Specify the OpenMP Runtime for MSVC")
    endif()

    set (CMAKE_EXE_LINKER_FLAGS "${CMAKE_EXE_LINKER_FLAGS} ${OpenMP_EXE_LINKER_FLAGS}")
endif()
endif()

find_package(JPEG REQUIRED)
set(IMAGEVIEWER_JPEG_TARGET JPEG::JPEG)
find_package(glfw3 CONFIG REQUIRED)
find_package(glad CONFIG REQUIRED)
find_package(WebP CONFIG REQUIRED)
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
    if(NOT _TIFF_LIBRARY)
        message(FATAL_ERROR "Could not locate TIFF library for triplet ${VCPKG_TARGET_TRIPLET}")
    endif()
    add_library(TIFF::TIFF UNKNOWN IMPORTED)
    set_target_properties(TIFF::TIFF PROPERTIES
        IMPORTED_LOCATION "${_TIFF_LIBRARY}"
        INTERFACE_INCLUDE_DIRECTORIES "${_TIFF_INCLUDE_DIR}"
    )
else()
    find_package(TIFF REQUIRED)
endif()
find_package(Stb REQUIRED)

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
)

target_sources(iview PRIVATE
    main.cpp
)


set_property(TARGET imageviewer PROPERTY CXX_STANDARD 23)
set_property(TARGET iview PROPERTY CXX_STANDARD 23)

find_package(PNG REQUIRED)

target_link_libraries(imageviewer PRIVATE
    glfw
    glad::glad
)

target_link_libraries(iview PRIVATE
    glfw
    glad::glad
    imageviewer
)

# JPEG Plugin DLL
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

# PNG Plugin DLL
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

# WebP Plugin DLL
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

# TIFF Plugin DLL
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

# Stb Plugin DLL (BMP, TGA, GIF, HDR, PIC, PPM, PGM)
add_library(ImageLoaderStb SHARED)
target_sources(ImageLoaderStb PUBLIC
    FILE_SET stbloader TYPE CXX_MODULES
    FILES
        ImageLoaderStb.ixx
)
set_property(TARGET ImageLoaderStb PROPERTY CXX_STANDARD 23)
target_include_directories(ImageLoaderStb PRIVATE ${Stb_INCLUDE_DIR})
