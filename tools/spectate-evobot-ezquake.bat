@echo off
setlocal
powershell.exe -NoProfile -ExecutionPolicy Bypass -File "%~dp0spectate-evobot-ezquake.ps1" %*
if errorlevel 1 pause
