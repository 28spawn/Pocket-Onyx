# SPDX-License-Identifier: GPL-3.0-only
# Copyright (C) 2026 The Onyx Authors (@28spawn)
# Build-Onyx-BLE.ps1 - one step from "a Windows PC" to "an Onyx firmware with Bluetooth LE, built on this PC".
#
#     double-click Build-Onyx-BLE.bat          (or:  powershell -ExecutionPolicy Bypass -File Build-Onyx-BLE.ps1)
#
# WHY IT EXISTS. The published Onyx packages are USB-only (PA_BLE=0): a firmware with the radio on is not distributed. A person who wants BLE-MIDI builds it from the
# source, and that needed four things installed by hand. This script checks each one, offers to install what is missing, and runs the normal build with -Ble:
#   1. the source      - the folder this script is in when that is the repository; otherwise the latest "main" from GitHub (a zip: git is not needed)
#   2. Python 3.10+    - installed with winget when missing
#   3. the JieLi pi32v2 compiler - downloaded from JieLi's own server (the official installer, SHA-256 checked) and UNPACKED with innoextract into
#                        %USERPROFILE%\tools\jieli: the installer is not run, no administrator rights, nothing else on the PC is touched
#   4. a host C compiler (llvm-mingw) - found or installed by custom-fw\tools\Find-Clang.ps1
# The result is copied to the folder Onyx-BLE-build next to this script. Nothing is flashed: that is done by hand, in the web editor.
#
#   -Yes         answer every question with yes (also: ONYX_AUTO_INSTALL=1)
#   -CheckOnly   check and install the requirements, do not build
#   -WorkDir     where the source is downloaded to when this script is not inside the repository (default: Pocket-Onyx next to the script)

[CmdletBinding()]
param(
    [switch]$Yes = $false,
    [switch]$CheckOnly = $false,
    [string]$WorkDir = ""
)

$ErrorActionPreference = "Stop"
$Here = $PSScriptRoot
[Net.ServicePointManager]::SecurityProtocol = [Net.ServicePointManager]::SecurityProtocol -bor [Net.SecurityProtocolType]::Tls12

$RepoZipUrl   = "https://github.com/28spawn/Pocket-Onyx/archive/refs/heads/main.zip"
# JieLi's Windows toolchain installer (an Inno Setup program). https://pkgman.jieliapp.com/s/win-toolchain redirects to this file; the version is pinned so that the SHA-256 can be checked (2026-11-09).
$JlUrl        = "https://jl-update.oss-cn-shenzhen.aliyuncs.com/2.5.3.exe"
$JlSha256     = "399bd518a28e47a7a0394506cebd5bf658748a58cd6b7c02512e24b4b88c5a5e"
$JlBytes      = 52860672
# innoextract unpacks an Inno Setup installer without running it (zlib licence, https://constexpr.org/innoextract/)
$InnoUrl      = "https://github.com/dscharrer/innoextract/releases/download/1.9/innoextract-1.9-windows.zip"
$InnoSha256   = "6989342c9b026a00a72a38f23b62a8e6a22cc5de69805cf47d68ac2fec993065"
$JlHome       = Join-Path $env:USERPROFILE "tools\jieli"                 # build.ps1 and the other scripts already look in here
$PythonWinget = "Python.Python.3.12"

function Step([string]$t) { Write-Host ""; Write-Host ("=== " + $t + " ===") -ForegroundColor Cyan }
function Ok([string]$t)   { Write-Host ("  ok    " + $t) -ForegroundColor Green }
function Note([string]$t) { Write-Host ("        " + $t) -ForegroundColor DarkGray }

# A yes/no question. -Yes and ONYX_AUTO_INSTALL=1 answer yes; a session nobody can type into answers no.
function Confirm-Step([string]$Question) {
    if ($Yes -or $env:ONYX_AUTO_INSTALL -eq "1") { return $true }
    if ($env:ONYX_AUTO_INSTALL -eq "0") { return $false }
    try { $a = Read-Host ($Question + " [Y/n]") } catch { return $false }
    return ($a -eq "" -or $a -match '^[Yy]')
}

function Get-Download([string]$Url, [string]$Dest, [string]$Sha256) {
    (New-Object System.Net.WebClient).DownloadFile($Url, $Dest)
    if ($Sha256) {
        $got = (Get-FileHash -Algorithm SHA256 -Path $Dest).Hash.ToLower()
        if ($got -ne $Sha256) {
            Remove-Item -Force $Dest -ErrorAction SilentlyContinue
            throw "the download of $Url does not match its expected SHA-256 (got $got): it was deleted and nothing was installed"
        }
    }
}

function Expand-Zip([string]$Zip, [string]$Dest) {
    Add-Type -AssemblyName System.IO.Compression.FileSystem
    [IO.Compression.ZipFile]::ExtractToDirectory($Zip, $Dest)
}

# True when the program runs and exits 0 (a native program's stderr must not stop the script in here).
function Test-Runs([string]$Exe, [string[]]$Arguments) {
    $ErrorActionPreference = "Continue"
    try { $null = & $Exe @Arguments 2>&1; return ($LASTEXITCODE -eq 0) } catch { return $false }
}

Write-Host ""
Write-Host "Onyx - build the firmware WITH Bluetooth LE on this PC" -ForegroundColor White
Write-Host ""
Write-Host "Please read:" -ForegroundColor Yellow
Write-Host "  - This build switches the unit's 2.4 GHz radio ON. The published Onyx packages keep it off. A radio transmitter is" -ForegroundColor Yellow
Write-Host "    regulated in most countries: whether you may operate it is your own responsibility." -ForegroundColor Yellow
Write-Host "  - The firmware you build is for your own unit. Please do not pass the built files on." -ForegroundColor Yellow
Write-Host "  - No warranty. Back up your unit in the Onyx Web Editor before you install any firmware." -ForegroundColor Yellow
Write-Host ""
if (-not (Confirm-Step "Continue?")) { Write-Host "Stopped. Nothing was changed."; exit 1 }

# ---- 1. the source ----------------------------------------------------------
Step "1/5  The source"
$Repo = $null
if (Test-Path (Join-Path $Here "custom-fw\build.ps1")) {
    $Repo = $Here
    Ok "this folder is the repository: $Repo"
    Note "it is used as it is. To build the newest source, update it first (git pull), or run this script from an empty folder."
} else {
    if (-not $WorkDir) { $WorkDir = Join-Path $Here "Pocket-Onyx" }
    Write-Host "  Downloading the latest source (branch main) from github.com/28spawn/Pocket-Onyx ..."
    $tmp = Join-Path $Here ("_onyx_src_{0}" -f $PID)
    if (Test-Path $tmp) { Remove-Item -Recurse -Force $tmp }
    New-Item -ItemType Directory -Force $tmp | Out-Null
    $zip = Join-Path $tmp "main.zip"
    Get-Download $RepoZipUrl $zip ""
    Expand-Zip $zip $tmp
    Remove-Item -Force $zip
    $inner = Get-ChildItem $tmp -Directory | Select-Object -First 1
    if (-not $inner -or -not (Test-Path (Join-Path $inner.FullName "custom-fw\build.ps1"))) { throw "the downloaded source has no custom-fw\build.ps1" }
    # A compiler an earlier run unpacked inside the old copy (and the three SDK files) are kept: they are large and do not change.
    foreach ($keep in @("custom-fw\tools\toolchain", "custom-fw\third_party\jieli_sdk_files")) {
        $old = Join-Path $WorkDir $keep
        if (Test-Path $old) { Move-Item -Force $old (Join-Path $inner.FullName $keep) }
    }
    if (Test-Path $WorkDir) { Remove-Item -Recurse -Force $WorkDir }
    Move-Item $inner.FullName $WorkDir
    Remove-Item -Recurse -Force $tmp
    $Repo = $WorkDir
    Ok "the latest source is in $Repo"
}

# ---- 2. Python --------------------------------------------------------------
Step "2/5  Python 3.10 or newer"
function Find-Python {
    $cands = @()
    $c = Get-Command python.exe -ErrorAction SilentlyContinue
    if ($c) { $cands += $c.Source }
    foreach ($pat in @("$env:LOCALAPPDATA\Programs\Python\Python3*\python.exe", "$env:ProgramFiles\Python3*\python.exe", "C:\Python3*\python.exe")) {
        $cands += (Get-ChildItem $pat -ErrorAction SilentlyContinue | Sort-Object FullName -Descending | ForEach-Object { $_.FullName })
    }
    foreach ($p in ($cands | Where-Object { $_ } | Select-Object -Unique)) {
        # "python.exe" in WindowsApps can be the Microsoft Store placeholder: it exists but is not a Python. Only a program that really answers counts.
        if (Test-Runs $p @("-c", "import sys; sys.exit(0 if sys.version_info >= (3, 10) else 1)")) { return $p }
    }
    return $null
}
$py = Find-Python
if (-not $py) {
    Write-Host "  Python 3.10+ was not found on this PC." -ForegroundColor Yellow
    $winget = Get-Command winget.exe -ErrorAction SilentlyContinue
    if ($winget -and (Confirm-Step "  Install Python 3.12 with winget (for your user only)?")) {
        try { & winget install -e --id $PythonWinget --scope user --accept-package-agreements --accept-source-agreements } catch { }
        $py = Find-Python
    }
    if (-not $py) { throw "Python is needed. Install it from https://www.python.org/downloads/ (tick 'Add python.exe to PATH'), then run this script again." }
}
$env:PATH = (Split-Path -Parent $py) + ";" + $env:PATH                    # the build calls "python": this one is first now, also right after an install
Ok $py

# ---- 3. the JieLi pi32v2 compiler -------------------------------------------
Step "3/5  The JieLi pi32v2 compiler (it compiles for the unit's processor)"
function Test-Pi32([string]$Root) {
    if (-not $Root) { return $false }
    foreach ($f in @("bin\clang.exe", "bin\pi32v2-ld.exe", "bin\llvm-objcopy.exe", "bin\llvm-nm.exe", "bin\llvm-objdump.exe", "pi32v2-lib\r3\libc.a", "pi32v2-lib\r3\libm.a", "pi32v2-lib\r3\libcompiler_rt.a")) {
        if (-not (Test-Path (Join-Path $Root $f))) { return $false }
    }
    return (Test-Path (Join-Path $Root "pi32v2-include"))
}
function Find-Pi32 {
    foreach ($r in @($env:PI32_ROOT, (Join-Path $JlHome 'C$\JL\pi32'), "C:\JL\pi32", "C:\jieli\pi32")) { if (Test-Pi32 $r) { return $r } }
    return $null
}
$pi32 = Find-Pi32
if (-not $pi32) {
    Write-Host "  It was not found on this PC." -ForegroundColor Yellow
    Write-Host ("  It can be downloaded from JieLi's own server ({0}, {1} MB) and unpacked into" -f $JlUrl, [int]($JlBytes / 1MB))
    Write-Host ("  {0} . The installer is NOT run: it is unpacked with innoextract (github.com/dscharrer/innoextract)." -f $JlHome)
    Write-Host "  The toolchain is JieLi's software, under JieLi's own terms; it is not part of Onyx."
    if (-not (Confirm-Step "  Download and unpack it now?")) {
        throw "The JieLi toolchain is needed. Get it from https://pkgman.jieliapp.com/s/win-toolchain , install it (it goes to C:\JL\pi32), then run this script again."
    }
    $dl = Join-Path $JlHome "_download"
    if (Test-Path $dl) { Remove-Item -Recurse -Force $dl }
    New-Item -ItemType Directory -Force $dl | Out-Null
    try {
        Write-Host "  Downloading innoextract ..."
        Get-Download $InnoUrl (Join-Path $dl "innoextract.zip") $InnoSha256
        Expand-Zip (Join-Path $dl "innoextract.zip") (Join-Path $dl "innoextract")
        Write-Host "  Downloading the JieLi toolchain (the server is in China: this can take a few minutes) ..."
        Get-Download $JlUrl (Join-Path $dl "jl_toolchain.exe") $JlSha256
        Write-Host "  SHA-256 checked. Unpacking ..."
        & (Join-Path $dl "innoextract\innoextract.exe") -s -d $JlHome (Join-Path $dl "jl_toolchain.exe")
        if ($LASTEXITCODE -ne 0) { throw "innoextract could not unpack the installer (exit $LASTEXITCODE)" }
    } finally {
        Remove-Item -Recurse -Force $dl -ErrorAction SilentlyContinue
    }
    $pi32 = Find-Pi32
    if (-not $pi32) { throw "the toolchain was unpacked into $JlHome but its files were not found there" }
}
# One real compile for the unit's processor: a toolchain that cannot start (a missing runtime, an antivirus block) is reported here, not in the middle of the build.
$probe = Join-Path ([IO.Path]::GetTempPath()) ("onyx_pi32_probe_{0}.c" -f $PID)
Set-Content -Path $probe -Encoding Ascii -Value "int onyx_probe(int a) { return a + 1; }"
$probeOk = Test-Runs (Join-Path $pi32 "bin\clang.exe") @("-target", "pi32v2", "-mcpu=r3", "-O2", "-ffreestanding", "-c", $probe, "-o", ($probe + ".o"))
Remove-Item -Force $probe, ($probe + ".o") -ErrorAction SilentlyContinue
if (-not $probeOk) { throw "the JieLi compiler in $pi32 is there but could not compile a one-line test for the pi32v2 processor (run  `"$pi32\bin\clang.exe`" --version  to see why)" }
$env:PI32_ROOT = $pi32
Ok $pi32

# ---- 4. the host C compiler -------------------------------------------------
Step "4/5  A C compiler for the tests and the emulator (llvm-mingw)"
$finder = Join-Path $Repo "custom-fw\tools\Find-Clang.ps1"
if (Test-Path $finder) {
    if ($Yes) { $env:ONYX_AUTO_INSTALL = "1" }
    . $finder
    $hostClang = Resolve-HostClang                                           # asks before it installs; throws with the steps by hand when there is none
    Ok $hostClang
} elseif (Get-Command clang.exe -ErrorAction SilentlyContinue) {
    Ok ((Get-Command clang.exe).Source)
} else {
    throw "No clang was found. Install llvm-mingw (winget install -e --id MartinStorsjo.LLVM-MinGW.UCRT), open a NEW terminal, then run this script again."
}

if ($CheckOnly) {
    Write-Host ""
    Write-Host "Everything the build needs is on this PC. (-CheckOnly: the firmware was not built.)" -ForegroundColor Green
    exit 0
}

# ---- 5. the build -----------------------------------------------------------
Step "5/5  Building the firmware with Bluetooth LE (tests, compile, effect matrix, package: several minutes)"
$git = Get-Command git.exe -ErrorAction SilentlyContinue
$isGit = [bool]($git -and (Test-Path (Join-Path $Repo ".git")))
# In a git checkout the build writes over the published USB-only package in custom-fw\release, which is tracked. When that folder had no changes before,
# it is put back afterwards, so that a firmware with the radio on cannot be committed by accident.
$releaseWasClean = $false
if ($isGit) { $releaseWasClean = (-not (& git -C $Repo status --porcelain -- custom-fw/release)) }
$t0 = Get-Date
& powershell.exe -NoProfile -ExecutionPolicy Bypass -File (Join-Path $Repo "custom-fw\build.ps1") -Ble
if ($LASTEXITCODE -ne 0) { throw "The build stopped with an error (see the messages above). No firmware was produced." }

$rel = Get-ChildItem (Join-Path $Repo "custom-fw\release") -Directory | Where-Object { Get-ChildItem $_.FullName -Filter "*.fwsc" | Where-Object { $_.LastWriteTime -ge $t0 } } | Select-Object -First 1
if (-not $rel) { throw "the build finished but no new package was found in custom-fw\release" }
$base = $rel.Name
$out = Join-Path $Here "Onyx-BLE-build"
if (Test-Path $out) { Remove-Item -Recurse -Force $out }
New-Item -ItemType Directory -Force $out | Out-Null
Copy-Item -Force (Join-Path $rel.FullName "$base.fwsc") (Join-Path $out "${base}_BLE.fwsc")
Copy-Item -Force (Join-Path $rel.FullName "$base.bin")  (Join-Path $out "${base}_BLE.bin")
Copy-Item -Recurse -Force (Join-Path $rel.FullName "LICENSES") (Join-Path $out "LICENSES")
Copy-Item -Force (Join-Path $rel.FullName "NOTICE.md") (Join-Path $out "NOTICE.md")
$sums = ""
foreach ($f in (Get-ChildItem $out -File)) { $sums += ("{0}  {1}`r`n" -f (Get-FileHash $f.FullName -Algorithm SHA256).Hash.ToLower(), $f.Name) }
$readme = "Onyx firmware WITH Bluetooth LE (PA_BLE=1), built on this PC on $($t0.ToString('yyyy-MM-dd HH:mm')).`r`n`r`n" +
          "  ${base}_BLE.fwsc   the update package: Onyx Web Editor, Firmware tab, Install Firmware`r`n" +
          "  ${base}_BLE.bin    the app image, for custom-fw\tools\jl_safe_flash.py`r`n`r`n" +
          "This build switches the unit's 2.4 GHz radio on. It is for your own unit: please do not pass these files on.`r`n" +
          "Back up the unit in the Onyx Web Editor before you install.`r`n`r`nSHA-256:`r`n" + $sums
[System.IO.File]::WriteAllText((Join-Path $out "README.txt"), $readme)
if ($isGit -and $releaseWasClean) {
    $ErrorActionPreference = "Continue"
    & git -C $Repo checkout -q -- custom-fw/release 2>$null
    & git -C $Repo clean -fdq -- custom-fw/release 2>$null
    $ErrorActionPreference = "Stop"
    Note "custom-fw\release was put back to the published USB-only package (the BLE build is in Onyx-BLE-build only)."
}

Write-Host ""
Write-Host "Done. Your firmware with Bluetooth LE:" -ForegroundColor Green
Write-Host ("    " + (Join-Path $out "${base}_BLE.fwsc")) -ForegroundColor Green
Write-Host "Install it with the Onyx Web Editor (custom-fw\web\Onyx_Web_Editor.html, in Chrome or Edge): back up first, then Firmware tab, Install Firmware."
exit 0
