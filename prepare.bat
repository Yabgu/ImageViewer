@echo off
setlocal

pushd %~dp0
set BUILD_ROOT_DIR=%CD%
popd

set VCPKG_DEFAULT_TRIPLET=x64-windows

md %BUILD_ROOT_DIR%\third-party
rem This actually saves a lot of space and it usually is faster this way
compact /c "%BUILD_ROOT_DIR%\third-party" /i /Q

pushd %BUILD_ROOT_DIR%\third-party

set VCPKG_DISABLE_METRICS=1

if not exist vcpkg\vcpkg.exe (
    mkdir vcpkg
    compact /c vcpkg /i /Q
    git clone https://github.com/microsoft/vcpkg --depth 1
    cmd /C vcpkg\bootstrap-vcpkg.bat
)
popd

set CMAKE_TOOLCHAIN_FILE=%BUILD_ROOT_DIR%\third-party\vcpkg\scripts\buildsystems\vcpkg.cmake

md "%BUILD_ROOT_DIR%\build"
compact /c "%BUILD_ROOT_DIR%\build" /i /Q
pushd "%BUILD_ROOT_DIR%\build"
rem IMAGEVIEWER_USE_VCPKG=ON is what selects the CONFIG find_package() calls in
rem src/imageviewer/CMakeLists.txt.  With it at its default OFF, the build asks
rem for a *system* GLFW through cmake/FindGLFW3.cmake ("GLFW3 library not found,
rem install libglfw3-dev") even though the vcpkg toolchain is active.
rem
rem VCPKG_MANIFEST_FEATURES must be given here, on the command line, and not
rem left to CMakeLists.txt: the toolchain file runs during compiler detection,
rem before the project body, and that is where vcpkg performs the manifest
rem install.  Without it only vcpkg.json's default dependencies (doctest,
rem libpng) are installed and glfw3/glad are missing.  Keep this list in sync
rem with vcpkg.json and with .github/workflows/build.yml.
call :CALL_CMAKE %~dp0 ^
    -DCMAKE_TOOLCHAIN_FILE="%CMAKE_TOOLCHAIN_FILE%" ^
    -DIMAGEVIEWER_USE_VCPKG=ON ^
    -DVCPKG_MANIFEST_FEATURES="viewer;plugin-jpeg;plugin-webp;plugin-tiff;plugin-stb;plugin-pixman" ^
    && call :CALL_CMAKE --build .
popd
goto :EOF

:CALL_CMAKE
setlocal

where cmake && (
  call cmake %* && exit /B 0 || exit /B 1
) || (
  echo CMake is not in path. Searching CMake installed from VCpkg
  pushd %~dp0third-party\vcpkg\downloads\tools
  for /F %%F in ('dir /B /S cmake.exe') do (
    popd
    pushd %%F\..
    goto :FOUND_CMAKE_ADD_PATH
:FOUND_CMAKE_ADD_PATH_RETURN
    popd
    call cmake %*
    goto :EOF
  )
  popd
  exit /B 1
)
goto :EOF
:FOUND_CMAKE_ADD_PATH
set PATH=%PATH%;%CD%
goto :FOUND_CMAKE_ADD_PATH_RETURN
