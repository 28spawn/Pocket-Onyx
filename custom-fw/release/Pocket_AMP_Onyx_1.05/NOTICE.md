# NOTICE — scope, third-party materials, and legal

> **NOT AFFILIATED WITH M-VAVE OR DONNER.** This is an independent, unofficial interoperability project. It contains no M-VAVE/Donner firmware, amp models, cabinet impulse responses or preset banks, and it does not redistribute any such data. Ownership of a device does not by itself grant redistribution rights over its contents; the vendor's EULA/terms may restrict reverse engineering or use of extracted data. Check them (and consult counsel) before any public release.

## What this repo is
Original open-source custom firmware and DSP project authored for interoperability and
personal-modding research on a device the author owns (Donner / M-VAVE Pocket Amp, JieLi AC791N).

## What is deliberately NOT included (and where to obtain it)
To avoid redistributing third-party copyrighted software, none of the following is in this repo:
- **M-VAVE / Donner firmware** (the stock `.fwsc` or vendor updates). Obtain from the vendor's official updater.
- **Vendor editor / updater software** (binaries, DLLs, models). Obtain directly from the vendor.
- **JieLi pi32v2 toolchain & post-build tools** — https://pkgman.jieliapp.com/s/linux-toolchain and
  https://pkgman.jieliapp.com/s/linux-postbuild
- **JieLi AC79 AIoT SDK** — https://gitee.com/Jieli-Tech/fw-AC79_AIoT_SDK (Apache-2.0; commit
  `5fa84a9b2f723a5e9337ad79dc2ad741fc98fe2d`). Referenced for the BLE/GATT stack, the linker map
  and the Flash partition layout.
- **m-vave/fm1_open** — https://github.com/m-vave/fm1_open. The M-VAVE FM1 open dev kit. Per its
  official `THIRD_PARTY.md`, `sdk/lib/libfm1_engine.a` is **separately licensed** (proprietary source
  is not supplied, and its license grant is limited to FM1 devices), and its `app/` sources are
  licensed under **GPLv3 with FM1 SDK linking exception**. **Neither `libfm1_engine.a` nor any `fm1_open`
  `app/` code is included, linked, or copied into this repository.**
- **`flutter_midi_command_windows` / `_ble` / `_platform_interface`** (pub.dev, MIT/BSD) — the
  public BLE-MIDI client used as the reference for standard MIDI service UUIDs, the BLE-MIDI 1.0
  framing and the message-length table.

The hardware implementation references the public JieLi AC79 SDK (Apache-2.0). Only public protocol identifiers,
standard UUIDs, and hardware register mappings are referenced for interoperability; no proprietary vendor source is
redistributed here.

## Factory presets and stock amp/cab content
`custom-fw/src/sys/factory_presets.inc` is an original, independently authored 40-preset bank (original names and values; no vendor preset data). Stock amplifier models and cabinet impulse responses are **not** embedded in the v1.0.5+ firmware image, this repository at v1.0.5+, or the v1.0.5+ release packages: they are read at run time, read-only, from the user's own device flash partition (`0x0BF000`) by an independent reader written for interoperability (`src/sys/stock_assets.c`). Updates never erase or write that partition (`pa_flash_rw_ok()`, `onyx_loader.c`). With no such partition the firmware uses neutral synthetic placeholders. Never share or upload your factory partition.

> **Historical note:** Earlier pre-cleanup releases (`v1.0.0` and `v1.0.1`, removed from distribution) predated the `v1.0.5` clean-room refactor and contained vendor-derived tables and factory preset parameters. Those commits and objects have been removed from all branches, tags, and releases at `v1.0.5`; external clones, forks, or cached copies created prior to `v1.0.5` may retain historical objects and must not be redistributed.

## Synthesized Emulator & Workbench Test Audio
The emulator and WASM workbench test phrases (`custom-fw/emu/samples/di_*.wav`), test cabinet and reverb impulse responses (`custom-fw/emu/irs/*.wav`), default fallback cabinet table (`custom-fw/src/fx/cab_default_table.inc`), and test capture (`custom-fw/emu/test_capture.nam`) are **synthesized locally on demand** by `custom-fw/tools/gen_test_audio.py` (and the drum voices and rhythm patterns in `src/dsp/` by `gen_drum_kit.py` and `gen_drum_patterns.py`). No GuitarSet or other third-party audio recordings are tracked or distributed in this repository.

## Bluetooth LE stack (from Optimist) — third-party code, imported with our marked changes
The BLE-MIDI stack for the AC791N / WL82 radio in `custom-fw/third_party/optimist_ble/` is **not ours**. We changed it in a few places, every one marked `PA-MOD` in the source: `src/ble_hw_wl82.c`, `src/ble_ll.c`, `src/ble_smp.c`, `src/pa_prof.h`, `hal/fm1_ble_rf.h` (and the note in its `README.md`); the changes are published under the same terms as the files. It comes from
**Optimist**, an open firmware platform for the M-VAVE FM-1 (<https://github.com/w0ts/optimist>), imported verbatim from commit
`fcdc4881e8788365c62ea8be7fa11b4d82966bac` (pinned in `UPSTREAM-COMMIT.txt`; every file keeps its upstream `SPDX-License-Identifier` header).
- **Credit.** Optimist's own Bluetooth code (`src/`, `hal/fm1_ble.h`, `hal/fm1_ble_rf.h`, the `tests/ble_*` and the `docs/`) was written for Optimist by
  **hdavid** and is offered under **GPL-3.0-only OR MPL-2.0**, at the recipient's choice (Optimist's `LICENSING.md`, section "The Bluetooth stack"; text of the MPL
  in `third_party/optimist_ble/LICENSES/MPL-2.0.txt`). Optimist is copyright (C) 2026 Leo Kuroshita (@kurogedelic), Hügelton Instruments, and the Optimist contributors,
  and builds on Felucca, SLOOP and other projects named in its `LICENSING.md`.
- **Radio start-up values.** `hal/ble_rf_tables_v15.h` follows the hardware register specification under Apache-2.0 (Zhuhai Jieli Technology), as recorded in Optimist's LICENSING.md. Those values belong to
  **JieLi (Zhuhai Jieli Technology) and M-VAVE under Apache-2.0** (`SPDX-License-Identifier: Apache-2.0`, text in `LICENSES/Apache-2.0.txt`), as Optimist records in its `LICENSING.md`.
  This is data, not vendor code.
- **Provenance claim (Optimist's, not verified by us).** The stack and driver were written from the Bluetooth Core Specification (v5.x, LE 1M), the MIDI Association's
  "MIDI over Bluetooth Low Energy" 1.0, and hardware register specifications; no vendor code was copied.
- **Not imported:** Optimist's classic Bluetooth (A2DP source) stack, its UI, storage and the Felucca/SLOOP parts (`midi_ble.c`, the pages).
- **Our reading of the licences (not legal advice).** The imported files stay under their own headers (`GPL-3.0-only OR MPL-2.0`, both options compatible with this project's `GPL-3.0-only` license); our changes to them are marked `PA-MOD` and published under the same terms. Anything we write around them is `GPL-3.0-only`.

## Build
`custom-fw/` builds against the JieLi AC79 SDK + toolchain (download links above); see
`custom-fw/tools/pack.sh` and `custom-fw/README.md`.

## License
This project's original source code, firmware application layer, and tools are distributed under the terms of the **GNU General Public License v3.0 only** (`SPDX-License-Identifier: GPL-3.0-only`), with file-level `MPL-2.0` components (`custom-fw/third_party/pendel_loader/`, `custom-fw/loader/onyx_loader.c`, `custom-fw/src/sys/pa_ota_session.c`) and `GPL-3.0-only OR MPL-2.0` components (`custom-fw/third_party/optimist_ble/`) keeping their respective upstream headers. See the root [LICENSE](LICENSE) file for the full GPL-3.0 license text.

## Disclaimer
Provided as-is, for research and interoperability. Flashing custom firmware is at your own risk and may
void warranties or brick the device. All trademarks (including M-VAVE, Donner, JieLi, and TONE3000) belong to their respective owners; this project is an independent interoperability and research effort and is not affiliated with or endorsed by M-VAVE, Donner, JieLi, or TONE3000.

## The Onyx update loader and the release package - third-party code and files
The release package (`custom-fw/release/Pocket_AMP_Onyx_1.05/`, built by `custom-fw/build.ps1`) is built from scratch by `custom-fw/tools/onyx_pkg_make.py` and contains **no M-VAVE firmware**: only our own app, an update loader, and three files of
the public JieLi AC79 SDK. The package builder (`tools/onyx_pkg_make.py`, `tools/lz4blk.py`) and the layout of the package are taken from the **Pendel** project (<https://github.com/hugelton/Pendel>, `tools/pa_pkg_make.py`,
GPL-3.0-only, (C) 2026 Leo Kuroshita (@kurogedelic), Hugelton Instruments), which builds its own Pocket AMP packages the same way; they stay GPL-3.0-only here, like the rest of this repository.
- **The update loader and the firmware's side of the vendor's MIDI update.** `custom-fw/third_party/pendel_loader/` is the **pendel-bsp** hardware layer (<https://github.com/hugelton/pendel-bsp>), **MPL-2.0**, (C) 2026 Leo Kuroshita, Hugelton
  Instruments, imported unmodified with its `LICENSING.md`, `NOTICE` and licence text. `custom-fw/loader/onyx_loader.c` (MPL-2.0, from its `boards/pocketamp/pa_loader.c`) and `custom-fw/src/sys/pa_ota_session.c` (MPL-2.0, from its
  `chip/ota.c`) are our changes of those files: they stay MPL-2.0 with their headers, and their source is in this repository. MPL-2.0 is file-level: it does not change the licence of the rest of the firmware (GPL-3.0-only); MPL-2.0 allows the
  combination (Pendel's `LICENSING.md` records that its files carry no "Incompatible With Secondary Licenses" notice).
- **JieLi SDK files** (`uboot.boot`, `cfg_tool.bin`, `cfg/eq_cfg_hw.bin`), **Apache-2.0**, Zhuhai Jieli Technology: fetched by `custom-fw/tools/get_sdk_files.py` from the SDK commit named above and checked against the SHA-256 recorded in
  that tool; **not stored in this repository**. The licence text is in `custom-fw/LICENSES/Apache-2.0.txt` and ships with every release.
- User device backups and local packages remain local on the user's machine and are not part of any release.
