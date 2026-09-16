@echo off
setlocal EnableExtensions

set "ROOT=%~dp0.."
for %%I in ("%ROOT%") do set "ROOT=%%~fI"
set "VERSION=1.0.0"
set "BUILD=%ROOT%\build\Release"
if not "%ARTMINER_BUILD_DIR%"=="" set "BUILD=%ARTMINER_BUILD_DIR%"
set "DIST=%ROOT%\dist"
set "NAME=ArtMiner-%VERSION%-win-x64"
set "STAGE=%DIST%\%NAME%"
set "ZIP=%DIST%\%NAME%.zip"

if not exist "%BUILD%\ArtMiner.exe" (
  echo ERROR: release executable not found at "%BUILD%\ArtMiner.exe"
  exit /b 2
)

if exist "%DIST%" rmdir /s /q "%DIST%"
mkdir "%STAGE%" || exit /b 3
mkdir "%STAGE%\recipes\examples" || exit /b 3
mkdir "%STAGE%\palettes" || exit /b 3
mkdir "%STAGE%\output" || exit /b 3
mkdir "%STAGE%\cache" || exit /b 3
mkdir "%STAGE%\docs" || exit /b 3

copy /y "%BUILD%\ArtMiner.exe" "%STAGE%\ArtMiner.exe" >nul || exit /b 4
copy /y "%ROOT%\README.md" "%STAGE%\README.md" >nul || exit /b 4
copy /y "%ROOT%\RAG.md" "%STAGE%\RAG.md" >nul || exit /b 4
copy /y "%ROOT%\docs\*.md" "%STAGE%\docs\" >nul || exit /b 4
copy /y "%ROOT%\examples\*.amr" "%STAGE%\recipes\examples\" >nul || exit /b 4

rem Smoke commands execute against the exact staged portable layout before archiving.
"%STAGE%\ArtMiner.exe" --version || exit /b 5
"%STAGE%\ArtMiner.exe" --workspace "%STAGE%" --check-workspace || exit /b 5
"%STAGE%\ArtMiner.exe" recipe validate "%STAGE%\recipes\examples\am002-minimal.amr" || exit /b 5
set "SMOKE=%TEMP%\artminer-package-smoke-%RANDOM%-%RANDOM%.png"
"%STAGE%\ArtMiner.exe" render "%STAGE%\recipes\examples\am002-minimal.amr" "%SMOKE%" || exit /b 5
del /q "%SMOKE%" 2>nul
del /q "%SMOKE%.artminer.txt" 2>nul

powershell -NoLogo -NoProfile -NonInteractive -Command ^
  "$ErrorActionPreference='Stop'; Compress-Archive -Path '%STAGE%' -DestinationPath '%ZIP%' -CompressionLevel Optimal"
if errorlevel 1 exit /b 6

echo Packaged %STAGE%
echo Archived  %ZIP%
exit /b 0
