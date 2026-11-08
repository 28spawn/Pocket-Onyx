@echo off
REM ---------------------------------------------------------------------------
REM  Onyx - live firmware emulator.
REM
REM  Runs the REAL firmware (compiled as pocket_amp_emu.dll) with the board
REM  layer replaced, and serves the front panel in your browser.
REM
REM    Start-Emulator.bat            serve the panel (builds the DLL if missing)
REM    Start-Emulator.bat --rebuild  recompile the DLL first (~90 s)
REM
REM  The panel is served on the first free port in 8080..8099, so it still
REM  works when something else already owns 8080.
REM  Close this window (or Ctrl+C) to stop the server.
REM ---------------------------------------------------------------------------
setlocal
cd /d "%~dp0"

where python >nul 2>nul
if errorlevel 1 (
  echo [ERROR] python was not found on PATH.
  echo         Install it from https://python.org and tick "Add to PATH".
  pause
  exit /b 1
)

if not exist "custom-fw\emu\ui.html" (
  echo [ERROR] custom-fw\emu\ui.html is missing - is this the repo root?
  pause
  exit /b 1
)

rem First free port in 8080..8099 (same rule as run_emu.ps1).
set PORT=8080
for /f %%p in ('powershell -NoProfile -Command "$u=@((Get-NetTCPConnection -State Listen -ErrorAction SilentlyContinue).LocalPort); $p=8080; while(($u -contains $p) -and ($p -lt 8100)){$p++}; $p"') do set PORT=%%p

if /I "%~1"=="--rebuild" goto build
if not exist "custom-fw\emu\pocket_amp_emu.dll" goto build

echo.
echo   Emulator DLL found - starting the panel on http://localhost:%PORT%
echo.
start "" "http://localhost:%PORT%"
python "custom-fw\emu\server.py" --port %PORT%
goto done

:build
echo.
echo   Building the emulator DLL from the firmware sources ...
echo.
powershell -NoProfile -ExecutionPolicy Bypass -File "custom-fw\emu\run_emu.ps1"
goto done

:done
if errorlevel 1 (
  echo.
  echo [ERROR] the emulator stopped with an error - see the messages above.
  pause
)
