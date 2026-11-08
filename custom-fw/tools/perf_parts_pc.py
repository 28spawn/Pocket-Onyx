#!/usr/bin/env python3
"""
custom-fw/tools/perf_parts_pc.py - READ-ONLY: where does the time of the CABINET, the DRIVE and the REVERB go, on the unit, per audio block (include/pa_prof.h, fft_conv.c FC_PROF)?

Reads the effects' timing sums from SRAM over the USB diag channel (the sums are over the last up to 2048 blocks the effect ran) and prints microseconds per block. Switch the effects on first (a preset
that uses them, or perf_monitor_pc.py --mode sweep in another run); an effect that has not run since the build started has no sums.

    python custom-fw/tools/perf_parts_pc.py
"""
import struct, sys
from pathlib import Path
sys.path.insert(0, str(Path(__file__).resolve().parent))
import ble_matrix_pc as m

def words(u, addr, n): return struct.unpack(f"<{n}I", u.ram(addr, 4 * n))

def main():
    u = m.Unit("Custom MIDI")
    print("BUILD", u.build_id())
    w = words(u, 0x01C7E440, 8)
    if w[0] == 0x52504346 and w[1]:
        n = w[1]; t = [w[i] / 24.0 / n for i in (2, 3, 4, 5)]
        print(f"CABINET   {sum(t):6.1f} us per block over {n} blocks:  buffer moves {t[0]:.1f}  forward FFT {t[1]:.1f}  multiply-accumulate {t[2]:.1f}  inverse FFT {t[3]:.1f}   ({w[6]} of {w[7]} partitions)")
    else: print("CABINET   no sums (the cabinet has not run, or the build has no FC_PROF)")
    w = words(u, 0x01C7E480, 8)
    if w[0] == 0x50565244 and w[1]:
        n = w[1]; t = [w[i] / 24.0 / n for i in range(2, 7)]
        print(f"DRIVE     {sum(t):6.1f} us per block over {n} blocks ({100 * w[7] / n:.0f} % at 4x oversampling):  gain+clean path {t[0]:.1f}  up {t[1]:.1f}  clipper {t[2]:.1f}  down {t[3]:.1f}  tone+DC+level {t[4]:.1f}")
    else: print("DRIVE     no sums (the drive has not run, or the build has no pa_prof.h)")
    w = words(u, 0x01C7E4C0, 18)
    names = ["ROOM", "HALL", "SWELL", "SPRING", "SHIMMER", "CLOUD", "PLATE", "AMBIENCE"]
    if w[0] == 0x50564552 and w[1]:
        print(f"REVERB    blocks counted {w[1]}:")
        for ty in range(8):
            if w[10 + ty]: print(f"            type {ty} {names[ty]:9s} {w[2 + ty] / 24.0 / w[10 + ty]:6.1f} us per block over {w[10 + ty]} blocks")
    else: print("REVERB    no sums (the reverb has not run, or the build has no pa_prof.h)")

if __name__ == "__main__":
    main()

# SPDX-License-Identifier: GPL-3.0-only
# Copyright (C) 2026 The Onyx Authors (@28spawn)
