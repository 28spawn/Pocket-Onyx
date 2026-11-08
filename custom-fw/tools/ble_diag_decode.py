#!/usr/bin/env python3
"""
custom-fw/tools/ble_diag_decode.py - decode the Bluetooth link instrumentation block (include/pa_ble_diag.h) and print the report.

The block is 2,456 bytes of plain data: the last 256 connection events (heard / CRC bad / channel / interrupt timing), per-channel seen and heard counters in two alignments, the
interrupt-duration histogram, the channel map in force and the experiment profile word. Both readers use this module: jl_ram_read.py bleev (a download-mode readout) and
ble_matrix_pc.py (live over USB-MIDI).

    python custom-fw/tools/ble_diag_decode.py dump.bin       # decode a saved block
"""
from __future__ import annotations

import statistics
import struct
import sys

MAGIC = 0x47444C42
SIZE = 2456
RING_N = 256
HIST_EDGES = ["<=50", "<=100", "<=200", "<=300", "<=500", "<=1000", ">1000"]
CTX = ["advertising", "connected", "connected+enc"]
PROFILE_BITS = ["NO_ENC", "CHM_LATE", "CI30", "WIDEN2", "UPD_LATE", "HW_REMAP", "VENDOR_WIN"]
HEAD = struct.Struct("<7I")                 # magic size profile wr hop chm_lo chm_hi            (28 bytes)
OFF_TABLES = 28
OFF_HIST = 324
OFF_RING = 408


def decode(buf: bytes) -> dict:
    if len(buf) < SIZE:
        raise ValueError(f"block is {len(buf)} bytes, expected {SIZE}")
    magic, size, profile, wr, hop, chm_lo, chm_hi = HEAD.unpack_from(buf, 0)
    if magic != MAGIC:
        raise ValueError(f"no instrumentation block here (magic {magic:#010x})")
    if size != SIZE:
        raise ValueError(f"block size {size} in the firmware, {SIZE} in this tool: the layouts differ")
    seen0 = list(struct.unpack_from("<37H", buf, OFF_TABLES))
    heard0 = list(struct.unpack_from("<37H", buf, OFF_TABLES + 74))
    seen1 = list(struct.unpack_from("<37H", buf, OFF_TABLES + 148))
    heard1 = list(struct.unpack_from("<37H", buf, OFF_TABLES + 222))
    hist = [[list(struct.unpack_from("<7H", buf, OFF_HIST + 2 * 7 * (irq * 3 + c))) for c in range(3)] for irq in range(2)]
    ring = []
    for i in range(RING_N):
        evt, flags, ch, dt, ch_alt, used = struct.unpack_from("<HBBhBB", buf, OFF_RING + 8 * i)
        ring.append(dict(i=i, evt=evt, heard=bool(flags & 1), crc=bool(flags & 2), direct=bool(flags & 4), enc=bool(flags & 8), ch=ch, ch_alt=ch_alt, dt=dt, used=used))
    n = min(wr, RING_N)
    order = [(wr - n + k) % RING_N for k in range(n)]                  # oldest first
    return dict(profile=profile, wr=wr, hop=hop, chm=chm_lo | chm_hi << 32, seen0=seen0, heard0=heard0, seen1=seen1, heard1=heard1,
                hist=hist, events=[ring[i] for i in order])


def profile_text(p: int) -> str:
    on = [n for b, n in enumerate(PROFILE_BITS) if p >> b & 1]
    return f"{p:#04x} ({', '.join(on) if on else 'none'})"


def _spread(seen, heard):
    """how unequal the per-channel heard rates are (standard deviation of the rate over the channels with enough events)"""
    rates = [heard[c] / seen[c] for c in range(37) if seen[c] >= 8]
    return (statistics.pstdev(rates) if len(rates) > 2 else 0.0), len(rates)


def report(d: dict, indent: str = "  ") -> str:
    out = []
    ev = d["events"]
    used = [c for c in range(37) if d["chm"] >> c & 1]
    out.append(f"{indent}profile {profile_text(d['profile'])}; hop {d['hop']}; channel map {d['chm']:#012x} = {len(used)} channels used: {' '.join(str(c) for c in used)}")
    if not ev:
        out.append(f"{indent}no connection events recorded yet")
        return "\n".join(out)
    heard = sum(e["heard"] for e in ev)
    crc = sum(e["crc"] for e in ev)
    direct = [e for e in ev if e["direct"]]
    remap = [e for e in ev if not e["direct"]]
    out.append(f"{indent}events recorded in all: {d['wr']}; the last {len(ev)}: heard {heard} ({100 * heard // len(ev)}%), CRC bad {crc}; on channels the map uses {sum(e['heard'] for e in direct)}/{len(direct)}, "
               f"on remapped channels {sum(e['heard'] for e in remap)}/{len(remap)}")
    dts = [e["dt"] for e in ev if e["dt"] != 0]
    if dts:
        out.append(f"{indent}interrupt time against the interval (us): mean {statistics.mean(dts):+.0f}, std {statistics.pstdev(dts):.0f}, worst {max(dts, key=abs):+d}; "
                   f"{sum(abs(x) > 500 for x in dts)} of {len(dts)} off by more than 0.5 ms")
    run = max_run = 0
    for e in ev:
        run = 0 if e["heard"] else run + 1
        max_run = max(max_run, run)
    out.append(f"{indent}longest run of events with nothing heard: {max_run}")
    out.append(f"{indent}last 64 events (# heard, x CRC bad, . nothing): " + "".join("x" if e["crc"] else "#" if e["heard"] else "." for e in ev[-64:]))
    sp0, n0 = _spread(d["seen0"], d["heard0"])
    sp1, n1 = _spread(d["seen1"], d["heard1"])
    out.append(f"{indent}per-channel heard rate spread: alignment 0 {sp0:.3f} over {n0} channels, alignment 1 {sp1:.3f} over {n1}  (the alignment with the larger spread is the right one; both near 0 = every channel behaves alike)")
    out.append(f"{indent}channel  MHz  used |   seen0 heard0   rate |   seen1 heard1   rate")
    for c in range(37):
        mhz = 2404 + 2 * c if c <= 10 else 2428 + 2 * (c - 11)
        r0 = f"{100 * d['heard0'][c] // d['seen0'][c]:3d}%" if d["seen0"][c] else "  - "
        r1 = f"{100 * d['heard1'][c] // d['seen1'][c]:3d}%" if d["seen1"][c] else "  - "
        out.append(f"{indent}   {c:2d}   {mhz}  {'yes' if c in used else ' - '}  | {d['seen0'][c]:6d} {d['heard0'][c]:6d}  {r0} | {d['seen1'][c]:6d} {d['heard1'][c]:6d}  {r1}")
    out.append(f"{indent}radio interrupt durations (counts) " + "  ".join(f"{h:>6}" for h in HIST_EDGES) + "  us")
    for irq, name in ((0, "IRQ 45 event"), (1, "IRQ 29 RX")):
        for c in range(3):
            row = d["hist"][irq][c]
            if sum(row):
                out.append(f"{indent}  {name:13s} {CTX[c]:14s} " + "  ".join(f"{v:6d}" for v in row))
    return "\n".join(out)


if __name__ == "__main__":
    if len(sys.argv) != 2:
        print(__doc__)
        sys.exit(2)
    print(report(decode(open(sys.argv[1], "rb").read())))

# SPDX-License-Identifier: GPL-3.0-only
# Copyright (C) 2026 The Onyx Authors (@28spawn)
