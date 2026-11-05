# custom-fw/emu/run_emu.ps1 — Build & Launch Pocket Amp Live Emulator + ASIO Audio Workbench
$ErrorActionPreference = "Stop"
$RepoRoot = Split-Path -Parent (Split-Path -Parent $PSScriptRoot)
Set-Location $RepoRoot

$clangExe = Get-Command clang.exe -ErrorAction SilentlyContinue
if (-not $clangExe) {
    $clangDir = (Get-ChildItem "$env:LOCALAPPDATA\Microsoft\WinGet\Packages\MartinStorsjo.LLVM-MinGW.UCRT*" -Recurse -Filter "clang.exe" | Select-Object -First 1).DirectoryName
    $env:PATH = "$clangDir;$env:PATH"
}

Write-Host "[1/2] Compiling Pocket Amp Firmware into Native Emulator DLL (pocket_amp_emu.dll)..." -ForegroundColor Cyan

# ---- the source list -------------------------------------------------------
# Globbed through custom-fw/sources.txt, so a new module can never be
# silently forgotten by one target. The only difference between the targets is
# which board files they replace themselves, and that is written down ONCE.
. custom-fw\tools\Read-Sources.ps1
$manifest = Read-SourceManifest
$sources = @(Get-SourceFiles -Target "emu" -Sections $manifest | ForEach-Object { "custom-fw/" + $_ })

clang -O3 -shared -Wall -Wextra -Icustom-fw/include -DPA_HOST_SIM=1 -DPA_EMU_DLL=1 `
  custom-fw/emu/emu_bridge.c `
  $sources `
  -o custom-fw/emu/pocket_amp_emu.dll

# Pick the first free port in 8080..8099. 8080 is commonly taken by some other
# dev server; before this the emulator just died with "address already in use".
$used = @((Get-NetTCPConnection -State Listen -ErrorAction SilentlyContinue).LocalPort)
$port = 8080
while (($used -contains $port) -and ($port -lt 8100)) { $port++ }

Write-Host "[2/2] Starting Low-Latency ASIO Audio Host & Web UI at http://localhost:$port ..." -ForegroundColor Green
Start-Process "http://localhost:$port"
python custom-fw/emu/server.py --port $port
