# CreateGladWrapper.cmake - Create a glad.h wrapper for compatibility
# This script creates glad/glad.h that includes glad/gl.h
#
# Called from FindGlad.cmake after glad generation

set(WRAPPER_FILE "${CMAKE_BINARY_DIR}/glad-generated/include/glad/glad.h")

file(WRITE "${WRAPPER_FILE}" "// Auto-generated glad.h wrapper\n")
file(APPEND "${WRAPPER_FILE}" "#ifndef __GLAD_WRAPPER_H__\n")
file(APPEND "${WRAPPER_FILE}" "#define __GLAD_WRAPPER_H__\n")
file(APPEND "${WRAPPER_FILE}" "#include \"gl.h\"\n")
file(APPEND "${WRAPPER_FILE}" "#endif\n")

message(STATUS "Created glad wrapper: ${WRAPPER_FILE}")
