setlocal

set VCPKG_DEFAULT_TRIPLET=x64-windows
md %~dp0third-party
pushd %~dp0third-party

if not exist vcpkg\vcpkg.exe (
    git clone https://github.com/microsoft/vcpkg
    vcpkg\bootstrap-vcpkg.bat
)

vcpkg\vcpkg install SDL2
vcpkg\vcpkg install boost
vcpkg\vcpkg install libjpeg-turbo
vcpkg\vcpkg install SDL2-Image

set VCPKG_ROOT=%CD%\vcpkg
set CMAKE_TOOLCHAIN_FILE=C:\Workspace\ImageViewer\third-party\vcpkg\scripts\buildsystems\vcpkg.cmake
popd

md %~dp0build
pushd %~dp0build
cmake ..  -DCMAKE_TOOLCHAIN_FILE="%CMAKE_TOOLCHAIN_FILE%"
popd
