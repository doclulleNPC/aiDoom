@echo off
REM ===========================================================================
REM build_all_win.bat -- build EVERYTHING on Windows (MSVC + SDL3):
REM   files\aidoom.exe   +   tools\aidoom_config.exe   +   tools\gpumon.exe
REM   +   tools\director.exe
REM   +   tools\launcher.exe
REM All outputs are copied into run\.  Self-contained: finds VS 2019 via vswhere
REM and sets up the x86 build environment automatically.
REM
REM Usage:  build_all_win.bat            (or pass nmake args, e.g. SDL=C:\path\SDL3)
REM ===========================================================================
setlocal
set "ROOT=%~dp0"

REM --- locate Visual Studio and the x86 build environment ---
set "VSWHERE=%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe"
if not exist "%VSWHERE%" ( echo [build] vswhere not found -- is Visual Studio installed? & exit /b 1 )
for /f "usebackq tokens=*" %%i in (`"%VSWHERE%" -latest -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath`) do set "VSDIR=%%i"
if not defined VSDIR ( echo [build] VC++ tools not found & exit /b 1 )
call "%VSDIR%\VC\Auxiliary\Build\vcvars32.bat" >nul || ( echo [build] vcvars32 failed & exit /b 1 )

echo [build] === aiDoom ===
cd /d "%ROOT%files"
nmake /nologo /f Makefile.msvc %* || exit /b 1
echo [build] === tools (config + gpumon + launcher + director) ===
cd /d "%ROOT%tools"
nmake /nologo /f Makefile.msvc %* || exit /b 1

REM --- copy the aiDoom engine + tool binaries (+SDL3.dll) into run\ ---
echo [build] === copy outputs to run\ ===
copy /Y "%ROOT%files\aidoom.exe"      "%ROOT%run\aidoom.exe"      >nul || exit /b 1
copy /Y "%ROOT%tools\aidoom_config.exe" "%ROOT%run\aidoom_config.exe" >nul || exit /b 1
copy /Y "%ROOT%tools\gpumon.exe"      "%ROOT%run\gpumon.exe"      >nul || exit /b 1
copy /Y "%ROOT%tools\launcher.exe"    "%ROOT%run\launcher.exe"    >nul || exit /b 1
copy /Y "%ROOT%tools\director.exe"    "%ROOT%run\director.exe"    >nul || exit /b 1
if exist "%ROOT%files\SDL3.dll" copy /Y "%ROOT%files\SDL3.dll" "%ROOT%run\SDL3.dll" >nul

echo.
echo [build] OK -- aidoom.exe + aidoom_config.exe + gpumon.exe + launcher.exe + director.exe built and copied to run\.
endlocal
