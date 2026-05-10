find_package(doctest REQUIRED)

add_executable(iview_dither_tests
    test_filtercore_dither.cpp
)

# C++17 is intentional: FilterCore.hpp and its dependencies use only C++17
# features (no modules, no C++23 extensions).  This allows the dither tests
# to run without IMAGEVIEWER_BUILD_VIEWER and without a C++23-capable build.
# The GUI tests (src/tests/gui) require C++23 for module imports.
set_property(TARGET iview_dither_tests PROPERTY CXX_STANDARD 17)

# FilterCore.hpp and ImagePluginDef.h live in the imageviewer source tree.
target_include_directories(iview_dither_tests PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/../../imageviewer
)

target_link_libraries(iview_dither_tests PRIVATE
    doctest::doctest
)

include(CTest)
add_test(NAME iview_dither_tests COMMAND iview_dither_tests --no-intro)
