#!/usr/bin/env python3
"""
custom-fw/tools/boot_input_trace_pc.py - the RAW input signal after the audio started: its peak and mean per 46 ms for the first 8.8 s (pack 177). READ-ONLY.
Answers "what is the 17 % raw input peak since boot": a steady offset that settles (the input stage powering up) shows in the mean column, a click or a burst of noise in the peak column,
and the time says whether it comes with the boot (the first buckets) or later. Run it right after a power-on (the trace is kept in RAM; a power cycle clears it).

    python custom-fw/tools/boot_input_trace_pc.py [--elf custom-fw/build_pi32/pocket_amp_custom.elf]
"""
import argparse
import struct
import subprocess
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
import ble_matrix_pc as m

NM = Path.home() / "tools/jieli/C$/JL/pi32/bin/llvm-nm.exe"


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--elf", default=str(Path(__file__).resolve().parent.parent / "build_pi32" / "pocket_amp_custom.elf"))
    a = ap.parse_args()
    out = subprocess.run([str(NM), a.elf], capture_output=True, text=True).stdout
    sym = {l.split()[2]: int(l.split()[0], 16) for l in out.splitlines() if len(l.split()) == 3}
    u = m.Unit("Custom MIDI")
    print("BUILD", u.build_id(), "(the ELF must be the one of this build: the addresses come from it)")
    # read the two arrays in the same pass, 16 buckets at a time: reading one whole array and then the other takes a second, in which more buckets were written to the second
    pk, mean = [], []
    for k in range(0, 192, 16):
        pk += struct.unpack("<16H", bytes(u.ram(sym["g_pa_in_trace"] + 2 * k, 32)))
        mean += struct.unpack("<16h", bytes(u.ram(sym["g_pa_in_trace_mean"] + 2 * k, 32)))
    print(" t(s)   peak  (of 32767)    mean")
    for i in range(192):
        if pk[i] == 0 and mean[i] == 0 and i > 0 and pk[i - 1] == 0:
            continue
        print(f"{i * 0.0464:5.2f}  {pk[i]:6d}  {100.0 * pk[i] / 32767:5.1f} %   {mean[i]:6d}")
    mx = max(pk)
    print(f"largest peak in the first 8.8 s: {mx} = {100.0 * mx / 32767:.1f} % of full scale, at t = {pk.index(mx) * 0.0464:.2f} s")


if __name__ == "__main__":
    main()

# SPDX-License-Identifier: GPL-3.0-only
# Copyright (C) 2026 The Onyx Authors (@28spawn)
