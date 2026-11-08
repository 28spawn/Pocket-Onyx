#!/usr/bin/env python3
"""
custom-fw/tools/preset_perf_pc.py - per-preset performance test (pack 158): select each preset by MIDI Program Change, let it settle, and report the audio block time, the governor level, whether core 1 is still
online, the overruns it added and the swap probe's numbers for that swap. It only selects presets (nothing is stored) and reads RAM.

    python custom-fw/tools/preset_perf_pc.py [--presets 0-39] [--settle 3.0] [--order 0,1,2]     the preset selected before the run is not restored: pick yours afterwards
"""
import argparse, struct, sys, time
from pathlib import Path
sys.path.insert(0, str(Path(__file__).resolve().parent))
import ble_matrix_pc as m
import swap_probe_pc as sp

def snap(u):
    w = struct.unpack("<8I", u.ram(0x01C7D200, 32)); g = struct.unpack("<I", u.ram(0x01C7D4C0, 4))[0]; s = struct.unpack("<6I", u.ram(0x01C7C250, 24))
    return dict(on=w[1], why=w[6], level=g & 0xFF, avg=g >> 8, entries=s[0], over=s[3], isrmax=s[2] / 24, job=w[4] / 24)

def probe(u):
    return struct.unpack("<40I", u.ram(0x01C7D240, 64) + u.ram(0x01C7D280, 64) + u.ram(0x01C7D2C0, 32))

def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--presets", default="0-39"); ap.add_argument("--order", default=""); ap.add_argument("--settle", type=float, default=3.0)
    a = ap.parse_args()
    if a.order: seq = [int(x) for x in a.order.split(",")]
    else:
        lo, _, hi = a.presets.partition("-"); seq = list(range(int(lo), int(hi or lo) + 1))
    u = m.Unit("Custom MIDI")
    print("BUILD", u.build_id(), flush=True)
    bad = 0
    for p in seq:
        b = snap(u); n0 = probe(u)[1]
        u.mo.send_message([0xC0, p]); time.sleep(a.settle)
        e = snap(u); w = probe(u)
        line = f"P{p + 1:02d}: core1 {'ON ' if e['on'] else 'OFF'} L{e['level']} avg block {e['avg']:4d} us  overruns +{e['over'] - b['over']:<4d} isr max {e['isrmax']:6.0f} us"
        if w[1] != n0:
            last = sp.decode(w, only_idx=[(w[2] - 1) % 6])
            line += "\n      " + (last[-1].strip() if len(last) > 1 else "no record")
        print(line, flush=True)
        if not e["on"] or e["level"] or e["over"] - b["over"] > 2: bad += 1      # the swap block itself runs over (about 2.7 ms on core 0, the output muted): 1-2 overruns per swap are expected
    print(f"RESULT: {len(seq)} presets, {bad} with core 1 off, a raised governor level or more than 2 new overruns -> {'PASS' if not bad else 'FAIL'}")
    return 1 if bad else 0

if __name__ == "__main__":
    sys.exit(main())

# SPDX-License-Identifier: GPL-3.0-only
# Copyright (C) 2026 The Onyx Authors (@28spawn)
