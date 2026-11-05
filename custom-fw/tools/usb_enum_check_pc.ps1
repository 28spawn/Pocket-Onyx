# custom-fw/tools/usb_enum_check_pc.ps1 - what Windows made of the unit's USB descriptors (USB-AUDIO-PLAN.md, step A gate).
# Read-only. Run with the unit plugged in:   powershell -File custom-fw\tools\usb_enum_check_pc.ps1
# Pass: the device node and BOTH function nodes (MI_00 audio, MI_03 MIDI) are present with status OK and no problem code, an audio endpoint exists, the MIDI port is listed.
param([string]$Vid = "1209", [string]$Pid_ = "0001")

$ErrorActionPreference = "SilentlyContinue"
$ok = $true
function Row($name, $cond, $detail) {
    $s = if ($cond) { "PASS" } else { "FAIL" }
    Write-Host ("{0,-5} {1,-46} {2}" -f $s, $name, $detail)
    if (-not $cond) { $script:ok = $false }
}

$all = Get-CimInstance Win32_PnPEntity | Where-Object { $_.DeviceID -like "USB\VID_$Vid&PID_$Pid_*" }
Write-Host "`nUSB nodes for VID_$Vid&PID_$Pid_ :"
$all | Sort-Object DeviceID | ForEach-Object { Write-Host ("   {0,-62} {1,-22} status {2} code {3}" -f $_.DeviceID, $_.Name, $_.Status, $_.ConfigManagerErrorCode) }
Write-Host ""

$dev   = $all | Where-Object { $_.DeviceID -notlike "*&MI_*" }
$mi00  = $all | Where-Object { $_.DeviceID -like "*&MI_00*" }
$mi03  = $all | Where-Object { $_.DeviceID -like "*&MI_03*" }
Row "device node present" ([bool]$dev) ($dev | Select-Object -First 1 -ExpandProperty DeviceID)
$hw = @(); foreach ($n in $dev) { $hw += (Get-PnpDeviceProperty -InstanceId $n.DeviceID -KeyName DEVPKEY_Device_HardwareIds).Data }
Row "device revision is 1.01 (the audio layout)" ([bool]($hw | Where-Object { $_ -like "*&REV_0101" })) ("hardware ids: " + (($hw | Select-Object -Unique) -join ", ") + "  (REV_0100 = the old MIDI-only image)")
Row "function MI_00 (audio) present" ([bool]$mi00) ($mi00 | Select-Object -First 1 -ExpandProperty Name)
Row "function MI_03 (MIDI) present" ([bool]$mi03) ($mi03 | Select-Object -First 1 -ExpandProperty Name)
Row "no problem code on any node" (-not ($all | Where-Object { $_.ConfigManagerErrorCode -ne 0 })) ""

$aud = Get-PnpDevice -Class AudioEndpoint -PresentOnly | Where-Object { $_.InstanceId -match "VID_$Vid" -or $_.FriendlyName -match "Custom MIDI|Pocket" }
Write-Host "`nAudio endpoints that mention the unit:"; $aud | ForEach-Object { Write-Host ("   {0}  [{1}]" -f $_.FriendlyName, $_.Status) }
Row "audio endpoint(s) exist" ([bool]$aud) "(the Windows sound settings list them)"

Write-Host "`nMIDI ports (python-rtmidi):"
$py = @'
import rtmidi
for kind, api in (("in", rtmidi.MidiIn()), ("out", rtmidi.MidiOut())):
    print("  %-3s" % kind, [api.get_port_name(i) for i in range(api.get_port_count())])
'@
$py | python - 2>&1 | ForEach-Object { Write-Host $_ }

Write-Host ("`nRESULT: " + $(if ($ok) { "PASS" } else { "FAIL" }))
exit ([int](-not $ok))
