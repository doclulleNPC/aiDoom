@echo off
REM Double-click launcher: FULL LLM by default -- AI co-op buddy (-aicoop) + the LLM
REM director (monsters + L4D spawns).  Waits for Ollama, then starts BuddyDoom + director.exe.
REM Pass-through args, e.g.:  start_buddydoom.bat -Skill 4 -NoFriendlyFire -RuleCoop
powershell -NoProfile -ExecutionPolicy Bypass -File "%~dp0start_buddydoom.ps1" %*
