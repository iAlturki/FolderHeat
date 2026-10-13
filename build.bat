@rem SPDX-License-Identifier: CC-BY-NC-SA-4.0
@rem Copyright (c) 2026 iAlturki
@echo off
REM ============================================================
REM  Build FolderHeat.
REM
REM  Prefers MSVC (Visual Studio Build Tools) because unsigned
REM  MinGW GUI exes tend to trigger antivirus heuristic false
REM  positives; the MSVC static-CRT build does not, and is still
REM  a single self-contained FolderHeat.exe (no VC++ redist).
REM  Falls back to MinGW-w64 (MSYS2 or any gcc on PATH).
REM
REM  Just double-click this file, or run it from a terminal.
REM ============================================================
setlocal
cd /d "%~dp0"

set "LIBS=user32.lib shell32.lib gdi32.lib advapi32.lib ole32.lib oleaut32.lib shlwapi.lib"

REM ---- locate MSVC via vswhere ----
set "VSWHERE=%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe"
set "VSPATH="
if exist "%VSWHERE%" (
    for /f "usebackq tokens=*" %%i in (`"%VSWHERE%" -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath`) do set "VSPATH=%%i"
)

if defined VSPATH if exist "%VSPATH%\VC\Auxiliary\Build\vcvars64.bat" (
    echo [1/2] MSVC toolchain: "%VSPATH%"
    call "%VSPATH%\VC\Auxiliary\Build\vcvars64.bat" >nul
    rc /nologo /fo app.res app.rc || goto :fail
    echo [2/2] Compiling FolderHeat.exe ^(MSVC, static CRT^) ...
    cl /nologo /O2 /W3 /MT /D_CRT_SECURE_NO_WARNINGS /Fe:FolderHeat.exe main.c app.res ^
       %LIBS% /link /SUBSYSTEM:WINDOWS /MANIFEST:NO || goto :fail
    del main.obj app.res >nul 2>&1
    echo.
    echo [OK] Built FolderHeat.exe ^(MSVC^). Run it; look for the folder in your tray.
    goto :eof
)

REM ---- fallback: MinGW-w64 ----
set "MINGW=C:\msys64\mingw64\bin"
if exist "%MINGW%\gcc.exe" set "PATH=%MINGW%;%PATH%"
echo [1/2] MinGW fallback ^(note: unsigned MinGW exes may trip AV false positives^)
windres app.rc -O coff -o app.res || goto :fail
echo [2/2] Compiling FolderHeat.exe ^(MinGW^) ...
gcc -O2 -Wall -Wextra -municode -mwindows -o FolderHeat.exe main.c app.res ^
    -luser32 -lshell32 -lgdi32 -ladvapi32 -lole32 -loleaut32 -lshlwapi || goto :fail
del app.res >nul 2>&1
echo.
echo [OK] Built FolderHeat.exe ^(MinGW^).
goto :eof

:fail
echo.
echo [FAILED] Build error - see messages above.
exit /b 1
