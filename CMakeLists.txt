cmake_minimum_required(VERSION 3.19)
project(ImageViewer)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED on)

add_compile_options(-fmodules-ts)

include(third-party/vcpkg/scripts/buildsystems/vcpkg.cmake) 

add_subdirectory(src)
