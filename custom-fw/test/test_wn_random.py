#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-only
# Copyright (C) 2026 The Onyx Authors (@28spawn)
"""custom-fw/test/test_wn_random.py - the WaveNet block engine against the float reference on RANDOM A2-Lite models (several seeds, several block sizes).

The real captures (test_wn_block.py) test what ships; random models test what a capture might contain that the shipped ones do not: weights of either sign
at every tap, large biases, a mono layer 0 with unequal rechannel weights. Weights are drawn small enough that the net stays in the region the trained ones live in
(the largest real weight is about 1.7), so the float reference and the Q21 / Q24 engine are comparable. Same build as test_wn_block.py.
"""
import random
import struct
import subprocess
import sys
import tempfile
from pathlib import Path

FW = Path(__file__).resolve().parent.parent
K = [6] * 14 + [15, 15] + [6] * 7
SEEDS = [int(x) for x in sys.argv[1:]] or [1, 2, 3, 4, 5, 6]


def model(seed):
    r = random.Random(seed)
    w = [r.uniform(0.2, 1.2) * r.choice((-1, 1)) for _ in range(3)]            # the rechannel
    for k in K:
        sd = 0.45 / (3 * k) ** 0.5
        w += [r.gauss(0, sd) for _ in range(9 * k)]                              # conv [o][j][k]
        w += [r.gauss(0, 0.05) for _ in range(3)]                                # conv bias
        w += [r.gauss(0, 0.3) for _ in range(3)]                                 # mixin
        w += [r.gauss(0, 0.25) for _ in range(9)]                                # 1x1
        w += [r.gauss(0, 0.05) for _ in range(3)]                                # 1x1 bias
    w += [r.gauss(0, 0.12) for _ in range(48)]                                   # head
    w += [r.gauss(0, 0.02)]                                                      # head bias
    w += [0.02]                                                                  # head scale
    assert len(w) == 1871, len(w)
    return struct.pack("<4sIfIf", b"WAVE", 1871, 0.02, 0, 1.0) + struct.pack("<1871f", *w)


tmp = Path(tempfile.mkdtemp(prefix="pa_wnrnd_"))
exe = tmp / "t.exe"
cmd = ["clang", "-O2", "-Wall", "-Wextra", "-Wno-unused-function", f"-I{FW / 'include'}", "-DPA_HOST_SIM=1",
       str(FW / "test" / "test_wn_block.c"), str(FW / "src" / "nam" / "pa_wavenet.c"), str(FW / "src" / "dsp" / "hw_accel.c"), "-lm", "-o", str(exe)]
r = subprocess.run(cmd, cwd=FW, capture_output=True, text=True)
if r.returncode:
    print(r.stdout, r.stderr[-3000:])
    sys.exit("build failed")
bad = 0
for seed in SEEDS:
    f = tmp / f"m{seed}.bin"
    f.write_bytes(model(seed))
    for n in ("64", "32", "21"):
        env = {"WN_N": n}
        p = subprocess.run([str(exe), str(f)], capture_output=True, text=True, env=env)
        last = [ln for ln in p.stdout.splitlines() if "ESR" in ln]
        print(f"seed {seed} block {n}: {'PASSED' if p.returncode == 0 else 'FAILED'}  {last[0].split('error rms')[1].strip() if last else p.stdout.strip()[-120:]}")
        bad += p.returncode != 0
print("RESULT:", "ALL PASSED" if not bad else f"{bad} FAILED")
sys.exit(1 if bad else 0)
