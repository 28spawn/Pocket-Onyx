# SPDX-License-Identifier: GPL-3.0-only
# Copyright (C) 2026 The Onyx Authors (@28spawn)
"""The USB audio mode (an audio interface's direct monitoring), through the real audio core and the real USB audio driver (usb_ble.c) in the emulator DLL: a 220 Hz tone is the guitar at the
input, a 600 Hz tone is what the PC plays. AMP: the unit's own sound (and the PC's) on the output. INTERFACE: the output is the PC's tone and nothing of the unit's, the recording is the dry
guitar and never contains the PC's tone (no loopback possible). INTERFACE + DIRECT MONITOR: the dry guitar is also on the output.

    python custom-fw/test/test_usb_mode.py [path/to/pocket_amp_emu.dll]
"""
import ctypes, math, os, sys, tempfile
import numpy as np
HERE = os.path.dirname(os.path.abspath(__file__)); ROOT = os.path.dirname(HERE)
os.environ["PA_FLASH_FILE"] = os.path.join(tempfile.mkdtemp(prefix="pa_usbmode_"), "flash.bin")
d = ctypes.CDLL(sys.argv[1] if len(sys.argv) > 1 else os.path.join(ROOT, "emu", "pocket_amp_emu.dll"))
d.emu_init(); d.emu_midi_event.argtypes = [ctypes.c_int] * 3; d.emu_get_ui.argtypes = [ctypes.c_int]
F = ctypes.POINTER(ctypes.c_float); d.emu_process_audio_f32.argtypes = [F, F, F, ctypes.c_int]
d.emu_usb_capture_read.argtypes = [ctypes.c_char_p, ctypes.c_int]; d.emu_usb_capture_read.restype = ctypes.c_int
d.emu_usb_playback_write.argtypes = [ctypes.c_char_p, ctypes.c_int]
fails = 0
def check(ok, msg):
    global fails
    print(("  ok   " if ok else "  FAIL ") + msg); fails += ok is False
SR = 44118.0
def tone_db(x, hz, sr):
    x = np.asarray(x) * np.hanning(len(x)); sp = np.abs(np.fft.rfft(x)) / (len(x) / 4); f = np.fft.rfftfreq(len(x), 1 / sr)
    return 20 * np.log10(sp[(f > hz - 15) & (f < hz + 15)].max() + 1e-12)
def run(mode, secs=3.0):
    for c, v in ((99, 8), (98, 19), (6, mode)): d.emu_midi_event(0xB0, c, v)
    d.emu_usb_audio_start()
    inp = (ctypes.c_float * 64)(); oL = (ctypes.c_float * 64)(); oR = (ctypes.c_float * 64)()
    out, cap = [], []
    t_blk = t_host = 0.0; k = 0; ph_host = 0
    pkt = ctypes.create_string_buffer(300)
    while k * 64 / SR < secs:
        for i in range(64): inp[i] = 0.1 * math.sin(2 * math.pi * 220 * (k * 64 + i) / SR)
        while t_host <= (k + 1) * 64 / SR:                    # the host: a 1 ms OUT packet of the PC's tone, and a 1 ms IN packet taken
            n = 44 + (1 if int(t_host * 1000) % 10 == 0 else 0)
            b = bytearray()
            for i in range(n):
                v = int(0.05 * 8388607 * math.sin(2 * math.pi * 600 * (ph_host + i) / 44100.0)); b += (v & 0xFFFFFF).to_bytes(3, "little") * 2
            ph_host += n
            d.emu_usb_playback_write(bytes(b), len(b))
            m = d.emu_usb_capture_read(pkt, 300)
            for j in range(0, m - 5, 6):
                v = int.from_bytes(pkt.raw[j:j + 3], "little", signed=True) / 8388608.0; cap.append(v)
            t_host += 0.001
        d.emu_process_audio_f32(inp, oL, oR, 64); out += list(oL); k += 1
    return np.array(out[int(0.5 * SR):]), np.array(cap[int(0.5 * 44100):])
print("=== AMP (mode 0): the stock's way ===")
o, c = run(0)
check(tone_db(o, 600, SR) > -60, f"the PC's tone is on the output ({tone_db(o, 600, SR):.1f} dB)")
check(d.emu_get_ui(40) == 0, "the mode reads back as 0")
print("=== INTERFACE (mode 1) ===")
o, c = run(1)
check(d.emu_get_ui(40) == 1, "the mode reads back as 1")
check(tone_db(o, 600, SR) > -50, f"the output carries the PC's 600 Hz tone ({tone_db(o, 600, SR):.1f} dB)")
check(tone_db(o, 220, SR) < tone_db(o, 600, SR) - 50, f"...and none of the unit's own sound: the guitar's 220 Hz is {tone_db(o, 600, SR) - tone_db(o, 220, SR):.0f} dB below")
check(tone_db(c, 220, 44100.0) > -35, f"the recording is the dry guitar (220 Hz at {tone_db(c, 220, 44100.0):.1f} dB)")
check(tone_db(c, 600, 44100.0) < tone_db(c, 220, 44100.0) - 60, f"...and never contains what the PC plays (600 Hz {tone_db(c, 220, 44100.0) - tone_db(c, 600, 44100.0):.0f} dB below): no loopback is possible")
print("=== INTERFACE + DIRECT MONITOR (mode 2) ===")
o, c = run(2)
check(tone_db(o, 600, SR) > -50 and tone_db(o, 220, SR) > -35, f"the output carries the PC's tone ({tone_db(o, 600, SR):.1f} dB) and the dry guitar ({tone_db(o, 220, SR):.1f} dB)")
check(tone_db(c, 600, 44100.0) < tone_db(c, 220, 44100.0) - 60, "the recording is still the dry guitar only")
for c_, v in ((99, 8), (98, 19), (6, 9)): d.emu_midi_event(0xB0, c_, v)
for _ in range(40): d.emu_process_audio_f32((ctypes.c_float * 64)(), (ctypes.c_float * 64)(), (ctypes.c_float * 64)(), 64)
check(d.emu_get_ui(40) == 2, "a value above the range is clamped to 2")
print("\n" + ("THE USB AUDIO MODE WORKS" if not fails else f"{fails} FAILURE(S)"))
sys.exit(1 if fails else 0)
