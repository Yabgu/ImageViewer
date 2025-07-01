cmake_minimum_required(VERSION 3.30)
project(ImageViewer)

set(CMAKE_EXPORT_COMPILE_COMMANDS ON)
set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

include(third-party/vcpkg/scripts/buildsystems/vcpkg.cmake)

add_subdirectory(src)
