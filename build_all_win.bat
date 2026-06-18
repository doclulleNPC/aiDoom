@echo off
REM ===========================================================================
REM build_all_win.bat -- build EVERYTHING on Windows (MSVC + SDL3):
REM   files\aidoom.exe   +   tools\aidoom_config.exe   +   tools\gpumon_sdl.exe
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
echo [build] === tools (config + gpumon) ===
cd /d "%ROOT%tools"
nmake /nologo /f Makefile.msvc %* || exit /b 1

echo.
echo [build] OK -- aidoom.exe + aidoom_config.exe + gpumon.exe built and copied to run\.
endlocal
