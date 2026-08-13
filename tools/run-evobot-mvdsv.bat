@echo off
setlocal
powershell.exe -NoExit -ExecutionPolicy Bypass -File "%~dp0run-evobot-mvdsv.ps1" %*
