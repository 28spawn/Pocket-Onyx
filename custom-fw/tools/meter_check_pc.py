#!/usr/bin/env python3
"""
custom-fw/tools/meter_check_pc.py - does the OUT level bar on the screen show how loud the output really is? READ-ONLY.

The bar is drawn from ui->out_peak_pct (pa_audio_core.c: the last 64-sample block's output peak, mapped linearly over -45 dBFS .. 0 dBFS = 0 .. 100 %, refreshed at most every 66 ms and only when it moved 4 % or more),
12 segments: 1-8 green (up to 66 %, -15 dBFS), 9-11 yellow (67 - 95 %, -15 .. -2.3 dBFS), the 12th red (96 % and over, -1.8 dBFS and over). No peak hold: a pluck that lasts 10 ms is mostly missed.

While you play LOUD (master knob at maximum, the loudest sound you can make) this samples, over the diag channel, the true peaks the audio path records and what the bar was told:
    - the largest sample since boot after the tail (SRAM 0x01C7D300 word 5; the master level is applied after this point, 1.0 at the maximum knob)
    - the same for the last audio block (word 13), sampled as fast as the channel allows: the loudest block seen
    - the bar's value (TSTA word 5, byte 3) and the number of lit segments it means
and prints what the bar should have shown against what it showed.

    python custom-fw/tools/meter_check_pc.py [--seconds 10]       start it, then play loud
"""
import argparse, math, struct, sys, time
from pathlib import Path
sys.path.insert(0, str(Path(__file__).resolve().parent))
import ble_matrix_pc as m

ONE = 2097152.0           # Q21 full scale

def db(v): return 20 * math.log10(v / ONE) if v > 0 else -120.0
def pct(d): return max(0, min(100, int((d + 45.0) / 45.0 * 100.0)))
def segs(p): return min(12, (p * 12 + 50) // 100)

def main():
    ap = argparse.ArgumentParser(); ap.add_argument("--seconds", type=float, default=10.0); a = ap.parse_args()
    u = m.Unit("Custom MIDI")
    print("BUILD", u.build_id())
    lv0 = struct.unpack("<16I", u.ram(0x01C7D300, 64))
    print(f"since boot so far: after the tail {db(lv0[5]):.1f} dBFS, right channel {db(lv0[6]):.1f} dBFS")
    print(f"play LOUD now for {a.seconds:.0f} s ...", flush=True)
    t0 = time.time(); worst_block = 0; max_bar = 0; n = 0; seen = []; worst_dac = 0
    while time.time() - t0 < a.seconds:
        try:
            w = struct.unpack("<4I", u.ram(0x01C7D300 + 4 * 12, 16))          # words 12..15 : last-block levels (stage 4 = [12], 5 = [13])
            bar = u.ram(0x01C0D840 + 31, 1)[0]                                 # ui->out_peak_pct, what the bar is drawn from
            dac = struct.unpack("<I", u.ram(0x01C1CE34 + 12, 4))[0]            # g_pa_audio.output_peak_s24: the last block's peak AT THE DAC (after the master level)
        except Exception:
            continue
        n += 1
        worst_block = max(worst_block, w[1]); max_bar = max(max_bar, bar); worst_dac = max(worst_dac, dac)
        seen.append(bar)
    lv1 = struct.unpack("<16I", u.ram(0x01C7D300, 64))
    since = lv1[5]
    print(f"\nsamples taken: {n} ({n / a.seconds:.0f}/s)")
    print(f"true peak: since boot after the tail {db(since):.1f} dBFS ({100 * since / ONE:.0f} % of full scale); loudest block I caught while sampling {db(worst_block):.1f} dBFS")
    ddb = 20 * math.log10(max(worst_dac, 1) / 8388608.0)
    print(f"at the DAC (after the master level) the loudest block I caught: {ddb:.1f} dBFS -> the bar should show {pct(ddb)} % for that block; it showed at most {max_bar} % in my samples")
    should = pct(db(since))
    print(f"the bar should reach {should} % = {segs(should)} of 12 segments for that peak ({'red' if segs(should) >= 12 else 'yellow' if segs(should) > 8 else 'green'})")
    print(f"the bar was told at most {max_bar} % = {segs(max_bar)} of 12 segments ({'red' if segs(max_bar) >= 12 else 'yellow' if segs(max_bar) > 8 else 'green'}) in the moments I looked")
    print("verdict: " + ("the bar underreports: the true peak is higher than the bar shows (it samples one block per refresh, no peak hold)" if segs(max_bar) < segs(should) else "the bar matches the true peak"))
    return 0

if __name__ == "__main__":
    sys.exit(main())

# SPDX-License-Identifier: GPL-3.0-only
# Copyright (C) 2026 The Onyx Authors (@28spawn)
