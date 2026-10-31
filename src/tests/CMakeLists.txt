find_package(doctest REQUIRED)

add_executable(iview_tests
    test_iwimageformat.cpp
    test_config.cpp
    test_filter_job.cpp
)

set_property(TARGET iview_tests PROPERTY CXX_STANDARD 17)

# Config.hpp includes <windows.h> on Windows; keep its min/max macros out of
# the test translation units.
if(WIN32)
    target_compile_definitions(iview_tests PRIVATE NOMINMAX)
endif()

# Headers from the imageviewer source directory (ImagePluginDef.h, Utils.hpp)
target_include_directories(iview_tests PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/../imageviewer
)

target_link_libraries(iview_tests PRIVATE
    doctest::doctest
)

include(CTest)
add_test(NAME iview_tests COMMAND iview_tests)

add_subdirectory(format)

# ABI conformance tests: load the built plugin binaries and drive them through
# their exported C ABI. Only contributes targets when the plugins are built
# (IMAGEVIEWER_BUILD_VIEWER=ON).
add_subdirectory(abi)
