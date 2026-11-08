# SPDX-License-Identifier: GPL-3.0-only
# Copyright (C) 2026 The Onyx Authors (@28spawn)
"""Every IR in the emulator's cabinet bank must load.

Python's `wave` module only understands PCM (format tag 1). Half of the bank
is IEEE float (format tag 3) - the Mesa 4x12 among them - and `wave` refused
those with "unknown format: 3", so the button that loads that cabinet did
nothing but show an error. The RIFF parser in nam_loader.py handles both,
and this checks that every file in the bank parses and gives sane taps.

Usage:  python custom-fw/emu/test_ir.py
"""
import os
import pathlib
import sys

HERE = pathlib.Path(__file__).resolve().parent
sys.path.insert(0, str(HERE))

from nam_loader import parse_wav_ir   # noqa: E402

# the bank is generated on first use (tools/gen_test_audio.py); make sure it exists
sys.path.insert(0, str(HERE.parent / "tools"))
import gen_test_audio                   # noqa: E402
gen_test_audio.ensure_emu_assets(str(HERE), log=lambda s: None)

fails = 0


def check(ok, msg):
    global fails
    print(("  ok   " if ok else "  FAIL ") + msg)
    if not ok:
        fails += 1


irs = sorted((HERE / "irs").glob("*.wav"))
print(f"=== the cabinet bank: {len(irs)} files ===")
check(len(irs) > 0, "the bank is not empty")

for p in irs:
    try:
        taps, meta = parse_wav_ir(p.read_bytes(), max_taps=1024)
    except Exception as e:
        check(False, f"{p.name}: {type(e).__name__}: {e}")
        continue
    peak = max((abs(t) for t in taps), default=0.0)
    finite = all(t == t and abs(t) < 10.0 for t in taps)
    check(len(taps) == 1024 and 0.5 < peak <= 1.0 and finite,
          f"{p.name}: {len(taps)} taps, peak {peak:.3f}, "
          f"{meta['bit_depth']}-bit tag {meta['format_tag']}")

print("\n=== the IR the emulator preloads ===")
default_ir = HERE / "default_cab_ir.wav"
if default_ir.exists():
    try:
        taps, meta = parse_wav_ir(default_ir.read_bytes(), max_taps=1024)
        check(len(taps) == 1024, f"{default_ir.name}: {len(taps)} taps")
    except Exception as e:
        check(False, f"{default_ir.name}: {type(e).__name__}: {e}")
else:
    check(False, f"{default_ir.name} is missing")

print("\n" + ("THE CABINET BANK OK" if fails == 0 else f"{fails} FAILURE(S)"))
sys.exit(0 if fails == 0 else 1)
