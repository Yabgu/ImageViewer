if(NOT IMAGEVIEWER_BUILD_VIEWER)
    message(STATUS "IMAGEVIEWER_BUILD_VIEWER=OFF — skipping GUI E2E tests")
    return()
endif()

find_package(doctest REQUIRED)

# Re-locate GLFW / GLAD using the same logic as src/imageviewer/CMakeLists.txt.
if(IMAGEVIEWER_USE_VCPKG)
    find_package(glfw3 CONFIG REQUIRED)
    find_package(glad CONFIG REQUIRED)
    set(_GUI_TEST_GLFW_TARGET glfw)
    set(_GUI_TEST_GLAD_TARGET glad::glad)
else()
    list(APPEND CMAKE_MODULE_PATH "${CMAKE_SOURCE_DIR}/cmake")
    find_package(GLFW3 REQUIRED)
    find_package(Glad REQUIRED)
    set(_GUI_TEST_GLFW_TARGET glfw3::glfw3)
    set(_GUI_TEST_GLAD_TARGET glad::glad)
endif()

find_package(PNG REQUIRED)

add_executable(iview_gui_tests
    test_gui.cpp
)

set_property(TARGET iview_gui_tests PROPERTY CXX_STANDARD 23)

target_include_directories(iview_gui_tests PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/../../imageviewer
)

target_link_libraries(iview_gui_tests PRIVATE
    imageviewer
    iw_format
    ${_GUI_TEST_GLFW_TARGET}
    ${_GUI_TEST_GLAD_TARGET}
    PNG::PNG
    doctest::doctest
)

# Make the test binary find plugin shared libraries (libImageLoaderPng.so, etc.)
# from the imageviewer build output directory at run time.
set_target_properties(iview_gui_tests PROPERTIES
    BUILD_RPATH   "$<TARGET_FILE_DIR:ImageLoaderPng>"
    INSTALL_RPATH "$<TARGET_FILE_DIR:ImageLoaderPng>"
)

include(CTest)
add_test(NAME iview_gui_tests COMMAND iview_gui_tests --no-intro)

# In CI, set LIBGL_ALWAYS_SOFTWARE=1 (Mesa SW render) and point to a Xvfb
# display via DISPLAY=:99.  These are set by the CI job, not here, so the
# test binary itself stays portable.
