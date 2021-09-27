cmake_minimum_required(VERSION 3.2)
project(ImageViewer)

find_package(OpenMP)
if (OPENMP_FOUND)
    set (CMAKE_C_FLAGS "${CMAKE_C_FLAGS} ${OpenMP_C_FLAGS}")
    set (CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} ${OpenMP_CXX_FLAGS}")

    if(CMAKE_CXX_COMPILER_ID MATCHES "MSVC")
        set (CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -openmp:llvm -openmp:experimental")
    endif()

    set (CMAKE_EXE_LINKER_FLAGS "${CMAKE_EXE_LINKER_FLAGS} ${OpenMP_EXE_LINKER_FLAGS}")
endif()

find_package(SDL2 CONFIG REQUIRED)
find_package(sdl2-image CONFIG REQUIRED)
find_package(JPEG REQUIRED)
#get_target_property(SDL2_INCLUDE_DIRS SDL2::SDL2 INTERFACE_INCLUDE_DIRECTORIES)
#get_target_property(SDL2_IMAGE_INCLUDE_DIRS SDL2::SDL2_image INTERFACE_INCLUDE_DIRECTORIES)


# Main executable
add_executable(ImageViewer main.cpp)
set_property(TARGET ImageViewer PROPERTY CXX_STANDARD 20)
#target_include_directories(ImageViewer PUBLIC ${SDL2_INCLUDE_DIRS})
#target_include_directories(ImageViewer PUBLIC ${SDL2_IMAGE_INCLUDE_DIRS})

target_include_directories(
    ImageViewer
        PRIVATE
            ${JPEG_INCLUDE_DIR}
)

target_link_libraries(
    ImageViewer
        PRIVATE
            SDL2::SDL2
            SDL2::SDL2main
            SDL2::SDL2_image
            ${JPEG_LIBRARIES}
)
