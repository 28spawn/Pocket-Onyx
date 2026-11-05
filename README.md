# M-VAVE Pocket AMP Custom Firmware (Onyx)

> **NOT AFFILIATED WITH M-VAVE OR DONNER.** This is an independent, unofficial interoperability project. It contains no M-VAVE/Donner firmware, amp models, cabinet impulse responses or preset banks, and it does not redistribute any such data. Ownership of a device does not by itself grant redistribution rights over its contents; the vendor's EULA/terms may restrict reverse engineering or use of extracted data. Check them (and consult counsel) before any public release.

An open-source custom firmware and DSP project for the M-VAVE / Donner Pocket AMP, based on the JieLi AC791N (pi32v2) platform. The firmware, emulator, tools, and generated audio assets are developed in this repository.

<p align="center">
  <img src="docs/images/03_moving_block.png" width="23%" alt="8-Block Signal Grid" />
  <img src="docs/images/02_list_view.png" width="23%" alt="Linear List View" />
  <img src="docs/images/04_param_edit.png" width="23%" alt="Parameter Editor" />
</p>
<p align="center">
  <img src="docs/images/05_eq_editor.png" width="23%" alt="Cabinet & EQ Controls" />
  <img src="docs/images/06_tuner.png" width="23%" alt="Chromatic Tuner" />
  <img src="docs/images/07_groove.png" width="23%" alt="Groove Station Drums" />
  <img src="docs/images/08_bank_mode.png" width="23%" alt="Live Bank Mode" />
</p>

## What it does

- Eight reorderable effect slots, guitar amp capture loading, cabinet IR convolution, presets, tuner, and a built-in drum machine.
- A 240 × 240 front-panel UI, USB MIDI/SysEx paths, and a desktop emulator that runs the shared audio/UI code.
- Host tests, firmware build and package checks, and a QA gate that combines automated results with per-device recovery evidence.

Audio runs at 44.1 kHz in 64-sample blocks. The configured 360 MHz CPU clock is an assumption; the chip datasheet lists 320 MHz, and the clock and cycle budget still need device measurement. See [ARCHITECTURE.md](ARCHITECTURE.md).

## Audio engine and effect blocks

The default signal order is `DYN → DRIVE → AMP → CAB → EQ → MOD → DELAY → REVERB`. The order can be changed in the firmware UI. Each slot can be bypassed; delay and reverb can preserve tails where supported.

| Block | Models / algorithms in the current source |
|---|---|
| **DYN** | GATE, BOOST+GATE, COMP+GATE, AI GATE MS, COMP. Gate models expose threshold/release/attack controls as applicable; LEVEL is output volume for GATE and AI GATE MS. Compressor models provide ratio, attack/release behavior, threshold, and level/makeup controls according to type. |
| **DRIVE** | TS-808, Klon-style, OCD-style, RAT-style, Big Muff-style, Fuzz Face-style. Four-times oversampled clipping; GRIT, TONE, LEVEL. |
| **AMP / NAM** | Float32 WaveNet inference for NAM A2 captures, including the embedded A2-Lite configuration. User captures are loaded into the active WaveNet path. This is the NAM engine used by the current firmware flow; the older fixed-point GRU code is not the active NAM A2 path. |
| **CAB** | Cabinet impulse-response convolution (up to 2048 taps), with level, low-cut, and high-cut controls. Includes a generated default cabinet and supports user IR loading. |
| **EQ** | 5-BAND graphic EQ; LOW/HIGH CUT; PARAMETRIC-3 (low shelf, sweepable mid, high shelf). |
| **MOD** | CHORUS, PHASER, FLANGER, TREMOLO, VIBRATO, UNIVIBE, AUTO FILTER. |
| **DELAY** | ANALOG, DUCKING, dTAPE, DUAL, LOFI, DIGITAL, PING PONG, REVERSE, SLAPBACK. |
| **REVERB** | ROOM, HALL, SWELL, SPRING, SHIMMER, CLOUD, PLATE, AMBIENCE. |

The detailed algorithms and parameter mappings live beside their implementations in `custom-fw/src/fx/`. DSP uses both fixed-point and floating-point paths; the AC791N has a single-precision FPU. Actual real-time margins depend on device clock and must be measured on hardware.

## Groove Station drum machine

The drum machine uses **project-authored rhythm patterns** and a **project-generated synthesized kit**. The patterns are stored as a 48-step-per-4/4-bar grid, with per-step lane and velocity data. The kit voices are created by `custom-fw/tools/gen_drum_kit.py` from deterministic oscillators, noise, and filters; they are not recordings sampled from another product. Both generated include files are checked by the provenance test and should be regenerated from their tools rather than edited by hand.

The kit has kick, snare, rim/side-stick, closed hat, pedal hat, open hat, crash, ride, bell, and three tom voices, plus a metronome click. To keep the firmware small the kick and the toms are re-synthesised on the device (a pitch-gliding sine, a decaying overtone and a noise burst, a few multiplies a sample), the snare, rim, hats, ride and bell are stored as 4-bit IMA-ADPCM, the open hat and the crash as G.711 mu-law, and only the click is 16-bit PCM (`custom-fw/src/dsp/drum_codec.h`; the generator measures each stored form against its reference recipe). The sequencer uses a fractional fixed-point clock to keep tempo stable. The UI provides 49 patterns, tempo control, and drum volume. Source of truth: `custom-fw/src/dsp/drums_synth.c`, `drum_patterns.inc`, and `drum_kit.inc`.

## Front panel and connectivity

The firmware includes the LCD UI, physical button/encoder handling, tuner, preset navigation, settings, performance display, and Groove Station controls. The desktop emulator provides a browser front panel and audio I/O around the shared firmware core.

USB MIDI and the SysEx/editor paths are implemented in the source and covered by host/emulator tests. **Bluetooth LE MIDI** (for wireless footswitches like the Chocolate Plus, and mobile editor apps) is fully supported at boot via the integrated open Optimist BLE stack. **Bluetooth Audio (A2DP media streaming from phone)** is not supported due to DSP RAM and dual-core real-time constraints.

## Controls and User Guide

### 1. Front-Panel Buttons and Gestures

| Gesture | Function |
|---|---|
| **`HOME` (Tap)** | **Universal Home**: Returns to the main home screen from any sub-menu or editor. When already on the Home screen, opens the **Settings Menu** (Global EQ, Display Grid/List view, footswitch mappings, system information, and factory reset). |
| **`HOME` (Hold 1.5 s)** | **Toggle Stomp / Preset Mode**: Switches the main home screen between **PRESET Mode** (preset switching) and **STOMP Mode** (using buttons as instant effect on/off pedals). |
| **`PARA` (Rotate)** | **Navigate**: Moves the cursor across the 8 effect blocks in the signal chain. Inside menus and editors, adjusts knob values and options. |
| **`PARA` (Click)** | **Edit Block**: Opens the deep parameter editor for the highlighted effect block. Click again inside the editor to toggle between selecting parameters and editing their values. |
| **`PARA` (Hold 1.0 s)** | **Save Preset**: Opens the "Save to" dialog to store the current tone, edit preset names, or copy settings to another preset slot (`P01`–`P80`). |
| **`PARA` (Hold 2.0 s)** | **Reorder Signal Chain (Moving Mode)**: Lifts the highlighted block. Rotate `PARA` left or right to move the block anywhere in the 8-slot chain. Click `PARA` to drop it into place, or press `HOME` / `VOL-` to cancel. |
| **`<` / `>` (Tap)** | Changes current preset down/up (`P01` .. `P80`). Inside editors, navigates or modifies values. |
| **`VOL-` / `VOL+` (Tap)** | Adjusts master device output volume with an on-screen level overlay. |

### 2. Quick-Access Key Combinations (Chords)

Press the following button pairs simultaneously from the home screen:

| Key Combination | Shortcut Target | Description |
|---|---|---|
| **`VOL-` + `VOL+`** | **Chromatic Tuner** | High-precision tuner with needle display, detected note, pitch frequency in cents, and selectable Reference Pitch (435–445 Hz). Toggle between MUTE and LIVE pass-through. Press `HOME` or the combo again to exit. |
| **`<` + `>`** | **Live Bank Mode** | Groups presets into 4-slot performance banks (`P01-P04`, `P05-P08`, etc.). Switch slots instantly with a single button press (`VOL-` = Slot 1, `VOL+` = Slot 2, `<` = Slot 3, `>` = Slot 4). |
| **`HOME` + `VOL-`** | **Groove Station** | Built-in drum machine. Press `PARA` to start/stop playback, turn `PARA` to dial tempo (40–250 BPM), and press `<` / `>` to select from 49 rhythm styles. |
| **`HOME` + `PARA` (Hold 2.0 s)** | **Manual ROM Download Mode** | Hardware shortcut to reboot the processor directly into the mask-ROM USB download mode for flashing or recovery. |

### 3. Entering Mask-ROM Download Mode

The Pocket AMP contains a built-in mask-ROM bootloader (`WL82 UBOOT1.00`) used for low-level flashing, memory inspection, and recovery:

1. **Manual Hardware Button Combination**:
   - While the unit is powered on and running, press and hold **`HOME` + `PARA` simultaneously for 2 seconds**.
   - The display turns black, audio ceases, and the device reboots into mask-ROM download mode.
   - On Windows, the unit enumerates on USB as a SCSI disk device named `WL82 UBOOT1.00` (VID/PID `4C4A:8057`).
   - **To exit Download Mode**: Simply toggle the hardware power switch OFF and back ON. The device will boot normally into the custom firmware.
2. **One-Click via Web Editor**:
   - Open [`custom-fw/web/Onyx_Web_Editor.html`](custom-fw/web/Onyx_Web_Editor.html) in Chrome or Edge over Web MIDI.
   - On the firmware flasher panel, click **"Put the unit in download mode"**.
3. **Emergency Hardware Recovery (Bricked / Non-Booting Units)**:
   - If the firmware image in flash is ever damaged and the unit cannot boot to respond to button presses, a hardware USB dongle (an RP2040 running the Transporter recovery firmware) can force ROM download mode at power-on via the USB D+/D− data lines. See [RECOVERY.md](RECOVERY.md) for full schematics and instructions.

## Repository map

```text
custom-fw/
  include/       Firmware interfaces and configuration
  src/           Board support, DSP, effects, NAM, audio core, presets and protocols
  test/          C and Python regression suites, including effect-specific tests
  emu/           Native desktop emulator and browser workbench
  wasm/          WebAssembly build and tests
  tools/         Build, packaging, QA, recovery and asset-generation tools
  qa/            Hardware evidence ledger and QA baselines/reports
  build.ps1      Checked Windows build and packaging pipeline
  app.ld         AC791N linker layout
ARCHITECTURE.md  Module map, call graphs and execution model
TEST-PROTOCOL.md Flash/recovery requirements and regression protocol
NOTICE.md        Licensing, attribution and third-party-material scope
```

## Installation prerequisites

The project does not currently provide a single dependency installer. Install the tools for the workflow you plan to use:

### Firmware build and full QA gate

- **Windows 10/11 with Windows PowerShell 5.1 or PowerShell 7** for the documented `.ps1` scripts.
- **Python 3** available as `python` on `PATH`. The QA and packaging tools use the standard library; the full QA gate also invokes the build and functional suites.
- **LLVM/Clang for the host build** (`clang.exe` on `PATH`). The build script can find a typical LLVM-MinGW WinGet installation.
- **JieLi pi32v2 toolchain for the target build**, including `clang.exe`, `pi32v2-ld.exe`, `llvm-objcopy.exe`, and the `pi32v2-include` and `pi32v2-lib/r3` directories. The build script checks `$env:PI32_ROOT`, `$env:USERPROFILE\tools\jieli\C$\JL\pi32`, and `C:\JL\pi32`.
- **A valid stock `.fwsc` template** for packaging. Set `$env:STOCK_FWSC`, place `Pocket AMP.fwsc` in the repository root or in your `Downloads` folder. The packer uses this template to construct the signed container structure.
- **Node.js** for the WebAssembly functional suites included in the full QA gate.

### Desktop emulator

The emulator also needs **NumPy** and **python-sounddevice** in the Python environment used to launch it. A working audio input/output device is needed for live audio; ASIO support depends on the installed driver and host setup. `soxr` is optional and used when available for streaming resampling.

### Flashing and recovery (One-Click Web Editor)

Firmware installation, verification, and factory restoration can be done directly in your browser without compiling or installing Python tools:
- Open [`custom-fw/web/Onyx_Web_Editor.html`](custom-fw/web/Onyx_Web_Editor.html) in Google Chrome or Microsoft Edge (requires Web MIDI).
- Connect the Pocket AMP with a USB data cable.
- **Install Onyx Firmware**: One-click install over USB MIDI SysEx; the running firmware verifies the package, stages the update loader, and writes the app partition.
- **Restore Stock Firmware**: One-click button to restore the factory M-VAVE stock firmware; also recovers a unit stuck in update mode if a transfer was interrupted.
- **Verify Flash**: Real-time byte-for-byte comparison of the unit's flash memory against the firmware file.

For deep unbricking of a non-responsive unit (ROM download mode), see [RECOVERY.md](RECOVERY.md).

## Build, emulator, and regression checks

### Firmware build

On Windows, with the JieLi pi32v2 toolchain and Python available:

```powershell
powershell -ExecutionPolicy Bypass -File "custom-fw/build.ps1"
```

The script runs checked host suites, cross-compiles and links the firmware, verifies packaging, and only emits final images after preceding build checks pass. The exact source list is maintained in `custom-fw/sources.txt`; use that manifest and the build output rather than relying on a fixed module count in documentation.

### QA gate

Run the full gate after a patch:

```powershell
python custom-fw/tools/qa_gate.py --base main
```

The gate runs static, build, unit, functional, packaging, and recorded hardware/recovery checks. It writes a report under `custom-fw/qa/reports/`. A passing host gate is not equivalent to a hardware release approval. Follow [TEST-PROTOCOL.md](TEST-PROTOCOL.md), especially its backup, recovery-drill, and verdict requirements, before any device flash.

### Desktop emulator

Build and run the emulator from the repository root:

```powershell
powershell -ExecutionPolicy Bypass -File "custom-fw/emu/run_emu.ps1"
```

The workbench is served at `http://localhost:8080`. Host audio interfaces and ASIO support depend on the local Python/audio setup. Emulator coverage is useful for DSP and UI regression, but does not replace checks on the physical Pocket AMP.

## Flashing and recovery status

Firmware installation and recovery are supported through two paths:
1. **Browser One-Click (Recommended)**: Use [`custom-fw/web/Onyx_Web_Editor.html`](custom-fw/web/Onyx_Web_Editor.html) in Chrome or Edge over Web MIDI. It handles flashing, update recovery, flash integrity verification, and one-click restoration to the stock M-VAVE firmware.
2. **Hardware ROM Recovery**: If a unit is completely non-responsive or has a corrupted boot, a hardware USB recovery route using ROM download mode can restore the factory firmware sector by sector. Procedure: [RECOVERY.md](RECOVERY.md).

## Open-source scope

The firmware, DSP implementations, pattern generator, synthesized drum-kit generator, and generated drum assets are intended to remain open source in this repository. The drum machine does not depend on proprietary product rhythm libraries or product recordings. Review [NOTICE.md](NOTICE.md) for licensing and provenance notes before redistributing builds or bundled materials.

## AI-Assisted Development Notice

This project was developed through human-in-the-loop pair programming using artificial intelligence (Anthropic Claude and Google DeepMind Antigravity / Gemini models):
- **Architecture, prompt steering, and hardware debugging:** Directed and tested on physical hardware by the human maintainer (@28spawn).
- **DSP algorithms, firmware architecture, and test suites:** Generated, refined, and documented with AI assistance.
- **Verification:** All DSP routines, memory allocations, and USB/SysEx protocols undergo host unit testing and strict automated regression gates (`qa_gate.py`) before flashing to physical hardware.

## Credits

- **Bluetooth LE MIDI.** The BLE stack for this chip's radio is the work of the **Optimist** project (<https://github.com/w0ts/optimist>): the link layer, GATT/ATT, pairing and the radio driver were written by **hdavid** for Optimist, and Optimist builds on **Felucca** (Leo Kuroshita / Hügelton Instruments), **SLOOP** and the other projects it lists. We import it into `custom-fw/third_party/optimist_ble/` (with marked `PA-MOD` adaptations) under its `GPL-3.0-only OR MPL-2.0` licence; the radio start-up values in it are licensed under Apache-2.0 (JieLi). Details and exact commit: [NOTICE.md](NOTICE.md).
- **Prior art on the FM-1.** The Felucca maintainers' write-up of why a GPL firmware can't link the vendor Bluetooth stack (issue #17) and the firmwares that already ship Bluetooth MIDI on this chip (Optimist, fm1-dock, Saadet) are summarised in [custom-fw/BT-PRIOR-ART.md](custom-fw/BT-PRIOR-ART.md). Thank you to all of them.
- **Vendor SDK.** The public AC79 SDK and open development interfaces (Apache-2.0) are referenced for interface specifications only; no proprietary code is linked into the custom firmware.

## Disclaimer

Provided as-is, for research and interoperability. Flashing custom firmware is at your own risk and may void warranties or brick the device. All trademarks (including M-VAVE, Donner, JieLi, and TONE3000) belong to their respective owners; this project is an independent interoperability and research effort and is not affiliated with or endorsed by M-VAVE, Donner, JieLi, or TONE3000.

## License

This project is licensed under the **GNU General Public License v3.0 only** (`SPDX-License-Identifier: GPL-3.0-only`) to ensure alignment with upstream open-source components (`MPL-2.0` and `GPL-3.0-only OR MPL-2.0` files retain their respective headers; see [NOTICE.md](NOTICE.md)). See [LICENSE](LICENSE) for the full text.

