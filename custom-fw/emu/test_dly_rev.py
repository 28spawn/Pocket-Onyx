# SPDX-License-Identifier: GPL-3.0-only
# Copyright (C) 2026 The Onyx Authors (@28spawn)
"""Delay and reverb, end to end through the firmware's audio core (the emulator DLL).

  [1] DELAY: every type's echo lands at the TIME the screen shows (pa_dly_time_ms), on
      both sides; the single-tap modes put the same echo left and right (one echo per
      repeat in mono); DUAL's second tap is TIME x RATIO on the right; repeats decay
  [2] DELAY: maximum feedback stays bounded
  [3] DELAY / REVERB: switching the effect on does not turn the dry guitar down
  [4] REVERB: every type leaves a tail; SWELL's tail rings on after the note stops
  [5] REVERB: bypassing it lets the tail ring out (spillover)
  [6] with a NAM capture as the amp, delay and reverb still act
  [7] the new delays: DIGITAL (clean, same both sides), PING PONG (left T, right 2T),
      REVERSE (a rising chirp comes back falling), SLAPBACK (40..200 ms, one repeat)
  [8] the new reverbs: PLATE leaves a tail; AMBIENCE's reflections sit inside SIZE and
      it leaves no long tail

    python custom-fw/emu/test_dly_rev.py [pocket_amp_emu.dll]
"""
import ctypes
import math
import os
import sys

import numpy as np

HERE = os.path.dirname(os.path.abspath(__file__))
FW = os.path.dirname(HERE)
sys.path.insert(0, HERE)
sys.path.insert(0, os.path.join(FW, "tools"))
import nam_loader        # noqa: E402
import gen_test_audio    # noqa: E402

# a private simulated flash: the user-capture registry lives in it, and the shared emu/pocket_amp_flash.bin carries what other tests left (the 1 MB chip has room
# for ONE flash capture, so a leftover one would stop this test's capture from registering)
import tempfile  # noqa: E402
os.environ["PA_FLASH_FILE"] = os.path.join(tempfile.mkdtemp(prefix="pa_dlyrev_"), "flash.bin")

d = ctypes.CDLL(sys.argv[1] if len(sys.argv) > 1 else os.path.join(HERE, "pocket_amp_emu.dll"))
F = ctypes.POINTER(ctypes.c_float)
d.emu_process_audio_f32.argtypes = [F, F, F, ctypes.c_int]
d.emu_load_nam_struct.argtypes = [ctypes.POINTER(ctypes.c_uint8), ctypes.c_uint32]
d.emu_register_user_nam.argtypes = [ctypes.c_char_p]
FS = 44100
fails = 0


def check(ok, msg):
    global fails
    print(("  ok   " if ok else "  FAIL ") + msg)
    fails += 0 if ok else 1


def run(x):
    n = len(x) - len(x) % 64
    L = np.zeros(n, np.float32); R = np.zeros(n, np.float32)
    for i in range(0, n, 64):
        xi = np.ascontiguousarray(x[i:i + 64], dtype=np.float32)
        d.emu_process_audio_f32(xi.ctypes.data_as(F), L[i:].ctypes.data_as(F), R[i:].ctypes.data_as(F), 64)
    return L, R


def setup(nam=False):
    d.emu_init()
    for s in range(8):
        d.emu_set_slot_enabled(s, 0)
    if nam:
        gen_test_audio.ensure_emu_assets(HERE, log=lambda s: None)
        blob, meta = nam_loader.parse_nam_file(open(os.path.join(HERE, "test_capture.nam"), "rb").read(), "t.nam")
        buf = (ctypes.c_uint8 * len(blob)).from_buffer_copy(blob)
        d.emu_load_nam_struct(buf, len(blob))
        d.emu_register_user_nam(b"t")
        d.emu_set_slot_enabled(2, 1)
    run(np.zeros(FS // 2, np.float32))                     # let the model-change fade settle


def click(sec):
    x = np.zeros(int(sec * FS), np.float32)
    x[FS // 10: FS // 10 + 40] = np.hanning(40) * 0.5       # a 1 ms click at 0.1 s
    return x


def note(sec, dur=1.0, amp=0.3):
    t = np.arange(int(sec * FS)) / FS
    x = amp * np.sin(2 * np.pi * 220 * t) * (t < dur)
    return x.astype(np.float32)


def dly_ms(pct, t=0):
    """fx_delay.c dly_len_from_knob(): the range per type"""
    val = (pct * 2097152) // 100
    lo, hi = 1024, 32768 - 16
    if t in (6, 7):
        hi = 16384 - 8
    if t == 8:
        lo, hi = 1764, 8820
    return (lo + ((val * (hi - lo)) >> 21)) * 1000.0 / FS


def echoes(y, start, thr, n=3):
    """Echo arrival times (ms after the click). The search starts 15 ms after the click,
    past the dry click's own ringing, and the threshold is relative to the echoes."""
    i = start + int(0.015 * FS)
    thr = max(thr, 0.25 * float(np.max(np.abs(y[i:]))))
    out = []
    while i < len(y) and len(out) < n:
        if abs(y[i]) > thr:
            j = i + int(np.argmax(np.abs(y[i:i + 300])))
            out.append((j - start) * 1000.0 / FS)
            i = j + 400
        else:
            i += 1
    return out


def rms(v):
    return float(np.sqrt(np.mean(np.asarray(v, np.float64) ** 2)))


def db(v):
    return 20 * math.log10(v + 1e-12)


print("=== [1] DELAY: the echoes land where the screen says ===")
for t, name in enumerate(["ANALOG", "DUCKING", "TAPE", "DUAL", "LO-FI", "DIGITAL"]):
    for pct in (20, 60):
        setup()
        d.emu_set_slot_type(6, t)
        for p, v in enumerate((pct, 50, 50, 50)):
            d.emu_set_slot_param(6, p, v)
        d.emu_set_slot_enabled(6, 1)
        L, R = run(click(3.0))
        want = dly_ms(pct, t)
        el, er = echoes(L, FS // 10, 0.01), echoes(R, FS // 10, 0.01)
        tol = 0.06 if t in (2, 4) else 0.01                    # TAPE / LO-FI wobble the time
        if t == 3:
            ok = bool(el) and bool(er) and abs(el[0] - want) / want < tol \
                and abs(er[0] - 0.75 * want) / (0.75 * want) < tol
            check(ok, f"{name:7s} TIME {pct}%: left {el[0] if el else 0:6.1f} ms (screen {want:6.1f}), "
                      f"right {er[0] if er else 0:6.1f} ms (x0.75 = {0.75 * want:6.1f})")
        else:
            same = len(el) >= 2 and len(er) >= 2 and abs(el[0] - er[0]) < 1.0
            falls = len(el) >= 2 and np.max(np.abs(L)[int(FS * (0.1 + el[1] / 1000)) - 100:][:400]) < \
                np.max(np.abs(L)[int(FS * (0.1 + el[0] / 1000)) - 100:][:400])
            ok = bool(el) and abs(el[0] - want) / want < tol and same and falls
            check(ok, f"{name:7s} TIME {pct}%: echo at {el[0] if el else 0:6.1f} ms (screen {want:6.1f}), "
                      f"same both sides: {same}, repeats fall: {falls}")

print("=== [2] DELAY: maximum feedback stays bounded ===")
setup()
d.emu_set_slot_type(6, 0)
for p, v in enumerate((30, 100, 100, 50)):
    d.emu_set_slot_param(6, p, v)
d.emu_set_slot_enabled(6, 1)
L, R = run((np.random.default_rng(1).standard_normal(FS * 8) * 0.1).astype(np.float32))
pk = float(np.max(np.abs(L[-FS:])))
check(np.isfinite(L).all() and pk < 2.0, f"FDBK 100 % with 8 s of noise: last-second peak {pk:.2f} FS")

print("=== [3] switching the effect on keeps the dry level ===")
for slot, name in ((6, "DELAY"), (7, "REVERB")):
    setup()
    dry = run(note(0.6, 0.6))[0]
    setup()
    for p, v in enumerate((60, 40, 30, 50) if slot == 6 else (50, 0, 30, 50)):
        d.emu_set_slot_param(slot, p, v)
    d.emu_set_slot_enabled(slot, 1)
    wet = run(note(0.6, 0.6))[0]
    # the first 0.1 s of the note, before any echo / much reverb has built up
    a, b = int(0.05 * FS), int(0.12 * FS)
    diff = db(rms(wet[a:b])) - db(rms(dry[a:b]))
    check(diff > -1.0, f"{name} at MIX 30 %: the note is {diff:+.1f} dB vs bypass (it was -2.5 / -3 dB)")

print("=== [4] REVERB: every type leaves a tail ===")
for t, name in enumerate(["ROOM", "HALL", "SWELL", "SPRING", "SHIMMER", "CLOUD", "PLATE"]):
    setup()
    d.emu_set_slot_type(7, t)
    for p, v in enumerate((60, 0, 50, 100)):
        d.emu_set_slot_param(7, p, v)
    d.emu_set_slot_enabled(7, 1)
    L, R = run(note(2.5, 1.0))                              # a 1 s note, then 1.5 s of tail
    tail = rms(L[int(1.2 * FS):int(1.4 * FS)])
    check(tail > 1e-4, f"{name:7s}: 0.2 s after the note stops the tail is at {db(tail):.0f} dBFS")

print("=== [5] REVERB: bypassing it lets the tail ring out ===")
setup()
d.emu_set_slot_type(7, 1)
for p, v in enumerate((70, 0, 50, 100)):
    d.emu_set_slot_param(7, p, v)
d.emu_set_slot_enabled(7, 1)
run(click(0.3))
d.emu_set_slot_enabled(7, 0)
L, R = run(np.zeros(FS, np.float32))
check(rms(L[:4410]) > 1e-5, f"after bypass the tail keeps ringing ({db(rms(L[:4410])):.0f} dBFS)")

print("=== [6] with a NAM capture as the amp ===")
# The synthetic test capture is very quiet (about -51 dB of gain), so the tails sit only
# ~20 dB over the output's floor (a ~-92 dBFS DC offset and noise). The tails are measured
# without their DC, and the reverb is set explicitly (HALL, DECAY 80 %, MIX 80 %) instead of whatever
# type the emulator boots with (ROOM at DECAY 40 % has little left 200 ms after the note).
def nam_note_run(slot=None, rtype=None, vals=(40, 50, 50, 60)):
    setup(nam=True)
    if slot is not None:
        if rtype is not None:
            d.emu_set_slot_type(slot, rtype)
        for p, v in enumerate(vals):
            d.emu_set_slot_param(slot, p, v)
        d.emu_set_slot_enabled(slot, 1)
    run(np.zeros(FS // 4, np.float32))
    return run(note(1.6, 0.5))[0]


for slot, name, rtype, vals in ((6, "DELAY", None, (40, 50, 50, 60)), (7, "REVERB", 1, (80, 10, 50, 80))):
    base, got = nam_note_run(), nam_note_run(slot, rtype, vals)
    tg, tb = got[int(0.7 * FS):], base[int(0.7 * FS):]
    lg, lb = rms(tg - tg.mean()), rms(tb - tb.mean())
    check(lg > 3 * lb + 1e-7, f"{name}: after the note the effect is there ({db(lg):.0f} vs {db(lb):.0f} dBFS dry)")


print("=== [7] the new delays ===")


def dly_setup(t, vals):
    setup()
    d.emu_set_slot_type(6, t)
    for p_, v in enumerate(vals):
        d.emu_set_slot_param(6, p_, v)
    d.emu_set_slot_enabled(6, 1)


# PING PONG: left at TIME, right at 2 x TIME
for pct in (20, 60):
    dly_setup(6, (pct, 50, 50, 100))
    L, R = run(click(3.0))
    want = dly_ms(pct, 6)
    el, er = echoes(L, FS // 10, 0.01), echoes(R, FS // 10, 0.01)
    ok = bool(el) and bool(er) and abs(el[0] - want) / want < 0.01 and abs(er[0] - 2 * want) / (2 * want) < 0.01
    check(ok, f"PING PONG TIME {pct}%: left {el[0] if el else 0:6.1f} ms (screen {want:6.1f}), "
              f"right {er[0] if er else 0:6.1f} ms (2x = {2 * want:6.1f})")


# REVERSE: a rising chirp (200 Hz -> 2 kHz over 0.25 s) comes back FALLING
def chirp_rate_slope(y, a, b):
    """zero crossings per second in the first and second half of y[a:b]"""
    seg = y[a:b]
    h = len(seg) // 2
    zc = lambda v: np.count_nonzero(np.diff(np.signbit(v))) * FS / len(v)
    return zc(seg[:h]), zc(seg[h:])


def chirp_in(sec):
    tt = np.arange(int(sec * FS)) / FS
    x = np.zeros_like(tt)
    m = (tt >= 0.1) & (tt < 0.35)
    tc = tt[m] - 0.1
    x[m] = 0.3 * np.sin(2 * np.pi * (200 * tc + 0.5 * (1800 / 0.25) * tc * tc))
    return x.astype(np.float32)


res = {}
for t, name in ((5, "DIGITAL"), (7, "REVERSE")):
    dly_setup(t, (60, 0, 100, 100))                        # TIME 60 %, no repeats, wet only
    L, R = run(chirp_in(2.0))
    want = dly_ms(60, t) / 1000
    a = int((0.1 + want) * FS)                              # where the first echo of the chirp starts
    res[name] = chirp_rate_slope(L, a, a + int(0.25 * FS))
print(f"       crossings/s, first vs second half of the echo: DIGITAL {res['DIGITAL'][0]:.0f} -> "
      f"{res['DIGITAL'][1]:.0f}, REVERSE {res['REVERSE'][0]:.0f} -> {res['REVERSE'][1]:.0f}")
check(res["DIGITAL"][1] > res["DIGITAL"][0] * 1.3 and res["REVERSE"][0] > res["REVERSE"][1] * 1.3,
      "REVERSE plays the chirp backwards (it falls), DIGITAL forwards (it rises)")

# SLAPBACK: 40 .. 200 ms, one repeat
for pct in (0, 100):
    dly_setup(8, (pct, 50, 50, 50))
    L, R = run(click(1.5))
    want = dly_ms(pct, 8)
    el = echoes(L, FS // 10, 0.01, n=2)
    first = float(np.max(np.abs(L[int(FS * (0.1 + want / 1000)) - 100:][:400])))
    second = float(np.max(np.abs(L[int(FS * (0.1 + 2 * want / 1000)) - 100:][:400])))
    drop = db(second) - db(first)
    check(bool(el) and abs(el[0] - want) / want < 0.02 and drop < -12.0,
          f"SLAPBACK TIME {pct}%: echo at {el[0] if el else 0:6.1f} ms (screen {want:.0f}), "
          f"the second repeat {drop:.0f} dB under the first")


print("\n" + ("DELAY + REVERB OK" if fails == 0 else f"{fails} FAILURE(S)"))
sys.exit(0 if fails == 0 else 1)
