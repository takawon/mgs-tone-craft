@echo off
setlocal
powershell.exe -NoLogo -NoProfile -ExecutionPolicy Bypass -File "%~dp0capture-ui.ps1" %*
exit /b %ERRORLEVEL%
