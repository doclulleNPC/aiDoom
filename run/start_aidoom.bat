@echo off
REM Double-click launcher: waits for Ollama, then starts aiDoom + LLM director.
REM Pass-through args, e.g.:  start_aidoom.bat -Skill 4 -FriendlyFire
powershell -NoProfile -ExecutionPolicy Bypass -File "%~dp0start_aidoom.ps1" %*
