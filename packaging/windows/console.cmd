@echo off
setlocal
set "FIREBIRD=%~dp0bin"
set "PATH=%~dp0bin;%PATH%"
if "%~1"=="" (
  "%~dp0bin\cloudiemail.exe" setup "%~dp0config\local.json"
) else (
  "%~dp0bin\cloudiemail.exe" %*
)
exit /b %errorlevel%
