@echo off
REM -----------------------------------------------------------------------
REM start_aibuddy.bat -- launch aiDoom with the AI/LLM co-op buddy.
REM Windows counterpart of start_aibuddy.sh: starts BOTH the game (-aicoop +
REM the AI-director TCP server) and the native director.exe that drives the
REM buddy's (and the monsters') tactics via a local Ollama model.
REM
REM Default IWAD is doom.wad (DOOM1). Override with: -iwad doom2.wad
REM   start_aibuddy.bat
REM   start_aibuddy.bat -iwad doom2.wad -skill 5 -warp 3 1
REM
REM Ollama host/port/model come from aidoom.cfg (next to this file).  Needs a
REM reachable Ollama.  Extra args pass to the game; defaults: -skill 4 -warp 1 1.
REM
REM Offline rule-based buddy instead?  start_buddy.bat  (no LLM)
REM -----------------------------------------------------------------------
setlocal
cd /d "%~dp0"

if not exist "aidoom.exe"   ( echo [aibuddy] aidoom.exe not found -- build it first ^(see README^). & pause & exit /b 1 )
if not exist "director.exe" ( echo [aibuddy] director.exe not found -- build it first ^(see README^). & pause & exit /b 1 )

REM Default IWAD: doom.wad in cwd or in ..\files\, unless -iwad already given.
set "IWAD_FLAG="
echo %* | find "-iwad" >nul
if errorlevel 1 (
    if exist "doom.wad"         set "IWAD_FLAG=-iwad doom.wad"
    if not defined IWAD_FLAG if exist "..\files\doom.wad" set "IWAD_FLAG=-iwad ..\files\doom.wad"
)

REM Default skill / warp unless the caller passed their own.
set "ARGS=%*"
echo %ARGS% | find "-skill" >nul || set "ARGS=-skill 4 %ARGS%"
echo %ARGS% | find "-warp"  >nul || set "ARGS=-warp 1 1 %ARGS%"

echo [aibuddy] game:     aidoom.exe -aicoop -aidirector 31666 %IWAD_FLAG% %ARGS%
start "aiDoom" aidoom.exe -aicoop -aidirector 31666 %IWAD_FLAG% %ARGS%

REM give the game a moment to open the listening socket
ping -n 3 127.0.0.1 >nul

echo [aibuddy] director: director.exe --port 31666  (Ollama from aidoom.cfg)
director.exe --port 31666
