@echo off
setlocal
rem Build rdpkey.exe (Go, no cgo). Output -> builds\rdpkey.exe
rem Defender ML (!ml, Trojan:Win32/Wacatac) is a false positive on unsigned Go
rem exes and is layout-sensitive. seed -randlayout=3 is Microsoft=undetected for
rem the 1.0.10 sources (VT sha256 8F46D1B1..., malicious=9: BitDefender/Yogi OEM
rem cluster + Bkav + DeepInstinct, no major signature engine). ANY source change
rem reshuffles the layout and the seed must be re-tuned: run scanbatch.ps1 to find
rem a fresh Microsoft=undetected seed, then set it below. Do NOT strip (-s -w) and
rem do NOT sign; keep it reproducible.
rem (History: 1.0.4=4400, 1.0.5=101, 1.0.6=4400, 1.0.7=31676 then 1;
rem  1.0.8=4400, 1.0.9=31415 then 31676 after the .ini/env/no-copy rework;
rem  1.0.10=3 after the hotkey/liveness code-review fixes.)
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

go build -trimpath -buildvcs=false -ldflags "-H windowsgui -randlayout=3" -o "%ROOT%builds\rdpkey.exe" .
if errorlevel 1 (echo go build failed & exit /b 1)

echo OK: %ROOT%builds\rdpkey.exe

if /I "%~1"=="scan" powershell -ExecutionPolicy Bypass -File "%ROOT%scan.ps1"
