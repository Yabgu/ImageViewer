# FindGlad.cmake - Find or generate glad OpenGL loader

find_package(PkgConfig QUIET)
if(PkgConfig_FOUND)
    pkg_check_modules(PC_GLAD glad)
endif()

# First try a pre-built library (e.g. from a manual install or other distro)
find_path(GLAD_INCLUDE_DIR
    NAMES glad/glad.h glad.h
    HINTS ${PC_GLAD_INCLUDE_DIRS}
    PATHS /usr/include /usr/local/include
)
find_library(GLAD_LIBRARY
    NAMES glad glad0 libglad
    HINTS ${PC_GLAD_LIBRARY_DIRS}
    PATHS /usr/lib /usr/lib64 /usr/lib/x86_64-linux-gnu /usr/local/lib
)

# If no pre-built glad, generate via the Python tool at configure time
if(NOT GLAD_INCLUDE_DIR)
    find_program(GLAD_GENERATOR glad)
    if(GLAD_GENERATOR)
        set(_GLAD_OUT "${CMAKE_BINARY_DIR}/glad-generated")
        set(_GLAD_INCLUDE "${_GLAD_OUT}/include")
        set(_GLAD_HEADER "${_GLAD_INCLUDE}/glad/gl.h")
        set(_GLAD_COMPAT "${_GLAD_INCLUDE}/glad/glad.h")
        set(_GLAD_SRC    "${_GLAD_OUT}/src/gl.c")

        if(NOT EXISTS "${_GLAD_HEADER}")
            message(STATUS "Generating glad (gl:compatibility) via Python tool…")
            execute_process(
                COMMAND ${GLAD_GENERATOR}
                    --api gl:compatibility
                    --out-path "${_GLAD_OUT}"
                    c
                RESULT_VARIABLE _GLAD_RET
                OUTPUT_QUIET
                ERROR_QUIET
            )
            if(NOT _GLAD_RET EQUAL 0)
                message(WARNING "glad generation failed (exit ${_GLAD_RET})")
            else()
                # Write a glad/glad.h compatibility shim for glad 0.x source code.
                # Maps the two renamed symbols; all gl* functions are identical.
                file(WRITE "${_GLAD_COMPAT}"
"/* Auto-generated glad 0.x compatibility wrapper */\n"
"#ifndef GLAD_COMPAT_H_\n"
"#define GLAD_COMPAT_H_\n"
"#include \"gl.h\"\n"
"typedef GLADloadfunc GLADloadproc;\n"
"static inline int gladLoadGLLoader(GLADloadfunc load) {\n"
"    return gladLoadGL(load) != 0;\n"
"}\n"
"#endif /* GLAD_COMPAT_H_ */\n"
                )
            endif()
        endif()

        if(EXISTS "${_GLAD_HEADER}")
            set(GLAD_INCLUDE_DIR "${_GLAD_INCLUDE}")
            set(GLAD_LIBRARY     "${_GLAD_SRC}")   # source file, compiled below
            set(_GLAD_GENERATED  TRUE)
        endif()
    endif()
endif()

include(FindPackageHandleStandardArgs)
find_package_handle_standard_args(Glad
    REQUIRED_VARS GLAD_INCLUDE_DIR
    FAIL_MESSAGE "glad not found and could not be generated. Install glad (pacman -S glad) or use -DIMAGEVIEWER_USE_VCPKG=ON."
)

if(Glad_FOUND AND NOT TARGET glad::glad)
    if(_GLAD_GENERATED)
        # Compile the generated source into a static library
        if(NOT TARGET _glad_generated)
            add_library(_glad_generated STATIC "${_GLAD_SRC}")
            target_include_directories(_glad_generated PUBLIC "${GLAD_INCLUDE_DIR}")
        endif()

        add_library(glad::glad INTERFACE IMPORTED)
        target_include_directories(glad::glad INTERFACE "${GLAD_INCLUDE_DIR}")
        target_link_libraries(glad::glad INTERFACE _glad_generated)
    else()
        add_library(glad::glad INTERFACE IMPORTED)
        set_target_properties(glad::glad PROPERTIES
            INTERFACE_INCLUDE_DIRECTORIES "${GLAD_INCLUDE_DIR}"
        )
        if(GLAD_LIBRARY)
            set_target_properties(glad::glad PROPERTIES IMPORTED_LOCATION "${GLAD_LIBRARY}")
        endif()
    endif()
endif()

mark_as_advanced(GLAD_INCLUDE_DIR GLAD_LIBRARY GLAD_GENERATOR)
