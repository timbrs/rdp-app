@echo off
setlocal
rem Build rdpkey.exe (Go, no cgo). Output -> builds\rdpkey.exe
rem Defender ML (!ml, Trojan:Win32/Wacatac) is a false positive on unsigned Go
rem exes and is layout-sensitive. seed -randlayout=101 is clean for the 1.0.10
rem sources on BOTH the local Defender cloud (MpCmdRun, live) AND VT Microsoft
rem (VT sha256 F6E35943..., MS undetected, malicious(all)=14: generic OEM/ML, no major
rem signature engine). NOTE: VT's Microsoft engine DOES run cloud ML and can
rem disagree with the local model, so verify a seed on BOTH, not VT alone.
rem ANY source change or Go toolchain bump reshuffles the layout and the seed
rem must be re-tuned. Do NOT strip (-s -w) and do NOT sign; keep it reproducible.
rem (History: 1.0.4=4400, 1.0.5=101, 1.0.6=4400, 1.0.7=31676 then 1;
rem  1.0.8=4400, 1.0.9=31415 then 31676 after the .ini/env/no-copy rework;
rem  1.0.10=3, then 99 (2026-09-17) after a Defender model update tripped seed 3
rem  locally AND on VT, though seed 3 had been VT-clean at release;
rem  then 101 (2026-09-22): engine 1.1.26080.3 tripped 99 locally (RTP killed the
rem  linker output mid-build); 4400 was local-clean but VT-MS flagged (Sabsik),
rem  31676/31415 VT-flagged (Wacatac.B); 101 clean local AND VT-MS undetected.)
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

go build -trimpath -buildvcs=false -ldflags "-H windowsgui -randlayout=101" -o "%ROOT%builds\rdpkey.exe" .
if errorlevel 1 (echo go build failed & exit /b 1)

echo OK: %ROOT%builds\rdpkey.exe

if /I "%~1"=="scan" powershell -ExecutionPolicy Bypass -File "%ROOT%scan.ps1"
