@echo off
setlocal enabledelayedexpansion

rem Compile without deploying - safe while mergerino.exe is running (the
rem running instance lives in Program Files, not build\bin). Pass ninja
rem targets as arguments (e.g. ".compile-only.bat mergerino-lib" to skip the
rem exe link); no arguments builds everything. Use .local-build.bat /
rem .dev-cycle.bat for the full build+deploy cycle.

set "ROOT=%~dp0"
if "!ROOT:~-1!"=="\" set "ROOT=!ROOT:~0,-1!"

rem Locate VS 2022 (any edition), same scan as .local-build.bat. Without this
rem env a bare "cmake --build"/"ninja" dies with C1083 on 'type_traits'.
if defined VSCMD_VER goto :vs_ready
set "VCVARS="
for %%R in ("%ProgramFiles%\Microsoft Visual Studio\2022" "%ProgramFiles(x86)%\Microsoft Visual Studio\2022") do (
    if not defined VCVARS (
        for /f "delims=" %%E in ('dir /b /ad "%%~R" 2^>nul') do (
            if not defined VCVARS if exist "%%~R\%%E\VC\Auxiliary\Build\vcvars64.bat" set "VCVARS=%%~R\%%E\VC\Auxiliary\Build\vcvars64.bat"
        )
    )
)
if not defined VCVARS (
    echo [compile] No Visual Studio 2022 vcvars64.bat found
    exit /b 1
)
call "%VCVARS%" >nul 2>&1 || (echo [compile] vcvars64.bat failed & exit /b 1)
:vs_ready

if not exist "%ROOT%\build\build.ninja" (
    echo [compile] build\build.ninja missing - run .local-build.bat once first
    exit /b 1
)
cd /d "%ROOT%\build" || exit /b 1
ninja %*
exit /b %errorlevel%
