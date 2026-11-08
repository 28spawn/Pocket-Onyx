# Onyx

[![License: GPL-3.0](https://img.shields.io/badge/License-GPL--3.0-blue.svg)](LICENSE)
[![Platform: JieLi AC791N](https://img.shields.io/badge/Platform-JieLi%20AC791N%20%2F%20pi32v2-orange.svg)](ARCHITECTURE.md)
[![Audio: 44.1kHz / 64-sample](https://img.shields.io/badge/Audio-44.1kHz%20%7C%2064--sample%20(~1.45ms)-green.svg)](custom-fw/)
[![QA Gate: Verified](https://img.shields.io/badge/QA%20Gate-Verified-brightgreen.svg)](TEST-PROTOCOL.md)

> [!CAUTION]
> **Early access: work in progress, no warranty, use at your own risk.** Onyx runs on real physical hardware, in the native desktop emulator, and in the WebAssembly simulator. Firmware flashing writes directly to the physical SPI-NOR flash memory of your device: always back up first (using the Onyx Web Editor or the recovery tools below), and read [Getting back & Recovery](#getting-back--recovery) before you start.
>
> **NOT AFFILIATED WITH M-VAVE OR DONNER.** Onyx is an independent, unofficial interoperability project. It contains no vendor application firmware, amp models, cab IRs or proprietary preset banks; the only vendor-derived data is the Apache-2.0 RF start-up table described below.

An open-source, modular custom firmware and DSP platform (**Onyx**) targeting the JieLi AC791N (pi32v2) audio hardware platform. The project provides an 8-block reorderable DSP engine, Neural Amp Modeler (NAM) WaveNet capture loading, cabinet IR convolution, an algorithmic drum sequencer, a responsive front-panel UI, desktop emulation, and web-based flashing and preset management tools.

<p align="center">
  <img src="docs/images/03_moving_block.png" width="23%" alt="8-Block Signal Grid" />
  <img src="docs/images/02_list_view.png" width="23%" alt="Linear List View" />
  <img src="docs/images/04_param_edit.png" width="23%" alt="Parameter Editor" />
  <img src="docs/images/05_eq_editor.png" width="23%" alt="Cabinet & EQ Controls" />
</p>
<p align="center">
  <img src="docs/images/06_tuner.png" width="23%" alt="Chromatic Tuner" />
  <img src="docs/images/07_groove.png" width="23%" alt="Groove Station Drums" />
  <img src="docs/images/08_bank_mode.png" width="23%" alt="Live Bank Mode" />
</p>

---

## Web Tools & Workbench Hub

- **The Web Editor & Installer** ([`custom-fw/web/Onyx_Web_Editor.html`](custom-fw/web/Onyx_Web_Editor.html)):
  A self-contained browser workstation running in Google Chrome or Microsoft Edge over Web MIDI. Provides one-click firmware installation, real-time flash byte verification, deep preset management, and one-click stock restoration.
- **The Live Desktop Simulator** ([`custom-fw/emu/ui.html`](custom-fw/emu/ui.html) / `Start-Emulator.bat`):
  Runs the identical compiled C firmware core natively via Windows DLL and PortAudio ASIO. Features live dual rotary controls, 240x240 LCD display streaming, oscilloscope, peak metering, drag-and-drop `.nam` / `.wav` IR hot-loaders, and dry guitar capture playback courtesy of Jonas Hemmersbach.
- **The WebAssembly Workbench** (`Start-WASM-Workbench.bat`):
  Runs the complete DSP engine and tone pipeline directly inside any modern browser using WebAssembly.

---

## Features

### 1. Modular 8-Block Audio DSP Chain
Audio runs at **44.1 kHz in 64-sample blocks** (~1.45 ms latency). Each effect slot can be bypassed, dynamically freeing SRAM and CPU load (reclaiming ~115 KB SRAM and ~25% CPU when bypassed). Delay and reverb blocks preserve natural decay tails.

Default signal chain: `DYN → DRIVE → AMP → CAB → EQ → MOD → DELAY → REVERB` (dynamically reorderable on the hardware front panel).

| Block | Models & Algorithms in Source |
|---|---|
| **DYN** | **GATE**, **BOOST+GATE**, **COMP+GATE**, **AI GATE MS**, **COMP**. Exposes threshold, release, attack, ratio, and makeup gain depending on selected algorithm. |
| **DRIVE** | **TS-808**, **Klon-style**, **OCD-style**, **RAT-style**, **Big Muff-style**, **Fuzz Face-style**. 4x oversampled non-linear clipping with GRIT, TONE, and LEVEL controls. |
| **AMP / NAM** | Single-precision Float32 WaveNet inference for **Neural Amp Modeler (NAM)** A2 captures, including embedded A2-Lite configuration and hot-loadable user captures. |
| **CAB** | Cabinet impulse-response convolution (up to 2048 taps in Q18) with output level, low-cut, and high-cut shelving filters. Supports user `.wav` IR loading. |
| **EQ** | **5-BAND** Graphic EQ, **LOW/HIGH CUT** filter, and **PARAMETRIC-3** (low shelf, sweepable mid, high shelf). |
| **MOD** | **CHORUS**, **PHASER**, **FLANGER**, **TREMOLO**, **VIBRATO**, **WAH**, **AUTO WAH**. |
| **DELAY** | **ANALOG**, **DUCKING**, **dTAPE**, **DUAL**, **LOFI**, **DIGITAL**, **PING PONG**, **REVERSE**, **SLAPBACK**. |
| **REVERB** | **ROOM**, **HALL**, **SWELL**, **SPRING**, **SHIMMER**, **CLOUD**, **PLATE**. |

### 2. Groove Station Drum Machine
- **Project-authored rhythm patterns**: 49 rhythm styles stored across a 48-step-per-4/4-bar grid (`custom-fw/src/dsp/drum_patterns.inc`).
- **Procedural synthesized kit**: 12 drum voices generated deterministically by `tools/gen_drum_kit.py` without third-party audio recordings (`custom-fw/src/dsp/drum_kit.inc`).
- **Optimized on-device codecs**: Glide-sine kick/toms, 4-bit IMA-ADPCM snare/hats/ride/bell, G.711 mu-law open hat and crash, and 16-bit PCM metronome click.

### 3. Dry Guitar Captures (Courtesy of Jonas Hemmersbach)
The emulator and test pipeline incorporate self-captured dry guitar DI phrases courtesy of **Jonas Hemmersbach** (`custom-fw/emu/samples/`), providing natural instrument-level clean chord progressions, rock riffs, and melodic licks. Third-party GuitarSet recordings are completely excluded.

### 4. Front Panel & Ergonomics
- **ST7735S 240 × 240 Color LCD**: High-contrast Onyx dark design system with dynamic VU meters, block icons, and parameter knobs.
- **Hardware Controls**: Illuminated tactile buttons (`HOME`, `VOL-`, `VOL+`, `<`, `>`) and push-rotary `PARA` encoder.
- **Live Bank Mode**: Groups presets into 4-slot performance banks (`P01-P04`, `P05-P08`, etc.) for instant one-touch switching.
- **Chromatic Tuner**: Needle display with detected note, octave, frequency in cents, and selectable 435–445 Hz reference pitch.

### 5. Connectivity & Bluetooth LE MIDI
- **USB MIDI & SysEx**: Real-time parameter sync, bidirectional preset librarian transfer, and bootloader communication.
- **Bluetooth LE MIDI**: Wireless MIDI footswitches (e.g., Chocolate Plus) and mobile editor app connectivity via the integrated open **Optimist** BLE stack.

> [!NOTE]
> **Builder item `BLE` (off by default):** By default, the build never powers or touches the Bluetooth / Wi-Fi radio hardware on the device (`PA_BLE=0`). To build a firmware image with active wireless Bluetooth LE radio support, you must explicitly enable the builder item (`-Ble` in `build.ps1` or `BLE=1` in `make`):
> ```powershell
> powershell -ExecutionPolicy Bypass -File custom-fw/build.ps1 -Ble
> ```

---

## Getting Started

### Your First Install

1. **Take a full-chip backup.** Before writing anything to flash, create a byte-for-byte backup of your pedal using the recovery tool:
   ```powershell
   python custom-fw/tools/pa_transporter.py --timeout 300 dump backup/
   python custom-fw/tools/qa_recovery.py verify backup/dump1.bin backup/dump2.bin --record
   ```

2. **Build and package.** Compile the verified Onyx firmware release package:
   ```powershell
   powershell -ExecutionPolicy Bypass -File custom-fw/build.ps1 -Product onyx
   ```
   The verified release image is placed in `custom-fw/release/Onyx_<version>/`.

3. **Install.**
   - **Via Web Editor (Recommended)**: Open [`custom-fw/web/Onyx_Web_Editor.html`](custom-fw/web/Onyx_Web_Editor.html) in Chrome or Edge, click **Connect MIDI**, navigate to the **Firmware** tab, select `Onyx_<version>.bin` (or `.fwsc`), and click **Flash**.
   - **Via Command Line**:
     ```powershell
     python custom-fw/tools/flash_unit.py custom-fw/build_app.bin
     ```

4. **First boot.** The pedal restarts into Onyx. On first cold boot, the firmware detects an uninitialised store and automatically formats the 20 preset slots (writing the 12 factory ambient chains to P01–P12 and preparing P13–P20 for user saves).

### Getting Back & Recovery

- **To the stock firmware**: In the Web Editor or via `pa_transporter.py`, restore the app partition (`0x4000..0x85FFF`) from your baseline backup dump:
  ```powershell
  python custom-fw/tools/pa_transporter.py --timeout 300 drill backup/dump1.bin 0x4000-0x86000 --repair --full-verify
  python custom-fw/tools/pa_transporter.py runapp
  ```
- **If the unit fails to boot**: Onyx includes an automatic boot guard. If a boot does not complete within 5 seconds, the watchdog will trip the unit into mask ROM download mode. You can also force this manually by holding the `PARA` encoder down for 2.5 seconds during power-on (the screen remains black; the unit enumerates as `WL82 UBOOT1.00`, USB `4C4A:8057`).
- **Emergency unbrick (RP2040 Transporter dongle)**: If the flash is erased or corrupted, an RP2040 dongle (such as a Seeed XIAO RP2040 connected to the USB-C port via three wires: D+, D-, GND) injects the ROM USB key at power-on, recovering the device over USB with no soldering required. See [RECOVERY.md](RECOVERY.md).

---

## Controls and Hardware User Guide

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

The hardware contains a built-in mask-ROM bootloader (`WL82 UBOOT1.00`) used for low-level flashing, memory inspection, and recovery:

1. **Manual Hardware Button Combination**:
   - While the unit is powered on and running, press and hold **`HOME` + `PARA` simultaneously for 2 seconds**.
   - The display turns black, audio ceases, and the device reboots into mask-ROM download mode.
   - On Windows, the unit enumerates on USB as a SCSI disk device named `WL82 UBOOT1.00` (VID/PID `4C4A:8057`).
   - **To exit Download Mode**: Simply toggle the hardware power switch OFF and back ON. The device will boot normally into Onyx firmware.
2. **One-Click via Web Editor**:
   - Open [`custom-fw/web/Onyx_Web_Editor.html`](custom-fw/web/Onyx_Web_Editor.html) in Chrome or Edge over Web MIDI.
   - On the firmware flasher panel, click **"Put the unit in download mode"**.
3. **Emergency Hardware Recovery (Bricked / Non-Booting Units)**:
   - If the firmware image in flash is ever damaged and the unit cannot boot to respond to button presses, a hardware USB dongle (an RP2040 running the Transporter recovery firmware) forces ROM download mode at power-on via USB D+/D− data lines. See [RECOVERY.md](RECOVERY.md).

---

## Local Development & Building from Source

### Prerequisites

- **Windows 10/11 with Windows PowerShell 5.1 or PowerShell 7**.
- **Python 3** available as `python` on `PATH`.
- **LLVM/Clang for the host build** (`clang.exe` on `PATH`).
- **JieLi pi32v2 toolchain for the target build**, including `clang.exe`, `pi32v2-ld.exe`, `llvm-objcopy.exe`, and the `pi32v2-include` and `pi32v2-lib/r3` directories (`$env:PI32_ROOT` or `C:\JL\pi32`).
- **Node.js** for the WebAssembly and editor functional test suites.

### Firmware Build

From the repository root on Windows:

```powershell
# Standard build: Builder item BLE off (PA_BLE=0) — radio hardware untouched
powershell -ExecutionPolicy Bypass -File "custom-fw/build.ps1"

# With working Bluetooth LE radio enabled:
powershell -ExecutionPolicy Bypass -File "custom-fw/build.ps1" -Ble
```

The build script compiles and runs host verification suites, cross-compiles all C modules using JieLi Clang (`-target pi32v2 -mcpu=r3 -O2`), links with `app.ld`, packages the signed release container, and verifies build integrity.

### QA Gate

Run the automated regression and provenance gate:

```powershell
python custom-fw/tools/qa_gate.py --base main
```

The gate validates static code standards, unit tests, functional suites, flash layout budgets, provenance, and recorded hardware recovery baselines.

### Running the Desktop Emulator

Launch the live emulator workbench:

```powershell
Start-Emulator.bat
```
*(or run `powershell -ExecutionPolicy Bypass -File custom-fw/emu/run_emu.ps1`)*

The workbench opens at `http://localhost:8080`, connecting to your audio interface over ASIO and streaming the live front-panel display.

---

## Repository Map

```text
custom-fw/
  include/       Firmware interfaces, DSP headers, and configuration
  src/           Board support, DSP engine, effect algorithms, NAM inference, presets
  test/          Host C and Python unit and functional regression suites
  emu/           Native desktop emulator (DLL, ASIO server, Jonas dry captures, UI)
  wasm/          WebAssembly build and browser workbench
  tools/         Build pipeline, packaging, QA gates, recovery tools, and asset generators
  qa/            Hardware evidence ledger and verification reports
  build.ps1      Automated Windows build and packaging script
  app.ld         JieLi AC791N linker memory layout
ARCHITECTURE.md  System architecture, module map, and execution model
TEST-PROTOCOL.md Hardware recovery verification and regression protocol
RECOVERY.md      Hardware USB unbricking and recovery runbook
NOTICE.md        Scope, third-party material licenses, and provenance records
LICENSE          GNU General Public License v3.0 text
```

---

## AI-Assisted Development Notice

This project was developed through human-in-the-loop pair programming using artificial intelligence (Anthropic Claude and Google DeepMind Antigravity / Gemini models):
- **Architecture, prompt steering, and hardware debugging:** Directed and tested on physical hardware by the human maintainer (@28spawn).
- **DSP algorithms, firmware architecture, and test suites:** Generated, refined, and documented with AI assistance.
- **Verification:** All DSP routines, memory allocations, and USB/SysEx protocols undergo host unit testing and strict automated regression gates (`qa_gate.py`) before flashing to physical hardware.

---

## Credits & Upstream Prior Art

- **Dry Guitar Captures:** Sincere gratitude to **Jonas Hemmersbach** for providing high-fidelity clean dry guitar DI captures for testing, emulation, and tone validation.
- **Bluetooth LE MIDI:** The BLE stack is from the **Optimist** project (<https://github.com/w0ts/optimist>): link layer, GATT/ATT, pairing, and radio driver written by **hdavid** for Optimist, building upon **Felucca** (Leo Kuroshita / Hügelton Instruments), **SLOOP**, and upstream contributors. Integrated in `custom-fw/third_party/optimist_ble/` under `GPL-3.0-only OR MPL-2.0`.
- **FM-1 & Community Research:** Gratitude to the open-source firmware community (Optimist, fm1-dock, Saadet, Felucca) for documenting hardware behaviors and open BLE MIDI development on this architecture.
- **Vendor SDK:** Public AC79 SDK interface specifications (Apache-2.0) are referenced solely for register mapping and interface definitions; no proprietary code is redistributed.

---

## Disclaimer

Provided as-is, for research and interoperability. Flashing custom firmware is performed at your own risk and may void warranties. All trademarks belong to their respective owners; Onyx is an independent interoperability and research project and is not affiliated with, endorsed by, or sponsored by M-VAVE, Donner, or JieLi.

---

## License

This project is licensed under the **GNU General Public License v3.0 only** (`SPDX-License-Identifier: GPL-3.0-only`). Component files retain their respective upstream licenses (`MPL-2.0` and `GPL-3.0-only OR MPL-2.0`); see [NOTICE.md](NOTICE.md) and [LICENSE](LICENSE) for full legal text.
