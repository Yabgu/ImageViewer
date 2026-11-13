find_package(doctest REQUIRED)

add_executable(iview_tests
    test_iwimageformat.cpp
    test_config.cpp
    test_filter_job.cpp
    test_zoom_scale.cpp
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

# ── Windows shell thumbnail provider ────────────────────────────────────────
#
# Covers the parts of IwiThumbnailProvider that can be checked without a
# desktop: which files are accepted, how they are decimated, what the resulting
# bytes mean, and whether the CLSID the DLL was built with still matches the
# one the install scripts write (a mismatch fails silently in Explorer).
#
# C++23 because the subject is a module: the test imports IwiThumbnail and
# IwiFormat exactly as the provider does.  Gated on the library target, which
# exists only on Windows with IMAGEVIEWER_BUILD_THUMBNAIL_PROVIDER=ON.
if(TARGET iw_iwi_thumbnail)
    find_package(doctest REQUIRED)

    add_executable(iview_shell_tests test_iwi_thumbnail.cpp)

    set_property(TARGET iview_shell_tests PROPERTY CXX_STANDARD 23)

    target_include_directories(iview_shell_tests PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}
        ${CMAKE_CURRENT_SOURCE_DIR}/../imageviewer
    )

    target_link_libraries(iview_shell_tests PRIVATE
        iw_iwi_thumbnail
        doctest::doctest
    )

    # repo_root.hpp walks up from the executable to find the checkout; no path
    # needs to be baked in, so the test works from any generator's output
    # directory.
    add_test(NAME iview_shell_tests COMMAND iview_shell_tests)
endif()

# ABI conformance tests: load the built plugin binaries and drive them through
# their exported C ABI. Only contributes targets when the plugins are built
# (IMAGEVIEWER_BUILD_VIEWER=ON).
add_subdirectory(abi)
