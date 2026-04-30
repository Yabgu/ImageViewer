find_package(doctest REQUIRED)

add_executable(iview_tests
    test_iwimageformat.cpp
)

set_property(TARGET iview_tests PROPERTY CXX_STANDARD 17)

# Headers from the imageviewer source directory (ImagePluginDef.h, Utils.hpp)
target_include_directories(iview_tests PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/../imageviewer
)

target_link_libraries(iview_tests PRIVATE
    doctest::doctest
)

include(CTest)
add_test(NAME iview_tests COMMAND iview_tests)
