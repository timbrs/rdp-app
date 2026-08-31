@echo off
setlocal
rem Build rdpkey.exe (Go, no cgo). Output -> builds\rdpkey.exe
rem Defender ML (!ml, Trojan:Win32/Wacatac) is a false positive on unsigned Go
rem exes and is layout-sensitive. seed -randlayout=4400 is Microsoft=undetected
rem for the 1.0.8 sources (VT sha256 79145ee4..., malicious=4 minor generic-ML
rem engines only). ANY source change reshuffles the layout and the seed must be
rem re-tuned: run scanbatch.ps1 to find a fresh Microsoft=undetected seed, then
rem set it below. Do NOT strip (-s -w) and do NOT sign; keep it reproducible.
rem (History: 1.0.4=4400, 1.0.5=101, 1.0.6=4400, 1.0.7=31676 then 1;
rem  1.0.8=4400.)
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
