"""The emulator's output level: the MASTER pot and the input -> output gain.

Why this test exists
--------------------
The emulator's bridge started the front-panel MASTER pot at 2048 - the middle
of the 12-bit range - so the firmware ran its master volume at 50 %. On the
firmware's audio taper (-40 dB at 0 % .. 0 dB at 100 %) that is TWENTY
DECIBELS of attenuation, so the emulator was 20 dB quieter than the browser
build (the WASM's pot starts at full scale) and than the device's factory
default (the settings' OUTPUT LEVEL is 100 %). The emulator's own UI showed
the knob at 100 %, so the panel said one thing and the firmware did another.

This checks the pot, the level law and the actual signal gain, so the
emulator cannot quietly go quiet again.

Usage:  python custom-fw/emu/test_level.py [path/to/pocket_amp_emu.dll]
"""
import ctypes
import math
import os
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
DEFAULT_DLL = os.path.join(HERE, "pocket_amp_emu.dll")

dll_path = sys.argv[1] if len(sys.argv) > 1 else DEFAULT_DLL
d = ctypes.CDLL(dll_path)
d.emu_init()

d.emu_process_audio_f32.argtypes = [ctypes.POINTER(ctypes.c_float), ctypes.POINTER(ctypes.c_float),
                                    ctypes.POINTER(ctypes.c_float), ctypes.c_int]
d.emu_ui_pot.argtypes = [ctypes.c_uint16]
d.emu_ui_pot.restype = None

N = 64
inp = (ctypes.c_float * N)()
outL = (ctypes.c_float * N)()
outR = (ctypes.c_float * N)()


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

AMP = 0.05          # a small guitar-level signal
HZ = 440.0

fails = 0


def check(ok, msg):
    global fails
    print(("  ok   " if ok else "  FAIL ") + msg)
    if not ok:
        fails += 1


def silence(blocks):
    for _ in range(blocks):
        for i in range(N):
            inp[i] = 0.0
        d.emu_process_audio_f32(inp, outL, outR, N)


def measure(blocks=140):
    """The peak of the settled output for a sine at the input."""
    ph = 0.0
    pk = 0.0
    for b in range(blocks):
        for i in range(N):
            inp[i] = AMP * math.sin(ph)
            ph += 2 * math.pi * HZ / 44100.0
        d.emu_process_audio_f32(inp, outL, outR, N)
        if b >= blocks // 3:
            for i in range(N):
                pk = max(pk, abs(outL[i]), abs(outR[i]))
    return pk


print("=== the MASTER pot starts at full scale ===")
silence(40)
d.emu_get_telemetry(ctypes.byref(tel))
check(tel.master_volume == 100,
      f"the master volume is {tel.master_volume} % (the firmware's factory default is 100 %)")

print("\n=== the input comes out of the output ===")
silence(240)                      # let the cabinet and reverb tails die
pk = measure()
gain = 20 * math.log10(pk / AMP) if pk > 0 else -99.0
check(gain > -12.0,
      f"a {AMP} input comes out at {pk:.5f} ({gain:+.2f} dB) - not 20 dB down")

print("\n=== the pot is the master's level, on the firmware's own law ===")
silence(240)
d.emu_ui_pot(0)                   # 0 %
silence(240)
pk0 = measure()
d.emu_ui_pot(4095)                # 100 %
silence(240)
pk100 = measure()
ratio = 20 * math.log10(pk100 / pk0) if pk0 > 0 else 99.0
check(pk0 < AMP * 0.05, f"the pot at 0 % is silent ({pk0:.6f})")
check(ratio > 30.0, f"0 % .. 100 % is {ratio:.1f} dB (the law is -40 dB .. 0 dB)")

print("\n" + ("THE EMULATOR LEVEL OK" if fails == 0 else f"{fails} FAILURE(S)"))
sys.exit(0 if fails == 0 else 1)
