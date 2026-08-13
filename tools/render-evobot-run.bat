@echo off
setlocal
powershell.exe -NoProfile -ExecutionPolicy Bypass -File "%~dp0render-evobot-run.ps1" %*
if errorlevel 1 pause
