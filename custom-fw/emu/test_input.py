# SPDX-License-Identifier: GPL-3.0-only
# Copyright (C) 2026 The Onyx Authors (@28spawn)
"""Verify that the emulator now runs the FIRMWARE'S input model.

The emulator used to call pa_ui_handle_inputs() directly with the raw button
mask, so a "press" was whatever the browser happened to send: no debounce, no
one-tick pulses, no HOLD bits and no chord combos. The device and the browser
ran the portable input layer (src/sys/pa_input.c); the emulator did not.

Now the emulator runs pa_audio_core_tick() - the same tick the device's main
loop runs and the browser's worklet runs - so a tap is exactly one action, a
held button repeats after the delay, and the MASTER pot reaches the DAC.

This drives the DLL exactly the way the Python server does.
"""
import ctypes, sys

d = ctypes.CDLL(sys.argv[1])
d.emu_init()

d.emu_ui_input.argtypes = [ctypes.c_uint32, ctypes.c_int32, ctypes.c_uint16]
d.emu_process_audio_f32.argtypes = [ctypes.POINTER(ctypes.c_float), ctypes.POINTER(ctypes.c_float),
                                    ctypes.POINTER(ctypes.c_float), ctypes.c_int]

N = 64
inp = (ctypes.c_float * N)()
outL = (ctypes.c_float * N)()
outR = (ctypes.c_float * N)()

LEFT = 1 << 3
PARA = 1 << 5
HOME = 1 << 0


class Telemetry(ctypes.Structure):
    _fields_ = [
        ("view", ctypes.c_uint32), ("preset_index", ctypes.c_uint32),
        ("preset_volume", ctypes.c_uint32), ("master_volume", ctypes.c_uint32),
        ("selected_slot", ctypes.c_uint32), ("selected_param", ctypes.c_uint32),
        ("slot_active_mask", ctypes.c_uint32), ("sram_used_bytes", ctypes.c_uint32),
        ("sram_peak_bytes", ctypes.c_uint32), ("cpu_total_permille", ctypes.c_uint32),
        ("cpu_stage_permille", ctypes.c_uint32 * 8),
        ("tuner_active", ctypes.c_uint32), ("tuner_locked", ctypes.c_uint32),
        ("tuner_note", ctypes.c_uint32), ("tuner_octave", ctypes.c_int32),
        ("tuner_cents", ctypes.c_int32), ("tuner_freq_tenths", ctypes.c_uint32),
        ("tuner_dots", ctypes.c_uint32), ("drum_playing", ctypes.c_uint32),
        ("drum_bpm", ctypes.c_uint32), ("drum_pattern", ctypes.c_uint32),
        ("in_peak", ctypes.c_float), ("out_peak", ctypes.c_float),
        ("bt_state", ctypes.c_uint32), ("cursor_slot", ctypes.c_uint32),
        ("editor_cursor", ctypes.c_uint32),
    ]


tel = Telemetry()
d.emu_get_telemetry.argtypes = [ctypes.POINTER(Telemetry)]

# the firmware's tick runs every 11 audio blocks (64 frames at 44.1 kHz)
TICK_BLOCKS = 11


def blocks(n):
    for _ in range(n):
        for i in range(N):
            inp[i] = 0.0
        d.emu_process_audio_f32(inp, outL, outR, N)


def tick(n=1):
    """Run n firmware ticks (11 audio blocks each)."""
    blocks(n * TICK_BLOCKS)
    d.emu_get_telemetry(ctypes.byref(tel))


def state():
    d.emu_get_telemetry(ctypes.byref(tel))
    return tel.view, tel.preset_index, tel.selected_slot, tel.master_volume     # [1]: the preset (the < > buttons pick it; the PARA knob moves the block cursor since 2026-11-03)


fails = 0
def check(ok, msg):
    global fails
    print(("  ok   " if ok else "  FAIL ") + msg)
    if not ok:
        fails += 1


print("=== the emulator runs the firmware's tick ===")
tick(20)
v0, c0, _, _ = state()
check(v0 == 0, f"the firmware booted into the home grid (view {v0})")

print("\n=== a tap is exactly ONE action ===")
before = state()
d.emu_ui_input(LEFT, 0, 0xFFFF)     # press
tick(4)
d.emu_ui_input(0, 0, 0xFFFF)        # release
tick(20)
after = state()
check((after[1] - before[1]) % 80 in (1, 79),
      f"one tap changed the preset by ONE step ({before[1]} -> {after[1]})")

print("\n=== a held button does NOT run away ===")
d.emu_ui_input(LEFT, 0, 0xFFFF)
tick(3)
held0 = state()
tick(6)                             # ~100 ms: inside the pre-repeat delay
held1 = state()
check(held1[1] == held0[1], f"no movement during the repeat delay ({held0[1]} -> {held1[1]})")

print("\n=== ... and then it repeats ===")
tick(30)                            # past the 350 ms delay
held2 = state()
check(held2[1] != held1[1], f"it started repeating ({held1[1]} -> {held2[1]})")
d.emu_ui_input(0, 0, 0xFFFF)
tick(3)

print("\n=== the MASTER pot reaches the firmware ===")
d.emu_ui_input(0, 0, 0)             # 0 %
tick(30)
m0 = state()[3]
d.emu_ui_input(0, 0, 4095)          # 100 %
tick(30)
m1 = state()[3]
check(m0 == 0 and m1 == 100, f"the pot drives the master volume ({m0} % -> {m1} %)")

print("\n" + ("EMULATOR INPUT MODEL OK" if fails == 0 else f"{fails} FAILURE(S)"))
sys.exit(0 if fails == 0 else 1)
