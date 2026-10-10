@echo off
setlocal
rem Compatibility entry point: use the common versioned build/export workflow.
powershell.exe -NoProfile -ExecutionPolicy Bypass -File "%~dp0build.ps1" -Configuration Debug %*
exit /b %errorlevel%
