setlocal

set VCPKG_DEFAULT_TRIPLET=x64-windows
md %~dp0third-party
rem This actually saves a lot of space and it usually is faster this way
pushd %~dp0third-party

if not exist vcpkg\vcpkg.exe (
	mkdir vcpkg
	compact /c vcpkg /i /Q
    git clone https://github.com/microsoft/vcpkg
    vcpkg\bootstrap-vcpkg.bat
)

vcpkg\vcpkg install glfw3
rem Glad package from vcpkg tries to load all OpenGL functionalities which we don't need at the time
rem vcpkg\vcpkg install glad
vcpkg\vcpkg install boost
vcpkg\vcpkg install libjpeg-turbo
popd

set CMAKE_TOOLCHAIN_FILE=%~dp0third-party\vcpkg\scripts\buildsystems\vcpkg.cmake

md %~dp0build
pushd %~dp0build
compact /c "%~dp0build" /i /Q
cmake ..  -DCMAKE_TOOLCHAIN_FILE="%CMAKE_TOOLCHAIN_FILE%" && cmake --build .
popd
