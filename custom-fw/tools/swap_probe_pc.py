#!/usr/bin/env python3
"""
custom-fw/tools/swap_probe_pc.py - READ-ONLY: the swap probe of pack 158 (src/sys/smp_cpu1.c SWAP PROBE, SRAM 0x01C7D240): how long did each preset / model swap take, and how long did core 1's first
amp job after it take? Why did a swap switch the amp core off? Prints the last swaps, oldest first, in microseconds.

    python custom-fw/tools/swap_probe_pc.py            (the unit on USB, nothing else holding its MIDI port)

Use decode(words) from the other tools (core1_watch_pc.py prints new records as they appear).
"""
import struct, sys
from pathlib import Path
sys.path.insert(0, str(Path(__file__).resolve().parent))

ADDR, NREC = 0x01C7D240, 6
STATE = ((1, "recorded"), (2, "first job in time"), (4, "first job was LATE (waited for again, blocks silent meanwhile)"), (8, "wait before the swap timed out"))

def us(t): return t / 24.0

def decode(w, only_idx=None):
    """w = the 40 words; returns text lines for the records (oldest first)"""
    if w[0] != 0x44575053:
        return ["no swap probe (build older than pack 158, or the memory was not initialised)"]
    n, nxt = w[1], w[2] % NREC
    order = [(nxt + k) % NREC for k in range(NREC)]
    lines = []
    for idx in order:
        r = w[4 + 6 * idx: 10 + 6 * idx]
        if not (r[0] >> 24) & 1: continue
        if only_idx is not None and idx not in only_idx: continue
        kind, a, b, st = r[0] & 0xFF, (r[0] >> 8) & 0xFF, (r[0] >> 16) & 0xFF, r[0] >> 24
        what = f"preset (amp {a}, cab {b})" if kind == 1 else f"model change (slot {a}, type {b})"
        flags = ", ".join(t for bit, t in STATE if st & bit and bit != 1)
        lines.append(f"  {what}: waited for core 1 {us(r[1]):.0f} us, swap on core 0 {us(r[2]):.0f} us; first job after it: core 0 waited {us(r[3]):.0f} us, "
                     f"core 1's own time {us(r[4]):.0f} us   [{flags or 'first job not finished yet'}]")
    return [f"swaps seen since boot: {n}; blocks in which core 1 was late, all told: {w[3]}"] + lines

def main():
    import ble_matrix_pc as m
    u = m.Unit("Custom MIDI")
    print("BUILD", u.build_id())
    w = struct.unpack("<40I", u.ram(ADDR, 160))
    print("\n".join(decode(w)))

if __name__ == "__main__":
    main()
