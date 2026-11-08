#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-only
# Copyright (C) 2026 The Onyx Authors (@28spawn)
"""
custom-fw/tools/abkx_to_json.py - convert an 80-slot 92-byte preset bank (".abkx")
into the Onyx preset-backup JSON ("pocket-amp-presets", version 1, 164-byte records).

The input is a user-supplied export from the stock app. This tool reads that file
only: it has no vendor data, no vendor code, no network access and no device access.
The record layout is the one documented in custom-fw/MIDI-MAP-CUSTOM.md section 4;
the module/knob mapping is section 5.

    python custom-fw/tools/abkx_to_json.py "Pocket AMP_AllPresets.abkx"
    python custom-fw/tools/abkx_to_json.py bank.abkx -o converted.json

Stock modules are mapped onto the Onyx slots:

    stock FX   -> DYN      stock AMP -> AMP       stock CAB -> CAB
    stock MOD  -> MOD      stock DLY -> DLY       stock REV -> REV

DRIVE and EQ exist only in the PA-EXT extension (the 13 free bytes of the record),
so they stay off unless the source record carries that extension. The converter
never writes a device and never touches the factory amp/cab partition.
"""
from __future__ import annotations

import argparse
import json
import sys
from pathlib import Path

RECORD_SIZE = 92
SLOT_COUNT = 80
TRAILER_SIZE = 10
BANK_SIZE = SLOT_COUNT * RECORD_SIZE + TRAILER_SIZE

# Stock module codes and the offsets of their six 16-bit parameter slots.
MODULE_CODES = ("FX", "AMP", "MOD", "DLY", "REV", "CAB")
MODULE_SLOT = {"FX": 0x14, "AMP": 0x20, "MOD": 0x2C, "DLY": 0x38, "REV": 0x44, "CAB": 0x50}

# Onyx slot indices (pa_fx.h).
DYN, DRIVE, AMP, CAB, EQ, MOD, DLY, REV = range(8)
SLOT_NAMES = ("DYN", "DRV", "AMP", "CAB", "EQ", "MOD", "DLY", "REV")

# Stock module code -> Onyx slot. DRIVE goes directly before AMP and EQ directly
# after CAB, which is the chain rule the firmware documents.
CHAIN_SLOT = {"FX": DYN, "AMP": AMP, "MOD": MOD, "DLY": DLY, "REV": REV, "CAB": CAB}

# stock MOD type -> Onyx MOD type. The stock app numbers Tremolo 2 / Flanger 3;
# the firmware has them the other way round (MIDI-MAP-CUSTOM.md section 5).
MOD_TYPE = {0: 0, 1: 1, 2: 3, 3: 2, 4: 4, 5: 5, 6: 6}

# stock DLY extended type (PA-EXT dly_ext 1..4) -> Onyx DELAY type.
DLY_EXT_TYPE = {1: 5, 2: 6, 3: 7, 4: 8}   # DIGITAL, PING PONG, REVERSE, SLAPBACK
# stock REV extended type (PA-EXT rev_ext 1..2) -> Onyx REVERB type.
REV_EXT_TYPE = {1: 6, 2: 0}               # PLATE; AMBIENCE has no Onyx twin since 1.13 (it was removed): the Room is the closest

DEFAULT_STOMP = [DRIVE, MOD, DLY, REV]


def _pct(v: int) -> int:
    return max(0, min(100, int(v)))


def _q16(v: int) -> int:
    return (_pct(v) * 65535 + 50) // 100


def _neutral_pct() -> list[list[int]]:
    """The firmware's neutral knob set (src/sys/preset.c, preset_set_neutral)."""
    p = [[50] * 6 for _ in range(8)]
    p[DYN][3] = 100       # DYN level: unity
    p[CAB][0] = 100       # CAB level: unity
    p[CAB][1] = 0         # CAB low cut: minimum
    p[CAB][2] = 0         # CAB high cut: open
    p[MOD][2] = 12        # MOD mix
    p[DLY][1] = 15        # DLY feedback
    p[DLY][2] = 10        # DLY mix
    p[REV][1] = 0         # REV low cut: minimum
    return p


def _params(rec: bytes, module: str) -> list[int]:
    base = MODULE_SLOT[module]
    return [rec[base + 2 * k] | (rec[base + 2 * k + 1] << 8) for k in range(6)]


def decode_extension(rec: bytes):
    """Decode PA-EXT v1 from the 13 free bytes, or return None.

    The header is byte 0x01: bit 7 = present, bits 6..5 = version 1,
    bits 4..0 = dly_ext + 5 * rev_ext + 15 * dyn_ext.
    """
    if not (rec[0x01] & 0x80):
        return None
    if ((rec[0x01] >> 5) & 0x03) != 1:
        return None
    code = rec[0x01] & 0x1F
    return {
        "dly_ext": code % 5,
        "rev_ext": (code // 5) % 3,
        "dyn_ext": code // 15,
        "amp_pres": rec[0x2A],
        "input_trim_db": rec[0x2B] - 50,
        "drive_on": rec[0x36] // 50,
        "drive_type": rec[0x36] % 50,
        "eq_on": rec[0x4E] // 50,
        "eq_type": rec[0x4E] % 50,
        "drive_drive": rec[0x56],
        "drive_tone": rec[0x58],
        "drive_level": rec[0x5A],
        "eq_k": [rec[0x37], rec[0x4F], rec[0x57], rec[0x59], rec[0x5B]],
    }


def _map_dyn(vendor_type: int, v: list[int], ext, p: list[list[int]]) -> tuple[int, bool]:
    """stock FX -> Onyx DYN. Returns (Onyx type, usable)."""
    t = vendor_type
    if ext and ext["dyn_ext"] == 1:
        t = 4   # COMP without gate
    if t not in (0, 1, 2, 3, 4):
        return 0, False
    p[DYN][0] = _pct(v[0])                     # thresh / gate
    if t == 1:                                  # BOOST+GATE
        p[DYN][3] = _pct(v[1])                 # boost gain
    elif t in (2, 4):                           # COMP+GATE / COMP
        p[DYN][1] = _pct(v[1])                 # ratio
        p[DYN][2] = _pct(v[2])                 # attack
        p[DYN][3] = _pct(v[3] * 2)             # stock 50 = unity -> firmware 100
    return t, True


def _map_amp(vendor_type: int, v: list[int], ext, p: list[list[int]],
             copy_level: bool) -> int:
    """stock AMP -> Onyx AMP. Onyx knob order: GAIN BASS MID TREBLE PRES MASTER."""
    t = vendor_type if 0 <= vendor_type < 20 else 0
    p[AMP][0] = _pct(v[0])                     # gain
    p[AMP][1] = _pct(v[2])                     # bass
    p[AMP][2] = _pct(v[3])                     # mid
    p[AMP][3] = _pct(v[4])                     # treble
    p[AMP][4] = _pct(ext["amp_pres"]) if ext else 50   # presence
    p[AMP][5] = _pct(v[1]) if copy_level else 50       # master
    return t


def _map_cab(vendor_type: int, v: list[int], p: list[list[int]]) -> int:
    """stock CAB -> Onyx CAB. The stock High Cut is inverted on the firmware."""
    t = vendor_type if 0 <= vendor_type < 20 else 0
    p[CAB][0] = _pct(v[0])                     # level
    p[CAB][1] = _pct(v[1])                     # low cut
    p[CAB][2] = _pct(100 - v[2])               # high cut: 100 = open
    return t


def _map_mod(vendor_type: int, v: list[int], p: list[list[int]]) -> tuple[int, bool]:
    """stock MOD -> Onyx MOD. Approximate where the stock control has no twin."""
    t = MOD_TYPE.get(vendor_type)
    if t is None:
        return 0, False
    if t == 0:                                  # CHORUS: speed, depth, mix
        p[MOD][0], p[MOD][1], p[MOD][2] = _pct(v[0]), _pct(v[1]), _pct(v[2])
    elif t == 1:                                # PHASER: speed, midcut, resonance, feedback
        p[MOD][0], p[MOD][1], p[MOD][3] = _pct(v[0]), _pct(v[2]), _pct(v[3])
    elif t == 2:                                # FLANGER: speed, depth, feedback, mix
        p[MOD][0], p[MOD][1], p[MOD][3], p[MOD][2] = (
            _pct(v[0]), _pct(v[1]), _pct(v[2]), _pct(v[3]))
    elif t == 3:                                # TREMOLO: speed, depth
        p[MOD][0], p[MOD][1] = _pct(v[0]), _pct(v[1])
    elif t == 4:                                # VIBRATO: speed, depth
        p[MOD][0], p[MOD][1] = _pct(v[0]), _pct(v[1])
    elif t == 5:                                # UNIVIBE: Onyx has no univibe since 1.13 (Wah took its place): the PHASER is its nearest cousin
        p[MOD][0], p[MOD][1], p[MOD][2] = _pct(v[0]), _pct(v[1]), _pct(v[2])
        return 1, True
    else:                                       # AUTO FILTER: speed, min, max, mix, feedback
        p[MOD][0] = _pct(v[0])
        p[MOD][1] = _pct((v[1] + v[2]) // 2)    # sweep range: min/max collapsed
        p[MOD][2] = _pct(v[3])
        p[MOD][3] = _pct(v[4])
    return t, True


def _map_dly(vendor_type: int, v: list[int], ext, p: list[list[int]]) -> int:
    """stock DLY -> Onyx DELAY: time, feedback, mix, aux."""
    t = vendor_type
    if ext and ext["dly_ext"]:
        t = DLY_EXT_TYPE.get(ext["dly_ext"], t)
    if not 0 <= t <= 8:
        t = 0
    p[DLY][0], p[DLY][1], p[DLY][2] = _pct(v[0]), _pct(v[1]), _pct(v[2])
    # Dual: the stock p3 is Grit, the firmware aux is Ratio - not the same control.
    p[DLY][3] = 0 if t == 3 else _pct(v[3])
    return t


def _map_rev(vendor_type: int, v: list[int], ext, p: list[list[int]]) -> int:
    """stock REV -> Onyx REVERB. Onyx order: DECAY LOWCUT TONE MIX."""
    t = vendor_type
    if ext and ext["rev_ext"]:
        t = REV_EXT_TYPE.get(ext["rev_ext"], t)
    if not 0 <= t <= 6:
        t = 0
    if t == 4:                                  # SHIMMER: decay, mix, tone, pitch, amount
        p[REV][0], p[REV][3], p[REV][2] = _pct(v[0]), _pct(v[1]), _pct(v[2])
    else:                                       # Room/Hall family: decay, mix, hpass, lpass
        p[REV][0], p[REV][1], p[REV][2], p[REV][3] = (
            _pct(v[0]), _pct(v[2]), _pct(v[3]), _pct(v[1]))
    return t


def _chain_order(codes, preserve: bool) -> list[int]:
    """Map the stock chain codes to a valid 8-slot order.

    By default the firmware's own order is used (DYN DRV AMP CAB EQ MOD DLY REV)
    because it keeps CAB directly after AMP. --preserve-chain keeps the order in
    the source record, with DRIVE before AMP and EQ after CAB.
    """
    if not preserve:
        return list(range(8))
    order: list[int] = []
    for code in codes:
        slot = CHAIN_SLOT[MODULE_CODES[code]]
        if slot == AMP:
            order.append(DRIVE)
        order.append(slot)
        if slot == CAB:
            order.append(EQ)
    for slot in range(8):
        if slot not in order:
            order.append(slot)
    return order


def _empty_preset(slot: int, genre: int) -> dict:
    pct = _neutral_pct()
    return {
        "slot": slot,
        "name": "EMPTY",
        "empty": True,
        "seq": 1,
        "bpm": 120,
        "genre": genre,
        "custom_cab_idx": 255,
        "custom_cab_blend": 100,
        "input_trim_db": 0,
        "gate_threshold_pct": 0,
        "slot_enable_mask": 0,
        "slot_order": list(range(8)),
        "amp_model": 0,
        "cab_model": 0,
        "preset_vol": 70,
        "flags": 1,
        "slot_type": [3, 0, 0, 0, 0, 0, 0, 0],
        "stomp_map": list(DEFAULT_STOMP),
        "params": [[_q16(x) for x in row] for row in pct],
        "params_pct": pct,
    }


def _record_to_preset(rec: bytes, index: int, *, preserve_chain: bool,
                      copy_amp_level: bool, name_prefix: str, genre: int) -> dict:
    ext = decode_extension(rec)
    enabled = [rec[0x08 + c] for c in range(6)]
    drive_on = bool(ext and ext["drive_on"])
    eq_on = bool(ext and ext["eq_on"])
    if not any(enabled) and not drive_on and not eq_on:
        return _empty_preset(index + 1, genre)

    pct = _neutral_pct()
    slot_type = [0] * 8
    mask = 0

    # DYN from stock FX (module code 0)
    dyn_type, dyn_ok = _map_dyn(rec[0x0E + 0], _params(rec, "FX"), ext, pct)
    slot_type[DYN] = dyn_type
    if enabled[0] and dyn_ok:
        mask |= 1 << DYN

    # AMP (module code 1)
    amp_type = _map_amp(rec[0x0E + 1], _params(rec, "AMP"), ext, pct, copy_amp_level)
    slot_type[AMP] = amp_type
    if enabled[1]:
        mask |= 1 << AMP

    # CAB (module code 5)
    cab_type = _map_cab(rec[0x0E + 5], _params(rec, "CAB"), pct)
    slot_type[CAB] = cab_type
    if enabled[5]:
        mask |= 1 << CAB

    # MOD (module code 2)
    mod_type, mod_ok = _map_mod(rec[0x0E + 2], _params(rec, "MOD"), pct)
    slot_type[MOD] = mod_type
    if enabled[2] and mod_ok:
        mask |= 1 << MOD

    # DLY (module code 3)
    slot_type[DLY] = _map_dly(rec[0x0E + 3], _params(rec, "DLY"), ext, pct)
    if enabled[3]:
        mask |= 1 << DLY

    # REV (module code 4)
    slot_type[REV] = _map_rev(rec[0x0E + 4], _params(rec, "REV"), ext, pct)
    if enabled[4]:
        mask |= 1 << REV

    # DRIVE / EQ only travel in the PA-EXT extension.
    if drive_on:
        slot_type[DRIVE] = ext["drive_type"] % 6
        pct[DRIVE][0] = _pct(ext["drive_drive"])
        pct[DRIVE][1] = _pct(ext["drive_tone"])
        pct[DRIVE][2] = _pct(ext["drive_level"])
        mask |= 1 << DRIVE
    if eq_on:
        slot_type[EQ] = ext["eq_type"] % 3
        for i in range(5):
            pct[EQ][i] = _pct(ext["eq_k"][i])
        mask |= 1 << EQ

    input_trim = max(-12, min(12, ext["input_trim_db"])) if ext else 0
    name = (name_prefix + "%02d" % (index + 1))[:23]

    return {
        "slot": index + 1,
        "name": name,
        "empty": False,
        "seq": 2,                       # >= 2: the firmware refreshes a factory slot whose save counter is still 1 from its own table at every boot (preset.c refresh_untouched_factory)
        "bpm": 120,
        "genre": genre,
        "custom_cab_idx": 255,
        "custom_cab_blend": 100,
        "input_trim_db": input_trim,
        "gate_threshold_pct": 0,
        "slot_enable_mask": mask,
        "slot_order": _chain_order(rec[0x02:0x08], preserve_chain),
        "amp_model": amp_type,
        "cab_model": cab_type,
        "preset_vol": _pct(rec[0x00]),
        "flags": 0,
        "slot_type": slot_type,
        "stomp_map": list(DEFAULT_STOMP),
        "params": [[_q16(x) for x in row] for row in pct],
        "params_pct": pct,
    }


NOTE = (
    "Onyx preset backup converted from an 80-slot 92-byte bank by "
    "custom-fw/tools/abkx_to_json.py. Edit names and values freely; 'params' are "
    "Q16 (0..65535 = 0..100 %), 'params_pct' is for reading only. Slot order: "
    "DYN DRV AMP CAB EQ MOD DLY REV. DRIVE and EQ are filled only when the source "
    "record carries the documented PA-EXT extension."
)


def convert(data: bytes, *, preserve_chain: bool = False,
            copy_amp_level: bool = False, name_prefix: str = "P",
            genre: int = 7, warn=None) -> dict:
    """Convert a whole 7,370-byte bank to the Onyx JSON document."""
    if len(data) != BANK_SIZE:
        raise ValueError("expected %d bytes, got %d" % (BANK_SIZE, len(data)))
    trailer = data[SLOT_COUNT * RECORD_SIZE:]
    if not trailer.startswith(b"PATCHEND"):
        raise ValueError("missing PATCHEND trailer")
    if trailer[-1] not in (0x00, 0x45):
        raise ValueError("unexpected trailer byte 0x%02x" % trailer[-1])

    genre = max(0, min(7, int(genre)))
    presets = []
    for i in range(SLOT_COUNT):
        rec = data[i * RECORD_SIZE:(i + 1) * RECORD_SIZE]
        if sorted(rec[0x02:0x08]) != list(range(6)):
            raise ValueError("slot %d: chain order is not a permutation of 0..5" % (i + 1))
        if rec[0x01] & 0x80 and ((rec[0x01] >> 5) & 0x03) != 1:
            if warn:
                warn("slot %d: unknown PA-EXT version %d; extension ignored"
                     % (i + 1, (rec[0x01] >> 5) & 0x03))
        presets.append(_record_to_preset(
            rec, i, preserve_chain=preserve_chain, copy_amp_level=copy_amp_level,
            name_prefix=name_prefix, genre=genre))

    return {
        "format": "pocket-amp-presets",
        "version": 1,
        "record_size": 164,
        "note": NOTE,
        "presets": presets,
    }


def main(argv=None) -> int:
    ap = argparse.ArgumentParser(
        description="Convert an 80-slot 92-byte preset bank (.abkx) to Onyx preset JSON.")
    ap.add_argument("input", help="the user-supplied .abkx bank")
    ap.add_argument("-o", "--output",
                    help="output JSON (default: <input>.onyx.json)")
    ap.add_argument("--preserve-chain", action="store_true",
                    help="keep the source chain order instead of the firmware's "
                         "DYN DRV AMP CAB EQ MOD DLY REV order")
    ap.add_argument("--copy-amp-level", action="store_true",
                    help="put the stock AMP Level into MASTER (default: leave MASTER 50)")
    ap.add_argument("--name-prefix", default="P",
                    help="name for non-empty slots: PREFIX + 2-digit slot (default P)")
    ap.add_argument("--genre", type=int, default=7,
                    help="genre tag 0..7 for converted presets (default 7 = custom)")
    a = ap.parse_args(argv)

    src = Path(a.input)
    dst = Path(a.output) if a.output else src.with_suffix(".onyx.json")
    data = src.read_bytes()
    doc = convert(data, preserve_chain=a.preserve_chain,
                  copy_amp_level=a.copy_amp_level, name_prefix=a.name_prefix,
                  genre=a.genre, warn=lambda m: print("warning: " + m, file=sys.stderr))
    dst.write_text(json.dumps(doc, indent=1) + "\n", encoding="utf-8", newline="\n")
    filled = sum(1 for p in doc["presets"] if not p["empty"])
    print("wrote %s: %d slots, %d non-empty" % (dst, len(doc["presets"]), filled))
    return 0


if __name__ == "__main__":
    sys.exit(main())
