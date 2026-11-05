# Pocket Amp Transporter firmware

The recovery dongle is a Seeed XIAO RP2040 running Leo Kuroshita's **FM-1 Transporter**
firmware (MIT), rebuilt with the Pocket Amp's flash write window. This folder holds the
change as a patch; the firmware source, the SDK and the JieLi loader blob are not in this
repository.

| | FM-1 build (upstream) | Pocket Amp build (this patch) |
|---|---|---|
| Write window | `0x4000`–`0x93000` | `0x0`–`0xB5000` (MBR, uboot, app) |
| Refused | everything else | everything from `0xB5000` up: VM, BTIF, USER, AM4 cab data, preset banks, settings |
| `info` reply | `OK key=980F type=3 id=856014` | `OK key=980F type=3 id=856014 win=0-B5000` |

`pa_transporter.py` reads the `win=` field. With the upstream build it falls back to the
FM-1 window and refuses Pocket Amp sectors outside it.

## Build (Windows)

Upstream: FM-1 Transporter commit `a632d92`, Pico-PIO-USB `fe9133f` (the pinned submodule),
pico-sdk `2.2.0` (TinyUSB `86ad6e5`).

1. Install the tools (once):
   ```bash
   winget install --id Kitware.CMake --exact
   ```
   ```bash
   winget install --id Ninja-build.Ninja --exact
   ```
   ```bash
   winget install --id Arm.ArmGnuToolchain --exact
   ```
2. Get the sources **outside this repository** (the build embeds a third-party blob):
   ```bash
   mkdir -p ~/Downloads/pa-transporter-build && cd ~/Downloads/pa-transporter-build
   git clone --depth 1 --branch 2.2.0 https://github.com/raspberrypi/pico-sdk.git pico-sdk
   git -C pico-sdk submodule update --init --depth 1 lib/tinyusb
   git clone <FM-1 Transporter repository or local copy> pa-transporter
   cd pa-transporter && git checkout a632d92 && git submodule update --init
   ```
3. Apply the patch:
   ```bash
   git apply <this repo>/custom-fw/tools/transporter/pocket-amp-window.patch
   ```
4. Configure and build (PowerShell; adjust the toolchain folder to the installed version):
   ```powershell
   $env:PATH = "C:\Program Files\CMake\bin;C:\Program Files (x86)\Arm GNU Toolchain arm-none-eabi\12.2 mpacbti-rel1\bin;$env:LOCALAPPDATA\Microsoft\WinGet\Links;C:\Program Files\Git\usr\bin;$env:PATH"
   cmake -S . -B build -G Ninja -DPICO_SDK_PATH=..\pico-sdk -DPICO_BOARD=seeed_xiao_rp2040 -DFM1T_LOADER_BIN=C:/Users/<you>/Downloads/jl-uboot-tool-main/jl-uboot-tool-main/data/loaderblobs/usb/wl82loader.bin
   cmake --build build
   ```
   - `wl82loader.bin` (24064 B) comes from jl-uboot-tool. The configure step must print
     `embedding ...wl82loader.bin`; without it `info`/`read` fail.
   - Git's `usr\bin` must be on PATH: the build runs `patch`.
   - **picotool build error** `no member named 'back_inserter' in namespace 'std'` (newer
     clang): add `#include <iterator>` near the top of `build\_deps\picotool-src\cli.h` and
     run the build again. It only affects the host-side packaging tool.
5. Result: `build\fm1_transporter.uf2` (about 212 KB). Keep a copy named
   `pocket-amp-transporter.uf2`.

## Install on the XIAO

1. Unplug the XIAO, hold its **B** (BOOT) button, plug it in, release. A drive named
   **RPI-RP2** appears.
2. Copy the `.uf2` onto it. The XIAO reboots by itself.
3. Check: `python custom-fw/tools/pa_transporter.py ports` finds two ports. The `win=` field
   shows up in `info` once a Pocket Amp is in download mode.

The RP2040's BOOT mode is in its ROM, so this cannot brick the XIAO. To go back to the FM-1
build, copy the upstream `fm1_transporter.uf2` the same way.

Build used on unit 1 (2026-10-30): `pocket-amp-transporter.uf2`, sha256 `2aa7e9e5cabee85d...`.
