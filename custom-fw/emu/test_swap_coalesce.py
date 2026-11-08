# SPDX-License-Identifier: GPL-3.0-only
# Copyright (C) 2026 The Onyx Authors (@28spawn)
"""Deferred swaps under fast changes: requests that pile up while the knob is turned are all kept, and one silent gap and one swap serve a whole run of clicks.
Before: one pending entry, so an amp pick followed a moment later by a cab pick lost the amp pick (the screen said one amp, the output played another), and every click was its own
fade-out / swap / half fade-in. Usage: python custom-fw/emu/test_swap_coalesce.py <dll>"""
import ctypes, sys
d = ctypes.CDLL(sys.argv[1]); d.emu_init()
d.emu_process_audio_f32.argtypes = [ctypes.POINTER(ctypes.c_float)] * 3 + [ctypes.c_int]
d.emu_midi_event.argtypes = [ctypes.c_int] * 3
d.emu_get_applied_type.argtypes = [ctypes.c_int]
d.emu_get_swap_count.restype = ctypes.c_uint32
inp, L, R = (ctypes.c_float * 64)(), (ctypes.c_float * 64)(), (ctypes.c_float * 64)()
fails = 0


def blocks(n):
    for _ in range(n):
        d.emu_process_audio_f32(inp, L, R, 64)


def pick(mod, ty):                      # NRPN: module, parameter 0x40 (type), value = the stock id
    for c, v in ((99, mod), (98, 0x40), (6, ty)):
        d.emu_midi_event(0xB0, c, v)


def check(ok, msg):
    global fails
    print(("  ok   " if ok else "  FAIL ") + msg); fails += 0 if ok else 1


AMP_MOD, CAB_MOD, AMP, CAB = 1, 5, 2, 3
blocks(300)
s0 = d.emu_get_swap_count()
pick(AMP_MOD, 7); pick(CAB_MOD, 4); blocks(400)
check(d.emu_get_applied_type(AMP) == 7 and d.emu_get_applied_type(CAB) == 4, f"an amp pick followed at once by a cab pick: both are applied (amp {d.emu_get_applied_type(AMP)}, cab {d.emu_get_applied_type(CAB)})")
check(d.emu_get_swap_count() - s0 == 1, f"...in ONE swap ({d.emu_get_swap_count() - s0})")
s0 = d.emu_get_swap_count()
for k in range(20):                      # the knob turned through 20 amps, one click every 4 blocks (6 ms)
    pick(AMP_MOD, k % 20); blocks(4)
blocks(500)
check(d.emu_get_applied_type(AMP) == 19, f"20 quick amp clicks: the last one wins ({d.emu_get_applied_type(AMP)})")
check(d.emu_get_swap_count() - s0 <= 2, f"...with at most 2 swaps instead of 20 ({d.emu_get_swap_count() - s0})")
s0 = d.emu_get_swap_count()
d.emu_midi_event(0xC0, 9, 0); blocks(12); pick(AMP_MOD, 3); blocks(500)
check(d.emu_get_applied_type(AMP) == 3, f"a preset change followed by an amp pick: the pick comes after the preset's own amp ({d.emu_get_applied_type(AMP)})")
pick(AMP_MOD, 11); blocks(12); d.emu_midi_event(0xC0, 2, 0); blocks(500)
check(d.emu_get_applied_type(AMP) != 11, f"an amp pick followed by a preset change: the preset's amp wins ({d.emu_get_applied_type(AMP)})")
pick(CAB_MOD, 9); blocks(100)
check(d.emu_get_applied_type(CAB) == 9, f"a single cab pick is applied within 100 blocks ({d.emu_get_applied_type(CAB)})")
print("RESULT:", "FAILED" if fails else "ALL PASSED"); sys.exit(1 if fails else 0)
