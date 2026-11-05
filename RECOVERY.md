# Pocket Amp recovery runbook

How to back up, restore and un-brick a Pocket Amp over USB, with no soldering. Proven on
unit 1 (serial `4150353835313708`) on 2026-10-30. For what went wrong along the way, see
[LESSONS-LEARNED.md](LESSONS-LEARNED.md).

## Status

| Capability | Status | Evidence |
|---|---|---|
| Enter ROM download mode (UBOOT1.00) on any firmware state | **proven** (6 entries, incl. a unit with a dead app) | `custom-fw/qa/backups/unit1*/console.log` |
| Full-chip dump, two identical reads | **proven** on V1.03 and V1.05 | `custom-fw/qa/backups/unit1`, `unit1_v105` |
| Restore the app `0x5000`–`0xB5000` | **proven** (all 176 sectors, stock V1.05) | `unit1_v105_patfw/drill_app.out` |
| Recover a unit whose app is dead | **proven** (sector `0x5000` blanked, unit dead, repaired, boots) | `unit1_v105_recovery/` |
| Restore sector `0x4000` (app file table + first 3.8 KB of app.bin) | supported, **not drilled** (the drills above predate finding that the app area starts at `0x4000`) | — |
| Restore the boot area `0x0`–`0x4000` | supported (`--boot-area`), **not drilled** on purpose | — |

The ledger (`custom-fw/qa/hw_ledger.json`) records all of this; the QA gate's R-01…R-04 pass.

## How it works

At power-on the AC791N's mask ROM checks the USB data lines for the JieLi **USB_KEY**
(`0x16EF`). If it sees it, it skips the flash entirely and enters **UBOOT1.00**, its built-in
download mode (USB `4C4A:8057`, a SCSI disk). Because that check lives in ROM, no flash
content (a bad update, a dead custom app) can disable it.

The dongle sends the key, then acts as a USB host for the Pocket Amp, uploads JieLi's
`wl82loader` into its RAM and exposes the flash to the PC as a simple text protocol on a
serial port. `custom-fw/tools/pa_transporter.py` is the PC side.

## What you need

- **Seeed XIAO RP2040** with the **Pocket Amp Transporter** firmware
  (build and install: [custom-fw/tools/transporter/README.md](custom-fw/tools/transporter/README.md)).
  The upstream FM-1 build also enters and dumps, but can only write `0x4000`–`0x93000`.
- A **USB-C pigtail** into the Pocket Amp's port, three wires:

  | XIAO pin | Pocket Amp USB |
  |---|---|
  | D6 (GP0) | D+ |
  | D7 (GP1) | D− |
  | GND | GND |
  | — | VBUS: **not connected** |

- The Pocket Amp on **battery** (charged). It is powered by its own battery, not by the dongle.
- Python 3 with `pyserial`, and for the identity check `mido` + `python-rtmidi`.
- The verified backup: `custom-fw/qa/backups/unit1_v105/dump1.bin`
  (sha256 `66ac9ffd51172b2aed3deeee35583cc29a4e9e8047341a00fffc9c0ee69a4522`).
  Its app area is byte-identical to the stock `Pocket AMP.fwsc` (V1.05).

## Flash map (1 MiB Puya P25Q80, JEDEC `856014`; read from the unit)

| Range | Contents | Written by vendor update? | Dongle may write? |
|---|---|---|---|
| `0x000000`–`0x000FFF` | MBR / flash header (`uboot.boot`, `isd_config.ini`, `app_dir_head` → `0x4000`) | yes | only with `--boot-area` |
| `0x001000`–`0x003FFF` | uboot (flash bootloader) | yes | only with `--boot-area` |
| `0x004000`–`0x0B4FFF` | app area: file table, `app.bin` at `0x4120`, `cfg_tool.bin`, EXT table, ENC-scrambled (= `.fwsc` payload; see `custom-fw/tools/jl_app_area.py`) | yes | yes |
| `0x0B5000`–`0x0BCFFF` | VM (runtime settings records) | no | **never** |
| `0x0BD000`–`0x0BDFFF` | BTIF (Bluetooth MAC + data) | no | **never** |
| `0x0BE000`–`0x0BEFFF` | USER (empty on unit 1) | no | **never** |
| `0x0BF000`–`0x0FAFFF` | AM4 amp/cab bundle | no | **never** |
| `0x0FB000`–`0x0FCFFF` | preset bank A (factory) | no | **never** |
| `0x0FD000`–`0x0FEFFF` | preset bank B (user edits) | no | **never** |
| `0x0FF000`–`0x0FFFFF` | settings record (`save1`) | no | **never** |

"Never" is enforced twice: by the dongle firmware (window ends at `0xB5000`) and by
`pa_transporter.py`.

## Procedures

Run everything from the backup folder you want logs in. Every command keeps reading the
dongle's console and appends it to `console.log` there.

### Before every session (the part that makes it work)

1. Pocket Amp **off**, pigtail connected to the dongle, Pocket Amp **not** plugged into the PC.
2. **Unplug and replug the dongle.** A dongle that has already run a session sits in host mode
   and does not send the key again.
3. **Start the command first**, then switch the Pocket Amp **on** when it prints
   `Waiting for the Pocket Amp's ROM download mode`.
4. Expect `UBOOT session up after 13–35 s`. The Pocket Amp's screen stays **black** in
   download mode; that is normal.
5. Do the work in one go. The Pocket Amp does not stay in download mode forever; if a later
   command prints `Waiting...` again, start over from step 1.

What to look for in `console.log`: `ACK` / `false ACK, resuming key` lines repeat while the key
is sent (normal). Success is `PULSE: D+ high (ROM pull-up)` → `VID=4C4A PID=8057` →
`UBOOT READY`.

### Dump the whole chip (backup)

```bash
python ../../../tools/pa_transporter.py --timeout 300 dump .
```
```bash
python custom-fw/tools/qa_recovery.py verify <dir>/dump1.bin <dir>/dump2.bin --record
```
Reads twice, compares, hashes; `qa_recovery` extracts BTIF/USER/VM, compares with the stock
`.fwsc` and writes `custom-fw/qa/backups/<hash>/`. Copy that folder to two places off the PC.

### Restore the stock app (unit boots or not)

```bash
python ../../../tools/pa_transporter.py --timeout 300 drill ../unit1_v105/dump1.bin 0x4000-0xB5000 --repair --full-verify
```
```bash
python ../../../tools/pa_transporter.py runapp
```
- Writes only bytes from the verified backup, and only if that backup's hash is the ledger
  baseline. Sectors that already match are rewritten identically; with `--repair`, sectors that
  differ get the backup's bytes.
- Each sector: erase, write, read back, compare. `--full-verify` re-reads the whole chip.
- The 176 app sectors take about 25 s to write; the per-sector checks and the full re-read
  add a few minutes. `runapp` boots the unit; or just power-cycle it without the dongle.
- Without `--repair` the drill refuses any sector that differs (a no-change rehearsal).

### Recover a dead unit (black screen, nothing on USB)

Same as above. This exact case was tested: app sector `0x5000` blanked, the unit showed a
black screen and no USB device, the dongle entered download mode, `drill 0x5000 --repair`
restored it and it booted as `Pocket AMP_105`. If you don't know which sectors are damaged,
restore the whole app range.

### Restore the boot area (emergency only)

Only if a vendor update died while writing `0x0`–`0x4000`, or to roll back to a version with a
different boot area:
```bash
python ../../../tools/pa_transporter.py --timeout 300 drill <baseline dump> 0x1000-0x4000 --boot-area --repair --full-verify
```
Not drilled. The ROM download mode does not depend on these sectors, so a failed attempt can
be retried. Avoid sector `0x0` unless it is the one damaged.

### Check the unit afterwards

Plug the Pocket Amp straight into the PC. It enumerates as `3654:7756` (V1.05; `4C4A:C755` on
V1.03) and answers the identity query with `Pocket AMP_105` (`custom-fw/tools/mvave_device.py`,
query `F0 00 32 45 00 00 00 40 7F F7`; unpack from the byte after `F0`).

### Recovery test (proves a route; destructive until repaired)

```bash
python ../../../tools/pa_transporter.py --timeout 300 break ../unit1_v105/dump1.bin 0x5000 --i-understand
```
Blanks one app sector. It refuses unless the backup is the ledger baseline and the chip holds
the baseline there. Confirm the unit is dead, then restore it as above. Record with
`qa_recovery.py route usb_key_dongle --status proven --evidence ...`.

## Other routes

| Route | Status | Notes |
|---|---|---|
| Vendor OTA Updater (`F0 22 24 35 7F F7`) | works while the app runs USB | Leads to the vendor OTA MIDI loader (`ota-Pocket AMP_xxx`), which writes but cannot read. Not a recovery route for a dead app |
| Button combinations at power-on | **failed** | 7 cycles, no download mode |
| Custom-firmware PARA hold → `rc_recover_uboot()` | **unproven** | Only runs if the custom app boots; its SRAM/flag constants have no evidence yet |
| SPI programmer on the flash chip | untested | Last resort if the ROM route ever fails |

## Never

- Never use `fm1t.py write` or any FM-1 write tool on the Pocket Amp: its window and map are
  the FM-1's.
- Never use `jl-uboot-tool`'s own `write`, `erase`, `erasechip` or `burnchipkey`.
- Never write from a backup whose hash is not recorded in the ledger.
- Keep local diagnostic logs and binary backups in an untracked directory.
