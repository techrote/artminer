@echo off
setlocal
pushd "%~dp0.."

call scripts\0Build.cmd
if errorlevel 1 goto :fail

build\Release\ArtMiner.exe %*
set "ERR=%ERRORLEVEL%"
popd
exit /b %ERR%

:fail
set "ERR=%ERRORLEVEL%"
popd
exit /b %ERR%
