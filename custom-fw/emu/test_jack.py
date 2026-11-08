# SPDX-License-Identifier: GPL-3.0-only
# Copyright (C) 2026 The Onyx Authors (@28spawn)
"""End-to-end check of the emulator's new JACK path.

The emulator no longer runs its own audio loop: it converts the host's float
samples to 24-bit and hands them to the firmware's jack (the DEVICE's audio
I/O path), then converts the jack output back. This drives the DLL exactly the
way the Python server does and checks that the audio actually comes out.

The metric is the RMS: the chain's start-up transient (the cabinet IR's onset)
decays over ~170 ms, and the per-block peak drifts because the convolution
rings across the block boundary. The RMS settles and stays put.
"""
import ctypes, math, sys

dll_path = sys.argv[1]
d = ctypes.CDLL(dll_path)

d.emu_init.restype = None
d.emu_init()

d.emu_process_audio_f32.argtypes = [ctypes.POINTER(ctypes.c_float), ctypes.POINTER(ctypes.c_float),
                                    ctypes.POINTER(ctypes.c_float), ctypes.c_int]
d.emu_ui_input.argtypes = [ctypes.c_uint, ctypes.c_int32, ctypes.c_uint]

N = 64
inp  = (ctypes.c_float * N)()
outL = (ctypes.c_float * N)()
outR = (ctypes.c_float * N)()

_phase = [0]

def run(frames, amp=0.4):
    """Feed `frames` jack quanta and return the output's RMS."""
    acc = 0.0
    n = 0
    for k in range(frames):
        base = _phase[0]
        for i in range(N):
            inp[i] = amp * math.sin(2 * math.pi * 220 * (base + i) / 44100.0)
        _phase[0] += N
        d.emu_process_audio_f32(inp, outL, outR, N)
        for i in range(N):
            acc += outL[i] * outL[i]
            n += 1
    return math.sqrt(acc / n)

fails = 0
def check(ok, msg):
    global fails
    print(("  ok   " if ok else "  FAIL ") + msg)
    if not ok:
        fails += 1

def set_pot(v):
    """v is 0..100 %; the firmware's pot is a 12-bit SARADC (0..4095)."""
    d.emu_ui_input(0, 0, int(round(v / 100 * 4095)))

print("=== the emulator's jack path produces audio ===")
# The emulator now persists its settings to the flash, exactly like the
# device, so the OUTPUT LEVEL it boots with is whatever was saved last. Pin
# it before measuring.
set_pot(100)
# 3 s to settle and 1 s measured: the cabinet IR onset, and the factory preset's delay and
# reverb tails (preset 1 has both) reaching their steady state
run(2100)
loud = run(700)
check(loud > 0.01, f"the jack output carries signal (rms {loud:.4f})")

print("\n=== the master pot reaches the firmware's jack ===")
set_pot(0)
run(60)                          # let the fade-out finish
quiet = run(120)
check(quiet < loud, f"the master pot lowers the output (rms {loud:.4f} -> {quiet:.4f})")
check(quiet <= 0.001, f"at 0 % only the dither is left (rms {quiet:.6f})")

set_pot(100)
run(2100)                        # the fade-in, and the delay / reverb back at steady state
back = run(700)
check(abs(back - loud) / loud < 0.02, f"and back up again (rms {loud:.4f} -> {back:.4f})")

print("\n" + ("EMULATOR JACK OK" if fails == 0 else f"{fails} FAILURE(S)"))
sys.exit(0 if fails == 0 else 1)
