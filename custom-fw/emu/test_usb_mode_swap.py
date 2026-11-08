# SPDX-License-Identifier: GPL-3.0-only
# Copyright (C) 2026 The Onyx Authors (@28spawn)
"""USB audio modes (AMP / INTERFACE / IF+MONITOR): a preset change must still be applied. In the two interface modes the chain is bypassed, and the fade state machine (which the swap waits on for
the mute) was only advanced by the chain's output stage: the swap never ran, the screen's redraw hold waited for it for ever and the presets looked stuck until a restart.
Usage: python custom-fw/emu/test_usb_mode_swap.py <dll>"""
import ctypes, sys
d = ctypes.CDLL(sys.argv[1]); d.emu_init()
d.emu_process_audio_f32.argtypes = [ctypes.POINTER(ctypes.c_float)] * 3 + [ctypes.c_int]
d.emu_midi_event.argtypes = [ctypes.c_int] * 3
d.emu_get_swap_count.restype = ctypes.c_uint32
inp, L, R = (ctypes.c_float * 64)(), (ctypes.c_float * 64)(), (ctypes.c_float * 64)()
def blocks(n):
    for _ in range(n): d.emu_process_audio_f32(inp, L, R, 64)
fails = 0
def check(ok, msg):
    global fails
    print(("  ok   " if ok else "  FAIL ") + msg); fails += 0 if ok else 1
blocks(300); d.emu_usb_audio_start()
prog = 3
for mode, name in ((1, "INTERFACE"), (2, "IF+MONITOR"), (0, "AMP"), (2, "IF+MONITOR again"), (0, "AMP again")):
    for c, v in ((99, 8), (98, 19), (6, mode)): d.emu_midi_event(0xB0, c, v)
    blocks(300)
    s0 = d.emu_get_swap_count(); prog += 5
    d.emu_midi_event(0xC0, prog % 40, 0); blocks(400)
    check(d.emu_get_swap_count() - s0 >= 1 and d.emu_get_ui(32) == prog % 40, f"{name}: a preset change is applied (swaps {d.emu_get_swap_count() - s0}, preset {d.emu_get_ui(32)})")
print("RESULT:", "FAILED" if fails else "ALL PASSED"); sys.exit(1 if fails else 0)
