if(NOT TARGET iw_format)
    message(STATUS "iw_format not available (protobuf missing) — iwconv will not be built")
    return()
endif()

find_package(PNG REQUIRED)

add_executable(iwconv iwconv.cpp)

set_property(TARGET iwconv PROPERTY CXX_STANDARD 17)

target_link_libraries(iwconv PRIVATE
    iw_format
    PNG::PNG
)
