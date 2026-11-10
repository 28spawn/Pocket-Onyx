#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-only
# Copyright (C) 2026 The Onyx Authors (@28spawn)
"""
custom-fw/tools/qa_gate.py - the release gate. Run it after every patch.

It runs every automated check in TEST-PROTOCOL.md, reads the hardware/recovery
ledger (custom-fw/qa/hw_ledger.json), and prints one verdict:

    NO-GO                   an automated check failed. Do not flash.
    FLASH BLOCKED           automated checks pass, but no recovery route is
                            proven for the bench unit. Do not flash.
    READY FOR BENCH TEST    safe to flash the BENCH unit and run the hardware
                            tests this patch needs (listed in the report).
    RELEASE GO              the hardware tests for this exact image are recorded
                            as PASS in the ledger.

Exit codes: 0 = READY FOR BENCH TEST or RELEASE GO, 1 = NO-GO, 2 = FLASH BLOCKED.

Usage (from the repo root or anywhere):
    python custom-fw/tools/qa_gate.py                  full run (builds first)
    python custom-fw/tools/qa_gate.py --skip-build     check the existing artifacts
    python custom-fw/tools/qa_gate.py --base main      also work out which hardware
                                                       tests this patch needs
    python custom-fw/tools/qa_gate.py --release        stricter: untracked sources fail
    python custom-fw/tools/qa_gate.py --update-baseline  accept today's sizes as baseline

Standard library only. The report goes to custom-fw/qa/reports/.
"""
from __future__ import annotations

import argparse
import datetime as dt
import fnmatch
import hashlib
import json
import math
import os
import re
import shutil
import subprocess
import sys
import tempfile
from collections import Counter
from pathlib import Path

TOOLS = Path(__file__).resolve().parent
FW = TOOLS.parent
ROOT = FW.parent
QA = FW / "qa"
LEDGER = QA / "hw_ledger.json"
BASELINE = QA / "baseline.json"
SPIN_ALLOW = QA / "spin_allowlist.txt"
REPORTS = QA / "reports"

sys.path.insert(0, str(TOOLS))
import pack_ufw as P  # noqa: E402

# ---- the device facts the gate checks against --------------------------------------
APP_FLASH_OFFSET = 0x4120        # where app.bin sits in flash (app area 0x4000 + table 0x120)
APP_SLOT_MAX = P.APP_SLOT_MAX     # largest app.bin the app area holds (pack_ufw.py)
ENTRY = 0x02000120               # isd_config.ini ENTRY=0x2000120
STOCK_FLASH_SIZE_DEFAULT = 1 << 20   # isd_config.ini FLASH_SIZE=1M (until the chip is read)
ENTROPY_ENCRYPTED = 7.5          # bits/byte; plain pi32 code measures ~6.7-7.0

# ---- the hardware tests (TEST-PROTOCOL.md section 5) --------------------------------
HW_TESTS = {
    "HW-01": "flash completes, flashed SHA-256 matches the candidate",
    "HW-02": "cold boot to the custom UI, 10/10 power cycles",
    "HW-03": "30 min idle: no reset, no hang, UI keeps updating",
    "HW-04": "clean audio in->out: quick check (2 min: 1 min dry + 1 min heavy DSP) or full release (10 min: 5 min dry + 5 min heavy DSP), 0 dropouts, xrun=0",
    "HW-05": "every button, combo and the encoder do the documented action",
    "HW-06": "every effect block and amp model: audible on, clean bypass",
    "HW-07": "presets P01-P80 switch click-free; save/reboot behaves as documented",
    "HW-08": "tuner within +/-2 cents at 440 Hz; mute works",
    "HW-09": "Groove Station: 40/240 BPM, exact sample-boundary timing (0 drift/dropouts, jitter < 1 block)",
    "HW-10": "output safety: max gain + hot input never exceeds the limiter ceiling",
    "HW-11": "display: no garbage, no offset, brightness steps work",
    "HW-12": "60 min soak on battery: no reset, temperature and gauge plausible",
    "HW-13": "USB MIDI: enumerates (tools/usb_enum_check_pc.ps1 PASS: device node and the MIDI function MI_03, no problem code), the editor handshake and the diagnostic channel answer, 10/10 replugs",
    "HW-14": "recovery drill: restore stock from this build by the proven route, then re-flash",
    "HW-15": "power cut during save x20: no corruption (N/A until flash writes are wired)",
    "HW-16": "worst-case preset (5 min): CPU < 80 %, peak callback <= 1.16 ms (80% of 1.45 ms block deadline), xrun=0",
    "HW-17": "USB audio (USB-AUDIO-PLAN.md): enumerates as an audio device next to the MIDI port (usb_enum_check_pc.ps1: MI_00 present, an audio endpoint listed); from step C on: a 10 min PC capture without a gap, sample-rate error measured in ppm; from step D on: a 60 s Android OTG recording without dropouts",
    "HW-18": "BLE-MIDI link, per host (Windows, iPhone, Android; tools/ble_link_trace_pc.py, judged by tools/ble_gate_eval.py): every gated attempt connects and switches notifications on, 0 supervision timeouts, established links heard >= 95 % and valid >= 95 %, advertising again within 10 s of our disconnect",
    "HW-19": "MIDI controller map (MIDI-MAP-CUSTOM.md): PC 0-79, CC 20-27 in the stock module order, CC 64-67 and every other implemented CC / NRPN do what the map says, on a real controller or a DAW",
    "HW-20": "factory preset levels on the unit: P01-P40 within +-1.5 dB of each other at the output with one reference DI, no clipping, none silent (the emulator check F-16 is not the DAC)",
}

# Which hardware tests a change to a given path requires. First match wins per file;
# a file that matches nothing requires every test.
IMPACT = [
    ("custom-fw/app.ld",                 "ALL"),
    ("custom-fw/build.ps1",              "ALL"),
    ("custom-fw/tools/pack_ufw.py",      "ALL"),
    ("custom-fw/src/board/entry.S",      "ALL"),
    ("custom-fw/src/board/main.c",       "ALL"),
    ("custom-fw/src/board/spi_nor.c",    "ALL"),
    ("custom-fw/include/pa_hw.h",        "ALL"),
    ("custom-fw/include/pa_board.h",     "ALL"),
    ("custom-fw/include/boards/*",       "ALL"),
    ("custom-fw/include/pa_flash_map.h", "ALL"),
    ("custom-fw/include/pa_config.h",    "ALL"),
    ("custom-fw/src/board/audio_alnk.c", "HW-01 HW-02 HW-03 HW-04 HW-10 HW-12 HW-16"),
    ("custom-fw/src/board/gpio_scan.c",  "HW-01 HW-02 HW-03 HW-05 HW-11 HW-12"),
    ("custom-fw/src/board/st7735s.c",    "HW-01 HW-02 HW-11"),
    ("custom-fw/src/board/ui_*.c",       "HW-01 HW-02 HW-05 HW-08 HW-09 HW-11"),
    ("custom-fw/src/board/usb_ble.c",    "HW-01 HW-02 HW-13 HW-17 HW-19"),      # also decodes the pedal PC / CC messages
    ("custom-fw/src/board/usb_dev.c",    "HW-01 HW-02 HW-13 HW-17"),
    ("custom-fw/src/board/ble_midi*.c",  "HW-01 HW-02 HW-13 HW-18 HW-19"),
    ("custom-fw/src/board/ble_radio.c",  "HW-01 HW-02 HW-18"),
    ("custom-fw/third_party/optimist_ble/*", "HW-01 HW-02 HW-18"),
    ("custom-fw/include/pa_usb_desc.h",  "HW-01 HW-02 HW-13 HW-17"),
    ("custom-fw/include/pa_usb_*.h",     "HW-01 HW-02 HW-13 HW-17 HW-19"),
    ("custom-fw/include/pa_ble_*.h",     "HW-01 HW-02 HW-18"),
    ("custom-fw/include/pa_sysex.h",     "HW-01 HW-02 HW-13 HW-19"),
    ("custom-fw/include/pa_protocol.h",  "HW-01 HW-02 HW-13 HW-19"),
    ("custom-fw/src/sys/factory_presets.inc", "HW-01 HW-02 HW-07 HW-20"),
    ("custom-fw/src/dsp/hw_accel.c",     "HW-01 HW-02 HW-03 HW-04 HW-06 HW-10 HW-16"),
    ("custom-fw/src/dsp/drum*",          "HW-01 HW-02 HW-09 HW-16"),
    ("custom-fw/src/dsp/tuner.c",        "HW-01 HW-02 HW-08"),
    ("custom-fw/src/dsp/*",              "HW-01 HW-02 HW-04 HW-06 HW-10 HW-16"),
    ("custom-fw/src/nam/*",              "HW-01 HW-02 HW-04 HW-06 HW-10 HW-16"),
    ("custom-fw/src/fx/*",               "HW-01 HW-02 HW-04 HW-06 HW-10 HW-16"),
    ("custom-fw/src/sys/preset.c",       "HW-01 HW-02 HW-07 HW-15 HW-20"),
    ("custom-fw/src/sys/settings.c",     "HW-01 HW-02 HW-07 HW-15"),
    ("custom-fw/src/sys/pa_user_nam.c",  "HW-01 HW-02 HW-06 HW-10 HW-15"),
    ("custom-fw/src/sys/sysex.c",        "HW-01 HW-02 HW-13 HW-19"),
    ("custom-fw/src/sys/pa_ota.c",       "HW-01 HW-02 HW-13 HW-14"),
    ("custom-fw/src/sys/pa_diag.c",      "HW-01 HW-02 HW-13"),
    ("custom-fw/src/sys/pa_audio_core.c","HW-01 HW-02 HW-03 HW-04 HW-05 HW-07 HW-10 HW-16"),
    ("custom-fw/src/sys/*",              "HW-01 HW-02 HW-03 HW-04"),
    ("custom-fw/src/*",                  "ALL"),
    ("custom-fw/include/*",              "HW-01 HW-02 HW-03 HW-04 HW-05 HW-06"),
    # host-only material never reaches the device
    ("custom-fw/emu/*",                  ""),
    ("custom-fw/wasm/*",                 ""),
    ("custom-fw/test/*",                 ""),
    ("custom-fw/tools/*",                ""),
    ("custom-fw/qa/*",                   ""),
    ("*.md",                             ""),
]

# The functional suites: (id, required, command, needs).
# {py}, {node}, {dll}, {wasm} are filled in at run time.
FUNCTIONAL = [
    ("F-01", True,  ["{py}", "custom-fw/emu/test_onyx_ui.py"], "dll"),
    ("F-02", True,  ["{py}", "custom-fw/emu/test_buttons.py", "{dll}"], "dll"),
    ("F-03", True,  ["{py}", "custom-fw/emu/test_editor.py", "{dll}"], "dll"),
    ("F-04", True,  ["{py}", "custom-fw/emu/test_input.py", "{dll}"], "dll"),
    ("F-05", True,  ["{py}", "custom-fw/emu/test_jack.py", "{dll}"], "dll"),
    ("F-06", True,  ["{py}", "custom-fw/emu/test_level.py", "{dll}"], "dll"),
    ("F-07", True,  ["{py}", "custom-fw/emu/test_ir.py"], "dll"),
    ("F-08", True,  ["{py}", "custom-fw/test/test_sysex_frame.py"], ""),
    ("F-09", True,  ["{py}", "custom-fw/test/test_sysex_read.py", "{dll}"], "dll"),
    ("F-10", True,  ["{py}", "custom-fw/test/test_ble_midi_emu.py", "{dll}"], "dll"),
    ("F-11", True,  ["{py}", "custom-fw/test/test_sysex_upload.py", "{dll}"], "dll"),
    ("F-12", True,  ["{py}", "custom-fw/test/test_groove_pixels.py"], "node"),
    # MIDI control over USB / BLE (CC, NRPN, save), driven through the real handler and the real tick; the map's proposals are not counted as implemented
    ("F-17", True,  ["{py}", "custom-fw/test/test_midi_control.py", "{dll}"], "dll"),
    # the web editor: its state record and name lists (cmd 80 / 81), the live cabinet upload, and the page's own protocol core (Node) against the firmware over USB and BLE-MIDI; the page's model tables equal ui_app.c's
    ("F-28", True,  ["{py}", "custom-fw/test/test_editor_protocol.py", "{dll}"], "dll"),
    ("F-29", True,  ["{py}", "custom-fw/test/test_editor_page.py", "{dll}"], "dll"),
    # the battery the stock way: SARADC channel 9, the stock capacity formula (0 below 528, 100 above 635), the charging flag, the warning levels
    ("F-30", True,  ["{py}", "custom-fw/test/test_battery.py", "{dll}"], "dll"),
    # the stored captures (16) and cabinets (15): upload, list, delete, overwrite, rename, the counts that follow, a reboot, the refusals, and nothing outside the user store written
    ("F-31", True,  ["{py}", "custom-fw/test/test_user_store.py", "{dll}"], "dll"),
    # the radio calibration is read from the unit's own stock record log (records 106 / 107 / 108 / 187) and equals what gen_ble_trims.py reads; damaged logs are refused
    # the USB audio mode (audio-interface direct monitoring): the output is the PC's audio only, the recording is the dry guitar, through the real audio core and USB audio driver
    ("F-33", True,  ["{py}", "custom-fw/test/test_usb_mode.py", "{dll}"], "dll"),
    # the pack number and the unique build code (shown on the unit, sent to the editor, kept out of the OTA ID block)
    # the firmware's half of the vendor MIDI update (one-click install / restore from the web editor): real packages, a simulated flash, a simulated host; nothing outside the five staging sectors is written, nothing is committed without "success"
    ("F-35", True,  ["{py}", "custom-fw/test/test_ota_session.py"], ""),
    # the Onyx release package: built from scratch, decodes, carries the build's app image, the Onyx loader, and no vendor loader or app code
    ("F-36", True,  ["{py}", "custom-fw/test/test_onyx_package.py"], ""),
    ("F-34", True,  ["{py}", "custom-fw/test/test_build_code.py"], ""),
    ("F-32", True,  ["{py}", "custom-fw/test/test_ble_trims.py", "{dll}"], "dll"),
    # every swap under live audio (40 preset changes, every model of every slot): no click above 4x the steady step. Pack 182: MOD / DLY / EQ type changes were not faded (up to 68x) and a cleared chorus / flanger / vibrato delay line started its wet path with a step (13x); a load-governor step to / from level 2 cut the cabinet's partitions with no fade (26x, pack 184)
    ("F-18", True,  ["{py}", "custom-fw/tools/swap_click_probe_emu.py", "--models", "--governor", "--limit", "4.0", "--dll", "{dll}"], "dll"),
    # any host frame count: no crackle from partial 64-sample blocks (emulator field report)
    ("F-13", True,  ["{py}", "custom-fw/emu/test_block_adapter.py", "{dll}"], "dll"),
    # delay / reverb end to end: echo times = the screen, stereo, dry level, tails, with a capture
    ("F-14", True,  ["{py}", "custom-fw/emu/test_dly_rev.py", "{dll}"], "dll"),
    # factory presets 1..40: generated, loaded, playing at level
    # the 40 presets stay level-matched on the real recordings they were calibrated on (skipped when those recordings are not on this machine)
    ("F-20", True,  ["{node}", "custom-fw/wasm/test_jack.js", "{wasm}"], "wasm"),
    ("F-21", True,  ["{node}", "custom-fw/wasm/test_chords.js", "{wasm}"], "wasm"),
    ("F-22", True,  ["{node}", "custom-fw/wasm/test_nam.js", "{wasm}"], "wasm"),
    ("F-23", True,  ["{node}", "custom-fw/wasm/test_nam_level.js", "{wasm}"], "wasm"),
    ("F-24", True,  ["{node}", "custom-fw/wasm/test_para.js", "{wasm}"], "wasm"),
    ("F-25", True,  ["{node}", "custom-fw/wasm/test_preset.js", "{wasm}"], "wasm"),
    ("F-26", True,  ["{node}", "custom-fw/wasm/test_sysex.js", "{wasm}"], "wasm"),
    # the drive on every kind of preset, through the real UI (a pool block reused without
    # zeroing made it silent after preset changes)
    ("F-27", True,  ["{node}", "custom-fw/wasm/test_preset_drive.js", "{wasm}"], "wasm"),
    ("F-37", True,  ["{py}", "custom-fw/emu/test_preset_bank.py", "{dll}"], "dll"),
    ("F-38", True,  ["{py}", "custom-fw/test/test_abkx_import.py"], ""),
    ("F-39", True,  ["{py}", "custom-fw/emu/test_swap_coalesce.py", "{dll}"], "dll"),
    ("F-40", True,  ["{py}", "custom-fw/emu/test_upload_ack.py", "{dll}"], "dll"),
    ("F-41", True,  ["{py}", "custom-fw/emu/test_usb_mode_swap.py", "{dll}"], "dll"),
    ("F-42", True,  ["{py}", "custom-fw/emu/test_trims_mirror.py", "{dll}"], "dll"),
    ("F-44", True,  ["{py}", "custom-fw/emu/test_settings_save.py", "{dll}"], "dll"),
    ("F-43", True,  ["{py}", "custom-fw/test/test_backup_doc.py"], ""),
]

# The host C suites build.ps1 does not run.
EXTRA_C_SUITES = [
    ("U-01", "custom-fw/test/test_ble_midi.c", ["custom-fw/src/board/ble_midi.c"]),
    ("U-02", "custom-fw/test/test_sysex_read.c", ["custom-fw/src/sys/sysex.c"]),
    # USB-MIDI end to end on the host (the real usb_ble.c / sysex.c / pa_diag.c / pa_ota.c / ble_midi.c): framing, replies, the pedal PC / CC table (stock module order), TX saturation.
    # The known defects (D2-D4 of the suite) are reported as KNOWN and do not fail it; any other failure does.
    ("U-16", "custom-fw/test/test_usb_midi_e2e.c",
     ["custom-fw/src/sys/sysex.c", "custom-fw/src/sys/pa_diag.c", "custom-fw/src/sys/pa_ota.c", "custom-fw/src/board/ble_midi.c", "custom-fw/test/comms_sysex_stubs.c"]),
    # the USB descriptor tables (include/pa_usb_desc.h): lengths, interfaces, interface associations, endpoints, links, the stock audio format
    ("U-17", "custom-fw/test/test_usb_desc.c", []),
    # the optimised WaveNet steps: fast must stay bit-exact with the baseline
    ("U-04", "custom-fw/test/test_wavenet_fast.c",
     ["custom-fw/src/nam/pa_wavenet.c", "custom-fw/src/dsp/hw_accel.c", "custom-fw/src/nam/pa_wavenet_fast.c", "-lm"]),
    # the float FFT cab prototype: impulse, direct convolution, and the firmware's Q18 cab path
    ("U-05", "custom-fw/bench/test_fft_conv.c",
     ["-Icustom-fw/bench", "custom-fw/bench/fft_conv.c", "custom-fw/src/dsp/hw_accel.c", "-lm"]),
    # every reverb type at high decay / high mix: tank headroom, tail smoothness, no DC,
    # Shimmer keeps sounding and shifts a clean octave up (field report 2026-10-22)
    ("U-06", "custom-fw/test/test_reverb_modes.c", ["custom-fw/src/fx/fx_reverb.c", "-lm"]),
    # the DYN block's noise gate: flat detection, unity when open, hysteresis, knob laws
    ("U-07", "custom-fw/test/test_gate.c", ["custom-fw/src/fx/fx_dynamics.c", "-lm"]),
    # DRV: LEVEL law, loudness match, GRIT range, TS clean path, RAT ceiling, aliasing
    ("U-08", "custom-fw/test/test_drive.c", ["custom-fw/src/fx/fx_drive.c", "-lm"]),
    # MOD: headroom, mix law, mono sum, phaser range, auto filter, RATE in Hz, "---" knobs
    ("U-09", "custom-fw/test/test_mod.c", ["custom-fw/src/fx/fx_mod.c", "-lm"]),
    # EQ: 5-BAND unchanged, LOW/HIGH CUT and PARAMETRIC-3 at the values the screen shows
    ("U-11", "custom-fw/test/test_eq.c", ["custom-fw/src/fx/fx_eq.c", "-lm"]),
    # DYN compressors (COMP+GATE, COMP): real ratio, attack/release, makeup, no ripple, THRESH
    ("U-12", "custom-fw/test/test_comp.c", ["custom-fw/src/fx/fx_dynamics.c", "-lm"]),
    # Groove Station: no voice distorted by the drum bus, no overs, no output clip under a -6 dBFS guitar
    ("U-13", "custom-fw/test/test_drum_clip.c", ["-lm"]),
    # the guitar + drums sum limiter: no output clip under a loud preset, no guitar latency, bit-exact when idle
    ("U-14", "custom-fw/test/test_mix_limiter.c", ["-lm"]),
    # the flash driver: its argument check equals pa_flash_rw_ok() for every address, nothing the stock owns is writable, and the boot guard's state machine
    ("U-22", "custom-fw/test/test_flash_guard.c", []),
    # the OVERLOAD popup's xrun-burst rule: a swap's own overrun (one each, measured) never raises it, a real burst does (pack 183)
    ("U-23", "custom-fw/test/test_overload_rule.c", []),
    # the firmware's own partitioned cabinet convolver (the register-accumulating multiply-accumulate, pack 185) against a direct time-domain convolution, 16 and 8 partitions, and through the governor's fade
    ("U-25", "custom-fw/test/test_wild.c", ["-lm"]),   # the wild-value checks that guard the drive output and the final limiter
    ("U-24", "custom-fw/test/test_fft_conv_mac.c", ["custom-fw/src/fx/fft_conv.c", "-lm"]),
    # the drum kit's stored forms: mu-law, IMA-ADPCM, the kick/tom synthesisers, block independence
    ("U-15", "custom-fw/test/test_drum_codecs.c", ["-lm"]),
]

# Python suites for the tools that touch a device's flash.
PY_SUITES = [
    ("U-03", "custom-fw/test/test_safe_flash.py"),   # jl_safe_flash.py: every anti-brick rule
    ("U-21", "custom-fw/test/test_flash_ram_readonly.py"),   # the flash controller probe sends only read commands and is entered at the probe's own address
    # the drum kit, rhythms, default cab and emulator test files are the project's own
    ("U-10", "custom-fw/test/test_provenance.py"),
    # the stock AM4 amps / cabs: converter bit-exact on AM3, biases present, models run, IRs whole
    # the PA-EXT layout (MIDI-MAP-CUSTOM.md 4.3): its 13 bytes are zero in every stock record we have, and the extension round-trips without touching another byte
    ("U-18", "custom-fw/tools/midi_map_check.py"),
    # the generated tables equal their sources: the factory presets (src/sys/factory_presets.inc) and the stock amps / cabs (src/sys/stock_assets.c)
]


# ======================================================================================
class Gate:
    def __init__(self, args):
        self.args = args
        self.results: list[tuple[str, str, str, str]] = []   # (id, status, title, detail)
        self.facts: dict[str, object] = {}

    def add(self, cid, status, title, detail=""):
        self.results.append((cid, status, title, detail.strip()))
        mark = {"PASS": "ok  ", "FAIL": "FAIL", "WARN": "warn", "SKIP": "skip", "INFO": "info"}[status]
        print(f"  [{mark}] {cid:6} {title}")
        if detail and status in ("FAIL", "WARN"):
            for line in detail.strip().splitlines()[:12]:
                print(f"              {line}")

    def status(self, prefix=""):
        return [r for r in self.results if r[0].startswith(prefix)]


def run(cmd, cwd=ROOT, timeout=1800, env=None):
    try:
        p = subprocess.run(cmd, cwd=str(cwd), capture_output=True, timeout=timeout, env=env)
        out = (p.stdout + p.stderr).decode("utf-8", errors="replace")
        return p.returncode, out
    except FileNotFoundError as e:
        return 127, str(e)
    except subprocess.TimeoutExpired:
        return 124, f"timed out after {timeout} s"


def tail(text, n=15):
    return "\n".join(text.strip().splitlines()[-n:])


def sha256(path: Path) -> str:
    return hashlib.sha256(path.read_bytes()).hexdigest()


def entropy(b: bytes) -> float:
    if not b:
        return 0.0
    n = len(b)
    return -sum(c / n * math.log2(c / n) for c in Counter(b).values())


def read_json(path: Path, default):
    try:
        return json.loads(path.read_text(encoding="utf-8"))
    except (OSError, ValueError):
        return default


def c_defines(path: Path) -> dict[str, int]:
    """#define NAME 0x... / decimal values (with optional u suffix) from a header."""
    out = {}
    if not path.exists():
        return out
    for m in re.finditer(r"^\s*#define\s+(\w+)\s+\(?\s*(0x[0-9A-Fa-f]+|\d+)u?\s*\)?", path.read_text(errors="replace"), re.M):
        out[m.group(1)] = int(m.group(2), 0)
    return out


def ps1_value(var: str) -> str | None:
    text = (FW / "build.ps1").read_text(errors="replace")
    m = re.search(r"^\s*\$" + re.escape(var) + r"\s*=\s*\"([^\"]+)\"", text, re.M)
    return m.group(1) if m else None


def toolchain_bin() -> Path | None:
    cands = [
        os.environ.get("JIELI_PI32", ""),
        os.environ.get("PI32_ROOT", ""),
        str(Path.home() / "tools" / "jieli" / "C$" / "JL" / "pi32"),
        ps1_value("pi32Root") or "",
        r"C:\JL\pi32"
    ]
    for c in cands:
        if c and (Path(c) / "bin" / "llvm-objcopy.exe").exists():
            return Path(c) / "bin"
    return None


# ======================================================================================
# Stage S - static checks on the tree (no build needed)
def stage_static(g: Gate):
    print("\n== S: static checks ==")
    rc, out = run([sys.executable, "custom-fw/tools/check_sources.py"])
    g.add("S-01", "PASS" if rc == 0 else "FAIL", "source manifest matches every build target", "" if rc == 0 else tail(out))

    # S-02: everything the device build needs is in git (a release must rebuild from git)
    rc, out = run(["git", "ls-files", "--others", "--exclude-standard", "--",
                   "custom-fw/src", "custom-fw/include", "custom-fw/sources.txt", "custom-fw/app.ld",
                   "custom-fw/build.ps1", "custom-fw/tools", "custom-fw/test"])
    untracked = [l for l in out.splitlines() if l.strip() and "__pycache__" not in l] if rc == 0 else []
    if rc != 0:
        g.add("S-02", "WARN", "build inputs tracked in git", "git not available: " + tail(out, 3))
    elif untracked:
        g.add("S-02", "FAIL" if g.args.release else "WARN",
              f"build inputs tracked in git ({len(untracked)} untracked)", "\n".join(untracked))
    else:
        g.add("S-02", "PASS", "build inputs tracked in git")

    # S-03: no huge files tracked, none waiting to be added by accident
    big_tracked, big_loose = [], []
    rc1, tracked = run(["git", "ls-files"])
    rc2, loose = run(["git", "ls-files", "--others", "--exclude-standard"])
    for lst, rcx, text in ((big_tracked, rc1, tracked), (big_loose, rc2, loose)):
        if rcx != 0:
            continue
        for rel in text.splitlines():
            p = ROOT / rel
            if p.is_file() and p.stat().st_size > 50 * 1024 * 1024:
                lst.append(f"{rel} ({p.stat().st_size // (1024 * 1024)} MB)")
    if big_tracked:
        g.add("S-03", "FAIL", "no file over 50 MB in git", "\n".join(big_tracked))
    elif big_loose:
        g.add("S-03", "WARN", "no file over 50 MB waiting to be committed (add them to .gitignore)", "\n".join(big_loose))
    else:
        g.add("S-03", "PASS", "no file over 50 MB in or near git")

    # S-04: hard-coded personal paths in the build tooling
    hits = []
    for p in [FW / "build.ps1", FW / "Makefile", *sorted((FW / "tools").glob("*.py"))]:
        if p.name == "qa_gate.py" or not p.exists():
            continue
        for i, line in enumerate(p.read_text(errors="replace").splitlines(), 1):
            if re.search(r"C:\\\\?Users\\\\?\w+", line) and not line.lstrip().startswith("#"):
                hits.append(f"{p.relative_to(ROOT)}:{i}: {line.strip()[:100]}")
    g.add("S-04", "WARN" if hits else "PASS", "no hard-coded personal paths in build tooling", "\n".join(hits))

    # S-05: no two peripherals share a base address (the WDT/Timer4 bug class)
    bases = {}
    for hdr in [*(FW / "include").glob("*.h"), *(FW / "include" / "hal").glob("*.h")]:
        for name, val in c_defines(hdr).items():
            if name.endswith("_BASE") and name.startswith(("JL_", "PA_")) and val not in (0, 0xFFFFFFFF):
                bases.setdefault(val, set()).add(name)
    dup = {v: n for v, n in bases.items() if len(n) > 1}
    hw = c_defines(FW / "include" / "pa_hw.h")
    wdt, t4 = hw.get("JL_WDT_BASE"), hw.get("JL_TIMER4_BASE", 0x00010800)
    msgs = [f"{v:#010x}: {', '.join(sorted(n))}" for v, n in dup.items()]
    if wdt is not None and wdt == t4:
        msgs.append(f"JL_WDT_BASE == JL_TIMER4_BASE ({wdt:#x}): feeding the watchdog stops the system timer")
    g.add("S-05", "FAIL" if msgs else "PASS", "no peripheral register base defined twice", "\n".join(msgs))
    if wdt == 0xFFFFFFFF:
        g.add("S-05b", "INFO", "watchdog address still unknown (feed is a no-op): the device has no hang protection")

    # S-06: the profiling pin is off unless explicitly asked for
    wn = (FW / "src" / "nam" / "pa_wavenet.c").read_text(errors="replace")
    pin_guarded = re.search(r"#if[^\n]*defined\(PA_PROFILE_PIN\)[^\n]*\n(?:[^\n]*\n){0,3}?#define PA_DBG_PIN_INIT\(\)\s+do", wn)
    build_defines = "PA_PROFILE_PIN" in (FW / "build.ps1").read_text(errors="replace")
    if not pin_guarded:
        g.add("S-06", "FAIL", "PB0 profiling pin is behind PA_PROFILE_PIN", "pa_wavenet.c drives PB0 in normal builds")
    elif build_defines:
        g.add("S-06", "FAIL", "PB0 profiling pin is behind PA_PROFILE_PIN", "build.ps1 defines PA_PROFILE_PIN (release builds must not)")
    else:
        g.add("S-06", "PASS", "PB0 profiling pin is behind PA_PROFILE_PIN and off in release")

    # S-07: busy-waits on hardware registers must be bounded
    allow = set()
    if SPIN_ALLOW.exists():
        allow = {l.strip() for l in SPIN_ALLOW.read_text().splitlines() if l.strip() and not l.startswith("#")}
    spins = []
    for p in sorted((FW / "src").rglob("*.c")):
        lines = p.read_text(errors="replace").splitlines()
        for i, line in enumerate(lines):
            if re.search(r"\bwhile\s*\(\s*0\s*\)", line) or line.lstrip().startswith(("#", "//", "*")):
                continue   # do { } while (0) macros, preprocessor lines and comments
            if re.search(r"\bwhile\s*\(", line) and re.search(r"PA_READ|reg\[|JL_\w+|volatile", line):
                window = "\n".join(lines[i:i + 8])
                if re.search(r"spin|LIMIT|timeout|budget|tries|--\w|\w--", window, re.I):
                    continue
                key = f"{p.relative_to(FW).as_posix()}::{line.strip()}"
                if key in allow:
                    continue
                spins.append(f"{p.relative_to(FW).as_posix()}:{i + 1}: {line.strip()[:90]}")
    g.add("S-07", "WARN" if spins else "PASS",
          f"hardware busy-waits are bounded ({len(spins)} unbounded, review or allowlist in qa/spin_allowlist.txt)",
          "\n".join(spins))

    # S-08 / S-09: flash map vs the chip, and the write fence
    fm = c_defines(FW / "include" / "pa_flash_map.h")
    ledger = read_json(LEDGER, {})
    cap = (ledger.get("device") or {}).get("flash_size_bytes") or STOCK_FLASH_SIZE_DEFAULT
    cap_src = "ledger" if (ledger.get("device") or {}).get("flash_size_bytes") else "stock isd_config (FLASH_SIZE=1M), chip not read yet"
    chip = fm.get("PA_FLASH_SIZE", 0)
    windows = [(fm.get("PA_FLASH_RW3_START", 0), fm.get("PA_FLASH_RW3_END", 0)), (fm.get("PA_FLASH_OTA_START", 0), fm.get("PA_FLASH_OTA_END", 0))]        # since pack 194 the firmware writes ONE window; the stock's VM, presets and save record are never written
    # the partitions the firmware stores things in: (start, size). The settings and the registry are written in place (A and B are one sector), so only
    # distinct partitions are tested against each other.
    parts = {
        "preset bank A": (fm.get("PA_FLASH_STORE_BANK_A_ADDR", 0), fm.get("PA_FLASH_STORE_BANK_SIZE", 0)),
        "preset bank B": (fm.get("PA_FLASH_STORE_BANK_B_ADDR", 0), fm.get("PA_FLASH_STORE_BANK_SIZE", 0)),
        "settings": (fm.get("PA_FLASH_SYS_SETTINGS_ADDR", 0), fm.get("PA_FLASH_SYS_SETTINGS_SIZE", 0)),
        "NAM registry": (fm.get("PA_FLASH_USER_NAM_META_A_ADDR", 0), fm.get("PA_FLASH_USER_NAM_META_SIZE", 0)),
        "Bluetooth bond log": (fm.get("PA_FLASH_BOND_ADDR", 0), fm.get("PA_FLASH_BOND_SIZE", 0)),
        "radio calibration mirror": (fm.get("PA_FLASH_TRIMS_MIRROR_ADDR", 0), fm.get("PA_FLASH_TRIMS_MIRROR_SIZE", 0)),
        "user NAM blob slots": (fm.get("PA_FLASH_USER_NAM_STORE_ADDR", 0), fm.get("PA_FLASH_USER_NAM_SLOT_SIZE", 0) * fm.get("PA_FLASH_USER_NAM_BLOB_SLOTS", 0)),
        "cabinet registry A": (fm.get("PA_FLASH_CAB_REG_A_ADDR", 0), fm.get("PA_FLASH_CAB_SLOT_SIZE", 0)),
        "cabinet registry B": (fm.get("PA_FLASH_CAB_REG_B_ADDR", 0), fm.get("PA_FLASH_CAB_SLOT_SIZE", 0)),
        "user cabinets": (fm.get("PA_FLASH_CAB_ADDR", 0), fm.get("PA_FLASH_CAB_SLOT_SIZE", 0) * fm.get("PA_FLASH_CAB_SLOTS", 0)),
    }
    over = []
    if chip != cap:
        over.append(f"the map's chip size {chip:#x} is not the chip's {cap:#x}")
    for k, (a, n) in parts.items():
        if n == 0 or a + n > cap:
            over.append(f"{k} [{a:#x}, {a + n:#x}) is empty or beyond the chip ({cap:#x})")
        elif not any(lo <= a and a + n <= hi for lo, hi in windows):
            over.append(f"{k} [{a:#x}, {a + n:#x}) is not inside a rewritable window")
    names = list(parts)
    for i in range(len(names)):
        for j in range(i + 1, len(names)):
            (a, n), (b, m) = parts[names[i]], parts[names[j]]
            if a < b + m and b < a + n:
                over.append(f"{names[i]} and {names[j]} overlap")
    nor = (FW / "src" / "board" / "spi_nor.c").read_text(errors="replace")
    device_part = nor.split("Bare-metal", 1)[-1]
    writes_stubbed = bool(re.search(r"pa_spi_nor_write\s*\([^)]*\)\s*\{(?:(?!\n\}).)*return -2;", device_part, re.S)) and \
                     bool(re.search(r"pa_spi_nor_erase_sector\s*\([^)]*\)\s*\{(?:(?!\n\}).)*return -2;", device_part, re.S))
    g.facts["flash_writes_stubbed"] = writes_stubbed
    if over and not writes_stubbed:
        g.add("S-08", "FAIL", f"every partition fits the flash chip ({cap_src})",
              "flash writes are WIRED and these partitions wrap onto the bootloader:\n" + "\n".join(over))
    elif over:
        g.add("S-08", "WARN", f"every partition fits the flash chip ({cap_src})",
              "latent (device writes are stubbed today):\n" + "\n".join(over))
    else:
        g.add("S-08", "PASS", f"every partition fits the flash chip ({cap_src})")

    # the rewritable windows must keep clear of everything that must never be written: the boot record, the bootloader and the app area (below the first window),
    # the radio and factory calibration (BTIF, USER) and the factory amp/cab bundle
    app_end = fm.get("PA_FLASH_IMAGE_END", fm.get("PA_FLASH_APP_WINDOW_END", 0)) + 1       # the app image only: the user store (0x86000..) sits above it
    prot = [(0, app_end, "boot record, bootloader and app area"),
            (fm.get("PA_FLASH_BTIF_ADDR", 0), fm.get("PA_FLASH_FACTORY_CAB_ADDR", 0) + fm.get("PA_FLASH_FACTORY_CAB_SIZE", 0), "BTIF, USER and the factory bundle"),
            (fm.get("PA_FLASH_STOCK_LOG_ADDR", 0), fm.get("PA_FLASH_STOCK_LOG_ADDR", 0) + 0x1000, "the stock firmware's record log"),
            (fm.get("PA_FLASH_STOCK_SAVE_ADDR", 0), fm.get("PA_FLASH_STOCK_SAVE_ADDR", 0) + 0x1000, "the stock firmware's save record")]
    guards = len(re.findall(r"pa_flash_rw_ok\s*\(", device_part))
    hard, soft = [], []
    for lo, hi in windows:
        if lo >= hi:
            hard.append(f"a rewritable window [{lo:#x}, {hi:#x}) is empty")
        for plo, phi, what in prot:
            if lo < phi and plo < hi:
                hard.append(f"the rewritable window [{lo:#x}, {hi:#x}) overlaps {what} [{plo:#x}, {phi:#x})")
    if guards < 2:
        hard.append("the device erase/write functions do not both check pa_flash_rw_ok()")
    read_fn = re.search(r"int\s+pa_spi_nor_read\s*\([^)]*\)\s*\{(?:(?!\n\}).)*", device_part, re.S)
    if not (read_fn and re.search(r"len\s*>|\+\s*len|_END|_SIZE", read_fn.group(0))):
        soft.append("the device pa_spi_nor_read() has no address/length bounds check "
                    "(any address, including from the diagnostic SysEx, is dereferenced)")
    g.add("S-09", "FAIL" if hard else ("WARN" if soft else "PASS"),
          "flash write fence protects MBR, uboot, app, calibration and factory data; reads are bounded",
          "\n".join(hard + soft))
    g.add("S-10", "INFO", "device flash writes are " + ("stubbed (return -2): saves fail safely" if writes_stubbed else "WIRED"))

    # S-12: the flash driver is wired the way it was proven, and every saver goes through the fence. Each line is something that broke or nearly broke on the way (packs 140-172).
    wiring = []
    def src_of(rel):
        return (FW / rel).read_text(errors="replace")
    spi_dev = nor.split("Bare-metal", 1)[-1]
    for fn_name in ("pa_spi_nor_erase_sector", "pa_spi_nor_write", "pa_spi_nor_read"):
        body = re.search(fn_name + r"\s*\([^)]*\)\s*\{(?:(?!\n\}).)*", spi_dev, re.S)
        if not body or "pa_flash_dev_op" not in body.group(0):
            wiring.append(f"the device {fn_name}() does not go through the RAM driver (pa_flash_dev_op)")
    for fn_name in ("pa_spi_nor_erase_sector", "pa_spi_nor_write"):
        body = re.search(fn_name + r"\s*\([^)]*\)\s*\{(?:(?!\n\}).)*", spi_dev, re.S)
        if body and ("pa_spi_nor_begin" not in body.group(0) or "plat_battery_is_brownout" not in body.group(0)):
            wiring.append(f"the device {fn_name}() must quiesce (pa_spi_nor_begin) and honour the brownout lock")
    if "pa_spi_nor_begin" not in src_of("src/sys/preset.c"):
        wiring.append("the preset bank commit is not bracketed by pa_spi_nor_begin / end (4 erases + 58 programs must be ONE quiesce)")
    if "pa_flash_shared_rewrite" not in src_of("src/sys/settings.c") or "pa_flash_shared_rewrite" not in src_of("src/sys/pa_user_nam.c"):
        wiring.append("the settings log and the NAM registry share a sector: both must save through pa_flash_shared_rewrite")
    # the Bluetooth bond (pack 189): stashed from the radio's interrupt, written by the main loop, restored at start-up, and kept through a sector rewrite
    rb = src_of("src/board/ble_radio.c"); st_c = src_of("src/sys/settings.c")
    if "pa_bond_stash(rand, ediv, ltk)" not in rb or "ble_host_set_key(b.rand, b.ediv, b.ltk)" not in rb:
        wiring.append("the Bluetooth bond is not wired: ble_app_bond must stash the keys (pa_bond_stash) and pa_ble_radio_restore_bond must put the stored ones back (ble_host_set_key)")
    if "pa_bond_poll(" not in src_of("src/board/main.c") or "pa_ble_radio_restore_bond()" not in src_of("src/board/main.c"):
        wiring.append("main.c must call pa_bond_poll in the main loop (never write the flash from the radio interrupt) and pa_ble_radio_restore_bond after the settings are loaded")
    if "pa_bond_snapshot(" not in st_c or "PA_FLASH_BOND_ADDR" not in st_c:
        wiring.append("pa_flash_shared_rewrite must put the newest Bluetooth bond back after the erase (the bond log shares the sector)")
    ub = src_of("src/board/usb_ble.c")
    if "PA_BLE_REPLY(reply, n)" not in ub:
        wiring.append("the OTA identity reply (check_ota_special_sysex) must also go out over BLE (PA_BLE_REPLY): it answered on USB only, so a BLE central never got it")
    if "pa_ui_midi_control(ui, chain, &ev)" not in src_of("src/sys/pa_audio_core.c") or "midi_ev_push" not in ub:
        wiring.append("the MIDI control events (usb_ble.c decoder -> pa_audio_core.c tick -> pa_ui_midi_control) are not wired")
    mainc = src_of("src/board/main.c")
    if "g_pa_irq_on = 1u" not in mainc or "pa_flash_dev_boot_done" not in mainc:
        wiring.append("main.c must set g_pa_irq_on and call pa_flash_dev_boot_done when the main loop starts (the quiesce and the boot guard depend on it)")
    ff = src_of("src/board/flash_ram.c")
    if "pa_fld_args_ok(" not in ff or "pa_flash_guard_blocked(" not in ff:
        wiring.append("flash_ram.c does not use the checked argument test / boot guard of pa_flash_guard.h")
    if '"flashfn.bin"' not in (FW / "build.ps1").read_text(errors="replace"):
        wiring.append("build.ps1 does not pack the .flashfn section: the RAM driver would be zeros in the image (packs 140-167)")
    g.add("S-12", "FAIL" if wiring else "PASS", "the flash driver is wired through the fence: RAM driver, quiesce, brownout lock, one bracket per bank, shared sector, boot guard, packed section",
          "\n".join(wiring))

    # S-13: our window overlaps nothing the stock keeps live data in: in the FACTORY dump of the unit every sector of the window is either blank or part of the stock's app area (0x4000..0xB4FFF), which a firmware
    # update restores whole; the stock's VM, its saved presets and its save record are never inside it
    ref = QA / "backups" / "unit1_v105" / "dump1.bin"
    if ref.exists():
        dump = ref.read_bytes()
        approved = [(0x4000, 0xB5000)]
        live = []
        for lo, hi in windows:
            for a in range(lo, hi, 0x1000):
                sec = dump[a:a + 0x1000]
                if sec and any(b != 0xFF for b in sec) and not any(x <= a < y for x, y in approved):
                    live.append(f"{a:#x} holds data in the factory dump ({sum(1 for b in sec if b != 0xFF)} bytes) and is not part of the stock's app area")
        g.add("S-13", "FAIL" if live else "PASS", "every sector the device may rewrite is blank or the stock's restorable app area in the factory dump: the stock's settings, presets and calibration are never in the window", "\n".join(live))
    else:
        g.add("S-13", "WARN", "reference dump of the unit not found: cannot check the rewrite windows against the stock's live data", str(ref))

    # S-11: the SRAM window that holds core 0 code (app.ld, region RAMFN) is not used by any address written into the sources. The first try at running code from SRAM
    # (pack 100, 2026-11-02) put it where the UI's text buffer lives (0x01C00400) and the unit never finished booting.
    ld = (ROOT / "custom-fw" / "app.ld").read_text(encoding="utf-8", errors="replace")
    mm = re.search(r"RAMFN\s*\(rwx\)\s*:\s*ORIGIN\s*=\s*(0x[0-9A-Fa-f]+)\s*,\s*LENGTH\s*=\s*(0x[0-9A-Fa-f]+)", ld)
    if not mm:
        g.add("S-11", "INFO", "no RAMFN region in app.ld (no code runs from SRAM)")
    else:
        lo, hi = int(mm.group(1), 16), int(mm.group(1), 16) + int(mm.group(2), 16)
        clash = []
        for sub, pats in (("src", ("*.c", "*.h", "*.S")), ("include", ("*.h",))):
            for pat in pats:
                for f in (ROOT / "custom-fw" / sub).rglob(pat):
                    try:
                        text = f.read_text(encoding="utf-8", errors="replace")
                    except OSError:
                        continue
                    in_comment = False
                    for ln, raw in enumerate(text.split("\n"), 1):
                        if "sram-window-ok" in raw:                        # a line that says it is compiled out / harmless
                            continue
                        code = raw
                        if in_comment:                                     # the inside of a block comment is not code
                            if "*/" not in code:
                                continue
                            code = code.split("*/", 1)[1]
                            in_comment = False
                        code = re.sub(r"/\*.*?\*/", "", code)
                        if "/*" in code:
                            code = code.split("/*", 1)[0]
                            in_comment = True
                        code = code.split("//", 1)[0]
                        for m2 in re.finditer(r"0x01[Cc]0[0-9A-Fa-f]{4}", code):
                            a = int(m2.group(0), 16)
                            if lo <= a < hi:
                                clash.append(f"{f.relative_to(ROOT).as_posix()}:{ln}: {m2.group(0)}")
        g.add("S-11", "FAIL" if clash else "PASS",
              f"no source uses an address inside the SRAM code window {lo:#x}-{hi:#x}", "\n".join(clash[:12]))


# ======================================================================================
# Stage B - the build (host suites, cross-compile, link asserts, effect matrix, package)
def stage_build(g: Gate):
    print("\n== B: build.ps1 (host suites, pi32 build, effect matrix, packaging) ==")
    if g.args.skip_build:
        g.add("B-01", "SKIP", "build.ps1 (skipped with --skip-build; artifacts must be fresh, see I-01)")
        return
    rc, out = run(["powershell", "-NoProfile", "-ExecutionPolicy", "Bypass", "-File", "custom-fw/build.ps1"], timeout=5400)
    (QA / "reports").mkdir(parents=True, exist_ok=True)
    (QA / "reports" / "last_build.log").write_text(out, encoding="utf-8")
    g.add("B-01", "PASS" if rc == 0 else "FAIL", "build.ps1 completes (all host suites, link, effect matrix, packaging)",
          "" if rc == 0 else tail(out, 20) + "\n(full log: custom-fw/qa/reports/last_build.log)")


# Stage U - host C suites build.ps1 does not run
def stage_unit(g: Gate):
    print("\n== U: extra host suites ==")
    cc = shutil.which("clang") or shutil.which("cc")
    if not cc:
        for cid, src, _ in EXTRA_C_SUITES:
            g.add(cid, "FAIL", f"{Path(src).name}", "no host C compiler (clang) on PATH")
        return
    tmp = Path(tempfile.mkdtemp(prefix="pa_qa_"))
    for cid, src, deps in EXTRA_C_SUITES:
        exe = tmp / (Path(src).stem + ".exe")
        rc, out = run([cc, "-O1", "-Wall", "-Icustom-fw/include", "-DPA_HOST_SIM=1", src, *deps, "-o", str(exe)])
        if rc != 0:
            g.add(cid, "FAIL", f"{Path(src).name} compiles", tail(out))
            continue
        rc, out = run([str(exe)], cwd=tmp, timeout=300)
        g.add(cid, "PASS" if rc == 0 else "FAIL", f"{Path(src).name}", "" if rc == 0 else tail(out))
    shutil.rmtree(tmp, ignore_errors=True)
    # Python suites for the flashing/recovery tools: these guard the anti-brick rules.
    for cid, script, *args in PY_SUITES:
        rc, out = run([sys.executable, script, *args], timeout=900)
        g.add(cid, "PASS" if rc == 0 else "FAIL", " ".join([Path(script).name, *args]), "" if rc == 0 else tail(out))


# Stage F - functional regression on the emulator DLL and the WebAssembly build
def stage_functional(g: Gate):
    print("\n== F: functional regression (emulator DLL + WebAssembly) ==")
    dll = FW / "emu" / "pocket_amp_emu.dll"
    node = shutil.which("node")
    wasm = FW / "wasm" / "out" / "pocket_amp.wasm"
    if g.args.skip_build:
        pass
    else:
        rc, out = run([sys.executable, "custom-fw/wasm/build_wasm.py"], timeout=1800)
        g.add("F-00", "PASS" if rc == 0 else "FAIL", "WebAssembly build of the firmware core", "" if rc == 0 else tail(out))
    have = {"dll": dll.exists(), "wasm": wasm.exists() and node is not None, "node": node is not None, "": True}
    for cid, required, cmd, need in FUNCTIONAL:
        name = Path(cmd[1]).name
        if not have.get(need, False):
            g.add(cid, "FAIL" if required else "SKIP", name, f"missing prerequisite: {need}")
            continue
        argv = [c.format(py=sys.executable, node=node or "node", dll=str(dll), wasm=str(wasm)) for c in cmd]
        rc, out = run(argv, timeout=900)
        if rc == 0:
            g.add(cid, "PASS", name)
        elif rc == 3:          # the suite's own "my inputs are not here": a skip, never a pass
            g.add(cid, "SKIP", name, tail(out, 3))
        else:
            g.add(cid, "FAIL" if required else "WARN", name, f"exit {rc}\n" + tail(out))


# ======================================================================================
# Stage I - the image: is this exact file safe to put on a device?
def stage_image(g: Gate):
    print("\n== I: image checks (safe-to-flash) ==")
    app = FW / "build_app.bin"
    fwsc = FW / "Pocket_AMP_Custom.fwsc"
    elf = FW / "build_pi32" / "pocket_amp_custom.elf"
    stock_path = Path(os.environ.get("POCKET_AMP_STOCK_FWSC") or os.environ.get("STOCK_FWSC") or g.args.stock or ps1_value("TemplateFwsc") or (Path.home() / "Downloads" / "Pocket AMP.fwsc"))
    missing = [p.name for p in (app, fwsc, elf) if not p.exists()]
    if missing:
        g.add("I-01", "FAIL", "build artifacts exist", "missing: " + ", ".join(missing))
        return
    # I-01 freshness
    inputs = [*(FW / "src").rglob("*"), *(FW / "include").rglob("*"), FW / "app.ld", TOOLS / "pack_ufw.py"]
    newest = max((p for p in inputs if p.is_file()), key=lambda p: p.stat().st_mtime)
    stale = fwsc.stat().st_mtime < newest.stat().st_mtime or app.stat().st_mtime < newest.stat().st_mtime
    g.add("I-01", "FAIL" if stale else "PASS", "artifacts are newer than every source",
          f"{newest.relative_to(ROOT)} changed after the image was built - rebuild" if stale else "")

    app_b, fwsc_b = app.read_bytes(), fwsc.read_bytes()
    g.facts["sha256"] = hashlib.sha256(fwsc_b).hexdigest()
    g.facts["app_sha256"] = hashlib.sha256(app_b).hexdigest()
    g.facts["app_size"] = len(app_b)

    # I-02 size
    free = APP_SLOT_MAX - len(app_b)
    g.add("I-02", "PASS" if free >= 0 else "FAIL", f"app fits its partition ({len(app_b):,} B, {free:,} B free)")
    if 0 <= free < 8192:
        g.add("I-02b", "WARN", "less than 8 KB left in the app partition")

    # I-03 the bin is this ELF (no stale packaging)
    tbin = toolchain_bin()
    if tbin:
        tmp = Path(tempfile.mkdtemp(prefix="pa_qa_"))
        ok = True
        parts = {}
        for sec in (".text", ".ram_text", ".c1_text", ".ramfn", ".flashfn", ".data"):
            outp = tmp / (sec.strip(".") + ".bin")
            rc, out = run([str(tbin / "llvm-objcopy.exe"), "-O=binary", f"-j={sec}", str(elf), str(outp)])
            ok &= rc == 0
            parts[sec] = outp.read_bytes() if outp.exists() else b""
        rc, nm_out = run([str(tbin / "llvm-nm.exe"), str(elf)])
        lma = {}
        for line in nm_out.splitlines():
            f = line.split()
            if len(f) == 3 and f[2] in ("_rt_load", "_c1_load", "_rf_load", "_ff_load", "_data_load", "_data_start", "_data_end"):
                lma[f[2]] = int(f[0], 16)
        XIP = 0x02000120
        same = ok and len(lma) == 7
        detail = "" if same else "build_app.bin does not come from build_pi32/pocket_amp_custom.elf"
        if same:
            # the image is laid out by the linker's LOAD addresses: the startup code copies .ram_text and .data from _rt_load / _data_load.
            # (It was .text + .data only once: the 48 bytes of RAM code between them were missing, so .data was copied 48 bytes late and every
            # initialised variable - the stock amp and cabinet tables - arrived shifted.)
            exp = bytearray(parts[".text"])
            for sec, sym in ((".ram_text", "_rt_load"), (".c1_text", "_c1_load"), (".ramfn", "_rf_load"), (".flashfn", "_ff_load"), (".data", "_data_load")):
                at = lma[sym] - XIP
                if at < len(exp) and parts[sec]:
                    same = False; detail = f"{sec} load address {lma[sym]:#x} overlaps the code"
                exp.extend(bytes(max(0, at - len(exp))))
                exp[at:at + len(parts[sec])] = parts[sec]
            if same and lma["_data_end"] - lma["_data_start"] != len(parts[".data"]):
                same = False; detail = ".data is not the size the startup code copies"
            if same and bytes(exp) != app_b:
                same = False; detail = "build_app.bin differs from the image laid out by the ELF's load addresses"
        g.add("I-03", "PASS" if same else "FAIL", "build_app.bin is the ELF laid out by its load addresses (.text, .ram_text, .c1_text, .ramfn, .flashfn, .data)", detail)
        # I-08/I-09 symbols
        rc, out = run([str(tbin / "llvm-nm.exe"), str(elf)])
        syms = {}
        for line in out.splitlines():
            f = line.split()
            if len(f) == 3:
                try:
                    syms[f[2]] = int(f[0], 16)
                except ValueError:
                    pass
        start = syms.get("_start")
        g.facts["start"] = start
        need = ["_start", "pa_firmware_init", "pa_main_loop", "isr_alnk0", "pa_alnk0_isr_c",
                "_bss_start", "_bss_end", "_data_load", "_sstack_top", "_ustack_top"]
        miss = [s for s in need if s not in syms]
        g.add("I-09", "FAIL" if miss else "PASS", "startup, ISR and section symbols present", ", ".join(miss))
        if start is None:
            g.add("I-08", "FAIL", "_start sits at the entry address", "no _start symbol")
        else:
            g.add("I-08", "PASS" if start == ENTRY else "FAIL",
                  f"_start is at the bootloader's entry {ENTRY:#x} (found {start:#x})")
        # I-10 section budget vs baseline
        rc, out = run([str(tbin / "llvm-objdump.exe"), "-h", str(elf)])
        sizes = {}
        for line in out.splitlines():
            f = line.split()
            if len(f) >= 3 and f[1] in (".text", ".data", ".bss", ".pool", ".ram_text", ".c1_text"):
                sizes[f[1]] = int(f[2], 16)
        g.facts["sections"] = sizes
        base = read_json(BASELINE, {}).get("sections", {})
        deltas = []
        for k, v in sizes.items():
            b = base.get(k)
            if b and abs(v - b) > max(4096, b * 0.05):
                deltas.append(f"{k}: {b:,} -> {v:,} B ({v - b:+,})")
        msg = ", ".join(f"{k} {v:,}" for k, v in sizes.items())
        if not base:
            g.add("I-10", "WARN", f"section sizes within 5 % of baseline ({msg})",
                  "no baseline recorded yet - run once with --update-baseline")
        else:
            g.add("I-10", "WARN" if deltas else "PASS", f"section sizes within 5 % of baseline ({msg})", "\n".join(deltas))
        shutil.rmtree(tmp, ignore_errors=True)
    else:
        g.add("I-03", "FAIL", "build_app.bin matches the ELF", "JieLi pi32 toolchain not found (set JIELI_PI32)")

    # I-04 container integrity
    import contextlib, io
    buf = io.StringIO()
    with contextlib.redirect_stdout(buf):
        ok = P.verify_package(fwsc_b)
    g.add("I-04", "PASS" if ok else "FAIL", "container: product marker, header, table and entry CRCs", "" if ok else buf.getvalue())

    import struct
    import jl_app_area as A
    c_ufw, _ = P.unwrap_fwsc(fwsc_b)
    c_off, c_size = P.flash_entry(c_ufw)
    c_flash = c_ufw[c_off:c_off + c_size]

    # I-05 against optional local stock template (skipped in vendor-free clean builds)
    if stock_path.is_file():
        stock_b = stock_path.read_bytes()
        want = read_json(BASELINE, {}).get("stock_fwsc_sha256")
        got = hashlib.sha256(stock_b).hexdigest()
        if want and want != got:
            g.add("I-05a", "FAIL", "stock template is the known V105 file", f"sha256 {got} != baseline {want}")
        else:
            g.add("I-05a", "PASS" if want else "WARN", "stock template is the known V105 file",
                  "" if want else "no baseline yet - run with --update-baseline once")
        g.facts["stock_sha256"] = got
        s_ufw, _ = P.unwrap_fwsc(stock_b)
        if len(s_ufw) == len(c_ufw):
            head = P.xor_header(s_ufw[:64])
            count = struct.unpack_from("<H", head, 8)[0]
            off, size = P.flash_entry(s_ufw)
            lo, hi = off + A.AREA_START, off + A.AREA_END
            table_end = 64 + count * 80
            bad = []
            i, n = 0, len(s_ufw)
            while i < n:
                if s_ufw[i] != c_ufw[i] and not (lo <= i < hi) and not (i < table_end):
                    j = i
                    while j < n and s_ufw[j] != c_ufw[j]:
                        j += 1
                    bad.append(f"flash.bin+{i - off:#x}..{j - off:#x}" if off <= i < off + size else f"container {i:#x}..{j:#x}")
                    i = j
                i += 1
            g.add("I-05", "FAIL" if bad else "PASS",
                  "only the app area (flash 0x4000-0xB4FFF) differs from stock", "\n".join(bad[:20]))
            mbr_ok = s_ufw[off:lo] == c_ufw[off:lo]
            g.add("I-05b", "PASS" if mbr_ok else "FAIL", "MBR + uboot (flash 0x0000-0x3FFF) identical to stock")
    else:
        g.add("I-05", "SKIP", "stock template diff skipped (vendor-free Onyx container build)")

    # I-06: the app area decodes (SFC encoding, chip key) with consistent CRCs, and its
    # app.bin is exactly build_app.bin.
    try:
        area = A.decode_area(c_flash)
        probs = A.check_area(area)
        app_in = A.file_bytes(area, "app.bin")
    except Exception as e:  # noqa: BLE001
        probs, app_in = [f"app area does not decode: {e}"], b""
    if not (app_in == app_b or (app_in[:len(app_b)] == app_b and not app_in[len(app_b):].strip(b"\xff"))):
        probs.append("app.bin in the image is not build_app.bin")
    g.add("I-06", "FAIL" if probs else "PASS",
          "app area decodes with consistent CRCs and its app.bin is exactly build_app.bin", "\n".join(probs))

    # I-07: the image uses the AC791N app-area layout. The app is linked where the
    # table puts app.bin (XIP 0x02000120 = flash 0x4120).
    start = g.facts.get("start")
    layout = f"jl-app-area@{A.AREA_START + A.TABLE_SIZE:#x}-start{(start or 0):#x}"
    g.facts["layout"] = layout
    xip_ok = start is None or start >= 0x02000120
    if not probs and xip_ok:
        g.add("I-07", "PASS", f"image uses the AC791N app-area layout '{layout}'")
    else:
        g.add("I-07", "FAIL", f"image uses the AC791N app-area layout '{layout}'",
              f"_start {start:#x} is below app.bin's XIP address 0x02000120")


# ======================================================================================
# Stage R - recovery readiness, Stage H - hardware results (both from the ledger)
def stage_ledger(g: Gate, required_hw: list[str]):
    print("\n== R: recovery readiness (from qa/hw_ledger.json) ==")
    led = read_json(LEDGER, None)
    if led is None:
        g.add("R-00", "FAIL", "hardware ledger exists", f"{LEDGER.relative_to(ROOT)} missing or not valid JSON")
        return
    dev = led.get("device", {})
    rec = led.get("recovery", {})
    g.add("R-01", "PASS" if dev.get("flash_chip") and dev.get("flash_size_bytes") else "FAIL",
          "flash chip identified (part number and size)", "" if dev.get("flash_chip") else "open the unit and record the flash chip marking")
    dumps_ok = bool(rec.get("full_dump_sha256")) and int(rec.get("dumps_identical", 0)) >= 2
    g.add("R-02", "PASS" if dumps_ok else "FAIL", "full-chip backup: 2 identical dumps, hash recorded",
          "" if dumps_ok else "dump the whole chip twice with a programmer and record the SHA-256")
    routes = rec.get("routes", {})
    proven = [k for k, v in routes.items() if str(v.get("status") if isinstance(v, dict) else v).lower() == "proven"]
    g.add("R-03", "PASS" if proven else "FAIL", "at least one recovery route proven by a real restore",
          ("proven: " + ", ".join(proven)) if proven else "do the restore drill (TEST-PROTOCOL.md 4.3) and mark the route 'proven'")
    drill = rec.get("restore_drill", {})
    g.facts["recovery_proven"] = bool(proven) and dumps_ok
    if drill.get("date"):
        try:
            age = (dt.date.today() - dt.date.fromisoformat(drill["date"])).days
            g.add("R-04", "PASS" if age <= 90 else "WARN", f"last restore drill {age} days ago (re-drill every 90 days)")
        except ValueError:
            g.add("R-04", "WARN", "restore drill date is not YYYY-MM-DD")
    else:
        g.add("R-04", "FAIL", "restore drill recorded with a date")

    print("\n== H: hardware results for this image ==")
    sha = g.facts.get("sha256")
    runs = [r for r in led.get("hw_runs", []) if r.get("sha256") == sha]
    g.facts["hw_required"] = required_hw
    if not runs:
        g.add("H-00", "INFO", f"no hardware run recorded for this image ({(sha or '?')[:16]}...)")
        g.facts["hw_pass"] = False
        return
    res = runs[-1].get("results", {})
    missing = [t for t in required_hw if str(res.get(t, "")).upper() not in ("PASS", "N/A")]
    failed = [t for t in required_hw if str(res.get(t, "")).upper() == "FAIL"]
    g.facts["hw_pass"] = not missing
    detail = []
    if failed:
        detail.append("FAILED: " + ", ".join(failed))
    not_done = [t for t in missing if t not in failed]
    if not_done:
        detail.append("not done: " + ", ".join(not_done))
    g.add("H-01", "FAIL" if failed else ("PASS" if not missing else "INFO"),
          f"hardware tests for this patch ({len(required_hw)} required)", "\n".join(detail))


def required_hw_tests(base: str | None) -> tuple[list[str], list[str]]:
    if not base:
        return sorted(HW_TESTS), ["no --base given: every hardware test is required"]
    rc, out = run(["git", "diff", "--name-only", f"{base}...HEAD"])
    rc2, out2 = run(["git", "diff", "--name-only", "HEAD"])          # uncommitted work too
    if rc != 0:
        return sorted(HW_TESTS), [f"git diff against {base} failed: every hardware test is required"]
    files = sorted({l.strip() for l in (out + "\n" + out2).splitlines() if l.strip()})
    need, why = set(), []
    for f in files:
        for pat, tests in IMPACT:
            if fnmatch.fnmatch(f, pat):
                if tests == "ALL":
                    need |= set(HW_TESTS)
                elif tests:
                    need |= set(tests.split())
                why.append(f"{f} -> {tests or 'host only'}")
                break
        else:
            need |= set(HW_TESTS)
            why.append(f"{f} -> ALL (no rule)")
    if need:
        need |= {"HW-01", "HW-02"}
    return sorted(need), why


# ======================================================================================
def write_report(g: Gate, verdict: str, why_hw: list[str]) -> Path:
    REPORTS.mkdir(parents=True, exist_ok=True)
    stamp = dt.datetime.now().strftime("%Y%m%d-%H%M%S")
    lines = [f"# QA gate report - {stamp}", "", f"**Verdict: {verdict}**", ""]
    f = g.facts
    lines += ["| fact | value |", "|---|---|",
              f"| image SHA-256 | `{f.get('sha256', '-')}` |",
              f"| app size | {f.get('app_size', '-')} B |",
              f"| layout | `{f.get('layout', '-')}` |",
              f"| sections | {f.get('sections', '-')} |",
              f"| flash writes | {'stubbed' if f.get('flash_writes_stubbed') else 'WIRED'} |", ""]
    lines += ["## Checks", "", "| id | status | check | detail |", "|---|---|---|---|"]
    for cid, st, title, detail in g.results:
        d = detail.replace("\n", "<br>").replace("|", "\\|")[:600]
        lines.append(f"| {cid} | {st} | {title} | {d} |")
    lines += ["", "## Hardware tests required for this patch", ""]
    for t in f.get("hw_required", []):
        lines.append(f"- [ ] **{t}** {HW_TESTS[t]}")
    if why_hw:
        lines += ["", "<details><summary>why</summary>", "", *[f"- {w}" for w in why_hw], "", "</details>"]
    text = "\n".join(lines) + "\n"
    path = REPORTS / f"qa-{stamp}.md"
    path.write_text(text, encoding="utf-8")
    (REPORTS / "latest.md").write_text(text, encoding="utf-8")
    return path


def update_baseline(g: Gate):
    data = read_json(BASELINE, {})
    if g.facts.get("sections"):
        data["sections"] = g.facts["sections"]
    if g.facts.get("stock_sha256"):
        data.setdefault("stock_fwsc_sha256", g.facts["stock_sha256"])
    data["updated"] = dt.date.today().isoformat()
    QA.mkdir(parents=True, exist_ok=True)
    BASELINE.write_text(json.dumps(data, indent=2) + "\n", encoding="utf-8")
    print(f"\nbaseline written: {BASELINE.relative_to(ROOT)}")


def main() -> int:
    ap = argparse.ArgumentParser(description="Pocket Amp firmware release gate")
    ap.add_argument("--skip-build", action="store_true", help="check existing artifacts instead of building")
    ap.add_argument("--base", help="git ref the patch is based on (works out which hardware tests it needs)")
    ap.add_argument("--release", action="store_true", help="release mode: untracked build inputs fail")
    ap.add_argument("--stock", help="path to the stock 'Pocket AMP.fwsc' (default: build.ps1's template)")
    ap.add_argument("--update-baseline", action="store_true", help="accept today's section sizes as the baseline")
    ap.add_argument("--only", choices=["static", "image", "ledger"], help="run one stage only (quick checks)")
    args = ap.parse_args()
    if not shutil.which("clang"):
        for c in (Path(os.environ.get("LOCALAPPDATA", "")) / "Microsoft" / "WinGet" / "Packages").glob("MartinStorsjo.LLVM-MinGW*/*/bin"):
            os.environ["PATH"] = f"{c}{os.pathsep}{os.environ['PATH']}"
            break

    g = Gate(args)
    print(f"Pocket Amp QA gate - {dt.datetime.now():%Y-%m-%d %H:%M}")
    required, why = required_hw_tests(args.base)
    if args.only in (None, "static"):
        stage_static(g)
    if args.only is None:
        stage_build(g)
        stage_unit(g)
        stage_functional(g)
    if args.only in (None, "image"):
        stage_image(g)
    if args.only in (None, "ledger", "image"):
        stage_ledger(g, required)

    auto_fail = [r for r in g.results if r[1] == "FAIL" and not r[0].startswith(("R-", "H-"))]
    hw_fail = [r for r in g.results if r[1] == "FAIL" and r[0].startswith("H-")]
    if auto_fail or hw_fail:
        verdict, code = "NO-GO", 1
    elif not g.facts.get("recovery_proven"):
        verdict, code = "FLASH BLOCKED - no proven recovery route", 2
    elif g.facts.get("hw_pass"):
        verdict, code = "RELEASE GO", 0
    else:
        verdict, code = "READY FOR BENCH TEST", 0
    if args.only:
        # a partial run never clears an image for flashing
        verdict = ("NO-GO" if code == 1 else "PARTIAL RUN - no verdict") + f" (--only {args.only})"
        code = 1 if code == 1 else 0
    if args.update_baseline:
        update_baseline(g)
    rpt = write_report(g, verdict, why)
    counts = Counter(r[1] for r in g.results)
    print("\n" + "=" * 70)
    print(f"VERDICT: {verdict}")
    print("  " + ", ".join(f"{k} {v}" for k, v in sorted(counts.items())))
    print(f"  report: {rpt.relative_to(ROOT)}")
    if g.facts.get("hw_required") and code == 0:
        print(f"  hardware tests needed: {' '.join(g.facts['hw_required'])}")
    print("=" * 70)
    return code


if __name__ == "__main__":
    sys.exit(main())
