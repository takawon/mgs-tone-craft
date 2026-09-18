@echo off
setlocal
call "%~dp0tools\build.cmd" %*
exit /b %ERRORLEVEL%
