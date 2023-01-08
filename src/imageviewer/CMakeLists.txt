
find_package(OpenMP)
if (OPENMP_FOUND)
    set (CMAKE_C_FLAGS "${CMAKE_C_FLAGS} ${OpenMP_C_FLAGS}")
    set (CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} ${OpenMP_CXX_FLAGS}")

    if(CMAKE_CXX_COMPILER_ID MATCHES "MSVC")
        set (CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -openmp:llvm -openmp:experimental")
    endif()

    set (CMAKE_EXE_LINKER_FLAGS "${CMAKE_EXE_LINKER_FLAGS} ${OpenMP_EXE_LINKER_FLAGS}")
endif()

find_package(JPEG REQUIRED)
find_package(glfw3 CONFIG REQUIRED)
find_package(glad CONFIG REQUIRED)
set(Boost_USE_STATIC_LIBS ON)
find_package(Boost REQUIRED COMPONENTS thread)

# Main executable
if(WIN32)
    add_executable(ImageViewer WIN32)
else()
    add_executable(ImageViewer)
endif()

target_sources(
    ImageViewer
        PRIVATE
            main.cpp
            Window.ixx
            TexturePool.ixx
            Image.ixx
)

set_property(TARGET ImageViewer PROPERTY CXX_STANDARD 20)

target_include_directories(
    ImageViewer
        PRIVATE
            ${JPEG_INCLUDE_DIR}
            ${Boost_INCLUDE_DIRS}
)

target_link_libraries(
    ImageViewer
        PRIVATE
            glfw
            glad::glad
            Boost::thread
            JPEG::JPEG
)
