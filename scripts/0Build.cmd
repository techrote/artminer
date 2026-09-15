@echo off
setlocal
pushd "%~dp0.."

cmake -S . -B build -A x64
if errorlevel 1 goto :fail

cmake --build build --config Release --parallel
if errorlevel 1 goto :fail

echo.
echo ArtMiner build complete: build\Release\ArtMiner.exe
popd
exit /b 0

:fail
set "ERR=%ERRORLEVEL%"
popd
exit /b %ERR%
