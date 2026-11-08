#!/usr/bin/env python3
"""
custom-fw/tools/flash_store_pc.py - what the unit has saved in its flash (settings log, preset banks, the neighbours that must stay untouched). READ-ONLY.

Reads the raw flash over the USB-MIDI diagnostic channel (the firmware reads it with the SRAM flash driver: each read masks the interrupts for a moment, so use it between tests, not while recording).

    python custom-fw/tools/flash_store_pc.py                 the settings log (0x88000), preset bank headers (A 0x8D000, B 0x89000), and the places that must not change
    python custom-fw/tools/flash_store_pc.py --ref dump1.bin  also compares 0xB5000 (the stock's record log), 0xBD000 (BTIF), 0xBE000 (USER) and 0xFF000 with a reference dump (they must be identical)
"""
import argparse
import struct
import sys
import zlib
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
import ble_matrix_pc as m

SET_ADDR, SET_SLOT, SET_SLOTS = 0x88000, 64, 64
BANKS = {"A": 0x8D000, "B": 0x89000}
PRESET_MAGIC = None


def flash(u, addr, n):
    out = bytearray()
    while n:
        k = min(n, 16)
        for attempt in range(6):
            try:
                body = u._call(bytes([0x7D, 0x01]) + struct.pack("<IH", addr, k), 0x81, timeout=0.6)
                break
            except TimeoutError:
                if attempt == 5:
                    raise
        if body[2:4] == b"\xff\xff":
            raise RuntimeError(f"the unit refused the flash read at {addr:#x}")
        got = struct.unpack_from("<H", body, 2)[0]
        out += body[8:8 + got]
        addr += got
        n -= got
        if not got:
            break
    return bytes(out)


def crc32(b):
    return zlib.crc32(b) & 0xFFFFFFFF


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--ref", help="a full-chip dump to compare the protected places with")
    a = ap.parse_args()
    u = m.Unit("Custom MIDI")
    print("BUILD", u.build_id())
    print(f"\nsettings log at {SET_ADDR:#x}: {SET_SLOTS} slots of {SET_SLOT} bytes")
    sect = flash(u, SET_ADDR, SET_SLOTS * SET_SLOT)
    best = None
    used = 0
    for k in range(SET_SLOTS):
        rec = sect[k * SET_SLOT:(k + 1) * SET_SLOT]
        if all(b == 0xFF for b in rec):
            continue
        used = k + 1
        magic, ver, size, seq = struct.unpack_from("<IHHI", rec, 0)
        ok = magic == 0x54455350 and size <= SET_SLOT and zlib.crc32(rec[:size - 4]) & 0xFFFFFFFF == struct.unpack_from("<I", rec, size - 4)[0]
        if ok and (best is None or (seq - best[1]) & 0xFFFFFFFF < 0x80000000):
            best = (k, seq, rec[:size])
        if not ok:
            print(f"  slot {k:2d}: NOT BLANK and not a valid record (torn or foreign): {rec[:16].hex(' ')} ...")
    print(f"  slots in use (highest non-blank + 1): {used} of {SET_SLOTS}")
    if best:
        k, seq, rec = best
        f = struct.unpack_from("<BBBBBBBBbbBBBBBB", rec, 12)
        print(f"  newest valid record: slot {k}, seq {seq}")
        print(f"    last_preset {f[0]}, home_view_mode {f[1]}, play_mode {f[2]}, fizz {f[3]}, brightness {f[4]}, auto_dim {f[5]}, battery% {f[6]}, theme {f[7]}")
        print(f"    input_gain {f[8]} dB, gate {f[9]} dB, out_level {f[10]}%, output_type {f[11]}, cab_on_line {f[12]}, usb_rate {f[13]}, bt_enabled {f[14]}, tuner_mute {f[15]}")
    else:
        print("  no valid record: the unit has never saved its settings (or they were lost)")
    for name, addr in BANKS.items():
        h = flash(u, addr, 16)
        magic, seq, count, crc = struct.unpack("<IIII", h)
        blank = all(b == 0xFF for b in h)
        print(f"\npreset bank {name} at {addr:#x}: header {h.hex(' ')}  -> " + ("blank" if blank else f"magic {magic:#010x} seq {seq} count {count} crc {crc:#010x}"))
    if a.ref:
        ref = Path(a.ref).read_bytes()
        bad = 0
        print("\nplaces that must stay as the reference dump has them:")
        for name, lo, hi in (("the stock's record log", 0xB5000, 0xB6000), ("BTIF", 0xBD000, 0xBE000), ("USER", 0xBE000, 0xBF000), ("the stock's save record", 0xFF000, 0x100000), ("NAM blob sector", 0xBC000, 0xBD000)):
            got = flash(u, lo, hi - lo)
            same = got == ref[lo:hi]
            bad += not same
            print(f"  {lo:#08x} {name}: {'identical' if same else 'DIFFERENT'}")
        print("VERDICT:", "the protected places are untouched" if not bad else "A PROTECTED PLACE CHANGED")
        return 1 if bad else 0
    return 0


if __name__ == "__main__":
    sys.exit(main())

# SPDX-License-Identifier: GPL-3.0-only
# Copyright (C) 2026 The Onyx Authors (@28spawn)
