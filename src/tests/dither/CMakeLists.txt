find_package(doctest REQUIRED)

add_executable(iview_dither_tests
    test_filtercore_dither.cpp
)

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
