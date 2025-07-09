
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

find_package(JPEG REQUIRED)
find_package(glfw3 CONFIG REQUIRED)
find_package(glad CONFIG REQUIRED)

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
)

target_sources(iview PRIVATE
    main.cpp
)


set_property(TARGET imageviewer PROPERTY CXX_STANDARD 23)
set_property(TARGET iview PROPERTY CXX_STANDARD 23)

target_include_directories(imageviewer PRIVATE
    ${JPEG_INCLUDE_DIR}
)

find_package(PNG REQUIRED)

target_include_directories(imageviewer PRIVATE
    ${JPEG_INCLUDE_DIR}
    ${PNG_INCLUDE_DIRS}
)

target_link_libraries(imageviewer PRIVATE
    glfw
    glad::glad
    JPEG::JPEG
    PNG::PNG
)

target_link_libraries(iview PRIVATE
    glfw
    glad::glad
    JPEG::JPEG
    imageviewer
)
