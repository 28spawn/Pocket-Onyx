# SPDX-License-Identifier: GPL-3.0-only
# Copyright (C) 2026 The Onyx Authors (@28spawn)
# custom-fw/tools/usb_replug_watch_pc.ps1 - counts complete enumerations while you unplug and replug the unit (hardware test HW-13: "10/10 replugs enumerate").
# Read-only. Start it, then unplug the USB cable, wait for "unplugged", plug it back in, wait for the result line, and repeat until it prints the verdict.
#   powershell -File custom-fw\tools\usb_replug_watch_pc.ps1 -Cycles 10
# A cycle is COMPLETE when the device node and both functions (MI_00 audio, MI_03 MIDI) are present with status OK and no problem code, and a speaker and a microphone endpoint exist.
# Verdict: PASS = every cycle complete; FAIL = a cycle timed out or came up with a problem code.
param([int]$Cycles = 10, [string]$Vid = "1209", [string]$Pid_ = "0001", [int]$TimeoutS = 40)

$ErrorActionPreference = "SilentlyContinue"
function State {
    # Windows keeps entries for an unplugged device as phantoms (problem code 45): they are neither present nor a problem
    $all = Get-CimInstance Win32_PnPEntity | Where-Object { $_.DeviceID -like "USB\VID_$Vid&PID_$Pid_*" -and $_.ConfigManagerErrorCode -ne 45 }
    $dev = $all | Where-Object { $_.DeviceID -notlike "*&MI_*" }
    $m0 = $all | Where-Object { $_.DeviceID -like "*&MI_00*" }
    $m3 = $all | Where-Object { $_.DeviceID -like "*&MI_03*" }
    $bad = $all | Where-Object { $_.ConfigManagerErrorCode -ne 0 }
    $aud = Get-PnpDevice -Class AudioEndpoint -PresentOnly | Where-Object { $_.FriendlyName -match "Custom MIDI" -and $_.Status -eq "OK" }
    $spk = [bool]($aud | Where-Object { $_.FriendlyName -match "Speakers" }); $mic = [bool]($aud | Where-Object { $_.FriendlyName -match "Microphone" })
    [pscustomobject]@{ Dev = [bool]$dev; M0 = [bool]$m0; M3 = [bool]$m3; Spk = $spk; Mic = $mic; Present = [bool]$all; Complete = ([bool]$dev -and [bool]$m0 -and [bool]$m3 -and -not $bad -and $spk -and $mic); Bad = ($bad | ForEach-Object { "$($_.DeviceID) code $($_.ConfigManagerErrorCode)" }) }
}

$ok = 0; $log = @(); $seen = @{}
for ($c = 1; $c -le $Cycles; $c++) {
    Write-Host ("[{0}/{1}] waiting for the unit to be unplugged ..." -f $c, $Cycles)
    while ((State).Present) { Start-Sleep -Milliseconds 400 }
    Write-Host "         unplugged. Plug it back in."
    $tu = Get-Date
    while (-not (State).Present -and ((Get-Date) - $tu).TotalSeconds -lt 120) { Start-Sleep -Milliseconds 300 }      # your hands: no clock on the enumeration yet
    $t0 = Get-Date; $res = "NO ARRIVAL (nothing came back within 120 s of the unplug)"; $bad = ""
    if ((State).Present) {
        $res = "TIMEOUT"
        Write-Host ("         the device node appeared {0:N1} s after the unplug; waiting for the complete state" -f ((Get-Date) - $tu).TotalSeconds)
        $seen = @{}
        while (((Get-Date) - $t0).TotalSeconds -lt $TimeoutS) {
            $s = State
            foreach ($k in "Dev", "M0", "M3", "Spk", "Mic") { if ($s.$k -and -not $seen.ContainsKey($k)) { $seen[$k] = [math]::Round(((Get-Date) - $t0).TotalSeconds, 1) } }
            if ($s.Complete) { $res = "COMPLETE"; break }
            if ($s.Present -and $s.Bad) { $bad = ($s.Bad -join "; ") }
            Start-Sleep -Milliseconds 400
        }
    }
    $dt = [math]::Round(((Get-Date) - $t0).TotalSeconds, 1)
    if ($res -eq "COMPLETE") { $ok++ } elseif ($bad) { $res = "PROBLEM: $bad" }
    $parts = if ($seen) { "  [first seen after arrival: " + (($seen.GetEnumerator() | Sort-Object Value | ForEach-Object { "{0} {1} s" -f $_.Key, $_.Value }) -join ", ") + "]" } else { "" }
    $log += ("cycle {0}: {1} after {2} s{3}" -f $c, $res, $dt, $parts)
    Write-Host ("         " + $log[-1])
}
Write-Host ""; $log | ForEach-Object { Write-Host $_ }
Write-Host ("`nREPLUG: {0}/{1} complete -> {2}" -f $ok, $Cycles, $(if ($ok -eq $Cycles) { "PASS" } else { "FAIL" }))
exit ([int]($ok -ne $Cycles))
