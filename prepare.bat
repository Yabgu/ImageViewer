@echo off
setlocal

set VCPKG_DEFAULT_TRIPLET=x64-windows
md %~dp0third-party
compact /c "%~dp0third-party" /i /Q

rem This actually saves a lot of space and it usually is faster this way
pushd %~dp0third-party

set VCPKG_DISABLE_METRICS=1

if not exist vcpkg\vcpkg.exe (
    mkdir vcpkg
    compact /c vcpkg /i /Q
    git clone https://github.com/microsoft/vcpkg
    cmd /C vcpkg\bootstrap-vcpkg.bat
)

vcpkg\vcpkg install glad
vcpkg\vcpkg install glfw3
vcpkg\vcpkg install libjpeg-turbo
popd

set CMAKE_TOOLCHAIN_FILE=%~dp0third-party\vcpkg\scripts\buildsystems\vcpkg.cmake

md %~dp0build
compact /c "%~dp0build" /i /Q
pushd build
call :CALL_CMAKE .. -DCMAKE_TOOLCHAIN_FILE="%CMAKE_TOOLCHAIN_FILE%" && call :CALL_CMAKE --build .
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
