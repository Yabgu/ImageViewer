@echo off
setlocal

pushd %~dp0..
set BUILD_ROOT_DIR=%CD%
popd

set VCPKG_DEFAULT_TRIPLET=x64-windows

md %BUILD_ROOT_DIR%\third-party
compact /c "%BUILD_ROOT_DIR%\third-party" /i /Q

rem This actually saves a lot of space and it usually is faster this way
pushd %BUILD_ROOT_DIR%\third-party

set VCPKG_DISABLE_METRICS=1

if not exist vcpkg\vcpkg.exe (
    mkdir vcpkg
    compact /c vcpkg /i /Q
    git clone https://github.com/microsoft/vcpkg --depth 1
    cmd /C vcpkg\bootstrap-vcpkg.bat
)

for %%i in (glad glfw3 libjpeg-turbo nanogui) do vcpkg\vcpkg install %%i

popd

set CMAKE_TOOLCHAIN_FILE=%BUILD_ROOT_DIR%\third-party\vcpkg\scripts\buildsystems\vcpkg.cmake

md "%BUILD_ROOT_DIR%\image-viewer-build"
compact /c "%BUILD_ROOT_DIR%\image-viewer-build" /i /Q
pushd "%BUILD_ROOT_DIR%\image-viewer-build"
call :CALL_CMAKE %~dp0 -DCMAKE_TOOLCHAIN_FILE="%CMAKE_TOOLCHAIN_FILE%" && call :CALL_CMAKE --build .
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
