# Render src/imageviewer/resources/iview.rc into the build tree with the icon
# path substituted.  Run as a build step (`cmake -P`) rather than with
# configure_file() so the resource script is a real build output: it is
# regenerated when the template or the icon path changes, and a cleaned build
# directory can still be rebuilt without re-running CMake.
#
# Required variables (all passed with -D):
#   IVIEW_ICON_FILE       absolute path of the .ico to embed
#   IVIEW_ICON_RC_TEMPLATE the .rc template in the source tree
#   IVIEW_ICON_RC_OUTPUT   where to write the rendered .rc

foreach(_var IVIEW_ICON_FILE IVIEW_ICON_RC_TEMPLATE IVIEW_ICON_RC_OUTPUT)
    if(NOT DEFINED ${_var} OR "${${_var}}" STREQUAL "")
        message(FATAL_ERROR "WriteIconRc.cmake: ${_var} is not set")
    endif()
endforeach()

file(READ "${IVIEW_ICON_RC_TEMPLATE}" _iview_rc_content)
string(REPLACE "@IVIEW_ICON_FILE@" "${IVIEW_ICON_FILE}"
       _iview_rc_content "${_iview_rc_content}")

# Rewrite only on a real change; a fresh timestamp would make the resource
# compiler and the linker rerun on every build.
if(EXISTS "${IVIEW_ICON_RC_OUTPUT}")
    file(READ "${IVIEW_ICON_RC_OUTPUT}" _iview_rc_existing)
    if(_iview_rc_existing STREQUAL _iview_rc_content)
        return()
    endif()
endif()

file(WRITE "${IVIEW_ICON_RC_OUTPUT}" "${_iview_rc_content}")
