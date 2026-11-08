# SPDX-License-Identifier: GPL-3.0-only
# Copyright (C) 2026 The Onyx Authors (@28spawn)
# custom-fw/tools/Find-Clang.ps1 - find a clang that can REALLY compile the firmware on this PC, and fetch one when there is none.
#
# Dot-source it, then call Resolve-HostClang (it puts the clang's folder first on PATH for the rest of the script):
#     . (Join-Path $PSScriptRoot "..\tools\Find-Clang.ps1"); $null = Resolve-HostClang
#
# WHY IT EXISTS. The emulator DLL, the host tests and the firmware build need a clang WITH a C library and its headers (stdlib.h, string.h, math.h ...). Two things go wrong on a fresh Windows PC:
#   1. No clang at all, or one installed a moment ago: winget changes PATH for NEW terminals only, so the terminal that is already open still says "clang is not recognized".
#   2. The plain "LLVM" package (winget id LLVM.LLVM) is a clang for Visual Studio's headers. Without Visual Studio Build Tools / a Windows SDK every file stops at
#      "fatal error: 'stdlib.h' file not found". The old scripts took ANY clang on PATH, so this looked like a bug in the project.
# The project uses llvm-mingw: ONE folder with clang, the headers and the UCRT import libraries, nothing else to install.
#
# WORKS ON A PC THAT HAS NOTHING: a clang is accepted only when it compiles a probe that includes the C library headers; llvm-mingw is looked for in winget's folder, scoop, C:\llvm-mingw and
# custom-fw\tools\toolchain; when there is none, the script offers to install it - with winget when the PC has it, otherwise by downloading the official portable zip from GitHub (pinned
# version, SHA-256 checked, unpacked inside the repo, no administrator rights, nothing outside the folder is touched). A person who prefers to do it by hand gets the exact steps.
# Set ONYX_AUTO_INSTALL=1 to answer the question with yes (a script, a CI job), ONYX_AUTO_INSTALL=0 to never install.

$script:OnyxWingetId   = "MartinStorsjo.LLVM-MinGW.UCRT"
# the portable zip winget itself installs (its manifest carries this URL and this SHA-256: checked 2026-11-08)
$script:OnyxZipVersion = "20261020"
$script:OnyxZipUrl     = "https://github.com/mstorsjo/llvm-mingw/releases/download/20261020/llvm-mingw-20261020-ucrt-x86_64.zip"
$script:OnyxZipSha256  = "fd5462b7f5d93b2ff981fcd3f9c7e427e47f3203c64ea91c27bcc444eebd74cd"
$script:OnyxZipBytes   = 190612575
$script:OnyxToolchainDir = Join-Path $PSScriptRoot "toolchain"

# True when `clang -fsyntax-only` can include the C library headers the firmware uses. $ExtraArgs is for the self-test only.
function Test-ClangUsable {
    param([Parameter(Mandatory = $true)][string]$Clang, [string[]]$ExtraArgs = @())
    $ErrorActionPreference = "Continue"            # a native program's stderr must not become a terminating error in here
    $probe = Join-Path ([IO.Path]::GetTempPath()) ("onyx_clang_probe_{0}.c" -f $PID)
    Set-Content -Path $probe -Encoding Ascii -Value "#include <stdlib.h>`n#include <string.h>`n#include <stdio.h>`n#include <math.h>`n#include <stdint.h>`nint main(void) { return 0; }`n"
    try {
        $null = & $Clang -fsyntax-only @ExtraArgs $probe 2>&1
        return ($LASTEXITCODE -eq 0)
    } catch {
        return $false
    } finally {
        Remove-Item -Force $probe -ErrorAction SilentlyContinue
    }
}

# The clang.exe of an llvm-mingw install, wherever winget, scoop, an unzip by hand or this script's own download put it; $null when there is none.
function Get-LlvmMingwClang {
    $found = @()
    $wg = Join-Path $env:LOCALAPPDATA "Microsoft\WinGet\Packages"
    if ($env:LOCALAPPDATA -and (Test-Path $wg)) {
        $found += Get-ChildItem $wg -Directory -Filter "MartinStorsjo.LLVM-MinGW*" -ErrorAction SilentlyContinue |
                  ForEach-Object { Get-ChildItem $_.FullName -Recurse -Filter "clang.exe" -ErrorAction SilentlyContinue }
    }
    foreach ($r in @($script:OnyxToolchainDir, "$env:USERPROFILE\scoop\apps\llvm-mingw\current", "C:\llvm-mingw", "$env:ProgramFiles\llvm-mingw")) {
        if ($r -and (Test-Path $r)) { $found += Get-ChildItem $r -Recurse -Filter "clang.exe" -ErrorAction SilentlyContinue }
    }
    $first = $found | Where-Object { $_ } | Select-Object -First 1
    if ($first) { return $first.FullName }
    return $null
}

# The pinned portable zip: download, SHA-256, unpack into custom-fw\tools\toolchain. Returns the clang.exe path or $null.
function Install-LlvmMingwPortable {
    $ErrorActionPreference = "Stop"
    try {
        [Net.ServicePointManager]::SecurityProtocol = [Net.ServicePointManager]::SecurityProtocol -bor [Net.SecurityProtocolType]::Tls12
        New-Item -ItemType Directory -Force -Path $script:OnyxToolchainDir | Out-Null
        $zip = Join-Path $script:OnyxToolchainDir ("llvm-mingw-{0}-ucrt-x86_64.zip" -f $script:OnyxZipVersion)
        Write-Host ("Downloading llvm-mingw {0} (about {1} MB) from github.com/mstorsjo/llvm-mingw ..." -f $script:OnyxZipVersion, [int]($script:OnyxZipBytes / 1MB)) -ForegroundColor Cyan
        (New-Object System.Net.WebClient).DownloadFile($script:OnyxZipUrl, $zip)
        $sha = (Get-FileHash -Algorithm SHA256 -Path $zip).Hash.ToLower()
        if ($sha -ne $script:OnyxZipSha256) {
            Remove-Item -Force $zip -ErrorAction SilentlyContinue
            throw "the download does not match its expected SHA-256 (got $sha): it was deleted and nothing was installed"
        }
        Write-Host "SHA-256 checked. Unpacking (a minute or two) ..." -ForegroundColor Cyan
        Add-Type -AssemblyName System.IO.Compression.FileSystem
        [IO.Compression.ZipFile]::ExtractToDirectory($zip, $script:OnyxToolchainDir)
        Remove-Item -Force $zip -ErrorAction SilentlyContinue
        return (Get-LlvmMingwClang)
    } catch {
        Write-Host ("The download did not work: " + $_.Exception.Message) -ForegroundColor Red
        return $null
    }
}

function Resolve-HostClang {
    [CmdletBinding()]
    param()

    $candidates = @()
    $mingw = Get-LlvmMingwClang
    if ($mingw) { $candidates += $mingw }                                   # llvm-mingw first: it is complete by itself
    $onPath = Get-Command clang.exe -ErrorAction SilentlyContinue
    if ($onPath) { $candidates += $onPath.Source }
    $broken = @()
    foreach ($c in ($candidates | Select-Object -Unique)) {
        if (Test-ClangUsable $c) {
            $env:PATH = (Split-Path -Parent $c) + ";" + $env:PATH
            return $c
        }
        $broken += $c
    }

    # Nothing usable on this PC.
    Write-Host ""
    if ($broken.Count) {
        Write-Host ("A clang was found (" + ($broken -join ", ") + ") but it cannot find the C library headers (stdlib.h, string.h, math.h).") -ForegroundColor Yellow
        Write-Host "That is usually the plain LLVM package (winget id LLVM.LLVM): it relies on Visual Studio's headers, which this PC does not have. The firmware tools use a different, self-contained package (llvm-mingw)." -ForegroundColor Yellow
    } else {
        Write-Host "No clang compiler was found on this PC. The firmware tools use llvm-mingw (one folder, nothing else to install)." -ForegroundColor Yellow
    }
    $winget = Get-Command winget.exe -ErrorAction SilentlyContinue
    $auto = $env:ONYX_AUTO_INSTALL
    $answer = $null
    if ($auto -eq "1") { $answer = "y" }
    elseif ($auto -ne "0") {
        try { if ([Environment]::UserInteractive) { $answer = Read-Host "Install it now? [Y/n]" } } catch { $answer = $null }
    }
    if ($answer -ne $null -and ($answer -eq "" -or $answer -match '^[Yy]')) {
        $c = $null
        if ($winget) {
            Write-Host "Installing with winget ..." -ForegroundColor Cyan
            try { & winget install -e --id $script:OnyxWingetId --accept-package-agreements --accept-source-agreements } catch { }
            $c = Get-LlvmMingwClang
            if (-not ($c -and (Test-ClangUsable $c))) { $c = $null }
        }
        if (-not $c) { $c = Install-LlvmMingwPortable }                     # no winget on this PC (older Windows 10, LTSC, a locked-down one), or winget failed
        if ($c -and (Test-ClangUsable $c)) {
            $env:PATH = (Split-Path -Parent $c) + ";" + $env:PATH
            Write-Host "llvm-mingw is ready: $c" -ForegroundColor Green
            return $c
        }
        Write-Host "The install did not finish." -ForegroundColor Red
    }
    $msg = "No usable C compiler. Install llvm-mingw, then run this script again (no new terminal is needed).`n" +
           "  with winget:   winget install -e --id $script:OnyxWingetId`n" +
           "  by hand:       download $script:OnyxZipUrl`n" +
           "                 (SHA-256 $script:OnyxZipSha256),`n" +
           "                 unzip it into  $script:OnyxToolchainDir  (so that ...\toolchain\llvm-mingw-...\bin\clang.exe exists)"
    throw $msg
}
