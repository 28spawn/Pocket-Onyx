# SPDX-License-Identifier: GPL-3.0-only
# Copyright (C) 2026 The Onyx Authors (@28spawn)
# Compile-only check of the imported BLE stack for the pi32v2 target (BLE-PORT-PLAN.md step 0). Nothing is linked, flashed or run.
#   powershell -File custom-fw\tools\check_ble_build.ps1
# Builds src/board/ble_radio.c with PA_BLE=1 (must compile with no warning and no error) and PA_BLE=0 (must produce an empty object),
# and prints the object's section sizes, which is the stack's cost before linking.
$ErrorActionPreference = "Stop"
$repo = (Resolve-Path (Join-Path $PSScriptRoot "..\..")).Path
$root = if ($env:PI32_ROOT) { $env:PI32_ROOT } else {
    $cands = @(
        $(if ($env:USERPROFILE) { Join-Path $env:USERPROFILE "tools\jieli\C$\JL\pi32" }),
        "C:\JL\pi32",
        "C:\jieli\pi32"
    )
    ($cands | Where-Object { $_ -and (Test-Path "$_\bin\clang.exe") } | Select-Object -First 1)
}
if (-not $root -or -not (Test-Path "$root\bin\clang.exe")) { $root = "C:\JL\pi32" }
$bin = "$root\bin"; $inc = "$root\pi32v2-include"
$tp = Join-Path $repo "custom-fw\third_party\optimist_ble"
$src = Join-Path $repo "custom-fw\src\board\ble_radio.c"
$tmp = Join-Path $env:TEMP "check_ble_build"; New-Item -ItemType Directory -Force $tmp | Out-Null
$bad = 0
foreach ($ble in 1, 0) {
    $obj = Join-Path $tmp "ble_radio_$ble.o"
    $args = @("-target", "pi32v2", "-mcpu=r3", "-O2", "-Wall", "-Wno-tautological-constant-out-of-range-compare", "-ffreestanding", "-fno-builtin",
              "-isystem", $inc, "-I$repo\custom-fw\include", "-I$repo\custom-fw\src\board\ble_platform", "-I$tp\src", "-I$tp\hal", "-DPA_BLE=$ble", "-c", $src, "-o", $obj)
    $psi = New-Object System.Diagnostics.ProcessStartInfo
    $psi.FileName = "$bin\clang.exe"
    $psi.Arguments = ($args | ForEach-Object { if ($_ -match '\s') { '"' + $_ + '"' } else { $_ } }) -join " "
    $psi.RedirectStandardError = $true; $psi.UseShellExecute = $false
    $p = [Diagnostics.Process]::Start($psi); $err = $p.StandardError.ReadToEnd(); $p.WaitForExit()
    $w = ([regex]::Matches($err, "warning:")).Count; $e = ([regex]::Matches($err, "error:")).Count
    $ro = & "$bin\llvm-readobj.exe" --sections $obj 2>$null | Out-String
    $sizes = [regex]::Matches($ro, "Name: (\S+) \(\d+\)\s+Type: (\S+)[\s\S]*?Size: (\d+)") | ForEach-Object { [pscustomobject]@{ Sec = $_.Groups[1].Value; Type = $_.Groups[2].Value; Size = [int]$_.Groups[3].Value } } |
              Where-Object { $_.Sec -notmatch "^\.(rela|symtab|strtab|shstrtab|comment|debug)" -and $_.Sec -ne "" }
    $code = ($sizes | Where-Object { $_.Sec -like ".text*" } | Measure-Object Size -Sum).Sum
    $ro_ = ($sizes | Where-Object { $_.Sec -like ".rodata*" } | Measure-Object Size -Sum).Sum
    $dat = ($sizes | Where-Object { $_.Sec -like ".data*" } | Measure-Object Size -Sum).Sum
    $bss = ($sizes | Where-Object { $_.Sec -like ".bss*" -or $_.Sec -like ".noinit*" } | Measure-Object Size -Sum).Sum
    "PA_BLE=$ble  exit $($p.ExitCode)  warnings $w  errors $e  | text $code  rodata $ro_  data $dat  bss/noinit $bss  (bytes, before linking)"
    if ($p.ExitCode -ne 0 -or $e -ne 0 -or $w -ne 0) { $err.Split("`n") | Select-Object -First 15; $bad = 1 }
    if ($ble -eq 0 -and ($code + $ro_ + $dat + $bss) -gt 0) { "PA_BLE=0 must be empty"; $bad = 1 }
}
exit $bad
