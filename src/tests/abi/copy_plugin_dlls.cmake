# Copy the plugin DLLs -- and the runtime DLLs they import, which the build has
# already deployed beside them -- next to the ABI test executable.
#
# Invoked from src/tests/abi/CMakeLists.txt as a POST_BUILD step with PLUGIN_DIR
# and DEST_DIR defined.
#
# The glob is deliberate.  The set is only known after the build has run:
# vcpkg's applocal step copies it from the PE import tables, and the CMake
# target graph cannot supply it.  FindJPEG and FindPNG create UNKNOWN imported
# targets, and the hand-written TIFF target here is one too, while
# $<TARGET_RUNTIME_DLLS> only follows SHARED imported targets.  A graph-based
# list therefore comes out empty for those plugins, jpeg62.dll is never copied,
# and the load fails with ERROR_MOD_NOT_FOUND (126).

file(GLOB _plugin_dlls "${PLUGIN_DIR}/*.dll")
if(NOT _plugin_dlls)
    message(FATAL_ERROR "no DLLs found in ${PLUGIN_DIR} -- was the plugin built?")
endif()
file(COPY ${_plugin_dlls} DESTINATION "${DEST_DIR}")
