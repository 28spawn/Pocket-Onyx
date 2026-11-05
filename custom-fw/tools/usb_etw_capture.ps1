# custom-fw/tools/usb_etw_capture.ps1 - record the Windows USB host-controller trace (ETW) around one unchanged run of usb_diag_trace_pc.py.
# Run from an ADMINISTRATOR PowerShell, with the unit freshly replugged and booted normally and no other program holding its MIDI port:
#     powershell -ExecutionPolicy Bypass -File custom-fw\tools\usb_etw_capture.ps1 [-Seconds 25]
# It only observes: it starts a trace session, runs the existing trace script (the MIDI port stays open the whole run), stops the session. Output: custom-fw\qa\usb-etw\
param([int]$Seconds = 25)
$ErrorActionPreference = 'Stop'
$id = [Security.Principal.WindowsPrincipal][Security.Principal.WindowsIdentity]::GetCurrent()
if (-not $id.IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator)) { throw 'run this from an administrator PowerShell' }
$out = Join-Path (Split-Path -Parent $PSScriptRoot) 'qa\usb-etw'
New-Item -ItemType Directory -Force $out | Out-Null
$stamp = Get-Date -Format 'yyyyMMdd_HHmmss'
$etl = Join-Path $out "usb_$stamp.etl"
$txt = Join-Path $out "trace_$stamp.txt"
$pf = Join-Path $out 'providers.txt'
@('{30E1D284-5D88-459C-83FD-6345B39B19EC} 0xFFFFFFFFFFFFFFFF 0xFF',   # Microsoft-Windows-USB-USBXHCI
  '{36DA592D-E43A-4E28-AF6F-4BC57C5A11E8} 0xFFFFFFFFFFFFFFFF 0xFF',   # Microsoft-Windows-USB-UCX
  '{AC52AD17-CC01-4F85-8DF5-4DCE4333C99B} 0xFFFFFFFFFFFFFFFF 0xFF') | Set-Content -Encoding ascii $pf   # last: Microsoft-Windows-USB-USBHUB3
logman stop pausbtrace -ets 2>$null | Out-Null
logman start pausbtrace -pf $pf -ets -o $etl -bs 1024 -nb 16 64 | Out-Null
try {
    python -u (Join-Path $PSScriptRoot 'usb_diag_trace_pc.py') --seconds $Seconds 2>&1 | Tee-Object -FilePath $txt
} finally {
    logman stop pausbtrace -ets | Out-Null
}
Write-Host "trace session saved: $etl"
Write-Host "script output saved: $txt"
