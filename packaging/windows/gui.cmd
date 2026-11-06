@echo off
setlocal
set "FIREBIRD=%~dp0bin"
set "PATH=%~dp0bin;%PATH%"
if "%~1"=="" (
  "%~dp0bin\cloudiemail-gui.exe" "%~dp0config\local.json"
) else (
  "%~dp0bin\cloudiemail-gui.exe" %*
)
exit /b %errorlevel%
