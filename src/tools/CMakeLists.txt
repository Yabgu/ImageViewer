if(NOT TARGET iw_format)
    message(STATUS "iw_format not available (protobuf missing) — iwconv will not be built")
    return()
endif()

if(NOT TARGET imageviewer)
    message(STATUS "imageviewer not available (IMAGEVIEWER_BUILD_VIEWER=OFF) — iwconv will not be built")
    return()
endif()

find_package(PNG REQUIRED)

add_executable(iwconv iwconv.cpp)

set_property(TARGET iwconv PROPERTY CXX_STANDARD 23)

target_link_libraries(iwconv PRIVATE
    imageviewer
    iw_format
    PNG::PNG
)
