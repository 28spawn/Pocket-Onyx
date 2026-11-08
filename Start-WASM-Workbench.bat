@echo off
REM ---------------------------------------------------------------------------
REM  Onyx - WebAssembly workbench (the same firmware, in the browser).
REM
REM  Serves the wasm build and opens the workbench (presets, the panel, tone loading and audio controls all run there).
REM
REM    Start-WASM-Workbench.bat            build if needed, serve, open browser
REM    Start-WASM-Workbench.bat --rebuild  force a rebuild first (~2 min)
REM
REM  The workbench is served on the first free port in 8099..8199, so it still
REM  works when something else already owns 8099.
REM  Close this window (or Ctrl+C) to stop the server.
REM ---------------------------------------------------------------------------
setlocal
cd /d "%~dp0"

if not exist "custom-fw\wasm\serve.py" (
  echo [ERROR] custom-fw\wasm\serve.py is missing - is this the repo root?
  pause
  exit /b 1
)

rem First free port in 8099..8199.
set PORT=8099
for /f %%p in ('powershell -NoProfile -Command "$u=@((Get-NetTCPConnection -State Listen -ErrorAction SilentlyContinue).LocalPort); $p=8099; while(($u -contains $p) -and ($p -lt 8200)){$p++}; $p"') do set PORT=%%p

if /I "%~1"=="--rebuild" (
  call "custom-fw\wasm\start_workbench.bat" --rebuild --port %PORT%
) else (
  call "custom-fw\wasm\start_workbench.bat" --port %PORT%
)

if errorlevel 1 (
  echo.
  echo [ERROR] the workbench stopped with an error (see above).
  pause
)
