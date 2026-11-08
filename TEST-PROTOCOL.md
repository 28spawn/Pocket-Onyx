# Pocket Amp Custom Firmware — Test Protocol

> **Owner:** QA · **Version:** 1.0 (2026-10-22) · **Applies to:** every change under `custom-fw/`
>
> One rule above all others: **no unit is flashed unless the gate says it may be, and no
> unit is flashed without a full-chip backup and a recovery route that has been proven by
> an actual restore.**

---

## 1. How to use this document

| When | What to run | Who |
|---|---|---|
| **After every patch**, before you share it | `python custom-fw/tools/qa_gate.py --base <branch-it-started-from>` | the developer |
| **Before the first flash of any unit** | Section 4, recovery readiness (once per unit, re-drill every 90 days) | QA |
| **Before flashing a candidate** | The gate must say `READY FOR BENCH TEST` | QA |
| **On the bench, per candidate** | The hardware tests the gate report lists (section 6) | QA |
| **For a release** | The gate must say `RELEASE GO` with `--release` | QA + developer |

The gate does every automated check in one go, reads the hardware evidence you record in
`custom-fw/qa/hw_ledger.json`, and writes a report to `custom-fw/qa/reports/latest.md`. The
report includes a **checklist of the hardware tests this particular patch needs**.

```bash
python custom-fw/tools/qa_gate.py --base main
```

Faster partial runs while you work (a partial run never clears an image for flashing):

```bash
python custom-fw/tools/qa_gate.py --only static
```

```bash
python custom-fw/tools/qa_gate.py --skip-build
```

---

## 2. Verdicts

| Verdict | Exit code | Meaning | What you may do |
|---|---|---|---|
| **NO-GO** | 1 | An automated check or a recorded hardware test failed | Fix it. Do not flash anything |
| **FLASH BLOCKED** | 2 | Automated checks pass, but no recovery route is proven (section 4) | Do the recovery work first. Do not flash |
| **READY FOR BENCH TEST** | 0 | All automated checks pass and recovery is proven | Flash **the bench unit only** and run the listed hardware tests |
| **RELEASE GO** | 0 | Same, plus every required hardware test for **this exact image hash** is recorded PASS | Publish the image |

A `WARN` never blocks on its own, but each one must be either fixed or explained in the patch
notes. `--release` turns the "untracked sources" warning into a failure, because a release
has to be rebuildable from git.

---

## 3. The automated checks (what the gate runs)

### 3.1 S: static checks on the source tree

| ID | Check | Why it exists | Fails when |
|---|---|---|---|
| S-01 | The source manifest matches every build target | A module silently missing from one build broke the emulator three times | `check_sources.py` fails |
| S-02 | Every build input is tracked in git | Core files (`pa_audio_core.c`, `pa_ota.c` …) existed only on one disk | Any untracked file under `src/`, `include/`, `tools/`, `test/` (FAIL with `--release`) |
| S-03 | No file over 50 MB in git or waiting to be added | Large external archives sat unignored in the root | Tracked: FAIL. Untracked and not ignored: WARN |
| S-04 | No hard-coded personal paths in build tooling | Builds only worked on one machine | WARN, with the list |
| S-05 | No two peripherals share a register base address | The watchdog feed once wrote Timer4's control register and stopped the clock | Two `JL_*_BASE` / `PA_*_BASE` defines with the same address |
| S-06 | The PB0 profiling pin is off in release builds | A debug pin toggled in the ISR raced the LCD control pins | The pin isn't behind `PA_PROFILE_PIN`, or `build.ps1` defines it |
| S-07 | Busy-waits on hardware registers are bounded | An unbounded wait in the audio ISR freezes the whole device | WARN for each `while` on a register with no timeout. Fix it, or allowlist it with a reason in `qa/spin_allowlist.txt` |
| S-08 | Every flash partition fits the chip, lies inside a rewritable window, and overlaps no other | A partition off the end of the 1 MB chip wraps onto the bootloader | FAIL once device flash writes are wired; WARN while they are stubbed. The chip size comes from the ledger, or 1 MB until the chip is read |
| S-09 | The rewritable windows keep clear of the boot record, bootloader, app area, calibration and factory bundle; every device erase/write uses `pa_flash_rw_ok()`; reads are bounds-checked | The fence is what keeps a firmware bug from erasing the bootloader | A window overlaps a protected area, a device erase/write function lacks the check, or there's no bounds check |
| S-10 | Reports whether device flash writes are wired | Turning writes on raises the risk level of every later patch | Info only |

### 3.2 B: build (`build.ps1`)

| ID | Check | Fails when |
|---|---|---|
| B-01 | `build.ps1` completes. That covers the source check, 12 host suites, the pi32 cross-compile, the link (with the memory-map `ASSERT`s), the effect matrix (164 cases) and packaging + verification | Any step exits non-zero. Full log: `custom-fw/qa/reports/last_build.log` |

### 3.3 U: host suites `build.ps1` does not run

| ID | Suite |
|---|---|
| U-01 | `test/test_ble_midi.c`: BLE-MIDI framing |
| U-02 | `test/test_sysex_read.c`: SysEx read commands |
| U-03 | `test/test_safe_flash.py`: every anti-brick rule of the safe writer (section 4.6), on a simulated flash |
| U-16 | `test/test_usb_midi_e2e.c`: USB-MIDI end to end on the host: framing, replies, the pedal PC / CC table in the stock module order, TX saturation. Known defects D2-D4 of the suite are reported as KNOWN and do not fail it |
| U-17 | `test/test_usb_desc.c`: the USB descriptor tables (`include/pa_usb_desc.h`): lengths, interface numbers, interface associations, endpoints, links, the stock audio format (34 checks, each broken in turn by a mutation test when the checks were written) |
| U-18 | `tools/midi_map_check.py`: the PA-EXT record extension (`MIDI-MAP-CUSTOM.md` 4.3): its 13 bytes are zero in all stock records, the extension round-trips, no other byte moves |

### 3.4 F: functional regression (emulator DLL + WebAssembly)

These run the **firmware's own code** (the same `pa_audio_core`, UI and DSP) on the PC.

| ID | Test | Covers |
|---|---|---|
| F-00 | WASM build (`wasm/build_wasm.py`) | The portable core still builds for the browser |
| F-01 | `emu/test_onyx_ui.py` | 30 UI flows with snapshots: grid, moving blocks, editors, bank, tuner, groove, save, settings |
| F-02 | `emu/test_buttons.py` | Every button and combo maps to one action |
| F-03 | `emu/test_editor.py` | The block parameter editor |
| F-04 | `emu/test_input.py` | Debounce, tap vs hold, combos |
| F-05 | `emu/test_jack.py` | The 24-bit audio I/O path produces audio |
| F-06 | `emu/test_level.py` | Output levels per model/preset |
| F-07 | `emu/test_ir.py` | Cab IR loading |
| F-08 | `test/test_sysex_frame.py` | SysEx framing |
| F-09 | `test/test_sysex_read.py` | Editor read commands against the DLL |
| F-10 | `test/test_ble_midi_emu.py` | BLE-MIDI path into the firmware |
| F-11 | `test/test_sysex_upload.py` | USB-MIDI upload session |
| F-12 | `test/test_groove_pixels.py` | Groove Station screen is pixel-identical across bars |
| F-13 | `emu/test_block_adapter.py` | Any host frame count: no crackle from partial 64-sample blocks |
| F-14 | `emu/test_dly_rev.py` | Delay and reverb end to end: echo times equal the screen, stereo, dry level, tails |
| F-20…F-26 | `wasm/test_*.js` | Jack, chords, NAM runs, NAM level, PARA encoder, preset round-trip, SysEx upload |

### 3.5 I: the image (is this exact file safe to put on a device?)

| ID | Check | Why |
|---|---|---|
| I-01 | The artifacts exist and are newer than every source | Never test or flash a stale image |
| I-02 | The app fits its 720,896-byte partition (WARN under 8 KB free) | An overrun runs into the VM/calibration partitions |
| I-03 | `build_app.bin` is exactly `.text` + `.data` of the linked ELF | Catches packaging from leftover files |
| I-04 | Container product marker, header CRC, table CRC, entry CRCs | The updater rejects (or worse, half-accepts) a bad container |
| I-05a | The stock template is the known V105 file (SHA-256 baseline) | A wrong template means the wrong bootloader goes on the device |
| I-05 / I-05b | Only the app window differs from stock. MBR + uboot (`0x0000–0x4FFF`) are byte-identical | **This is the anti-brick check.** If the bootloader bytes ever change, the device may not come back |
| I-06 | The app bytes inside the container are exactly `build_app.bin` | |
| I-07 | The image layout (plain or encrypted app, offset, entry) has **hardware proof** that it boots | The stock app region has hardware encoding and ours is plain. That only works if a device has been seen running this layout. Proof lives in the ledger's `layout_proofs` |
| I-08 | `_start` is at the bootloader's entry address `0x02000120` | The bootloader jumps there |
| I-09 | Startup, ISR and section symbols are present | A link that dropped them won't boot |
| I-10 | Section sizes are within 5 % of the baseline (WARN) | Catches accidental bloat; RAM growth eats the stacks |

### 3.6 R and H: from the ledger (section 4 and section 6)

| ID | Check |
|---|---|
| R-01 | Flash chip identified (part number, size) |
| R-02 | Full-chip backup: 2 identical dumps, SHA-256 recorded |
| R-03 | At least one recovery route marked `proven` by a real restore |
| R-04 | Last restore drill less than 90 days old |
| H-01 | Every hardware test this patch needs is recorded PASS (or N/A) for this exact image hash |

---

## 4. Recovery readiness (once per unit, before any custom flash)

This is the part that keeps a bad image from costing you a device. Do all of it on the
**stock** firmware, on the unit you'll use as the bench unit. The gate's R-01…R-04 checks
read the results from `custom-fw/qa/hw_ledger.json`. Don't edit that file by hand: the
`qa_recovery.py` commands below write it for you.

### 4.0 Tools

| Tool | Used for | Where |
|---|---|---|
| `custom-fw/tools/qa_recovery.py` | Verifies dumps, backs up calibration, records every R step in the ledger | this repo |
| `custom-fw/tools/enter_download_mode.py` | The experiment in 4.3 (where stock "enter upgrade" leads) | this repo |
| **`custom-fw/tools/pa_transporter.py`** | **Writes the flash through the USB_KEY dongle** (the proven route): dump, `drill` (restore from the ledger-baseline backup only, sector-verified), `break` (recovery test). See [RECOVERY.md](RECOVERY.md) | this repo |
| **`custom-fw/tools/jl_safe_flash.py`** | Writes the flash from a PC-side ROM download session: identity check, protected regions, backup check, sector-by-sector verify (section 4.6). Not used on unit 1 | this repo |
| `jl-uboot-tool` (kagaimiq, MIT) | The USB protocol layer `jl_safe_flash.py` uses. Has a **wl82 (AC791N)** loader; the FM-1 projects use it on the same chip | `Downloads\jl-uboot-tool-main\jl-uboot-tool-main` (its Python packages are installed) |
| **USB_KEY dongle**: Seeed XIAO RP2040 + the FM-1 Transporter firmware (MIT), wired to the USB data lines (D+, D−, GND; VBUS left off) | Forces ROM download mode whatever is in the flash. The way out when the app is dead or has no USB. Use the **Pocket Amp build** (write window `0x0`–`0xB5000`, `custom-fw/tools/transporter/`); the upstream FM-1 build only writes `0x4000`–`0x93000` | **built; proven on unit 1 (2026-10-30)** |
| Official updater + stock `Pocket AMP.fwsc` | The way out if the unit stays in upgrade mode; the `updater_stock` route | `Downloads\updater`, `Downloads\Pocket AMP.fwsc` |
| A way into ROM download mode: a **JieLi USB updater dongle**, or a DIY one (Raspberry Pi Pico sending the `0x16EF` key, see `jl-uboot-tool/docs/how-to-enter-uboot.md`) | Dumping and restoring over USB even when the app is dead | **to buy or build** |
| **CH341A programmer (3.3 V-fixed) + SOIC-8 clip** and NeoProgrammer / AsProgrammer / flashrom | Dump and restore when the flash is a separate 8-pin chip | **to buy, if 4.1 finds such a chip** |

**Safety rule:** never write with `jl-uboot-tool`'s own shell. Its `write`, `erase`,
`erasechip` and `burnchipkey` commands have no protection. `burnchipkey` burns the chip's
key fuses permanently, and `write 0 …` overwrites the boot head. Every write goes through
`pa_transporter.py` (dongle) or `jl_safe_flash.py`. Never use the FM-1's `fm1t.py write` on a
Pocket Amp: its window and map are the FM-1's.

### 4.1 Identify the flash (R-01)
1. Open the unit and look for an 8-pin SOIC/WSON flash chip next to the JieLi SoC.
   Photograph its marking and look up the part.
2. **No separate chip?** Then the flash is inside the SoC package. On JieLi parts, the number
   at the end of the SoC marking is the flash size **in megabits** (8 = 1 MB). The programmer
   route doesn't exist on such a unit, so the USB route (4.2 A) has to work.
3. The flash ID, read by `jl-uboot-tool` once you're in download mode, settles the size: the
   last byte is log2 of the size (`0x14` → 2²⁰ = 1 MB).
4. The part number is recorded with the dump, in step 4.2 (`--chip`).

### 4.2 Full-chip backup (R-02)

**A. Over USB (no soldering, no clip):**
1. Get the unit into ROM download mode. Try, in this order: the experiment in 4.3, then a
   JieLi USB updater dongle.
2. From an **Administrator** terminal (Windows only lets admins send raw SCSI commands to
   the drive):
   ```bash
   python custom-fw/tools/jl_safe_flash.py info
   ```
   ```bash
   python custom-fw/tools/jl_safe_flash.py dump backups/unit1
   ```
   `info` checks the chip key (`0x980F`) and reads the flash ID. `dump` reads the whole chip
   twice, saves `dump1.bin` and `dump2.bin`, and records the flash ID in the ledger.

**B. With a programmer** (separate flash chip only):
1. **Disconnect the battery.** Clip onto the chip. Use a programmer at **3.3 V**; many CH341A
   boards put 5 V on the data lines and need the 3.3 V fix.
2. Read the whole chip, save it as `dump1.bin`, re-seat the clip, read it again as `dump2.bin`.

**Then, for both routes:**
```bash
python custom-fw/tools/qa_recovery.py verify dump1.bin dump2.bin --chip "<part number>" --record
```
This command:
- refuses dumps that differ, or that are blank;
- **detects a read that wrapped**: if the upper half repeats the lower half, the chip is half
  the size you read, and it records the real size;
- shows every stock partition and warns if the per-unit **BTIF** (RF calibration and BLE MAC)
  or **USER** (factory calibration) areas are empty;
- **compares the dump with the stock `.fwsc`**. If the device's app area holds exactly the
  `.fwsc`'s encrypted bytes, the chip requires encryption on the app area, so a plain custom app can't
  work. The gate's I-07 check then fails with that evidence;
- writes `custom-fw/qa/backups/<hash>/`: the full dump plus `BTIF.bin`, `USER.bin`, `VM.bin`
  and a manifest;
- records the chip, the real size, the hash and the evidence in the ledger. The gate's flash
  size check (S-08) then uses the real size.

**Copy the backup folder to two places off this PC**, then record them:
```bash
python custom-fw/tools/qa_recovery.py verify dump1.bin dump2.bin --chip "<part number>" --record --copied-to "USB stick" "cloud drive"
```

### 4.3 Test the recovery routes (R-03)
Test each route on stock firmware and record the result, with evidence, every time:

```bash
python custom-fw/tools/qa_recovery.py route <route> --status proven|reachable|failed --evidence <photo/log>
```

`proven` means a restore with this route worked. `reachable` means the mode can be entered,
but no restore has been done yet. Only `proven` counts for R-03.

| Route | How to test it |
|---|---|
| `stock_enter_upgrade` | The experiment below |
| `spi_programmer` | Write the 4.2 dump back with the programmer, verify, power up: the unit runs stock |
| `jieli_isd_download` | In ROM download mode (reached by the soft key), restore with `jl_safe_flash.py restore --stock-fwsc` (dry run first), power up: the unit runs stock |
| `usb_key_dongle` | Unit off, dongle on the USB data lines, replug the dongle, start `pa_transporter.py`, switch the unit on: it must come up in ROM download mode. Then `drill … --repair`, and the break-and-repair test for `proven`. **This is the route that works when the app is dead. Proven on unit 1; procedure in [RECOVERY.md](RECOVERY.md)** |
| `uboot_update_mode` | Find how the bootloader enters update mode, enter it, flash stock |
| `button_dfu` | The README's "hold VOL− or HOME while plugging in USB". Watch Device Manager and record the VID:PID. "Nothing appeared" is a valid and important result: `--status failed` |
| `updater_stock` | Official updater flashing stock over stock. Works only while the running app speaks USB, which the custom app doesn't yet |

**The `stock_enter_upgrade` experiment.** It answers one question: when the stock firmware
gets the updater's "enter upgrade" command, does it land in the ROM download mode (good:
`jl-uboot-tool` can then dump over USB) or in JieLi's OTA loader (not usable for dumping)?
The script writes nothing itself, **but the stock firmware may keep the unit in upgrade mode
until an update completes. Have the official updater and the stock `.fwsc` ready.**
```bash
python custom-fw/tools/enter_download_mode.py --list
```
```bash
python custom-fw/tools/enter_download_mode.py --query
```
```bash
python custom-fw/tools/enter_download_mode.py --enter --i-understand --wait 30 --jl-tool "path/to/jl-uboot-tool"
```
- `--query` sends only the identification request. It's a read.
- `--enter` watches Windows Plug and Play for 30 s, classifies what appears (ROM download
  mode, OTA loader, or nothing), saves the evidence to `custom-fw/qa/reports/`, and prints the
  exact `qa_recovery.py route …` line to record the result.

**The restore drill (R-04).** One full round trip on stock: restore with a route, power up,
check it runs stock. **With the dongle** (as done on unit 1, see [RECOVERY.md](RECOVERY.md)):
```bash
python custom-fw/tools/pa_transporter.py --timeout 300 drill <baseline dump1.bin> 0x4000-0xB5000 --full-verify
```
then `runapp`, check the identity, and for `proven` the dead-unit test: `break <dump> 0x5000
--i-understand`, confirm the unit is dead, `drill <dump> 0x5000 --repair --full-verify`, boot.
Record partial drills separately: the gate passes R-04 on any dated drill.

With the PC-side writer, the drill on a stock unit is:
```bash
python custom-fw/tools/jl_safe_flash.py restore --stock-fwsc "path/to/Pocket AMP.fwsc" --ref backups/unit1/dump1.bin
```
On a stock unit this reports "nothing to do": the flash already holds the stock app area. That
proves the route reaches the flash, reads it correctly and agrees with the backup, but not
that it can write. For a write test, follow the FM-1 practice: change one unused sector
(for example in the factory-cab area), write it, then put it back:
```bash
python custom-fw/tools/jl_safe_flash.py restore --from-dump backups/unit1/dump1.bin --ref <the dump taken after the test write> --write
```
Then record it:
```bash
python custom-fw/tools/qa_recovery.py drill --route <route> --result pass --evidence <photo/log>
```
A passing drill also marks its route `proven`. Re-drill every 90 days, and whenever a patch
touches packaging, the linker script, startup code or the USB/OTA path. The gate's impact
table makes HW-14 required for those patches.

### 4.4 Prove the image layout once (I-07)
The gate's I-07 checks the layout offline: `pack_ufw.py` lays the app area out the vendor way
(`jl_app_area.py`: file table at `0x4000`, `app.bin` at `0x4120`, CRCs, SFC encoding). The first time a
layout runs on hardware, the gate report names it (e.g.
`jl-app-area@0x4120-start0x2000120`). Record what you saw:
```bash
python custom-fw/tools/qa_recovery.py layout-proof --sha <image sha256> --observed "Bluetooth row reads NO RADIO; System Info shows the custom version" --evidence photos/system-info.jpg
```
The observation must be something **only the custom firmware shows**. A unit that "boots"
may still be running stock after a rejected update.

### 4.5 Check where you stand
```bash
python custom-fw/tools/qa_recovery.py show
```
```bash
python custom-fw/tools/qa_gate.py --only ledger
```
Results of hardware runs (section 6) go in the same way:
```bash
python custom-fw/tools/qa_recovery.py hw-run --sha <image sha256> --tester <name> HW-01=PASS HW-02=PASS HW-13=N/A
```

---

### 4.6 The safe writer: the way in and the way out

`custom-fw/tools/jl_safe_flash.py` is the only tool that writes a unit's flash. It uses the
JieLi ROM download mode (USB, no opening the case) through `jl-uboot-tool`'s protocol layer,
and follows the rules the FM-1 community learned on the same chip.

| Rule | What it does |
|---|---|
| Identity | Chip key must be `0x980F`. The flash ID and size must match the ones recorded for this unit. Otherwise nothing happens |
| Protected | `0x0000–0x3FFF` (boot head: `uboot.boot`, `isd_config`, the way back in), BTIF `0xBD000`, USER `0xBE000` and the last sector (`key_mac`, `0xFF000` on 1 MB) are **never** erased or written. An image that would change them is refused, not trimmed |
| Backup | `--ref` must be this unit's backup, recorded in the ledger (4.2). Outside the sectors being written, the flash must still match it |
| Sectors | Only whole 4 KB sectors are erased: no block erase, no chip erase. Each is erased, written, read back and compared, with 3 attempts, then the run stops |
| Resume | Sectors that already hold their new bytes are skipped. After a stop, **don't power off**: run the same command again |
| Final | The whole chip is read back and must equal the expected image byte for byte |
| Consent | Dry run by default. `--write` asks you to type `WRITE` |
| Never | Chip erase, block erase, chip-key burning and memory pokes are not reachable from this tool (U-03 checks that) |
| Log | Every write leaves a journal in `custom-fw/qa/reports/` and a line in the ledger |

**Rehearse first, every time, with no device attached.** Point the writer at a copy of your
dump and use a scratch ledger, so the real ledger isn't touched:
```bash
python custom-fw/tools/jl_safe_flash.py --mock-flash backups/unit1/dump1.bin --mock-save rehearsal.bin --ledger rehearsal-ledger.json write app_area.bin --at 0x4000 --ref backups/unit1/dump1.bin --write
```

**The way in (flashing a custom image):**
1. Soft key on a stock unit (4.3), or the USB_KEY dongle.
2. `jl_safe_flash.py info`, then a fresh `dump` and `qa_recovery.py verify … --record` if the
   last backup isn't from today.
3. `jl_safe_flash.py write <app-area image> --at 0x4000 --ref <that backup>`: dry run, read the
   plan, then `--write`.

**The way out (recovery):**
1. Into ROM download mode: soft key if the running firmware supports it, otherwise the USB_KEY
   dongle. The dongle doesn't depend on anything in the flash, and the writer never touches
   the boot head, so this way in always stays available.
2. `jl_safe_flash.py restore --stock-fwsc "Pocket AMP.fwsc" --ref <backup>` puts the stock app
   area back, or `restore --from-dump <backup>` puts back this unit's whole backup (protected
   sectors excluded).

**Before the writer can flash a custom image,** the image has to be in the chip's format: the
app encrypted with the chip key and placed at `0x4120`, with the app directory at `0x4000`
rebuilt. Today's `pack_ufw.py` output is not in that format (QA report, C1/C2). The writer
will put any image on the flash that passes its rules, but a wrong image means a unit you
then have to recover.

---

## 5. Bench flashing procedure (every candidate)

1. Gate verdict is `READY FOR BENCH TEST`. Copy the image SHA-256 from the report.
2. Battery above 50 % **and** on stable USB power. Use a known-good data cable plugged
   straight into the PC (no hub).
3. **First boot of any new image:** volume at minimum, **no headphones on ears**. Use a
   speaker or an audio interface at low gain. A firmware bug can produce a full-scale burst.
4. Flash with the documented tool. Note the exact file and its SHA-256 (HW-01).
5. If the unit doesn't come up within 30 s: power-cycle once. If it still doesn't come up,
   stop and follow the proven recovery route from section 4. **Don't try random button
   combinations on a unit that's half-flashed.**

---

## 6. Hardware tests

**Equipment:** guitar or signal generator; audio interface to record the output; a reference
tone source (440 Hz, 82.41 Hz); a USB power meter; a stopwatch; a camera; the SPI programmer.

Record every result in the ledger with `qa_recovery.py hw-run` (section 4.5). The entry it writes looks like this:

```json
"hw_runs": [
  { "sha256": "<image sha256>", "date": "2026-10-23", "tester": "name",
    "results": { "HW-01": "PASS", "HW-02": "PASS", "HW-13": "N/A" },
    "notes": "…", "evidence": "photos/…, recordings/…" }
]
```

Use `PASS`, `FAIL` or `N/A`. `N/A` is only allowed where the test says so.

| ID | Test | Steps | Pass criteria |
|---|---|---|---|
| **HW-01** | Flash | Flash the candidate per section 5 | Tool reports success; the flashed file's SHA-256 equals the gate report's |
| **HW-02** | Cold boot | Power off, wait 5 s, power on. Repeat 10 times | 10/10 reach the custom home screen within 5 s; a custom-only marker is visible |
| **HW-03** | Idle stability | Leave on the home screen 30 min | No reset (no boot screen seen), meters and clock keep moving, buttons respond at the end |
| **HW-04** | Audio path | 1 kHz sine at −12 dBFS in. Two tiers:<br>• **Quick check (2 min):** 1 min all blocks bypassed + 1 min heavy DSP preset (WaveNet/NAM + all 7 FX active)<br>• **Full release gate (10 min):** 5 min all blocks bypassed (ALNK DMA baseline) + 5 min heavy DSP preset (thermal & load audio stability) | Continuous recording shows no gaps, clicks or level jumps; Performance page xrun counter stays strictly 0 across both dry and loaded phases |
| **HW-05** | Controls | Press every button, every combo (VOL−+VOL+ tuner, <+> bank, HOME+VOL− groove, HOME+VOL+ chain, HOME), turn and click PARA, long-presses | Each does exactly what the README says, once per press, nothing on release |
| **HW-06** | Effects and models | With guitar: each block on/off; every amp model; each cab | Audible change when on; bypass level within ±1 dB of dry; no noise bursts when switching |
| **HW-07** | Presets | Step P01→P80 and back; save a change, reboot | Click-free switching, no crash. Save: persists after reboot **or**, while flash writes are stubbed, shows a clear "save failed" message (no fake "Saved") |
| **HW-08** | Tuner | 440 Hz and 82.41 Hz reference tones; toggle mute; change the reference to 442 Hz | Reads A4 / E2 within ±2 cents; mute silences the output; reference change shifts the reading |
| **HW-09** | Groove Station | Start/stop; 40 and 240 BPM (and fractional steps); run a busy style 10 min. Record output and inspect event triggers against sample grid | Starts and stops on command; beat onsets land strictly on calculated crystal sample boundaries ($\Delta t = \frac{44100 \times 60}{\text{BPM} \times 4}$ samples/step); 0 missed or duplicated events; phase jitter < 1 DMA block (< 1.45 ms); 0 ppm cumulative sequencer drift; no crash or audio dropout |
| **HW-10** | Output safety | Highest-gain amp, input gain max, hot signal (full strum / 0 dBFS sine); switch presets; load a user NAM | Output never exceeds the limiter ceiling; no bursts, no DC thump on power on/off at normal volume |
| **HW-11** | Display | Look at every screen; step brightness through its range | No garbage, offset or tearing; every brightness step visible; never fully dark |
| **HW-12** | Soak | 60 min on battery, groove playing and guitar through | No reset; case not hot to the touch; battery gauge moves plausibly |
| **HW-13** | USB MIDI | Plug into a PC; run `tools/usb_enum_check_pc.ps1`; read the build id over the diagnostic channel (`ble_matrix_pc.py --report`); open the editor if it is available; replug 10 times | The script prints PASS (the device node and the MIDI function `MI_03` present, no problem code); the diagnostic channel answers; 10/10 replugs enumerate. **Required whenever the USB descriptors, the driver or the SysEx / MIDI path changed** |
| **HW-17** | USB audio | Plug into a PC; run `tools/usb_enum_check_pc.ps1`. From step C of `USB-AUDIO-PLAN.md`: record 10 min on the PC (test tone, then the chain output); from step D: record 60 s on an Android phone over OTG | Step A: the audio function `MI_00` present, an audio endpoint listed, MIDI unchanged. Step C: no gap over 10 min, the sample-rate error measured and recorded in ppm, the level equal to the DAC's. Step D: no dropout |
| **HW-18** | BLE-MIDI link, per host | For each of Windows, iPhone, Android: `tools/ble_link_trace_pc.py` (4 gated attempts on a PC, or `--watch` while you connect by hand); judge the JSON files with `tools/ble_gate_eval.py windows:... iphone:... android:...` | The evaluator prints `HW-18 PASS`: every gated attempt connects and switches notifications on; 0 established links die of a supervision timeout; every established link heard >= 95 % and valid >= 95 %; the link closes and the unit advertises again within 10 s of our disconnect. The failure-to-establish share is reported, not gated |
| **HW-19** | MIDI controller map | With a real foot controller or a DAW send PC 0, 1, 79; CC 20-27 (the stock module order: FX, AMP, MOD, DLY, REV, CAB, then our DRIVE and EQ); CC 64-67; every further CC or NRPN `MIDI-MAP-CUSTOM.md` lists as implemented | Each does what the map says, once per message; a stock controller's module CCs act on the module they name |
| **HW-20** | Factory preset levels on the unit | Play one reference DI through P01-P40 and measure the output level | All within +-1.5 dB of each other; none silent; none clipping. The emulator check F-16 is not the DAC |
| **HW-14** | Recovery from this build | Restore stock with the proven route, confirm stock boots, re-flash the candidate | Both directions work. **Required whenever packaging, linker, startup or USB/OTA changed** |
| **HW-15** | Power loss during save | Pull power mid-save 20 times | Every boot comes up with either the old or the new preset bank, never garbage. **N/A while flash writes are stubbed** |
| **HW-16** | CPU headroom | Load the heaviest preset (WaveNet/user NAM + every effect), feed signal for 5 min | Performance page average CPU below 80 %; peak audio callback duration $\le 1.16\text{ ms}$ ($\le 80\%$ of the 1.451 ms block deadline for =64$ at 44.1 kHz); xrun counter stays strictly 0 |

**Stop rule:** a loud burst, a reset loop, a frozen screen with live audio, or anything hot →
stop, power off, record FAIL with what you saw. Don't continue the run on that image.

---

## 7. Which hardware tests a patch needs

The gate works this out from `git diff` against `--base` and prints the checklist. In
summary:

| Changed | Hardware tests required |
|---|---|
| `app.ld`, `build.ps1`, `pack_ufw.py`, `entry.S`, `main.c`, `spi_nor.c`, `pa_hw.h`, `pa_flash_map.h`, `pa_config.h` | **All of them**, including the recovery drill HW-14 |
| `audio_alnk.c`, `hw_accel.c` | HW-01–04, 10, 12, 16 (plus 06 for the accelerator) |
| DSP / NAM / effects (`src/dsp`, `src/nam`, `src/fx`) | HW-01, 02, 04, 06, 10, 16 |
| UI (`ui_*.c`), `gpio_scan.c`, `st7735s.c` | HW-01, 02, 05, 11 (plus 08, 09 for UI) |
| `preset.c`, `settings.c`, `pa_user_nam.c` | HW-01, 02, 07, 15 (plus 06, 10 for NAM) |
| `usb_ble.c` | HW-01, 02, 13, 17, 19 |
| `usb_dev.c`, `pa_usb_desc.h` | HW-01, 02, 13, 17 |
| `pa_usb_*.h` | HW-01, 02, 13, 17, 19 |
| `sysex.c`, `pa_sysex.h`, `pa_protocol.h` | HW-01, 02, 13, 19 |
| `pa_ota.c` | HW-01, 02, 13 (plus 14) |
| `pa_diag.c` | HW-01, 02, 13 |
| `ble_midi*.c` | HW-01, 02, 13, 18, 19 |
| `ble_radio.c`, `third_party/optimist_ble/*`, `pa_ble_*.h` | HW-01, 02, 18 |
| `factory_presets.inc` | HW-01, 02, 07, 20 |
| Emulator, WASM, tests, tools, docs only | None (automated gate only) |
| Anything else under `custom-fw/src` | All of them |

HW-01 and HW-02 are always required when anything reaching the device changed. With no
`--base`, the gate requires every test.

---

## 8. Keeping the protocol honest

1. **Every bug gets a regression test.** When a bug is found, on the bench or anywhere else,
   the fix isn't done until a check fails without it: a host suite assertion, an emulator
   test, a gate rule, or a new HW step.
2. **Tests must be able to fail.** A suite that only prints numbers isn't a test. Today
   `test_full_firmware.c`, `test_dsp_chain.c`, `test_reclaim.c`, `test_cpu_meter.c` and
   `test_wavenet_cycles.c` still have no assertions. Adding them is open work.
3. **Baselines move only on purpose.** `--update-baseline` accepts the current section sizes
   (and records the stock template hash the first time). Do it in its own commit and say why.
4. **The allowlist needs a reason.** Every line in `qa/spin_allowlist.txt` gets a comment
   saying why that wait can't hang the device.
5. **Evidence, not memory.** A ledger entry without a photo, recording or log path counts as
   not done.

---

## 9. What the automated gate can't see

These only show up on hardware, which is why section 6 exists:

- whether the bootloader accepts and starts the app (layout, encryption, entry);
- real register behaviour: timers, the accelerator, ALNK, SPI LCD, GPIO;
- real CPU load (the host cycle numbers are estimates, and the WaveNet engine is float on a
  CPU without an FPU);
- audio quality and safety at the real DAC;
- power, battery and thermal behaviour;
- recovery itself.

Open items that will add tests here when they land: USB audio streaming (HW-17 grows with each step of `USB-AUDIO-PLAN.md`), Bluetooth audio playback, wired flash writes (HW-15 becomes mandatory and S-08 becomes a hard fail), the
real watchdog (add a hang-recovery test), and a JEDEC-ID read at boot (lets S-08 use the
detected chip size).
