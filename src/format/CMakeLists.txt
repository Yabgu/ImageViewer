find_package(Protobuf QUIET)

if(NOT Protobuf_FOUND)
    message(WARNING
        "protobuf not found — iw_format library, iwconv tool, and format tests "
        "will not be built. Install libprotobuf-dev or add protobuf to vcpkg.json.")
    return()
endif()

protobuf_generate_cpp(PROTO_SRCS PROTO_HDRS iw_image.proto)

add_library(iw_format STATIC ${PROTO_SRCS} ${PROTO_HDRS})

target_link_libraries(iw_format PUBLIC protobuf::libprotobuf)

# Export the generated header directory and the source directory (IWImageIO.hpp).
# Users of iw_format automatically get both include paths.
target_include_directories(iw_format PUBLIC
    ${CMAKE_CURRENT_BINARY_DIR}   # generated iw_image.pb.h
    ${CMAKE_CURRENT_SOURCE_DIR}   # IWImageIO.hpp
    ${CMAKE_CURRENT_SOURCE_DIR}/../imageviewer  # ImagePluginDef.h
)

set_property(TARGET iw_format PROPERTY CXX_STANDARD 17)
