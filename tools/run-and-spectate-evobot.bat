@echo off
setlocal
echo EvoBot launcher v20260813.4
echo Script: %~dp0run-and-spectate-evobot.ps1
powershell.exe -NoProfile -ExecutionPolicy Bypass -File "%~dp0run-and-spectate-evobot.ps1" %*
if errorlevel 1 pause
