@echo off
setlocal EnableExtensions DisableDelayedExpansion

rem ===========================================================================
rem  install-desktop-windows.bat -- register ImageViewer with the Windows shell
rem
rem  Writes, for the current user only (HKEY_CURRENT_USER, no UAC prompt):
rem
rem    HKCU\Software\Classes\.iwi
rem      (Default) = ImageViewer.IWI            the .iwi extension maps to it
rem    HKCU\Software\Classes\ImageViewer.IWI
rem      (Default) = ImageViewer Texture        the ProgID's description
rem      DefaultIcon\(Default) = "<iview>",0    the icon Explorer draws for .iwi
rem      shell\open\command\(Default) = "<iview>" "%1"
rem    HKCU\Software\Classes\*\shell\Open with ImageViewer\command
rem      (Default) = "<iview>" "%1"             right-click entry for any file
rem
rem    install-desktop-windows.bat                 register
rem    install-desktop-windows.bat uninstall       remove everything above
rem    install-desktop-windows.bat --binary PATH   use a specific iview.exe
rem
rem  # Why HKCU and not HKLM
rem
rem  HKLM\Software\Classes would make the association machine-wide, but it
rem  needs elevation, which means relaunching this script through UAC.  HKCU
rem  needs no administrator and the association still works for the user who
rem  ran the script.  Everything below is a per-user setting.
rem
rem  # Why UserChoice is left alone
rem
rem  HKCU\Software\Microsoft\Windows\CurrentVersion\Explorer\FileExts\.iwi\
rem  UserChoice is protected by a per-user hash.  Explorer validates that hash,
rem  so an entry written by a script is ignored or rejected -- the write looks
rem  like it worked and the double-click still goes elsewhere.  This script
rem  therefore writes only HKCU\Software\Classes\.iwi, the supported layer:
rem  ImageViewer appears in "Open With", and since nothing else claims .iwi it
rem  is the default handler.  If the user (or a previous install) already chose
rem  another handler for .iwi, Windows keeps that choice and ImageViewer is one
rem  "Open With" click away.  Overriding a deliberate user choice is not this
rem  script's call.
rem
rem  # The right-click entry covers every file type
rem
rem  HKCU\Software\Classes\*\shell\... is the "all file types" wildcard, so
rem  "Open with ImageViewer" appears for a .txt as readily as for an image.
rem  Narrowing it to image types means writing the same key once per
rem  extension (.png, .jpg, ...), and a .iwi file already gets the entry from
rem  its own ProgID above.  The wildcard is one key and no surprises for a
rem  file type that has no handler of its own -- which is the point.
rem
rem  # How iview.exe is located
rem
rem  %~dp0 first, for a packaged install where this script sits beside
rem  iview.exe.  Otherwise the build trees in this repository are probed,
rem  preferring a tree that has ImageLoaderIWI.dll beside the exe: iview loads
rem  .iwi through that plugin, and registering a build without it produces an
rem  association that opens a window with an error.  --binary PATH overrides
rem  both.  The resolved path is baked into the registry, so the association
rem  keeps working after the build tree moves only if this script is re-run.
rem ===========================================================================

set "EXT_KEY=HKCU\Software\Classes\.iwi"
set "PROGID=ImageViewer.IWI"
set "PROGID_KEY=HKCU\Software\Classes\ImageViewer.IWI"
set "MENU_KEY=HKCU\Software\Classes\*\shell\Open with ImageViewer"
set "PROGID_DESC=ImageViewer Texture"

set "IW_UNINSTALL=0"
set "IW_BINARY="
set "IW_EXE="
set "IW_FAIL=0"

rem -- Arguments --------------------------------------------------------------

:parse_args
if "%~1"=="" goto :args_done
set "IW_ARG=%~1"
if /i "%IW_ARG%"=="uninstall" set "IW_UNINSTALL=1"
if /i "%IW_ARG%"=="uninstall" shift
if /i "%IW_ARG%"=="uninstall" goto :parse_args
if /i "%IW_ARG%"=="--binary" goto :parse_binary
if /i "%IW_ARG%"=="--help" goto :usage
if /i "%IW_ARG%"=="-h" goto :usage
if /i "%IW_ARG%"=="/?" goto :usage
echo error: unknown argument: %IW_ARG%
echo        try --help
exit /b 2

:parse_binary
if "%~2"=="" goto :missing_binary_arg
set "IW_BINARY=%~2"
shift
shift
goto :parse_args

:missing_binary_arg
echo error: --binary needs a path, for example: --binary "C:\build\iview.exe"
exit /b 2

:usage
echo Usage: install-desktop-windows.bat [uninstall] [--binary PATH]
echo.
echo   Register ImageViewer as the handler for .iwi files, for the current
echo   user only.  No administrator rights and no UAC prompt.
echo.
echo   uninstall        remove the .iwi association, the ProgID and the
echo                    "Open with ImageViewer" right-click entry
echo   --binary PATH    register this iview.exe instead of an auto-detected one
echo   --help           this text
echo.
echo After a successful install, double-clicking an .iwi file opens it.
exit /b 0

:args_done
if "%IW_UNINSTALL%"=="1" goto :uninstall

rem -- Locate iview.exe -------------------------------------------------------

echo.
echo ==^> Finding iview.exe
call :resolve_binary
if errorlevel 1 exit /b 1

for %%F in ("%IW_EXE%") do set "IW_EXE_DIR=%%~dpF"
echo   binary: "%IW_EXE%"

if not exist "%IW_EXE_DIR%ImageLoaderIWI.dll" goto :no_iwi_plugin
echo   plugin: ImageLoaderIWI.dll found beside the binary
goto :pick_icon

:no_iwi_plugin
echo   warning: no ImageLoaderIWI.dll beside iview.exe.
echo            iview decodes .iwi through that plugin, so the association
echo            below will open a window that cannot show the file.
echo            Build with -DIMAGEVIEWER_BUILD_PLUGINS=ON, or pass --binary
echo            to point at a build that has the plugin next to the exe.

:pick_icon
rem A real .ico gives Explorer a per-size icon set; "exe,0" falls back to the
rem icon embedded in the executable, which is the same artwork.
set "IW_ICON=%IW_EXE%"
if exist "%IW_EXE_DIR%iview.ico" set "IW_ICON=%IW_EXE_DIR%iview.ico"
if not exist "%IW_EXE_DIR%iview.ico" if exist "%IW_EXE_DIR%icon\iview.ico" set "IW_ICON=%IW_EXE_DIR%icon\iview.ico"

rem reg add is invoked with the value already carrying its own quotes, so the
rem path must not contain a character cmd would read as a command separator.
if not "%IW_EXE%"=="%IW_EXE:&=%" goto :bad_binary_path

rem -- Register ---------------------------------------------------------------
rem
rem Each value is built once, as the plain text that must end up in the
rem registry, and is used for both the write and the verification below.  That
rem keeps one source of truth: a check that compares against a second,
rem hand-written copy of the string cannot disagree with itself.

echo.
echo ==^> Registering .iwi
set "IW_DATA_EXT=%PROGID%"
set "IW_DATA_DESC=%PROGID_DESC%"
set "IW_DATA_ICON="%IW_ICON%",0"
set "IW_DATA_OPEN="%IW_EXE%" "%%1""

call :reg_write "%EXT_KEY%" IW_DATA_EXT ".iwi = %PROGID%"
if errorlevel 1 set "IW_FAIL=1"
call :reg_write "%PROGID_KEY%" IW_DATA_DESC "ProgID description"
if errorlevel 1 set "IW_FAIL=1"
call :reg_write "%PROGID_KEY%\DefaultIcon" IW_DATA_ICON "default icon"
if errorlevel 1 set "IW_FAIL=1"
call :reg_write "%PROGID_KEY%\shell\open\command" IW_DATA_OPEN "open command"
if errorlevel 1 set "IW_FAIL=1"
call :reg_write "%MENU_KEY%\command" IW_DATA_OPEN "right-click menu entry"
if errorlevel 1 set "IW_FAIL=1"

rem -- Verify ----------------------------------------------------------------
rem
rem Read every value back out of the registry and compare it with the string
rem that was written.  "reg add exited 0" is not the claim being made; the
rem claim is that a double-click resolves to this iview.exe.

echo.
echo ==^> Verifying
rem Labels below are echoed unquoted, so they must stay free of cmd
rem metacharacters: a greater-than sign in one would redirect that result
rem line into a file instead of the console.
call :verify "%EXT_KEY%" IW_DATA_EXT ".iwi = %PROGID%"
if errorlevel 1 set "IW_FAIL=1"
call :verify "%PROGID_KEY%" IW_DATA_DESC "ProgID description"
if errorlevel 1 set "IW_FAIL=1"
call :verify "%PROGID_KEY%\DefaultIcon" IW_DATA_ICON "default icon"
if errorlevel 1 set "IW_FAIL=1"
call :verify "%PROGID_KEY%\shell\open\command" IW_DATA_OPEN "open command"
if errorlevel 1 set "IW_FAIL=1"
call :verify "%MENU_KEY%\command" IW_DATA_OPEN "right-click menu entry"
if errorlevel 1 set "IW_FAIL=1"

if "%IW_FAIL%"=="1" goto :failed

rem Explorer caches file-type icons and handler lookups; this refreshes them so
rem a .iwi file you are looking at picks up the icon without a re-login.
call :refresh_shell

echo.
echo Installation successful.
echo.
echo   handler: "%IW_EXE%"
echo   icon   : "%IW_ICON%"
echo.
echo   Double-click a .iwi file, or from a command prompt:
echo     "%IW_EXE%" image.iwi
exit /b 0

:failed
echo.
echo Installation FAILED -- the registry does not hold what this script wrote.
echo Check that no group policy or security product is blocking writes to
echo HKCU\Software\Classes.
exit /b 1

:bad_binary_path
echo error: the iview.exe path contains "&", which this script cannot pass
echo        through reg add safely:
echo          "%IW_EXE%"
echo        Move the build to a path without that character.
exit /b 1

rem -- Uninstall --------------------------------------------------------------

:uninstall
echo.
echo ==^> Removing the ImageViewer registration
reg delete "%MENU_KEY%" /f >nul 2>&1
reg delete "%PROGID_KEY%" /f >nul 2>&1
reg delete "%EXT_KEY%" /f >nul 2>&1

rem The deletes are only meaningful if the keys are actually gone, so assert
rem that rather than trusting reg delete's exit code.
call :assert_absent "%EXT_KEY%" ".iwi association"
if errorlevel 1 set "IW_FAIL=1"
call :assert_absent "%PROGID_KEY%" "ProgID ImageViewer.IWI"
if errorlevel 1 set "IW_FAIL=1"
call :assert_absent "%MENU_KEY%" "right-click menu entry"
if errorlevel 1 set "IW_FAIL=1"

if "%IW_FAIL%"=="1" goto :failed

call :refresh_shell
echo.
echo ImageViewer is no longer registered.
echo   .iwi files have no handler of their own again.
exit /b 0

rem ===========================================================================
rem  Subroutines
rem ===========================================================================

:resolve_binary
rem Sets IW_EXE.  Returns 1 when nothing usable was found.
if not defined IW_BINARY goto :rb_script_dir
if not exist "%IW_BINARY%" goto :rb_bad_binary
for %%F in ("%IW_BINARY%") do set "IW_EXE=%%~fF"
echo   source: --binary
exit /b 0

:rb_bad_binary
echo error: --binary path does not exist: "%IW_BINARY%"
exit /b 1

:rb_script_dir
if not exist "%~dp0iview.exe" goto :rb_trees
for %%F in ("%~dp0iview.exe") do set "IW_EXE=%%~fF"
echo   source: next to this script
exit /b 0

:rb_trees
for %%D in ("%~dp0..") do set "IW_ROOT=%%~fD"

rem First pass: only trees that can actually open a .iwi.  Second pass: any
rem tree, so a viewer-only build still gets an association (with the warning
rem printed by the caller) instead of no install at all.
call :scan_trees 1
if defined IW_EXE exit /b 0
call :scan_trees 0
if defined IW_EXE exit /b 0

for /f "delims=" %%F in ('where iview.exe 2^>nul') do if not defined IW_EXE set "IW_EXE=%%F"
if defined IW_EXE echo   source: PATH
if defined IW_EXE exit /b 0

echo error: no iview.exe found.
echo   Build one first:
echo     cmake -S "%IW_ROOT%" -B "%IW_ROOT%\build-release" -DIMAGEVIEWER_BUILD_VIEWER=ON -DIMAGEVIEWER_BUILD_PLUGINS=ON
echo     cmake --build "%IW_ROOT%\build-release"
echo   or point at an existing build:
echo     install-desktop-windows.bat --binary PATH
exit /b 1

:scan_trees
rem %1 = 1 to require ImageLoaderIWI.dll beside the exe, 0 to accept any build
for %%C in ("%IW_ROOT%\build-release\src\imageviewer"
            "%IW_ROOT%\build-ninja\src\imageviewer"
            "%IW_ROOT%\build\src\imageviewer\Release"
            "%IW_ROOT%\build\src\imageviewer\Debug"
            "%IW_ROOT%\build-clang64\src\imageviewer") do call :probe_tree "%%~C" %1
if not defined IW_EXE exit /b 0
echo   source: "%IW_EXE_SOURCE%"
exit /b 0

:probe_tree
rem %1 = candidate directory, %2 = require the IWI plugin (1) or not (0)
if defined IW_EXE exit /b 0
if not exist "%~1\iview.exe" exit /b 0
if "%2"=="1" if not exist "%~1\ImageLoaderIWI.dll" exit /b 0
for %%F in ("%~1\iview.exe") do set "IW_EXE=%%~fF"
set "IW_EXE_SOURCE=%~1"
exit /b 0

:reg_write
rem %1 = key, %2 = name of the variable holding the plain value
call set "IW_RAW=%%%2%%"
rem reg.exe receives the value with the quotes escaped, so double quotes inside
rem the value survive both cmd's tokenizer and reg.exe's argv parser.
set "IW_ESC=%IW_RAW:"=\"%"
reg add "%~1" /ve /d "%IW_ESC%" /f >nul 2>&1
if errorlevel 1 goto :rw_failed
echo   wrote %~1
exit /b 0

:rw_failed
echo   FAILED to write %~1
exit /b 1

:verify
rem %1 = key, %2 = name of the variable holding the expected plain value,
rem %3 = human-readable name for the check
set "IW_ACT="
for /f "tokens=2,*" %%A in ('reg query "%~1" /ve 2^>nul ^| findstr /i /c:"REG_SZ"') do set "IW_ACT=%%B"
call set "IW_EXP=%%%2%%"
rem Compare without the quotes: both sides would otherwise end a quoted if
rem expression early, and the quotes are not what is being checked.
set IW_ACT_NQ=%IW_ACT:"=%
set IW_EXP_NQ=%IW_EXP:"=%
if not defined IW_ACT goto :verify_failed
if /i "%IW_ACT_NQ%"=="%IW_EXP_NQ%" goto :verify_ok
:verify_failed
echo   FAIL  %~3
echo           expected: %IW_EXP%
echo           actual  : %IW_ACT%
exit /b 1
:verify_ok
echo   ok    %~3
exit /b 0

:assert_absent
rem %1 = key, %2 = human-readable name
reg query "%~1" >nul 2>&1
if not errorlevel 1 goto :absent_failed
echo   removed %~2
exit /b 0

:absent_failed
echo   FAIL  %~2 is still present at %~1
exit /b 1

:refresh_shell
if not exist "%SystemRoot%\System32\ie4uinit.exe" exit /b 0
"%SystemRoot%\System32\ie4uinit.exe" -show >nul 2>&1
exit /b 0
