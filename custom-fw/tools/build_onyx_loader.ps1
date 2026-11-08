# SPDX-License-Identifier: GPL-3.0-only
# Copyright (C) 2026 The Onyx Authors (@28spawn)
# tools/build_onyx_loader.ps1 - build the Onyx update loader (loader/onyx_loader.c + third_party/pendel_loader, MPL-2.0) into build_pi32/onyx_loader/loader.bin
# (the raw RAM image the SPL loads at 0x01C0A800). Called by build.ps1; can be run alone. The loader writes the app area only: see loader/onyx_loader.c.
param([string]$Out = "")
$ErrorActionPreference = "Stop"
$FW = Split-Path -Parent $PSScriptRoot
if (-not $Out) { $Out = Join-Path $FW "build_pi32\onyx_loader" }
New-Item -ItemType Directory -Force $Out | Out-Null
$cands = @($(if ($env:USERPROFILE) { Join-Path $env:USERPROFILE "tools\jieli\C`$\JL\pi32" }), "C:\JL\pi32", "C:\jieli\pi32")
$root = $cands | Where-Object { $_ -and (Test-Path "$_\bin\clang.exe") } | Select-Object -First 1
if (-not $root) { throw "the JieLi pi32 toolchain was not found (see build.ps1)" }
$bin = "$root\bin"; $tp = Join-Path $FW "third_party\pendel_loader"
$mark = '-DOTA_LOADER_MARK=\"ONYX-LOADER-0001\"'
$flags = @("-target", "pi32v2", "-mcpu=r3", "-Os", "-ffunction-sections", "-fno-builtin", "-ffreestanding", "-Wall", "-Wno-unused-function", "-I$tp\chip", "-I$tp\boards\fm1", "-I$tp\boards\pocketamp", $mark)
& "$bin\clang.exe" @flags -c "$tp\loader\crt0_ldr.S" -o "$Out\crt0_ldr.o"; if ($LASTEXITCODE) { throw "loader crt0 failed" }
& "$bin\clang.exe" @flags -c (Join-Path $FW "loader\onyx_loader.c") -o "$Out\loader.o"; if ($LASTEXITCODE) { throw "loader compile failed" }
& "$bin\pi32v2-ld.exe" --gc-sections -e _start -T "$tp\loader\loader.ld" "$Out\crt0_ldr.o" "$Out\loader.o" -o "$Out\loader.elf"; if ($LASTEXITCODE) { throw "loader link failed" }
& "$bin\llvm-objcopy.exe" "-O=binary" "-j=.text" "$Out\loader.elf" "$Out\loader.bin"; if ($LASTEXITCODE) { throw "loader objcopy failed" }
# the loader rewrites the flash: nothing of it may run from XIP (every section in RAM)
$h = & "$bin\llvm-objdump.exe" -h "$Out\loader.elf"
foreach ($ln in $h) { $p = ($ln -split "\s+") | Where-Object { $_ }; if ($p.Count -gt 4 -and $p[1].StartsWith(".") -and [Convert]::ToUInt64($p[3], 16) -ge 0x02000000 -and [Convert]::ToUInt64($p[2], 16)) { throw "loader: section $($p[1]) is outside RAM" } }
Write-Host ("Onyx loader: {0} bytes -> {1}" -f (Get-Item "$Out\loader.bin").Length, "$Out\loader.bin") -ForegroundColor Cyan
