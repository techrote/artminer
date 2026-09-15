@echo off
setlocal
pushd "%~dp0.."

call scripts\0Build.cmd
if errorlevel 1 goto :fail

ctest --test-dir build -C Release --output-on-failure
if errorlevel 1 goto :fail

echo.
echo ArtMiner tests passed.
popd
exit /b 0

:fail
set "ERR=%ERRORLEVEL%"
popd
exit /b %ERR%
