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

# ── The .iwi container format ──────────────────────────────────────────────
#
# IwiFile.h is the public C API and IwiFile.cpp is the one implementation of
# the container rules.  They live here, beside IWImageIO.ixx, because the
# grammar is a fact about the file format rather than about the viewer:
# Tension, iwi.mjs and a C game engine bind this surface, and none of them link
# viewer code.
#
# A real static library, not INTERFACE: the C ABI has symbols to link.
# IwiFile.h is one of the two headers the CI C11 gate compiles as C.
#
# Two include directories because the header set spans both: IwiFile.h is here,
# while ImagePluginDef.h - the component and colour-space enumerations it uses -
# is the plugin ABI and stays with the plugins in src/imageviewer.
add_library(iw_iwi_file STATIC)

target_sources(iw_iwi_file
    PRIVATE
        IwiFile.cpp
    PUBLIC
        FILE_SET iwi_file TYPE HEADERS
        BASE_DIRS
            # The header set spans two directories, so both are declared as
            # bases.  CMake rejects a FILE_SET entry outside its base
            # directories, and ImagePluginDef.h is the plugin ABI and belongs
            # with the plugins rather than being copied next to this header.
            ${CMAKE_CURRENT_SOURCE_DIR}
            ${CMAKE_CURRENT_SOURCE_DIR}/../imageviewer
        FILES
            IwiFile.h
            ${CMAKE_CURRENT_SOURCE_DIR}/../imageviewer/ImagePluginDef.h
)

target_include_directories(iw_iwi_file PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${CMAKE_CURRENT_SOURCE_DIR}/../imageviewer
)

set_target_properties(iw_iwi_file PROPERTIES
    CXX_STANDARD 20
    CXX_STANDARD_REQUIRED ON
)

# IWImageIO.ixx calls iwi_parse_structural / iwi_payload_encoding_is_raw from its
# global module fragment: the .iwi grammar lives in IwiFile.cpp and this module
# must not carry a second copy of it.
target_link_libraries(iw_format PUBLIC iw_iwi_file)
