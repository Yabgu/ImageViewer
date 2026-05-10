# FindGLFW3.cmake - Find GLFW3 library
#
# This module defines:
#   GLFW3_FOUND - System has GLFW3
#   GLFW3_INCLUDE_DIRS - Include directories
#   GLFW3_LIBRARIES - Libraries to link

find_package(PkgConfig QUIET)

if(PkgConfig_FOUND)
    pkg_check_modules(PC_GLFW3 glfw3)
endif()

find_path(GLFW3_INCLUDE_DIR
    NAMES GLFW/glfw3.h glfw3.h
    HINTS ${PC_GLFW3_INCLUDE_DIRS}
    PATHS /usr/include
          /usr/local/include
          /opt/local/include
          /sw/include
)

find_library(GLFW3_LIBRARY
    NAMES glfw glfw3 libglfw libglfw3
    HINTS ${PC_GLFW3_LIBRARY_DIRS}
    PATHS /usr/lib
          /usr/lib64
          /usr/lib/x86_64-linux-gnu
          /usr/local/lib
          /opt/local/lib
          /sw/lib
)

include(FindPackageHandleStandardArgs)
find_package_handle_standard_args(GLFW3 
    REQUIRED_VARS GLFW3_INCLUDE_DIR GLFW3_LIBRARY
    FAIL_MESSAGE "GLFW3 library not found. Install libglfw3-dev or glfw-devel package."
)

if(GLFW3_FOUND AND NOT TARGET glfw3::glfw3)
    add_library(glfw3::glfw3 UNKNOWN IMPORTED)
    set_target_properties(glfw3::glfw3 PROPERTIES
        IMPORTED_LOCATION "${GLFW3_LIBRARY}"
        INTERFACE_INCLUDE_DIRECTORIES "${GLFW3_INCLUDE_DIR}"
    )
    
    # Link against system libraries that GLFW depends on
    if(UNIX AND NOT APPLE)
        find_package(X11 REQUIRED)
        set_target_properties(glfw3::glfw3 PROPERTIES
            INTERFACE_LINK_LIBRARIES "X11::X11"
        )
    endif()
endif()

mark_as_advanced(GLFW3_INCLUDE_DIR GLFW3_LIBRARY)
