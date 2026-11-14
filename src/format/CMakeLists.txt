add_library(iw_format STATIC)

target_sources(iw_format PUBLIC
    FILE_SET iw_format_modules TYPE CXX_MODULES
    FILES
        IWImageIO.ixx
)

# Export the imageviewer source directory so IWImageIO.ixx can include
# ImagePluginDef.h from its global module fragment.
target_include_directories(iw_format PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/../imageviewer
)

# IWImageIO.ixx calls iwi_parse_structural / iwi_payload_encoding_is_raw from its
# global module fragment: the .iwi grammar lives in IwiFile.cpp and this module
# must not carry a second copy of it.
target_link_libraries(iw_format PUBLIC iw_iwi_file)

set_property(TARGET iw_format PROPERTY CXX_STANDARD 23)
