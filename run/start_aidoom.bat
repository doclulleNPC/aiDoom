@echo off
REM Double-click launcher: FULL LLM by default -- AI co-op buddy (-aicoop) + the LLM
REM director (monsters + L4D spawns).  Waits for Ollama, then starts aiDoom + director.exe.
REM Pass-through args, e.g.:  start_aidoom.bat -Skill 4 -FriendlyFire -RuleCoop
powershell -NoProfile -ExecutionPolicy Bypass -File "%~dp0start_aidoom.ps1" %*
