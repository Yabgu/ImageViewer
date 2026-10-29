/*
 * main.cpp — doctest entry point for the ABI test binary.
 *
 * The test cases are split across per-plugin translation units
 * (test_imageloader_jpeg.cpp, test_filterpixman.cpp) and each is compiled only
 * when its plugin target exists.  Keeping the doctest implementation in its own
 * file means a build with only one plugin available still links a valid binary.
 */

#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>
