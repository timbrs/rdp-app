@echo off
setlocal
rem Сборка rdpkey.exe (C, mingw-w64, без cgo/Go). Вывод -> builds\rdpkey.exe
rem Русские строки в исходниках — UTF-8; L"..." кодируем в UTF-16LE
rem (-fwide-exec-charset). В swprintf для wchar_t* используем %ls, НЕ %s (в mingw
rem широкий %s = char*). Иконка берётся из resources\app.ico, манифест и help.html
rem (встроен ресурсом RCDATA) — из корня.
set "MINGW=C:\msys64\mingw64\bin"
set "ROOT=%~dp0"
cd /d "%ROOT%"
if not exist "%ROOT%builds" mkdir "%ROOT%builds"

rem build.cmd log -> builds\rdpkey-log.exe с отладочным логом в %LOCALAPPDATA%\rdpkey\logs
set "OUT=rdpkey.exe"
set "FLAGS=-s"
if /I "%~1"=="log" (
  set "OUT=rdpkey-log.exe"
  set "FLAGS=-g -DRDPKEY_LOG"
)

"%MINGW%\windres.exe" --include-dir "%ROOT%." --include-dir "%ROOT%resources" "%ROOT%rdpkey.rc" -O coff -o "%ROOT%rdpkey_res.o"
if errorlevel 1 (echo windres failed & exit /b 1)

"%MINGW%\gcc.exe" -O2 -municode -mwindows -DUNICODE -D_UNICODE ^
  -finput-charset=UTF-8 -fexec-charset=UTF-8 -fwide-exec-charset=UTF-16LE ^
  %FLAGS% -Wl,--nxcompat,--dynamicbase,--high-entropy-va ^
  -o "%ROOT%builds\%OUT%" ^
  main.c winutil.c config.c assoc.c rdp.c cert.c hook.c session.c gui.c errdialog.c dlog.c ^
  rdpkey_res.o ^
  -luser32 -lgdi32 -lcomctl32 -lcomdlg32 -lshell32 -ladvapi32 -lcrypt32
if errorlevel 1 (echo gcc failed & exit /b 1)

echo OK: %ROOT%builds\%OUT%

if /I "%~1"=="scan" powershell -ExecutionPolicy Bypass -File "%ROOT%.avtools\scan.ps1"
