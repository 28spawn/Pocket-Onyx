# SPDX-License-Identifier: GPL-3.0-only
# Copyright (C) 2026 The Onyx Authors (@28spawn)
"""When the settings are written to flash (pa_audio_core.c, step 7). Each write quiesces the audio (the output mutes, the interrupts go off, the second core is parked), and the overload popup used to
read the overruns of a run of them as an overload: turning the master knob, pausing, turning it again was enough.
  - the master POT does not make the settings dirty (it is read again at every boot: the stored level is never used);
  - a real change is written only after 5 s without another change;
  - never more than once in 8 s;
  - a change back to what was stored is not written at all.
Time in the emulator is the audio blocks (1.451 ms each). Usage: python custom-fw/emu/test_settings_save.py <dll>"""
import ctypes, sys
d = ctypes.CDLL(sys.argv[1]); d.emu_init()
F = ctypes.POINTER(ctypes.c_float)
d.emu_process_audio_f32.argtypes = [F, F, F, ctypes.c_int]
d.emu_midi_event.argtypes = [ctypes.c_int] * 3
d.emu_ui_pot.argtypes = [ctypes.c_uint16]
d.emu_settings_save_seq.restype = ctypes.c_uint32
d.emu_mark_settings_dirty_ahead.argtypes = [ctypes.c_int]
inp, L, R = (ctypes.c_float * 64)(), (ctypes.c_float * 64)(), (ctypes.c_float * 64)()
fails = 0
BLK_PER_S = 44100.0 / 64.0


def blocks(n):
    for _ in range(int(n)):
        d.emu_process_audio_f32(inp, L, R, 64)


def secs(s):
    blocks(s * BLK_PER_S)


def check(ok, msg):
    global fails
    print(("  ok   " if ok else "  FAIL ") + msg); fails += 0 if ok else 1


def set_a4(v):                                  # a global setting (the tuner's A4 reference): NRPN module 8, parameter 16
    for c, x in ((99, 8), (98, 16), (6, v)):
        d.emu_midi_event(0xB0, c, x)


secs(12)                                        # boot settles; whatever the boot wanted to save is saved
s0 = d.emu_settings_save_seq(); dirty = lambda: d.emu_get_ui(25)
for pot in (500, 1500, 2500, 3500, 1200, 3000):
    d.emu_ui_pot(pot); secs(0.4)
secs(7)
check(d.emu_settings_save_seq() == s0 and dirty() == 0, f"turning the master pot to and fro makes no settings write (records {s0} -> {d.emu_settings_save_seq()}, dirty {dirty()})")

set_a4(5); secs(0.3)
check(dirty() == 1 and d.emu_settings_save_seq() == s0, "a real setting change marks the settings dirty, nothing is written yet")
secs(4)
check(d.emu_settings_save_seq() == s0, "4 s after the change: still not written (it waits for 5 s of quiet)")
secs(2)
s1 = d.emu_settings_save_seq()
check(s1 == s0 + 1 and dirty() == 0, f"6 s after the change: written once (records {s0} -> {s1})")

set_a4(7); secs(5.6)
check(d.emu_settings_save_seq() == s1 and dirty() == 1, "a second change 0.3 s later: quiet for 5 s but under 8 s since the last write: NOT written yet")
secs(3)
s2 = d.emu_settings_save_seq()
check(s2 == s1 + 1 and dirty() == 0, f"8+ s after the last write: written (records {s1} -> {s2})")

secs(9)
set_a4(7); secs(0.3)                            # the same value again
secs(7)
check(d.emu_settings_save_seq() == s2 and dirty() == 0, "a change back to what is stored is not written at all (dirty cleared, same record)")
# the stamp taken LATER in the tick than the tick's clock reading (what the device does: its UI tick takes tens to hundreds of ms): now - stamp is a small NEGATIVE number, which an unsigned
# subtraction turns into ~4.29 billion: the 5 s debounce passed at once and every change wrote the flash immediately
secs(10)
s3 = d.emu_settings_save_seq()
set_a4(9); secs(0.3)                            # the change is taken (dirty, stamped now)
d.emu_mark_settings_dirty_ahead(40); secs(1.5)  # ...and stamped again 40 ms ahead of the clock the next tick reads
check(d.emu_settings_save_seq() == s3 and dirty() == 1, f"a dirty stamp 40 ms after the tick's own clock reading does not skip the debounce (records {s3} -> {d.emu_settings_save_seq()}, dirty {dirty()})")
secs(5)
check(d.emu_settings_save_seq() == s3 + 1 and dirty() == 0, "...and the change is written once the 5 s of quiet are over")
print("RESULT:", "FAILED" if fails else "ALL PASSED"); sys.exit(1 if fails else 0)
