#!/usr/bin/env python3
"""
custom-fw/tools/ble_midi_e2e_pc.py - BLE-MIDI end to end against the unit: connect over BLE (bleak), turn the MIDI notifications on, and check on ONE link that
  1. the identity query is answered with a SysEx notification,
  2. a Program Change selects the preset (read back over the USB diagnostic channel),
  3. a SETTINGS change made over BLE takes effect AND is saved: the front-panel buttons are driven with the implemented CCs 64 (HOME), 65 (PARA click), 67 (PARA turn) - Settings > Display > BATTERY % is
     toggled - and the unit writes the new setting into the settings log in its flash about 2 s later (read back from the flash),
  4. the same change back to the original value is saved too,
  5. the link was not dropped by the flash saves (the unit's own link counters before / after: no supervision timeout; failed establishments are reported, Windows makes some on every connect).
(The map's NRPN / direct-knob CCs 7, 28..35, 80..119 are PROPOSALS: MIDI-MAP-CUSTOM.md tags them [N]; the firmware's handler implements 20..27, 64..67, Program Change and the bench CCs only.)
The unit must be on USB ("Custom MIDI" port) and not held by another BLE central; run after removing a stale "Pocket AMP_BLE" pairing from Windows.

    python custom-fw/tools/ble_midi_e2e_pc.py [--name Pocket] [--preset 11]
"""
import argparse
import asyncio
import struct
import sys
import time
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
import ble_matrix_pc as m
import flash_store_pc as F
import jl_ram_read as J
from bleak import BleakClient, BleakScanner

MIDI_CHAR = "7772e5db-3868-4112-a1a9-f2669d106bf3"
IDENTITY = bytes.fromhex("F000324500000040" "7F" "F7")
SET_ADDR, SLOT = 0x88000, 64


def ble_packet(msg):
    ts = int(time.monotonic() * 1000) & 0x1FFF
    hdr, lo = 0x80 | (ts >> 7), 0x80 | (ts & 0x7F)
    msg = bytes(msg)
    if msg[0] == 0xF0 and msg[-1] == 0xF7:
        return bytes([hdr, lo]) + msg[:-1] + bytes([lo, 0xF7])
    return bytes([hdr, lo]) + msg


def nrpn(module, param, value):
    """a stock-style NRPN set: CC99 module, CC98 parameter, CC6 high 7 bits, CC38 low 7 bits (value = the raw number D, see MIDI-MAP-CUSTOM.md section 6), as one BLE-MIDI packet of four CCs"""
    return [(0xB0, 99, module), (0xB0, 98, param), (0xB0, 6, value >> 7), (0xB0, 38, value & 0x7F)]


class Live:
    def __init__(self, u):
        self.u = u

    def read(self, a, n):
        out = b""
        while len(out) < n:
            k = min(256, n - len(out))
            out += bytes(self.u.ram(a + len(out), k))
        return out


def link_counters(u):
    r = Live(u)
    p = struct.unpack("<8I", r.read(0x01C7D5D0, 32))
    st = J.read_ble_status(r, p)
    return {k: st.get(k, 0) for k in ("LINK_CLOSES", "LINK_SUP_TO", "LINK_ESTAB_FAIL", "LINK_PEER_TERM", "LINK_ENC_REQ", "ISR_MAX_US", "ADV_EVENTS")}


def settings_log(u):
    """(slots in use, the newest valid record's (slot, seq, battery % shown 0/1, last preset)) from the flash"""
    sect = F.flash(u, SET_ADDR, 32 * SLOT)
    best = None
    used = 0
    for k in range(32):
        rec = sect[k * SLOT:(k + 1) * SLOT]
        if all(b == 0xFF for b in rec):
            continue
        used = k + 1
        magic, ver, size, seq = struct.unpack_from("<IHHI", rec, 0)
        if magic == 0x54455350 and size <= SLOT and F.crc32(rec[:size - 4]) == struct.unpack_from("<I", rec, size - 4)[0]:
            if best is None or ((seq - best[1]) & 0xFFFFFFFF) < 0x80000000:
                f = struct.unpack_from("<BBBBBBBBbb", rec, 12)
                best = (k, seq, f[6], f[0])       # (slot, seq, battery % shown, last preset)
    return used, best


def preset_now(u):
    t = struct.unpack("<I", bytes(u.ram(0x01C7C390 + 12, 4)))[0]
    return (t >> 8) & 0xFF


async def main(a):
    u = m.Unit("Custom MIDI")
    print("BUILD", u.build_id())
    c0 = link_counters(u)
    used0, best0 = settings_log(u)
    print(f"before: settings log {used0} slots in use, newest {best0}; preset P{preset_now(u) + 1:02d}")
    orig_gain = best0[2] if best0 else 0
    ok_all = True

    def check(ok, msg):
        nonlocal ok_all
        print(("  ok   " if ok else "  FAIL ") + msg, flush=True)
        ok_all &= bool(ok)

    dev = await BleakScanner.find_device_by_filter(lambda d, adv: a.name.lower() in (adv.local_name or d.name or "").lower(), timeout=15)
    if not dev:
        print("the unit is not advertising (is another central holding the link?)")
        return 1
    got = []
    async with BleakClient(dev, timeout=25) as c:
        await c.start_notify(MIDI_CHAR, lambda _h, data: got.append(bytes(data)))
        check(True, f"connected to {dev.address} and the MIDI notifications are on")

        async def send(msg):
            await c.write_gatt_char(MIDI_CHAR, ble_packet(msg), response=False)

        # 1. identity
        await send(IDENTITY)
        await asyncio.sleep(1.5)
        sysex = [g for g in got if b"\xf7" in g or len(g) > 8]
        check(bool(sysex), f"the identity query was answered ({len(got)} notification(s), longest {max((len(g) for g in got), default=0)} bytes)")

        # 2. program change
        want = a.preset
        await send([0xC0, want])
        await asyncio.sleep(1.0)
        check(preset_now(u) == want, f"Program Change {want} selected preset P{want + 1:02d} (the unit reports P{preset_now(u) + 1:02d})")

        # 3. a settings change over BLE: HOME, PARA turn x2 (Display), click, PARA turn x3 (BATTERY %), click = toggle; HOME = back
        async def front_panel(seq):
            for cc in seq:
                await send([0xB0, cc, 127])
                await asyncio.sleep(0.35)
        orig = best0[2] if best0 else 1
        await front_panel([64, 67, 67, 65, 67, 67, 67, 65, 64])
        print(f"  toggled Settings > Display > BATTERY % over BLE (was {orig}); waiting 4 s for the settings save ...", flush=True)
        await asyncio.sleep(4.0)
        used1, best1 = settings_log(u)
        check(best1 is not None and best1[2] == (1 - orig), f"the new BATTERY % setting {1 - orig} is in the flash settings log (newest record {best1}, {used1} slots in use, was {used0})")
        check(used1 > used0 or (used0 == 32 and used1 >= 1), "the save appended a new settings slot")

        # 4. back to the original, saved too
        await front_panel([64, 67, 67, 65, 67, 67, 67, 65, 64])
        await asyncio.sleep(4.0)
        used2, best2 = settings_log(u)
        check(best2 is not None and best2[2] == orig, f"the original BATTERY % setting {orig} is saved again (newest record {best2}, {used2} slots in use)")
        link_alive = c.is_connected
        await c.stop_notify(MIDI_CHAR)
    check(link_alive, "the BLE link stayed up through both flash saves")
    await asyncio.sleep(1.0)
    c1 = link_counters(u)
    check(c1["LINK_SUP_TO"] == c0["LINK_SUP_TO"], f"no supervision timeout during the test ({c0['LINK_SUP_TO']} -> {c1['LINK_SUP_TO']})")
    print(f"  info: failed establishments {c0['LINK_ESTAB_FAIL']} -> {c1['LINK_ESTAB_FAIL']} (Windows opens extra connections while it connects; the pack 156 baseline had 3 in 3 attempts)")
    print("link counters:", c0, "->", c1)
    print("VERDICT:", "PASS" if ok_all else "NOT PASSED")
    return 0 if ok_all else 1


if __name__ == "__main__":
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--name", default="Pocket")
    ap.add_argument("--preset", type=int, default=11, help="the Program Change number (0..79) to select")
    raise SystemExit(asyncio.run(main(ap.parse_args())))

# SPDX-License-Identifier: GPL-3.0-only
# Copyright (C) 2026 The Onyx Authors (@28spawn)
