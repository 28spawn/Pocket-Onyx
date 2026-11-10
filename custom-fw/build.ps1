# SPDX-License-Identifier: GPL-3.0-only

# Copyright (C) 2026 The Onyx Authors (@28spawn)

# custom-fw/build.ps1 — One-Command Windows Host Verification & JieLi pi32v2 Firmware Builder
#
# Usage:
#   .\custom-fw\build.ps1          # Default: Bluetooth / Wi-Fi hardware radio disabled (PA_BLE=0)
#   .\custom-fw\build.ps1 -Ble     # Enable Bluetooth LE hardware radio in the firmware (PA_BLE=1)
#
# The build is a straight line, and every step is checked: a failing test, a failing
# compile or a failing link STOPS the build, and the flashable images are only written at
# the very end, when everything else has passed.  (Before this, a native command's
# non-zero exit code was ignored - PowerShell's $ErrorActionPreference only covers
# cmdlets - so a failing test still produced a fresh-looking .fwsc built from whatever
# object files happened to be lying around.)
#
# The build directory (custom-fw/build_pi32) is deleted at the start of every run, so a
# stale object can never be linked into an image.

[CmdletBinding()]
param(
    [string]$Board = "pocket_amp",
    [string]$Product = "Onyx",
    [switch]$Experimental = $false,
    [switch]$Ble = $false,
    # EXPERIMENTAL: a firmware for 48 kHz only (PA_SAMPLE_RATE_HZ=48000; the codec clock is found by a probe at boot, src/board/audio_alnk.c). Without it: 44.1 kHz, as always.
    [switch]$Rate48k = $false
)

$ErrorActionPreference = "Stop"
$RepoRoot = Split-Path -Parent $PSScriptRoot
Set-Location $RepoRoot

# The boards this build can make. A board is in this list only when its own pins, flash layout and update package exist (custom-fw/include/boards/).
# "blackbox" is known but not ported: built today it would be the Pocket AMP's firmware under another name, which drives the wrong pins on a BlackBox and writes
# flash sectors that hold the BlackBox's own data. So it stops here, before anything is compiled or written.
if ($Board -eq "blackbox") {
    throw "-Board blackbox is not ported yet (custom-fw/include/boards/board_blackbox.h): nothing was built. A Pocket AMP image must never be flashed to a BlackBox."
}
if ($Board -ne "pocket_amp") {
    throw "-Board '$Board' is not a board this build knows. Use: pocket_amp"
}
$BoardDef = "-DBOARD_POCKET_AMP=1"
$BleDef  = if ($Ble) { "-DPA_BLE=1" } else { "-DPA_BLE=0" }
$RateDef = if ($Rate48k) { "-DPA_SAMPLE_RATE_HZ=48000" } else { "-DPA_SAMPLE_RATE_HZ=44100" }
if ($Rate48k) {
    Write-Host ">>> EXPERIMENTAL [48 kHz]: PA_SAMPLE_RATE_HZ=48000 - the device image runs at 48 kHz only (the host suites and the effect matrix still test the 44.1 kHz code)" -ForegroundColor Yellow
}

if ($Ble) {
    Write-Host ">>> Builder Item [BLE]: ENABLED (-Ble, PA_BLE=1) - Bluetooth LE radio active" -ForegroundColor Green
} else {
    Write-Host ">>> Builder Item [BLE]: DISABLED (PA_BLE=0, default) - radio hardware untouched" -ForegroundColor DarkGray
}

# ---- the one place a native command's failure is noticed --------------------
function Invoke-Checked {
    param(
        [Parameter(Mandatory = $true)][string]$What,
        [Parameter(Mandatory = $true)][scriptblock]$Cmd
    )
    & $Cmd
    if ($LASTEXITCODE -ne 0) {
        throw ("{0} FAILED (exit {1}) - the build stops here, and no image is written." -f $What, $LASTEXITCODE)
    }
}

# The images the device is flashed with.  They are removed at the start of the build, so
# a failed build can never leave a stale image behind to be flashed by accident, and they
# are written again only at the end, when every step has passed.
$AppBin    = Join-Path $RepoRoot "custom-fw\build_app.bin"
$OutUfw    = Join-Path $RepoRoot "custom-fw\Pocket_AMP_Custom.ufw"
$OutFwsc   = Join-Path $RepoRoot "custom-fw\Pocket_AMP_Custom.fwsc"
$OutArea   = Join-Path $RepoRoot "custom-fw\Pocket_AMP_Custom_app_area.bin"      # the app area (flash 0x4000..0x85FFF): what the web editor and jl_safe_flash.py take
$TemplateFwsc = if ($env:STOCK_FWSC -and (Test-Path $env:STOCK_FWSC)) {
    $env:STOCK_FWSC
} elseif (Test-Path (Join-Path $RepoRoot "Pocket AMP.fwsc")) {
    Join-Path $RepoRoot "Pocket AMP.fwsc"
} elseif ($env:USERPROFILE -and (Test-Path (Join-Path $env:USERPROFILE "Downloads\Pocket AMP.fwsc"))) {
    Join-Path $env:USERPROFILE "Downloads\Pocket AMP.fwsc"
} else {
    Join-Path $RepoRoot "Pocket AMP.fwsc"
}
foreach ($f in @($AppBin, $OutUfw, $OutFwsc)) {
    if (Test-Path $f) { Remove-Item -Force $f }
}

# The build directory: fresh on every run.
$BuildDir = Join-Path $RepoRoot "custom-fw\build_pi32"
if (Test-Path $BuildDir) { Remove-Item -Recurse -Force $BuildDir }
New-Item -ItemType Directory -Force -Path $BuildDir | Out-Null

# 1. Locate Host Clang (LLVM-MinGW)
# a clang that can really include the C library (stdlib.h ...); installs llvm-mingw when the PC has none (custom-fw\tools\Find-Clang.ps1)
. (Join-Path $PSScriptRoot "tools\Find-Clang.ps1")
$null = Resolve-HostClang

Write-Host "=== [1/5] Running Host Verification Suites ===" -ForegroundColor Cyan

# The source-list drift guard: every target's module list comes from
# custom-fw/sources.txt - and this fails the build if a module would be
# silently forgotten by one of them.
Invoke-Checked "the source manifest check" { python custom-fw/tools/check_sources.py }


# ---- the source lists -------------------------------------------------------
# Read from custom-fw/sources.txt - the single place that says which modules
# each target compiles. Before this, each build script carried its own
# hand-written list, and a new module was silently missing from one of them
# (the emulator's build broke three times that way).
. custom-fw\tools\Read-Sources.ps1
$manifest = Read-SourceManifest
function Srcs([string]$t) { @(Get-SourceFiles -Target $t -Sections $manifest | ForEach-Object { "custom-fw/" + $_ }) }

$dspSrcs   = Srcs "host_dsp"
$meterSrc  = Srcs "host_meter"

function Run-HostTest {
    param(
        [Parameter(Mandatory = $true)][string]$Name,
        [Parameter(Mandatory = $true)][string]$Source,
        [string[]]$Srcs = @(),
        [string[]]$Extra = @()
    )
    $exe = Join-Path $BuildDir "$Name.exe"
    Invoke-Checked "the $Name compile" {
        clang -O2 -Wall -Wextra -Icustom-fw/include -DPA_HOST_SIM=1 $BoardDef @Extra $Source @Srcs -o $exe
    }
    Invoke-Checked "the $Name suite" { & $exe }
}

Run-HostTest -Name "pa_test_meter" -Source "custom-fw/test/test_cpu_meter.c" -Srcs $meterSrc

# The cycle/clock report. It prints estimates rather than a pass/fail verdict, but
# it is compiled here so its numbers cannot drift from PA_CPU_HZ again - it used to
# carry three hard-coded clocks (360/320/240) that contradicted the firmware.
Invoke-Checked "the cycle/clock report compile" {
    clang -O2 -Wall -Wextra -Icustom-fw/include -DPA_HOST_SIM=1 $BoardDef custom-fw/test/test_wavenet_cycles.c custom-fw/src/nam/pa_wavenet.c custom-fw/src/dsp/hw_accel.c -o (Join-Path $BuildDir "pa_test_wn_cycles.exe")
}
Invoke-Checked "the cycle/clock report" { & (Join-Path $BuildDir "pa_test_wn_cycles.exe") }

Run-HostTest -Name "pa_test_reclaim" -Source "custom-fw/test/test_reclaim.c" -Srcs (Srcs "host_reclaim")
Run-HostTest -Name "pa_test_dsp" -Source "custom-fw/test/test_dsp_chain.c" -Srcs $dspSrcs
Run-HostTest -Name "pa_test_smp" -Source "custom-fw/test/test_smp_pipeline.c" -Srcs $dspSrcs
Run-HostTest -Name "pa_test_fft_conv2" -Source "custom-fw/test/test_fft_conv2.c" -Srcs @("custom-fw/src/fx/fft_conv.c")
# The stereo cabinet and EQ: width 0 is the mono path bit for bit, the right channel keeps the tone and loses the waveform, the width sets the correlation.
Run-HostTest -Name "pa_test_cab_stereo" -Source "custom-fw/test/test_cab_stereo.c" -Srcs @("custom-fw/src/fx/fx_cab_ir.c", "custom-fw/src/fx/fft_conv.c", "custom-fw/src/fx/fx_eq.c")
# USB audio is stereo end to end: the playback ring keeps L and R apart, the capture carries both output channels at 44.1 and 48 kHz.
Run-HostTest -Name "pa_test_ble_midi_bridge" -Source "custom-fw/test/test_ble_midi_bridge.c" -Srcs @("custom-fw/src/board/ble_midi_bridge.c")
# The BLE channel remap (sparse maps from Windows) done in software: every unused channel maps to the spec's used channel, the CSA #1 sequence.
Run-HostTest -Name "pa_test_ble_remap" -Source "custom-fw/test/test_ble_remap.c" -Srcs @()
# The diagnostic channel's profile command (7D 03): the Bluetooth experiment switches are written and read live over USB-MIDI.
# The USB bulk IN packetising rule: a reply never ends on a full 64-byte packet (the host kept such a transfer open: RAM-read replies of 32 and 240 bytes never arrived).
Run-HostTest -Name "pa_test_usb_chunk" -Source "custom-fw/test/test_usb_chunk.c" -Srcs @()
Run-HostTest -Name "pa_test_diag_profile" -Source "custom-fw/test/test_diag_profile.c" -Srcs @("custom-fw/src/sys/pa_diag.c")
Run-HostTest -Name "pa_test_usb_stereo" -Source "custom-fw/test/test_usb_stereo.c" -Srcs @("custom-fw/src/board/usb_ble.c")
Run-HostTest -Name "pa_test_usb_capture_tone" -Source "custom-fw/test/test_usb_capture_tone.c" -Srcs @("custom-fw/src/board/usb_ble.c")
Run-HostTest -Name "pa_test_usb_capture_missed" -Source "custom-fw/test/test_usb_capture_missed.c" -Srcs @("custom-fw/src/board/usb_ble.c")
Run-HostTest -Name "pa_test_usb_playback_tone" -Source "custom-fw/test/test_usb_playback_tone.c" -Srcs @("custom-fw/src/board/usb_ble.c")
# Communications QA gate, host stage. The USB-MIDI receive-to-response pipeline on the production code (usb_ble.c, pa_diag.c, sysex.c, pa_ota.c, ble_midi.c) with only the
# hardware boundary stubbed: the seven identical diagnostic requests of the 2026-11-03 investigation, malformed-input recovery, the ring, maximum sizes, every CIN.
# A pass is HOST-verified only; it says nothing about the USB controller. Documented defects print as KNOWN and do not fail the build (see the test header).
Run-HostTest -Name "pa_test_sysex_read" -Source "custom-fw/test/test_sysex_read.c" -Srcs @("custom-fw/src/sys/sysex.c")
Run-HostTest -Name "pa_test_usb_midi_e2e" -Source "custom-fw/test/test_usb_midi_e2e.c" -Srcs @("custom-fw/src/sys/sysex.c", "custom-fw/src/sys/pa_diag.c", "custom-fw/src/sys/pa_ota.c", "custom-fw/src/board/ble_midi.c", "custom-fw/test/comms_sysex_stubs.c")
# The USB descriptor tables (include/pa_usb_desc.h) walked and checked on the PC: lengths, interface numbers, endpoints, links, the stock audio format (USB-AUDIO-PLAN.md step A).
Run-HostTest -Name "pa_test_usb_desc" -Source "custom-fw/test/test_usb_desc.c" -Srcs @()
Run-HostTest -Name "pa_test_biquad" -Source "custom-fw/test/test_biquad.c" -Srcs @("custom-fw/src/dsp/pa_biquad.c")
Run-HostTest -Name "pa_test_stereo_spread" -Source "custom-fw/test/test_stereo_spread.c" -Srcs @()
Run-HostTest -Name "pa_test_wild" -Source "custom-fw/test/test_wild.c" -Srcs @()
# Changing the reverb type while the audio interrupt runs (it used to hang the unit when scrolled from Plate to Ambience: a write through a NULL line buffer).
Run-HostTest -Name "pa_test_reverb_switch_race" -Source "custom-fw/test/test_reverb_switch_race.c" -Srcs @() -Extra @("-lm")
# Turning the delay's knobs while a note rings must not click (TIME glides, FEEDBACK / MIX are smoothed).
# Changing the reverb's type while it rings: the wet fades out, the type changes in the audio thread, the wet fades back (every pair of the eight types).
Run-HostTest -Name "pa_test_reverb_type_clicks" -Source "custom-fw/test/test_reverb_type_clicks.c" -Srcs @("custom-fw/src/fx/fx_reverb.c") -Extra @("-lm")
Run-HostTest -Name "pa_test_delay_clicks" -Source "custom-fw/test/test_delay_clicks.c" -Srcs @() -Extra @("-lm")
Run-HostTest -Name "pa_test_display_list" -Source "custom-fw/test/test_display_list.c" -Srcs @("custom-fw/src/board/ui_5x7.c") -Extra @("-DPA_EMU_DLL=1")

# The WaveNet CPU-stage assembly, which no host suite runs, executed in an interpreter against the C reference (skips without the toolchain).
Invoke-Checked "the WaveNet kernel emulation" { python (Join-Path $PSScriptRoot "test/test_wn_asm_emu.py") }

# The reverb: tank regions must be disjoint, and the decay knob must produce the
# RT60 its comment claims (measured with an impulse, not assumed).
Run-HostTest -Name "pa_test_reverb" -Source "custom-fw/test/test_reverb.c" -Srcs $dspSrcs

# The DSP pool with every effect on, measured by the DEVICE allocator
# (-DPA_POOL_TEST) rather than budgeted by hand.
Run-HostTest -Name "pa_test_pool" -Source "custom-fw/test/test_pool.c" -Srcs $dspSrcs -Extra @("-DPA_POOL_TEST=1")

# The Groove Station: the generated pattern table's invariants (loop lengths on a
# 16th, a velocity on every hit, the lanes the reference library actually uses) and
# the engine's clock, which must walk the whole pattern and wrap at its own length.
# That wrap is what the UI used to get wrong ("the bar goes away after the 4th").
Run-HostTest -Name "pa_test_drums" -Source "custom-fw/test/test_drums.c" -Srcs $dspSrcs

# The millisecond clock: the 32-bit tick counter wraps every 178.96 s, so the clock has
# to keep a running count (the auto power-off, the toasts and the long-press timing
# depend on it).
Invoke-Checked "the millisecond clock test compile" {
    clang -O2 -Wall -Wextra -Icustom-fw/include -DPA_HOST_SIM=1 custom-fw/test/test_ms_clock.c -o (Join-Path $BuildDir "pa_test_ms_clock.exe")
}
Invoke-Checked "the millisecond clock suite" { & (Join-Path $BuildDir "pa_test_ms_clock.exe") }

# The app side of the JieLi OTA handshake: the two frames and the ID block.
Invoke-Checked "the OTA handshake test compile" {
    clang -O2 -Wall -Wextra -Icustom-fw/include -DPA_HOST_SIM=1 custom-fw/test/test_ota.c custom-fw/src/sys/pa_ota.c -o (Join-Path $BuildDir "pa_test_ota.exe")
}
Invoke-Checked "the OTA handshake suite" { & (Join-Path $BuildDir "pa_test_ota.exe") }

# The kit itself: the balance of the twelve voices, the cymbals' character, the bus
# level, and the loop's silence.
Run-HostTest -Name "pa_test_kit" -Source "custom-fw/test/test_kit.c" -Srcs $dspSrcs -Extra @("-lm")

# The decoders and synthesisers the kit is stored with (mu-law, IMA-ADPCM, the kick and tom synths) and the engine's block independence.
Run-HostTest -Name "pa_test_drum_codecs" -Source "custom-fw/test/test_drum_codecs.c" -Extra @("-lm")

# The flash driver's argument check against the map (every address) and the boot guard's state machine.
Run-HostTest -Name "pa_test_flash_guard" -Source "custom-fw/test/test_flash_guard.c"
Run-HostTest -Name "pa_test_overload_rule" -Source "custom-fw/test/test_overload_rule.c"
Run-HostTest -Name "pa_test_fft_conv_mac" -Source "custom-fw/test/test_fft_conv_mac.c" -Srcs @("custom-fw/src/fx/fft_conv.c")

# The chromatic tuner (YIN pitch detector, hold, stability): synthetic guitar strings, detuned, noisy, decaying, a peg turned.
Run-HostTest -Name "pa_test_tuner" -Source "custom-fw/test/test_tuner.c" -Srcs @("custom-fw/src/dsp/tuner.c") -Extra @("-lm")

Run-HostTest -Name "pa_test_full" -Source "custom-fw/test/test_full_firmware.c" -Srcs (Srcs "host_full")

# The torn-write / flash-protection / allocator-churn suite. It needs a directory
# with no pocket_amp_flash.bin in it, or the SPI-NOR sim will attach to it.
$robDir = Join-Path $BuildDir "rob"
New-Item -ItemType Directory -Force -Path $robDir | Out-Null
Invoke-Checked "the robustness suite compile" {
    clang -O2 -Wall -Wextra -Icustom-fw/include -DPA_HOST_SIM=1 $BoardDef -DPA_POOL_TEST=1 custom-fw/test/test_robustness.c custom-fw/src/sys/settings.c custom-fw/src/sys/pa_bond.c custom-fw/src/sys/pa_ble_trims_mirror.c (Srcs "host_robust") -o (Join-Path $robDir "pa_test_robust.exe")
}
Push-Location $robDir
Invoke-Checked "the robustness suite" { & (Join-Path $robDir "pa_test_robust.exe") }
Pop-Location

# ---- the effect matrix ------------------------------------------------------
# Every effect on its own, in pairs, in different orders, over a sine and a real
# recording - with a sanity check AND a behaviour check per effect, so a block
# that does nothing still fails. The method is in custom-fw/test/EFFECT_MATRIX.md.
#
# It needs the emulator DLL, which the [4/5] stage builds. It is run there
# instead, so the order is: build the DLL, then run the matrix.

Write-Host "`n=== [2/5] Cross-Compiling for JieLi AC791N (pi32v2 / r3) ===" -ForegroundColor Cyan

# A build id for every build (yyMMddHHmm, as a number and as text): the firmware writes it to SRAM at boot, and jl_ram_read.py prints it
# first in every readout, so a recovered log always says which build produced it. It is regenerated on each run.
$buildStamp = Get-Date
$buildNum = [uint32]($buildStamp.ToString("yyMMddHHmm"))
$buildTxt = $buildStamp.ToString("yyyy-MM-dd HH:mm")
$gitRev = ""
try { $gitRev = (& git rev-parse --short HEAD 2>$null) } catch { }
# Firmware release naming & versioning: Onyx_1.19
# Extracted from PA_FW_VERSION in custom-fw/include/pa_config.h (e.g. 105 -> "1.05")
$cfgHdr = Get-Content (Join-Path $RepoRoot "custom-fw/include/pa_config.h") -Raw
$fwVerNum = 119
if ($cfgHdr -match '#define\s+PA_FW_VERSION\s+(\d+)u') {
    $fwVerNum = [int]$Matches[1]
}
$verMajor = [int]($fwVerNum / 100)
$verMinor = $fwVerNum % 100
$verStr = ("{0}.{1:D2}" -f $verMajor, $verMinor)

$packNum = 194
$buildName = if ($Product -and $Product -ne "") { $Product } else { "Onyx" }
$boardPrefix = if ($Board -eq "blackbox") { "BlackBox_Onyx" } elseif ($Product -and $Product -ne "" -and $Product -ne "Onyx") { $Product } else { "Onyx" }
# (in a try: a PC without git, or a source folder that came from a zip, must still build)
$gitBranch = ""
try { $gitBranch = (& git branch --show-current 2>$null) } catch { }
$expSuffix = if ($Experimental -or $Rate48k -or ($gitBranch -like "experimental*")) { "_EXPERIMENTAL" } else { "" }
$rateSuffix = if ($Rate48k) { "_48K" } else { "" }
$releaseBaseName = ("{0}_{1}{2}{3}" -f $boardPrefix, $verStr, $rateSuffix, $expSuffix)
$buildCode = ("{0}-{1}" -f $releaseBaseName, $buildNum)
$idHdr = "/* SPDX-License-Identifier: GPL-3.0-only */`n/* Copyright (C) 2026 The Onyx Authors (@28spawn) */`n/* generated by build.ps1 - do not edit */`n#define PA_BUILD_ID $buildNum" + "u`n#define PA_BUILD_TEXT `"$buildTxt`"`n#define PA_BUILD_REV `"$gitRev`"`n#define PA_BUILD_NAME `"$buildName`"`n#define PA_PACK_NUM ${packNum}u`n#define PA_BUILD_CODE `"$buildCode`"`n"
[System.IO.File]::WriteAllText((Join-Path $PWD "custom-fw/include/pa_build_id.h"), $idHdr)
Write-Host "build id $buildNum ($buildTxt) | BLE radio: $(if ($Ble) { 'ENABLED (PA_BLE=1)' } else { 'DISABLED (PA_BLE=0, default)' })" -ForegroundColor Cyan

$pi32Root = if ($env:PI32_ROOT) { $env:PI32_ROOT } else {
    $cands = @(
        $(if ($env:USERPROFILE) { Join-Path $env:USERPROFILE "tools\jieli\C$\JL\pi32" }),
        "C:\JL\pi32",
        "C:\jieli\pi32"
    )
    ($cands | Where-Object { $_ -and (Test-Path "$_\bin\clang.exe") } | Select-Object -First 1)
}
if (-not $pi32Root -or -not (Test-Path "$pi32Root\bin\clang.exe")) {
    $pi32Root = "C:\JL\pi32"
}
$pi32Bin = "$pi32Root\bin"
$pi32Inc = "$pi32Root\pi32v2-include"
$pi32Lib = "$pi32Root\pi32v2-lib\r3"

    Invoke-Checked "the startup code compile" {
        & "$pi32Bin\clang.exe" -target pi32v2 -mcpu=r3 $BoardDef -c custom-fw/src/board/entry.S -o (Join-Path $BuildDir "entry.o")
    }

# The object names come from the source PATH, not its base name: two files with the same
# base name in different directories would otherwise overwrite each other's object, and
# the link would pick up the wrong one.
$objs = @((Join-Path $BuildDir "entry.o"))
$srcs = Get-ChildItem -Path "custom-fw/src" -Recurse -Filter "*.c" | Sort-Object FullName
foreach ($s in $srcs) {
    $rel = $s.FullName.Substring((Join-Path $RepoRoot "custom-fw\src").Length)
    $tag = ($rel -replace "[\\/:]", "_")
    $obj = Join-Path $BuildDir "$tag.o"
    Invoke-Checked "the compile of $($s.Name)" {
        & "$pi32Bin\clang.exe" -target pi32v2 -mcpu=r3 -O2 -Wall -Wno-tautological-constant-out-of-range-compare -ffreestanding -fno-builtin -isystem $pi32Inc -Icustom-fw/include $BoardDef $BleDef $RateDef -Icustom-fw/src/board/ble_platform -Icustom-fw/third_party/optimist_ble/src -Icustom-fw/third_party/optimist_ble/hal -c $s.FullName -o $obj
    }
    $objs += $obj
}

# The three libraries have to be scanned as a group: libm's tanh needs the
# soft-float helper __subdf3, which lives in libcompiler_rt.a. Without the
# group, the linker scans libcompiler_rt.a before libm has been pulled in, so
# the helper is not there yet and the link fails with "undefined reference to
# __subdf3".
#
# --gc-sections drops the code nothing reaches. It currently saves 0 bytes
# (every function is reachable), but it keeps it that way.
Invoke-Checked "the link" {
    & "$pi32Bin\pi32v2-ld.exe" -T custom-fw/app.ld --gc-sections $objs "--start-group" "$pi32Lib\libcompiler_rt.a" "$pi32Lib\libm.a" "$pi32Lib\libc.a" "--end-group" -o (Join-Path $BuildDir "pocket_amp_custom.elf")
}
Invoke-Checked "the .text extraction" {
    & "$pi32Bin\llvm-objcopy.exe" "-O=binary" "-j=.text" (Join-Path $BuildDir "pocket_amp_custom.elf") (Join-Path $BuildDir "text.bin")
}
Invoke-Checked "the .data extraction" {
    & "$pi32Bin\llvm-objcopy.exe" "-O=binary" "-j=.data" (Join-Path $BuildDir "pocket_amp_custom.elf") (Join-Path $BuildDir "data.bin")
}
# The two RAM-code sections (.ram_text, .c1_text) are loaded from the flash image too, between the code and the data. The image used to
# be .text + .data only: .data then sat 48 bytes before the address the startup code copies it from, so every initialised variable
# arrived shifted by 48 bytes - the stock amp and cabinet tables read one entry (amps) or six entries (cabinets) off, and the last
# entries read past the end of the image. The image is now laid out by the linker's own load addresses.
Invoke-Checked "the .ram_text extraction" {
    & "$pi32Bin\llvm-objcopy.exe" "-O=binary" "-j=.ram_text" (Join-Path $BuildDir "pocket_amp_custom.elf") (Join-Path $BuildDir "ram_text.bin")
}
Invoke-Checked "the .ramfn extraction" {
    & "$pi32Bin\llvm-objcopy.exe" "-O=binary" "-j=.ramfn" (Join-Path $BuildDir "pocket_amp_custom.elf") (Join-Path $BuildDir "ramfn.bin")
}
Invoke-Checked "the .c1_text extraction" {
    & "$pi32Bin\llvm-objcopy.exe" "-O=binary" "-j=.c1_text" (Join-Path $BuildDir "pocket_amp_custom.elf") (Join-Path $BuildDir "c1_text.bin")
}
# .flashfn (flash_ram.c: the flash controller probe) runs from SRAM and is copied from its load address in the image at boot. It was NEVER packed: the image held zeros there, the probe copied zeros
# and jumped into them (the "stage 0, died before its first line" of packs 140 and 168). It is packed now, and the image is checked below.
Invoke-Checked "the .flashfn extraction" {
    & "$pi32Bin\llvm-objcopy.exe" "-O=binary" "-j=.flashfn" (Join-Path $BuildDir "pocket_amp_custom.elf") (Join-Path $BuildDir "flashfn.bin")
}
$xipBase = 0x02000120
$syms = @{}
& "$pi32Bin\llvm-nm.exe" (Join-Path $BuildDir "pocket_amp_custom.elf") | ForEach-Object {
    if ($_ -match '^([0-9a-fA-F]{8})\s+\w\s+(_rt_load|_c1_load|_rf_load|_ff_load|_data_load|_data_start|_data_end)$') { $syms[$Matches[2]] = [Convert]::ToUInt32($Matches[1], 16) }
}
foreach ($n in '_rt_load', '_c1_load', '_rf_load', '_ff_load', '_data_load') { if (-not $syms.ContainsKey($n)) { throw "the link map has no $n symbol" } }
$textBytes = [System.IO.File]::ReadAllBytes((Join-Path $BuildDir "text.bin"))
$rtBytes   = [System.IO.File]::ReadAllBytes((Join-Path $BuildDir "ram_text.bin"))
$c1Bytes   = [System.IO.File]::ReadAllBytes((Join-Path $BuildDir "c1_text.bin"))
$rfBytes   = [System.IO.File]::ReadAllBytes((Join-Path $BuildDir "ramfn.bin"))
$ffBytes   = [System.IO.File]::ReadAllBytes((Join-Path $BuildDir "flashfn.bin"))
$dataBytes = [System.IO.File]::ReadAllBytes((Join-Path $BuildDir "data.bin"))
$imgLen = ($syms['_data_load'] - $xipBase) + $dataBytes.Length
$img = New-Object byte[] $imgLen
[Array]::Copy($textBytes, 0, $img, 0, $textBytes.Length)
[Array]::Copy($rtBytes, 0, $img, ($syms['_rt_load'] - $xipBase), $rtBytes.Length)
[Array]::Copy($c1Bytes, 0, $img, ($syms['_c1_load'] - $xipBase), $c1Bytes.Length)
[Array]::Copy($rfBytes, 0, $img, ($syms['_rf_load'] - $xipBase), $rfBytes.Length)
[Array]::Copy($ffBytes, 0, $img, ($syms['_ff_load'] - $xipBase), $ffBytes.Length)
[Array]::Copy($dataBytes, 0, $img, ($syms['_data_load'] - $xipBase), $dataBytes.Length)
if ($ffBytes.Length -eq 0 -or ($syms['_ff_load'] - $xipBase) + $ffBytes.Length -gt ($syms['_data_load'] - $xipBase)) { throw "the .flashfn load area does not fit before .data" }
for ($i = 0; $i -lt $ffBytes.Length; $i++) { if ($img[($syms['_ff_load'] - $xipBase) + $i] -ne $ffBytes[$i]) { throw "the image does not hold .flashfn at its load address (byte $i)" } }
if ($syms['_data_end'] - $syms['_data_start'] -ne $dataBytes.Length) { throw "the .data section is $($dataBytes.Length) bytes but the startup code copies $($syms['_data_end'] - $syms['_data_start'])" }
if (($syms['_rt_load'] - $xipBase) -lt $textBytes.Length) { throw "the RAM-code load address overlaps the code" }
$appBinBuilt = Join-Path $BuildDir "build_app.bin"
[System.IO.File]::WriteAllBytes($appBinBuilt, $img)
$appSize = (Get-Item $appBinBuilt).Length
Write-Host ("    the app image is {0} bytes; the app area holds an app.bin of up to {1} bytes (pack_ufw.APP_SLOT_MAX), so {2} bytes are free" -f $appSize, 0x81CD9, (0x81CD9 - $appSize))

# ---- the effect matrix ------------------------------------------------------
# The emulator DLL runs the firmware's own audio path, so the matrix measures the
# firmware rather than a model of it. The method is in
# custom-fw/test/EFFECT_MATRIX.md.
Write-Host "`n=== [3/5] The Effect Matrix (every effect, every pair, every order) ===" -ForegroundColor Cyan
Invoke-Checked "the emulator DLL build" {
    powershell -ExecutionPolicy Bypass -File custom-fw/emu/build_dll.ps1 -Board $Board $(if ($Ble) { "-Ble" })
}
Invoke-Checked "the effect matrix" {
    python custom-fw/tools/stress_effects.py --dll custom-fw/emu/pocket_amp_emu.dll --types
}

# ---- the Onyx release package: built from scratch, with NO M-VAVE material in it (tools/onyx_pkg_make.py) ----
# The update loader (loader/onyx_loader.c + third_party/pendel_loader, MPL-2.0) is built here, the three JieLi SDK files (Apache-2.0) are fetched once, and the package, the app image, the checksums and the
# licences go to custom-fw/release/<name>/. This is the package that is published.
Write-Host "`n=== [4/5] Building the Onyx Release Package (vendor-free) ===" -ForegroundColor Cyan
Invoke-Checked "the Onyx loader build" { powershell -NoProfile -ExecutionPolicy Bypass -File custom-fw/tools/build_onyx_loader.ps1 }
Invoke-Checked "the SDK files" { python custom-fw/tools/get_sdk_files.py }
$relDir = Join-Path $RepoRoot ("custom-fw\release\{0}" -f $releaseBaseName)
if (Test-Path $relDir) { Remove-Item -Recurse -Force $relDir }
New-Item -ItemType Directory -Force $relDir | Out-Null
$relPkg = Join-Path $relDir ("{0}.fwsc" -f $releaseBaseName)
Invoke-Checked "the Onyx package" {
    python custom-fw/tools/onyx_pkg_make.py $appBinBuilt custom-fw/build_pi32/onyx_loader/loader.bin $relPkg --sdk custom-fw/third_party/jieli_sdk_files --product ("Pocket AMP_{0}" -f $packNum)
}
Invoke-Checked "the Onyx package check" { python custom-fw/test/test_onyx_package.py $relPkg $appBinBuilt }
Copy-Item -Force $appBinBuilt $AppBin
if (Test-Path $TemplateFwsc) {
    Write-Host "`n=== [4b] Packaging local .ufw and .fwsc Containers ===" -ForegroundColor Cyan
    Invoke-Checked "the packer" {
        python custom-fw/tools/pack_ufw.py --in-bin $appBinBuilt --out-ufw $OutUfw --out-fwsc $OutFwsc --out-area $OutArea --template-fwsc $TemplateFwsc
    }
    Invoke-Checked "the package verification" {
        python custom-fw/tools/pack_ufw.py --verify $OutFwsc
    }
} else {
    Copy-Item -Force $relPkg $OutFwsc
    python -c "import sys; sys.path.insert(0,'custom-fw/tools'); import jl_app_area as J; f=J.flash_from_any(open(sys.argv[1],'rb').read()); f=f[0] if isinstance(f,tuple) else f; open(sys.argv[2],'wb').write(f[0x4000:0x86000])" $relPkg $OutArea
}
Copy-Item -Force $appBinBuilt (Join-Path $relDir ("{0}.bin" -f $releaseBaseName))
New-Item -ItemType Directory -Force (Join-Path $relDir "LICENSES") | Out-Null
foreach ($l in @("custom-fw\LICENSES\Apache-2.0.txt", "custom-fw\LICENSES\MPL-2.0.txt", "LICENSE")) { Copy-Item -Force (Join-Path $RepoRoot $l) (Join-Path $relDir "LICENSES\$(Split-Path -Leaf $l)") }
Copy-Item -Force (Join-Path $RepoRoot "custom-fw\third_party\pendel_loader\NOTICE") (Join-Path $relDir "LICENSES\NOTICE-pendel-bsp.txt")
Copy-Item -Force (Join-Path $RepoRoot "NOTICE.md") (Join-Path $relDir "NOTICE.md")
$sums = ""
foreach ($f in (Get-ChildItem $relDir -File)) { $sums += ("{0}  {1}`n" -f (Get-FileHash $f.FullName -Algorithm SHA256).Hash.ToLower(), $f.Name) }
[System.IO.File]::WriteAllText((Join-Path $relDir "SHA256SUMS"), $sums)
Write-Host ("    release   : {0}" -f $relDir)

Write-Host "`n=== [5/5] Done ===" -ForegroundColor Green
Write-Host ("    app image : {0} ({1} bytes)" -f $AppBin, $appSize)
Write-Host ("    container : {0}" -f $OutFwsc)
Write-Host ("    app area  : {0}   <- the file for the web editor (Firmware tab) and jl_safe_flash.py" -f $OutArea)
Write-Host ("    build dir : {0}" -f $BuildDir)
