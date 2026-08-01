@echo off
setlocal
rem Сборка rdpkey.exe (MinGW-w64). Итог -> builds\rdpkey.exe

set "GPP=C:\msys64\ucrt64\bin\g++.exe"
set "WINDRES=C:\msys64\ucrt64\bin\windres.exe"

set "ROOT=%~dp0"
if not exist "%ROOT%builds" mkdir "%ROOT%builds"

"%WINDRES%" --include-dir "%ROOT%src" --include-dir "%ROOT%resources" "%ROOT%src\rdpkey.rc" -O coff -o "%ROOT%builds\rdpkey.res.o"
if errorlevel 1 (echo windres failed & exit /b 1)

"%GPP%" -O2 -s -static -mwindows "%ROOT%src\rdpkey.cpp" "%ROOT%builds\rdpkey.res.o" -o "%ROOT%builds\rdpkey.exe" -luser32
if errorlevel 1 (echo g++ failed & exit /b 1)

del "%ROOT%builds\rdpkey.res.o" >nul 2>&1
echo OK: %ROOT%builds\rdpkey.exe
