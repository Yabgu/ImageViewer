# Threads::Threads is needed by FilterCore's segmented per-pixel loops, which are
# compiled into the FilterPixman plugin and instantiated by the FilterCore unit
# tests.  Declared here, in the common parent, because an imported target is
# only visible in the directory that found it and its subdirectories: finding
# it in src/imageviewer alone would leave src/tests unable to link it.
#
# std::thread is the C++ runtime, not OpenMP -- no extra runtime DLL on MSVC, and
# libwinpthread on MinGW, which the packaging step already ships.
find_package(Threads REQUIRED)

add_subdirectory(format)
add_subdirectory(imageviewer)
add_subdirectory(tools)
add_subdirectory(tests)
