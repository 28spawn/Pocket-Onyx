# Onyx Licensing

Onyx is free software. Its **code** is licensed under the GNU General Public
License, version 3 only (`GPL-3.0-only`, full text in [`LICENSE`](LICENSE)).

Onyx is an independent, clean-room custom firmware, digital signal processing (DSP)
engine, and desktop emulation suite for JieLi AC791N / WL82 guitar headphone amplifiers
and audio interfaces (such as the Donner / M-VAVE Pocket AMP hardware platform). Onyx is
not affiliated with, supported by, or connected to M-VAVE or Donner.

The project builds upon and integrates modular components from the open-source FM-1 / JieLi audio
ecosystem:
- **Optimist BLE Stack** (by hdavid / Optimist contributors): open-source Bluetooth Low Energy
  MIDI stack for the JieLi WL82/AC791N radio.
- **Pendel Update Loader and BSP** (by Leo Kuroshita / Hügelton Instruments): open-source stage-2
  update loader and hardware abstraction layer.
- **Pendel Package Maker & LZ4 Compression** (by Leo Kuroshita / Hügelton Instruments): vendor-free
  firmware packaging and block compression tools.
- **Neural Amp Modeler (NAM)** (by Steven Atkinson): WaveNet deep learning architecture for
  guitar amplifier emulation.
- **JieLi AC79 AIoT SDK** (by Zhuhai Jieli Technology Co., Ltd.): public peripheral headers,
  register layouts, and boot headers.

Copyright (C) 2026 the Onyx contributors\
Copyright (C) 2026 Jonas Hemmersbach (for natural instrument-level DI dry captures)\
Copyright (C) 2026 hdavid (for `optimist_ble`, imported from Optimist)\
Copyright (C) 2026 Leo Kuroshita (@kurogedelic), Hügelton Instruments (for `pendel_loader` and `onyx_pkg_make.py`)\
Copyright (C) 2024–2026 Zhuhai Jieli Technology Co., Ltd. (for JieLi AC79 SDK files under Apache-2.0)\
Copyright (C) 2020 Steven Atkinson (for the Neural Amp Modeler architecture under MIT)

Every ported or derived module retains its original authors' copyright notices and license headers
in its respective source files. Provenance details and repository locations are cataloged in the
[Third-Party Material](#third-party-material) table below.

---

## What is code (GPL-3.0-only)

Every file in this tree that carries an `SPDX-License-Identifier: GPL-3.0-only` header:

- **Firmware Application Layer (`custom-fw/src/`)**:
  - Audio processing core and dynamic 8-block signal graph: [`custom-fw/src/sys/pa_audio_core.c`](custom-fw/src/sys/pa_audio_core.c), [`custom-fw/src/fx/fx_graph.c`](custom-fw/src/fx/fx_graph.c)
  - Effect DSP modules (Dynamics, Drives, WaveNet NAM amp inference, Cabinet IR convolution, 5-band / parametric EQ, Modulations, Delays, Algorithmic Reverbs): [`custom-fw/src/fx/`](custom-fw/src/fx/)
  - Groove Station rhythm sequencer and procedural drum synthesizer: [`custom-fw/src/dsp/drums_synth.c`](custom-fw/src/dsp/drums_synth.c), [`custom-fw/src/dsp/drum_codec.h`](custom-fw/src/dsp/drum_codec.h)
  - Front-panel UI, 240×240 ST7789 LCD rendering engine, chromatic tuner, menus, and input handling: [`custom-fw/src/board/ui_app.c`](custom-fw/src/board/ui_app.c), [`custom-fw/src/board/gpio_scan.c`](custom-fw/src/board/gpio_scan.c), [`custom-fw/src/dsp/tuner.c`](custom-fw/src/dsp/tuner.c)
  - System configuration, preset banking, flash storage, and SysEx protocol engine: [`custom-fw/src/sys/`](custom-fw/src/sys/)
  - SoC bootstrap, linker script, and hardware interrupt service routines: [`custom-fw/src/board/entry.S`](custom-fw/src/board/entry.S), [`custom-fw/app.ld`](custom-fw/app.ld)
- **Desktop Emulator & WebAssembly Core (`custom-fw/emu/`, `custom-fw/wasm/`)**:
  - Bit-accurate C emulator bridge, native Win32/Linux emulator DLL, and test audio harness: [`custom-fw/emu/emu_bridge.c`](custom-fw/emu/emu_bridge.c), [`custom-fw/emu/server.py`](custom-fw/emu/server.py)
  - WebAssembly simulator compilation and audio worklet runtime: [`custom-fw/wasm/`](custom-fw/wasm/)
- **Host Verification Suites (`custom-fw/test/`)**:
  - Unit tests, DSP math assertions, torn-write flash robustness tests, and full 164-configuration effect matrix: [`custom-fw/test/`](custom-fw/test/)
- **Developer Tools & Automation (`custom-fw/tools/`)**:
  - Flashing and recovery tools: [`custom-fw/tools/jl_safe_flash.py`](custom-fw/tools/jl_safe_flash.py), [`custom-fw/tools/pa_transporter.py`](custom-fw/tools/pa_transporter.py), [`custom-fw/tools/jl_ram_read.py`](custom-fw/tools/jl_ram_read.py)
  - Procedural asset generators: [`custom-fw/tools/gen_drum_kit.py`](custom-fw/tools/gen_drum_kit.py), [`custom-fw/tools/gen_drum_patterns.py`](custom-fw/tools/gen_drum_patterns.py), [`custom-fw/tools/gen_test_audio.py`](custom-fw/tools/gen_test_audio.py)
  - Automated QA gates, stress benchmarks, and firmware packagers: [`custom-fw/tools/qa_gate.py`](custom-fw/tools/qa_gate.py), [`custom-fw/tools/pack_ufw.py`](custom-fw/tools/pack_ufw.py), [`custom-fw/tools/onyx_pkg_make.py`](custom-fw/tools/onyx_pkg_make.py)
- **Web UI & Management Console (`custom-fw/web/`)**:
  - Onyx Web Editor single-page application and WebMIDI SysEx bridge: [`custom-fw/web/Onyx_Web_Editor.html`](custom-fw/web/Onyx_Web_Editor.html)
- **Synthesized Data & Sound Profiles**:
  - Factory preset bank data: [`custom-fw/src/sys/factory_presets.inc`](custom-fw/src/sys/factory_presets.inc)
  - Procedural drum kit data and patterns: [`custom-fw/src/dsp/drum_kit.inc`](custom-fw/src/dsp/drum_kit.inc), [`custom-fw/src/dsp/drum_patterns.inc`](custom-fw/src/dsp/drum_patterns.inc)
  - Procedural default cabinet impulse response table: [`custom-fw/src/fx/cab_default_table.inc`](custom-fw/src/fx/cab_default_table.inc)

You may use, inspect, modify, and redistribute this software under the terms of the GNU General Public
License v3.0. If you distribute Onyx or any derivative firmware images, you must provide
the complete corresponding machine-readable source code under the same license terms, including to
recipients of physical devices containing modified builds.

---

## The Bluetooth LE stack (GPL-3.0-only OR MPL-2.0)

The Bluetooth Low Energy stack located in [`custom-fw/third_party/optimist_ble/`](custom-fw/third_party/optimist_ble/)
was authored by **hdavid** for **Optimist** (<https://github.com/w0ts/optimist>), an open firmware project for
the JieLi WL82/AC791N SoC.

The code was imported verbatim from Optimist commit `fcdc4881e8788365c62ea8be7fa11b4d82966bac`
(recorded in [`UPSTREAM-COMMIT.txt`](custom-fw/third_party/optimist_ble/UPSTREAM-COMMIT.txt)). Upstream offers
this Bluetooth subsystem under either **GPL-3.0-only** or the **Mozilla Public License 2.0** (`MPL-2.0`,
full text in [`custom-fw/LICENSES/MPL-2.0.txt`](custom-fw/LICENSES/MPL-2.0.txt)), at the recipient's option.
Files in that directory carry the header:
`SPDX-License-Identifier: GPL-3.0-only OR MPL-2.0`.

Onyx itself, and the final compiled firmware binary, remains governed by `GPL-3.0-only`.
The dual-license grant permits other open-source JieLi projects to incorporate the Bluetooth stack under
MPL-2.0 regardless of their overall application licensing model.

The imported Bluetooth files comprise:
- `custom-fw/third_party/optimist_ble/src/` (Link Layer, Host Controller Interface, Security Manager, ATT/GATT protocol engines)
- `custom-fw/third_party/optimist_ble/hal/` (`fm1_ble.h`, `fm1_ble_rf.h`)
- `custom-fw/third_party/optimist_ble/tests/` (unit verification suites)
- `custom-fw/third_party/optimist_ble/docs/` (`BLE-STACK.md`, `BLE-HW-FACTS.md`)

### Local Modifications (`PA-MOD`)
Modifications required for target hardware integration and host operating system tolerance are
strictly isolated and marked in the source with `/* PA-MOD: ... */`:
1. **RF Settle Timings (`hal/fm1_ble_rf.h`)**: Wrapped `FM1_VCO_EDGE_US` and `FM1_VCO_SETTLE_US` in `#ifndef` guards so board glue can adjust PLL settling dynamically without altering upstream defaults.
2. **RF Calibration Hooks (`hal/fm1_ble_rf.h`)**: Added `FM1_VCO_STOCK_SCAN()` and `FM1_LUT_W1()` optional macro hooks for run-time RF register tuning.
3. **Control Instant Tolerances (`src/ble_ll.c`)**: Introduced backward tolerance for `LL_CHANNEL_MAP_IND` and `LL_CONNECTION_UPDATE_IND` instants. If a Windows host transmits an update instant that passed by fewer than 64 connection events, the link no longer disconnects with error `0x28`; instead, the map is applied two events ahead.
4. **Link Layer Diagnostics (`src/ble_ll.c`)**: Extended diagnostic telemetry ring buffer to capture `CONNECTION_UPDATE_IND`, `CHANNEL_MAP_IND`, `TERMINATE_IND`, and parameter negotiation events.
5. **Static Random Address Hook (`src/ble_hw_wl82.c`)**: Integrated `pa_ble_static_addr()` to derive a persistent static random address from unit calibration data across power cycles.
6. **Adaptive Frequency Remapping (`src/ble_hw_wl82.c`)**: Added `PA_BLE_SOFT_REMAP` supporting software-based frequency remapping to resolve hardware baseband packet loss issues.
7. **Window Widening (`src/ble_hw_wl82.c`)**: Scaled `hw_widening` by `HW_WIDEN_X` to accommodate host-side Bluetooth jitter.

### Radio Startup Tables
The RF PHY register initialization data in [`custom-fw/third_party/optimist_ble/hal/ble_rf_tables_v15.h`](custom-fw/third_party/optimist_ble/hal/ble_rf_tables_v15.h)
reflects JieLi WL82 RF register definitions published by Zhuhai Jieli Technology and M-VAVE under **Apache-2.0**.
This is hardware configuration data, not proprietary application logic.

---

## The Update Loader and Packaging Engine (MPL-2.0 & GPL-3.0-only)

Updating firmware safely in flash requires a dedicated secondary bootloader capable of receiving new images
and validating checksums before committing writes.

### 1. Update Loader (`MPL-2.0`)
The update loader source in [`custom-fw/loader/onyx_loader.c`](custom-fw/loader/onyx_loader.c) and the runtime OTA session
handler in [`custom-fw/src/sys/pa_ota_session.c`](custom-fw/src/sys/pa_ota_session.c) are derived from the
**pendel-bsp** project (<https://github.com/hugelton/pendel-bsp>), authored by **Leo Kuroshita** (Hügelton Instruments).
The underlying BSP files in [`custom-fw/third_party/pendel_loader/`](custom-fw/third_party/pendel_loader/) are licensed
under the **Mozilla Public License 2.0** (`MPL-2.0`, full text in [`custom-fw/LICENSES/MPL-2.0.txt`](custom-fw/LICENSES/MPL-2.0.txt)).

MPL-2.0 is a file-level copyleft license without "Incompatible With Secondary Licenses" notices. As provided in
MPL-2.0 §3.3, MPL-licensed source files may be combined with GPL code into a single executable work. Our modified files
maintain their original MPL-2.0 headers and are published openly in this repository.

### 2. Package Builder & Block Compression (`GPL-3.0-only`)
The firmware packaging tool [`custom-fw/tools/onyx_pkg_make.py`](custom-fw/tools/onyx_pkg_make.py) and the LZ4 block
encoder [`custom-fw/tools/lz4blk.py`](custom-fw/tools/lz4blk.py) are adapted from Pendel's `pa_pkg_make.py`
(Copyright (C) 2026 Leo Kuroshita, GPL-3.0-only). They generate standard `.fwsc` update containers directly from
compiled ELF binaries without requiring any proprietary vendor toolchain utilities.

---

## How the Build Works Without Vendor Material

A primary goal of Onyx is total release independence: any user or developer must be able to
verify, build, test, and flash the firmware from source without downloading proprietary vendor firmware packages,
dumping copyrighted vendor assets, or relying on closed SDK runtime blobs.

Here is how the clean-room pipeline operates:

```
[ JieLi Open SDK Repo ]              [ Physical Device ]
   (Apache-2.0)                         (User's Own)
        |                                    |
  get_sdk_files.py                     0x0BF000 Sector
  (uboot.boot, cfg_tool.bin,                 |
   eq_cfg_hw.bin)                            | (Read-Only at runtime)
        |                                    v
        v                          +--------------------+
[ Sdk Aggregation ]                | stock_assets.c     |
        |                          | (Independent NOR   |
        |                          |  SPI Parser)       |
        |                          +--------------------+
        |                                    |
        +------------------+                 |
                           |                 v
[ Source Tree ]            |       +--------------------+
  custom-fw/src/           |       | Audio Core & FX    |
  (GPL-3.0-only)           |       | (NAM WaveNet,      |
        |                  |       |  Freeverb, Dattorro|
     build.ps1             |       |  Drive WDFs, etc.) |
  (Pi32v2 Clang/LD)        |       +--------------------+
        |                  |                 |
        v                  v                 v
   build_app.bin + onyx_loader.bin + SDK Configs
                           |
                           v
                   onyx_pkg_make.py
                   (LZ4 Compression)
                           |
                           v
                    Onyx_1.xx.fwsc
                    Onyx_1.xx.bin
```

### 1. No Vendor Firmware Binaries Distributed
This repository contains no vendor `.fwsc` update packages, stock firmware images, factory ROM dumps, or
proprietary shared libraries (`libfm1_engine.a` is deliberately absent). Pre-compiled release archives ship
only our compiled code, open loader, and Apache-2.0 public configuration structures.

### 2. Automated Fetching of Public SDK Headers (`get_sdk_files.py`)
To build a valid bootable flash container, the JieLi on-chip boot ROM requires three hardware initialization structures.
Rather than committing these binaries to the repository, [`custom-fw/tools/get_sdk_files.py`](custom-fw/tools/get_sdk_files.py)
downloads them on demand directly from Zhuhai Jieli Technology's official open-source repository
([fw-AC79_AIoT_SDK](https://gitee.com/Jieli-Tech/fw-AC79_AIoT_SDK), tag `AC79NN_SDK_V1.2.1`, Apache-2.0):
- `uboot.boot` (WL82 stage-1 bootloader stub, SHA-256 `64ccc63e4df3238046fadb200f1b134311d8fe6ec6e3ccb8c8b81379231b867e`)
- `cfg_tool.bin` (peripheral pin multiplexing definitions, SHA-256 `276579954f076886a6a7694f65dc71c034a63a2c204b76749065c0ac7b010d1b`)
- `cfg/eq_cfg_hw.bin` (hardware DAC analog filter coefficients, SHA-256 `179ac9f4501d2cf8c3bdbc06fcda1e46228b7ace7e339342c767bb6568de57d8`)

These files contain hardware configuration structures, not executable application code. They are placed in
the git-ignored `custom-fw/third_party/jieli_sdk_files/` directory, aggregated into the container package
by [`custom-fw/tools/onyx_pkg_make.py`](custom-fw/tools/onyx_pkg_make.py), and accompanied by the Apache-2.0
license text in all releases.

### 3. Zero Distribution of Factory Sound Models & IRs
Vendor amplifier models and cabinet impulse responses are subject to third-party copyright. To maintain strict
compliance:
- Stock models and factory IRs are **never stored** in the repository or embedded in release binaries.
- Instead, [`custom-fw/src/sys/stock_assets.c`](custom-fw/src/sys/stock_assets.c) implements an independent,
  read-only parser that accesses the device's factory SPI-NOR flash sector (`0x0BF000`) directly on the user's
  physical hardware at runtime.
- **Hardware Flash Write Protection**: Flash write boundaries are enforced by [`pa_flash_rw_ok()`](custom-fw/include/pa_flash_map.h)
  and [`onyx_loader.c`](custom-fw/loader/onyx_loader.c). The factory partition (`0x0BF000`) and low bootloader
  sectors (`0x000000..0x003FFF`) are write-locked and can never be modified or erased by firmware updates.
- **Graceful Fallbacks**: When run on uninitialized hardware or in desktop emulation where no factory flash
  partition exists, the audio engine automatically substitutes neutral synthetic impulse responses and models
  from [`custom-fw/src/fx/cab_default_table.inc`](custom-fw/src/fx/cab_default_table.inc).

### 4. Dry Instrument Captures and Synthesized Test Fixtures
No commercial guitar recordings or copyrighted third-party audio packages (such as GuitarSet) are used:
- **Dry Guitar Captures (Jonas Hemmersbach)**: The 15 natural instrument-level DI audio tracks used by the
  desktop emulator for tone audition and DSP evaluation (`chords 1..5.wav`, `riff 1..5.wav`, `Lick 1..5.wav` in
  `custom-fw/emu/samples/`) are original clean guitar captures recorded and provided courtesy of **Jonas Hemmersbach**.
  Legacy synthetic / GuitarSet phrases have been completely excluded and replaced.
- **Synthesized Test Audio & IRs**: Impulse responses (`custom-fw/emu/irs/*.wav`) and test neural captures
  (`custom-fw/emu/test_capture.nam`) are generated procedurally using mathematical models in
  [`custom-fw/tools/gen_test_audio.py`](custom-fw/tools/gen_test_audio.py).
- **Groove Station Drum Kit**: The drum voices are synthesized algorithmically by [`custom-fw/tools/gen_drum_kit.py`](custom-fw/tools/gen_drum_kit.py).
  The kick and tom drums are generated using parametric sine sweeps with noise decay bursts; snare, hi-hat,
  and cymbal voices use compressed ADPCM and mu-law procedural sound tables. Rhythm patterns are authored
  algorithmically in [`custom-fw/tools/gen_drum_patterns.py`](custom-fw/tools/gen_drum_patterns.py).

### 5. Open Toolchain & Custom Linker Architecture
The build environment does not rely on proprietary IDEs or vendor build servers:
- **Compiler**: Uses the open LLVM-based JieLi Pi32v2 toolchain (`clang -target pi32v2 -mcpu=r3`).
- **Startup Core**: Custom assembly bootstrap in [`custom-fw/src/board/entry.S`](custom-fw/src/board/entry.S)
  handles system clock setup, memory clear, and core-1 coordination.
- **Linker Script**: [`custom-fw/app.ld`](custom-fw/app.ld) defines an explicit memory map for XIP flash
  execution (`0x02000120`), critical fast-path SRAM functions (`.ram_text`), flash controller routines (`.flashfn`),
  and dual-core execution (`.c1_text`).
- **Verification Pipeline**: [`custom-fw/build.ps1`](custom-fw/build.ps1) executes a 5-stage verification gate:
  host source manifest validation, cross-compilation, automated 164-test audio effect matrix execution,
  vendor-free `.fwsc` packaging, and cryptographic SHA-256 manifest generation.

---

## Third-Party Material

| Component | Origin / Author | License | Tree Location | Description & Notes |
|---|---|---|---|---|
| **Dry Guitar Captures** | Jonas Hemmersbach | Courtesy / Project Attribution | `custom-fw/emu/samples/` (`chords 1..5.wav`, `riff 1..5.wav`, `Lick 1..5.wav`) | 15 natural instrument-level clean DI captures (44.1 kHz, 32-bit float) for emulator audition and DSP evaluation, replacing legacy GuitarSet phrases. |
| **Optimist BLE Stack** | hdavid (<https://github.com/w0ts/optimist>) | GPL-3.0-only OR MPL-2.0 | `custom-fw/third_party/optimist_ble/`, `custom-fw/src/board/ble_radio.c` | Bluetooth Low Energy link layer, HCI, and BLE-MIDI GATT driver for the AC791N / WL82 radio. Pinned at commit `fcdc4881e8`. Local modifications are tagged with `PA-MOD`. |
| **Pendel Loader & BSP** | Leo Kuroshita, Hügelton Instruments (<https://github.com/hugelton/pendel-bsp>) | MPL-2.0 | `custom-fw/third_party/pendel_loader/`, `custom-fw/loader/onyx_loader.c`, `custom-fw/src/sys/pa_ota_session.c` | Stage-2 flash update loader, SPI-NOR flash communication primitives, and SysEx firmware update session state machine. |
| **Pendel Package Maker & LZ4** | Leo Kuroshita, Hügelton Instruments (<https://github.com/hugelton/Pendel>) | GPL-3.0-only | `custom-fw/tools/onyx_pkg_make.py`, `custom-fw/tools/lz4blk.py` | Standalone Python packaging utility and block compressor that outputs `.fwsc` update files matching the hardware bootloader layout. |
| **JieLi AC79 SDK Boot Files** | Zhuhai Jieli Technology Co., Ltd. (<https://gitee.com/Jieli-Tech/fw-AC79_AIoT_SDK>) | Apache-2.0 | Fetched dynamically into `custom-fw/third_party/jieli_sdk_files/` | Public bootloader header `uboot.boot`, pin mapping `cfg_tool.bin`, and audio DAC profile `cfg/eq_cfg_hw.bin`. Aggregated at package creation; not committed to git. |
| **JieLi RF Calibration Tables** | Zhuhai Jieli Technology & M-VAVE | Apache-2.0 | `custom-fw/third_party/optimist_ble/hal/ble_rf_tables_v15.h` | Hardware register tables and PLL VCO initialization values for WL82 radio startup, published under Apache-2.0 as recorded in Optimist's licensing. |
| **Neural Amp Modeler (NAM)** | Steven Atkinson (<https://github.com/sdatkinson/neural-amp-modeler>) | MIT | `custom-fw/src/nam/nam_engine.c`, `custom-fw/src/nam/pa_wavenet.c` | WaveNet deep neural network architecture for guitar amplifier emulation, optimized for single-precision float execution on the Pi32v2 core. |
| **Freeverb** | Jezar at Dreampoint | Public Domain | `custom-fw/src/fx/fx_reverb.c` (ROOM / HALL algorithms) | Reverberation network using parallel comb filters and series allpass diffusers, adapted for embedded fixed/floating point DSP. |
| **Dattorro Figure 1 Reverb** | Jon Dattorro (Stanford CCRMA, 1997) | Public Domain / Academic | `custom-fw/src/fx/fx_reverb.c` (PLATE / SWELL algorithms) | Classic plate reverberation topology featuring input diffusion and a cross-coupled recirculating delay network. |
| **Analog Drive Waveshaper Models** | Public Domain / Mathematical Modeling | GPL-3.0-only | `custom-fw/src/fx/fx_drive.c` | 4x oversampled non-linear waveshaping algorithms modeling classic diode clipping behaviors (symmetrical silicon, germanium, and asymmetrical LED clipping). |
| **BLE-MIDI 1.0 Specification** | MIDI Association & Apple Inc. | Public Specification / Apache-2.0 reference | `custom-fw/include/pa_protocol.h`, `custom-fw/src/board/ble_radio.c` | Standard GATT service UUIDs (`03B80E5A-EDE8-4B33-A751-6CE34EC4C700`), characteristic definitions, and timestamp packet framing. |
| **`jl-uboot-tool`** | kagaimiq (<https://github.com/kagaimiq/jl-uboot-tool>) | MIT | Referenced in `custom-fw/tools/jl_safe_flash.py`, `custom-fw/tools/jl_ram_read.py` | Protocol specifications for communication with the JieLi mask-ROM USB download mode (UBOOT1.00) for device recovery and diagnostics. |

---

## Contributions

Contributions to Onyx are welcome under **GPL-3.0-only**.

By submitting a pull request or patch, you agree that your contribution is licensed under the
GNU General Public License, version 3 only. Contributions to files within the Bluetooth stack
(`custom-fw/third_party/optimist_ble/`) are accepted under **GPL-3.0-only OR MPL-2.0**, matching
the upstream dual-license grant. Contributions to the update loader (`custom-fw/loader/onyx_loader.c`)
are accepted under **MPL-2.0**.

---

## Trademarks

- **"M-VAVE"** and **"Donner"** are trademarks of their respective owners. Onyx is an
  independent open-source project and is not affiliated with, endorsed by, or sponsored by M-VAVE,
  Donner, or Zhuhai Shengke.
- **"JieLi"** is a trademark of Zhuhai Jieli Technology Co., Ltd.
- **"Felucca"**, **"Pendel"**, and **"Hügelton Instruments"** are trademarks or project names of
  Hügelton Instruments / Leo Kuroshita.
- **"Optimist"** is the project name of the Optimist firmware platform.
- **"Roland"**, **"BOSS"**, **"Tube Screamer" / "Ibanez"**, **"Klon"**, **"RAT" / "ProCo"**, and
  **"Big Muff" / "Electro-Harmonix"** are trademarks of their respective manufacturers. These names
  are used solely to describe the sonic and electronic characteristics modeled by the DSP algorithms;
  this firmware contains no proprietary code or sound libraries from those companies.

---

## Radio and Regulatory Compliance

In default builds (`PA_BLE=0`), the radio hardware remains completely unpowered and untouched for hardware safety.

When compiled with `-Ble` (`PA_BLE=1`), Bluetooth Low Energy is enabled exclusively for BLE-MIDI peripheral communication (allowing connectivity with wireless MIDI footswitches and mobile/desktop editor applications).

- **No Bluetooth Audio (A2DP)**: The firmware intentionally disables classic Bluetooth audio streaming
  to ensure strict real-time determinism and allocate available internal RAM to guitar DSP.
- **No Wi-Fi**: The Wi-Fi subsystem of the AC791N SoC is permanently uninitialized and powered down.
- **RF Parameters**: The radio initializes using standard frequency calibration values published
  under Apache-2.0. Users are responsible for ensuring that custom radio operational modes comply
  with local telecommunications regulations.

---

## Hardware Safety & Disclaimers

> [!WARNING]
> Flashing custom firmware involves writing to the physical SPI-NOR flash memory of your device.
> While Onyx enforces rigorous partition write protection (`pa_flash_rw_ok()`) and includes
> automated recovery mechanisms, flashing firmware is undertaken at your own risk. The authors and
> contributors accept no liability for hardware damage, warranty invalidation, or data loss.
> Ensure your device battery is adequately charged before initiating firmware flashing operations.
