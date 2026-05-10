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

set_property(TARGET iw_format PROPERTY CXX_STANDARD 23)
