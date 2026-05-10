if(NOT TARGET iw_format)
    message(STATUS "iw_format not available — format tests will not be built")
    return()
endif()

find_package(doctest REQUIRED)
find_package(PNG REQUIRED)

add_executable(iview_format_tests test_image_conversion.cpp)

set_property(TARGET iview_format_tests PROPERTY CXX_STANDARD 23)

target_link_libraries(iview_format_tests PRIVATE
    iw_format
    PNG::PNG
    doctest::doctest
)

# Path to the gradient reference images. Set at configure time so tests can
# locate them regardless of the working directory when ctest is invoked.
target_compile_definitions(iview_format_tests PRIVATE
    GRADIENT_TEST_DIR_DEFAULT="${CMAKE_SOURCE_DIR}/third-party/gradient/test_sequences/1920x1080"
)

include(CTest)
add_test(NAME iview_format_tests COMMAND iview_format_tests --no-intro)
