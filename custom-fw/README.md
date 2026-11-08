# Onyx Custom Firmware — Engine & Architecture

A maintainable, modular C firmware source tree for the **Onyx** audio platform targeting the **JieLi AC791N / pi32v2** SoC at **360 MHz assumed** (`PA_CPU_HZ` in `include/pa_config.h`; the data-sheet rate is 320 MHz).

---

## 🏗️ Architecture & Subsystems

### 1. DSP Audio Pipeline (`src/dsp/`, `src/fx/`, `src/nam/`)
- **Block Size**: 64 samples @ 44.1 kHz ($\approx 1.45\text{ ms}$ latency).
- **Fixed-Point Precision**: Q21 (integer `1 << 21` = 2,097,152) for all audio math, saturating addition, and non-linear tube GRU saturation curves.
- **Hardware Acceleration**: The `0x01EE0400` SIMD vector coprocessor executes 16-element parallel matrix MAC operations in hardware.
- **Dynamic Reclaim**: `pa_fx_chain_set_slot_enabled()` dynamically frees memory buffers for bypassed nodes (reclaiming 115,008 B SRAM and ~25% CPU load: reverb 49,384 + delay 65,624).
- **Groove Station drum machine** (`src/dsp/drums_synth.c`): 49 rhythm patterns authored in a 48-step-per-4/4-bar grid (`src/dsp/drum_patterns.inc`). The twelve drum voices are procedurally synthesised by `tools/gen_drum_kit.py` from deterministic oscillators, noise, and filters into `src/dsp/drum_kit.inc` (original material, no product recordings).

### 2. Physical Front Panel & UI (`src/board/ui_app.c`)
- **ST7735S Display**: 240x240 RGB565 display driver using the Onyx dark design system.
- **5 Physical Illuminated Buttons**: `HOME`, `VOL-`, `VOL+`, `<`, `>`.
- **Rotary PARA Encoder + Click**: Adjusts radial parameters, navigates menus, and confirms selections with hover retention.
- **Master Volume SARADC Potentiometer**: Analog input attenuation tracked in real-time.
- **Simultaneous Combos**:
  - `VOL- + VOL+`: Chromatic Tuner mode.
  - `< + >`: Bank Mode 01.
  - `HOME + VOL-`: Groove Station (Acoustic 808/909 drum machine).
  - `HOME + VOL+`: Signal Chain Sequence reordering.
  - `HOME`: From any sub-screen returns Home; from Home enters Settings Menu.

### 3. Hardware-in-the-Loop (HIL) Emulator (`emu/`)
- **`pocket_amp_emu.dll`**: C firmware compiled as a native shared library for Windows.
- **`server.py`**: PortAudio ASIO server streaming real-time guitar input from any USB audio interface through the live firmware DSP engine.
- **`ui.html`**: Front-panel web interface rendering the 240x240 LCD framebuffer, dual rotary knobs, live oscilloscope, peak meters, and drag-and-drop `.nam` and `.wav` IR hot-loaders.

---

## 🔧 Building & Verifying

### Master Automated Build
```powershell
powershell -ExecutionPolicy Bypass -File "build.ps1"
```
The script will:
1. Compile and run host verification test suites (`test/test_chain.c`, `test/test_tuner.c`, etc.).
2. Cross-compile all 24 C modules using JieLi Clang:
   ```
   clang -target pi32v2 -mcpu=r3 -O2 -Iinclude -nostdlib ...
   ```
3. Link into `build_app.bin` with `app.ld`.
4. Package verified `Onyx.fwsc` and `Onyx.ufw` firmware containers.

### Emulator DLL Compilation
```powershell
powershell -ExecutionPolicy Bypass -File "emu/build_dll.ps1"
```
