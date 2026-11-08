@echo off
REM ---------------------------------------------------------------------------
REM  Pocket Amp Workbench - build (if needed) and serve, then open the browser.
REM
REM    start_workbench.bat                build if needed, serve, open the browser
REM    start_workbench.bat --rebuild      force a rebuild first
REM    start_workbench.bat --port 8098    serve on another port (default 8099)
REM ---------------------------------------------------------------------------
setlocal
cd /d "%~dp0"

set SERVE_ARGS=
if /I "%~1"=="--port" set SERVE_ARGS=--port %~2
if /I "%~2"=="--port" set SERVE_ARGS=--port %~3

set ZIG=
if exist "%LOCALAPPDATA%\Temp\opencode\zig\zig-x86_64-windows-0.17.0\zig.exe" set ZIG=%LOCALAPPDATA%\Temp\opencode\zig\zig-x86_64-windows-0.17.0\zig.exe
if not "%ZIG%"=="" if not exist "%ZIG%" set ZIG=
if "%ZIG%"=="" for %%z in (zig.exe) do if not "%%~$PATH:z"=="" set ZIG=%%~$PATH:z

if /I "%~1"=="--rebuild" goto build
if not exist "pocket_amp.wasm" goto build
if "%ZIG%"=="" goto serve
rem the wasm exists; just serve it
:serve
echo.
echo   Starting the workbench ...
python serve.py %SERVE_ARGS%
goto :eof

:build
echo.
echo   Building the firmware for WebAssembly ...
rem build_wasm.py looks for zig in more places than this file does (winget, scoop, a plain unzip) and says how to install it when there is none
if "%ZIG%"=="" (
  python build_wasm.py
) else (
  python build_wasm.py --zig "%ZIG%"
)
if errorlevel 1 (
  pause
  goto :eof
)
echo.
goto serve
