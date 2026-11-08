#!/usr/bin/env python3
"""
custom-fw/emu/test_block_adapter.py - the emulator must not crackle whatever frame count
the host's audio API delivers.

The firmware runs 64-sample blocks. The old emu_process_audio_f32 ran a full block for
the last, short chunk of any call that was not a multiple of 64 (input padded with zeros,
extra output thrown away): the DSP moved 64 samples while the stream moved fewer, so every
such call spliced zeros into the signal - a steady crackle with a 48 kHz interface (the
resampler hands over varying counts), ASIO buffers like 441/480, or "native" WASAPI/MME
blocks. Measured before the fix: 100-frame calls differed from 64-frame calls by up to
0.4 full scale.

Checks, on the real DLL:
  * multiples of 64 (64, 128, 256, 1024): bit-identical to 64-frame calls, no added latency
  * anything else (100, 441, 470/471, 1000, random 1..700, 5000 > the internal slice):
    bit-identical to the 64-frame reference delayed by exactly 64 samples
  * the output queue never underruns
Runs are compared within 2e-6 (a few 24-bit LSBs), not bit for bit: the output stage's
TPDF dither keeps its random state across emu_init(), and the very first run after the DLL
loads starts from slightly different firmware state, so one warm-up run is done first.

    python custom-fw/emu/test_block_adapter.py [path/to/pocket_amp_emu.dll]
"""
import ctypes
import os
import random
import sys

import numpy as np

HERE = os.path.dirname(os.path.abspath(__file__))
DLL = sys.argv[1] if len(sys.argv) > 1 else os.path.join(HERE, "pocket_amp_emu.dll")
d = ctypes.CDLL(os.path.abspath(DLL))
fp = ctypes.POINTER(ctypes.c_float)
d.emu_process_audio_f32.argtypes = [fp, fp, fp, ctypes.c_int]
d.emu_get_block_adapter_state.restype = ctypes.c_uint32
SR = 44100
TOTAL = 1024 * 130            # ~3 s, a whole number of 64/128/256/1024-frame calls
LAT = 64

fails = 0


def check(ok, msg):
    global fails
    print(("  ok   " if ok else "  FAIL ") + msg)
    if not ok:
        fails += 1


def signal(n):
    t = np.arange(n) / SR
    k = np.arange(n) % (SR // 2)
    env = np.exp(-k / (0.15 * SR))
    return (0.3 * env * (np.sin(2 * np.pi * 110 * t) + 0.5 * np.sin(2 * np.pi * 220 * t))).astype(np.float32)


X = signal(TOTAL)


def run(sizes):
    d.emu_init()
    yl = np.zeros(TOTAL, np.float32)
    yr = np.zeros(TOTAL, np.float32)
    pos, i = 0, 0
    while pos < TOTAL:
        n = min(sizes(i), TOTAL - pos)
        i += 1
        xi = np.ascontiguousarray(X[pos:pos + n])
        ol = np.zeros(n, np.float32)
        orr = np.zeros(n, np.float32)
        d.emu_process_audio_f32(xi.ctypes.data_as(fp), ol.ctypes.data_as(fp), orr.ctypes.data_as(fp), n)
        yl[pos:pos + n] = ol
        yr[pos:pos + n] = orr
        pos += n
    return yl, yr, d.emu_get_block_adapter_state()


TOL = 2e-6
run(lambda i: 64)                                   # warm-up (see the docstring)
ref_l, ref_r, st = run(lambda i: 64)
check(st & 1 == 0, "64-frame calls run straight through (no adapter, no added latency)")
check(float(np.abs(ref_l).max()) > 0.01, "the reference is not silent")

for n in (128, 256, 1024):
    yl, yr, st = run(lambda i, n=n: n)
    check(st & 1 == 0 and np.allclose(yl, ref_l, atol=TOL) and np.allclose(yr, ref_r, atol=TOL),
          f"{n}-frame calls: identical to 64-frame calls, no added latency")

rng = random.Random(7)
cases = {
    "100": lambda i: 100,
    "441": lambda i: 441,
    "470/471/470 (a 48 kHz resampler)": lambda i: (470, 471, 470)[i % 3],
    "1000": lambda i: 1000,
    "5000 (more than one internal slice)": lambda i: 5000,
    "random 1..700": lambda i: rng.randint(1, 700),
}
for name, fn in cases.items():
    yl, yr, st = run(fn)
    same = np.allclose(yl[LAT:], ref_l[:-LAT], atol=TOL) and np.allclose(yr[LAT:], ref_r[:-LAT], atol=TOL)
    silent_head = not np.any(yl[:LAT]) and not np.any(yr[:LAT])
    under = st >> 8
    check(same and silent_head and under == 0,
          f"{name}-frame calls: the 64-frame output delayed by exactly {LAT} samples"
          f" (max diff {float(np.abs(yl[LAT:] - ref_l[:-LAT]).max()):.2e}, underruns {under})")

print("\nBLOCK ADAPTER OK" if not fails else f"\n{fails} FAILED")
sys.exit(1 if fails else 0)

# SPDX-License-Identifier: GPL-3.0-only
# Copyright (C) 2026 The Onyx Authors (@28spawn)
