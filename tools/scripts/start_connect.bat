@echo off
REM ---------------------------------------------------------------------------
REM start_connect.bat -- join a Chocolate/Crispy co-op server (run on each
REM client).  Windows counterpart of start_connect.sh.  No AI buddy -- it is
REM single-player only and ignored in netgames.
REM
REM Edit the server IP:port below to match your host.  Extra args pass through.
REM ---------------------------------------------------------------------------
setlocal
cd /d "%~dp0"

if not exist "aidoom.exe" ( echo [connect] aidoom.exe not found -- build it first ^(see README^). & pause & exit /b 1 )

aidoom.exe -connect 192.168.2.10:2342 -netplayers 2 -warp 1 1 %*
