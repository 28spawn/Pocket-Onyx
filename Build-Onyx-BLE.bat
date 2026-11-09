@echo off
REM ---------------------------------------------------------------------------
REM  Onyx - build the firmware WITH Bluetooth LE on this PC, in one step.
REM
REM  Checks what the build needs (Python, the JieLi compiler, llvm-mingw), offers
REM  to install what is missing, builds, and puts the result in Onyx-BLE-build.
REM  Nothing is flashed. The work is done by Build-Onyx-BLE.ps1 (read it first if
REM  you like): this file only starts it, and fetches it when it is not here.
REM
REM    Build-Onyx-BLE.bat              ask before each install
REM    Build-Onyx-BLE.bat -Yes         answer every question with yes
REM    Build-Onyx-BLE.bat -CheckOnly   check and install the requirements only
REM
REM  This file works alone too: put it in an empty folder and double-click it.
REM  It then downloads the script and the latest source from GitHub.
REM ---------------------------------------------------------------------------
setlocal
cd /d "%~dp0"

if exist "%~dp0Build-Onyx-BLE.ps1" goto run
echo   Build-Onyx-BLE.ps1 is not in this folder: downloading it from github.com/28spawn/Pocket-Onyx ...
powershell -NoProfile -ExecutionPolicy Bypass -Command "[Net.ServicePointManager]::SecurityProtocol = 'Tls12'; (New-Object Net.WebClient).DownloadFile('https://raw.githubusercontent.com/28spawn/Pocket-Onyx/main/Build-Onyx-BLE.ps1', '%~dp0Build-Onyx-BLE.ps1')"
if not exist "%~dp0Build-Onyx-BLE.ps1" goto nofetch

:run
powershell -NoProfile -ExecutionPolicy Bypass -File "%~dp0Build-Onyx-BLE.ps1" %*
if errorlevel 1 goto failed
echo.
pause
exit /b 0

:nofetch
echo.
echo [ERROR] the script could not be downloaded. Check the internet connection, or get the whole repository from
echo         https://github.com/28spawn/Pocket-Onyx and run Build-Onyx-BLE.bat from inside it.
pause
exit /b 1

:failed
echo.
echo [ERROR] the build did not finish - see the messages above. Nothing was flashed, and your unit was not touched.
pause
exit /b 1
