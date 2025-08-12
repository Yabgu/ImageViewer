cmake_minimum_required(VERSION 3.30)
project(wasmedge_glue)

add_executable(wasmedge_glue wasmedge_glue.cpp)

find_package(WasmEdge CONFIG REQUIRED)
target_link_libraries(wasmedge_glue PRIVATE WasmEdge::wasmedge_c)

target_include_directories(wasmedge_glue PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
