# SPDX-License-Identifier: GPL-3.0-only
# Copyright (C) 2026 The Onyx Authors (@28spawn)
# custom-fw/tools/usb_replug_etw_pc.ps1 - record the Windows USB host-controller trace (ETW) across timed replugs (what Windows does while the audio endpoints are late).
# Run from an ADMINISTRATOR PowerShell with the unit plugged in:
#     powershell -ExecutionPolicy Bypass -File custom-fw\tools\usb_replug_etw_pc.ps1 [-Cycles 3]
# It only observes: it starts the same trace session as usb_etw_capture.ps1, runs usb_replug_watch_pc.ps1 (you unplug / replug when it says so), stops the session,
# and converts the trace to CSV with tracerpt. Output: custom-fw\qa\usb-etw\replug_<stamp>.{etl,csv,log}
param([int]$Cycles = 3)
$ErrorActionPreference = 'Stop'
$id = [Security.Principal.WindowsPrincipal][Security.Principal.WindowsIdentity]::GetCurrent()
if (-not $id.IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator)) { throw 'run this from an administrator PowerShell' }
$out = Join-Path (Split-Path -Parent $PSScriptRoot) 'qa\usb-etw'
New-Item -ItemType Directory -Force $out | Out-Null
$stamp = Get-Date -Format 'yyyyMMdd_HHmmss'
$etl = Join-Path $out "replug_$stamp.etl"; $csv = Join-Path $out "replug_$stamp.csv"; $log = Join-Path $out "replug_$stamp.log"
$pf = Join-Path $out 'providers.txt'
@('{30E1D284-5D88-459C-83FD-6345B39B19EC} 0xFFFFFFFFFFFFFFFF 0xFF',   # Microsoft-Windows-USB-USBXHCI
  '{36DA592D-E43A-4E28-AF6F-4BC57C5A11E8} 0xFFFFFFFFFFFFFFFF 0xFF',   # Microsoft-Windows-USB-UCX
  '{AC52AD17-CC01-4F85-8DF5-4DCE4333C99B} 0xFFFFFFFFFFFFFFFF 0xFF') | Set-Content -Encoding ascii $pf   # Microsoft-Windows-USB-USBHUB3
logman stop pausbtrace -ets 2>$null | Out-Null
logman start pausbtrace -pf $pf -ets -o $etl -bs 1024 -nb 16 64 | Out-Null
$start = Get-Date
try { & (Join-Path $PSScriptRoot 'usb_replug_watch_pc.ps1') -Cycles $Cycles 2>&1 | Tee-Object -FilePath $log }
finally { logman stop pausbtrace -ets | Out-Null }
"trace started $($start.ToString('o'))" | Add-Content $log
tracerpt $etl -o $csv -of CSV -y | Out-Null
Write-Host "trace: $etl"; Write-Host "csv:   $csv"; Write-Host "log:   $log"
