@echo off

echo Set current directory as the local directory
set SCRIPT_DIR=%~dp0
echo Build Started in Release mode

echo Restoring nuget packages
"%~dp0Nuget\Nuget.exe" restore "%SCRIPT_DIR%\ImageViewer.sln"


%windir%\Microsoft.NET\Framework\v4.0.30319\MSBuild.exe "%SCRIPT_DIR%\ImageViewer.sln" "/p:OutputPath=%SCRIPT_DIR%\bin-release" /t:Build /p:Configuration=Release
if %ERRORLEVEL% equ 0 (
   echo Build finished with success
) else (
   >&2 echo Build failed!
   pause
)
