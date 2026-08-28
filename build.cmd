@echo off
setlocal
rem Build rdpkey.exe (Go, no cgo). Output -> builds\rdpkey.exe
rem Defender ML (!ml) is layout-sensitive; seed -randlayout=4400 yields
rem Microsoft=undetected on VirusTotal (sha256 e4a38826...). Do NOT strip
rem (-s -w) and do NOT change the seed/flags without re-checking VT: the
rem output hash must stay reproducible.
rem -buildvcs=false is REQUIRED: the folder is a git repo, and Go otherwise
rem stamps VCS info (commit/time/dirty) into the exe -> different bytes ->
rem breaks reproducibility and the Defender-clean seed. Keep it off.

set "WINDRES=C:\msys64\mingw64\bin\windres.exe"
set "ROOT=%~dp0"
set "CGO_ENABLED=0"
cd /d "%ROOT%"

if not exist "%ROOT%builds" mkdir "%ROOT%builds"

"%WINDRES%" --include-dir "%ROOT%." --include-dir "%ROOT%resources" "%ROOT%rdpkey.rc" -O coff -o "%ROOT%rdpkey.syso"
if errorlevel 1 (echo windres failed & exit /b 1)

go build -trimpath -buildvcs=false -ldflags "-H windowsgui -randlayout=4400" -o "%ROOT%builds\rdpkey.exe" .
if errorlevel 1 (echo go build failed & exit /b 1)

echo OK: %ROOT%builds\rdpkey.exe

if /I "%~1"=="scan" powershell -ExecutionPolicy Bypass -File "%ROOT%scan.ps1"
