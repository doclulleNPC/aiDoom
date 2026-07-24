@echo off
REM ---------------------------------------------------------------------------
REM start_buddy.bat -- launch BuddyDoom OFFLINE (no LLM / no AI director) with the
REM rule-based co-op buddy.  Windows counterpart of start_buddy.sh: no Ollama,
REM no PowerShell, no network -- just the companion.
REM
REM   -coop    = rule-based buddy (offline, no LLM)   <- this launcher
REM   -aicoop  = AI/LLM-backed buddy (use start_buddydoom.bat / .ps1)
REM
REM Pass-through args, e.g.:
REM   start_buddy.bat -warp 1 1 -skill 4
REM   start_buddy.bat -loadgame 0
REM ---------------------------------------------------------------------------
setlocal
cd /d "%~dp0"

if not exist "buddydoom.exe" (
    echo [start_buddy] buddydoom.exe not found in this folder -- build it first ^(see README^).
    pause
    exit /b 1
)

echo [start_buddy] offline ^(no AI director^) + rule-based buddy: buddydoom.exe -coop %*
buddydoom.exe -coop %*
