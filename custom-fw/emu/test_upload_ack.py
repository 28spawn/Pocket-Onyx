# SPDX-License-Identifier: GPL-3.0-only
# Copyright (C) 2026 The Onyx Authors (@28spawn)
"""Uploads that survive a lossy link (sysex.c, PA_REPLY_PA_UPLOAD): every begin and chunk is acknowledged with how many bytes the unit holds, a chunk that was dropped is sent again from
there, and a transfer that is not all there is REFUSED instead of stored (it used to be stored short: a capture that said 1,871 weights and held 1,671, which could be imported but never
selected). Usage: python custom-fw/emu/test_upload_ack.py <dll>"""
import os, struct, sys, tempfile
HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, os.path.join(HERE, "..", "tools"))
os.environ["PA_FLASH_FILE"] = os.path.join(tempfile.mkdtemp(prefix="pa_upack_"), "flash.bin")
import preset_bank_pc as P
P._js_setup()
link = P.EmuLink(sys.argv[1]); js = P.js
fails = 0


def check(ok, msg):
    global fails
    print(("  ok   " if ok else "  FAIL ") + msg); fails += 0 if ok else 1


def ask(cmd, payload, want):
    link.send(js(["buildFrame", cmd, payload])[0]); r = link.reply(want, 2.0); return r["data"] if r else None


def amp_total():
    d = ask(81, [0, 0], 0x61); return d[1] if d else None


def container(n_weights=1871):
    w = [((i * 37) % 200 - 100) / 4000.0 for i in range(n_weights)]
    return bytes(b"WAVE" + struct.pack("<IfIf", n_weights, 0.0171, 1, 0.28) + struct.pack("<%df" % n_weights, *w))


blob = container()
t0 = amp_total()
check(len(blob) == 7504 and t0 == 20, f"a clean unit: 20 amps, a capture container of {len(blob)} bytes")

# 1. the normal way: acknowledged chunks
ok = P.upload_reliable(link, blob, 1, 0xFF, 200)
check(ok is True and amp_total() == t0 + 1, f"an acknowledged upload is stored (amps {t0} -> {amp_total()})")

# 2. a chunk that never arrives: the next acknowledgement says where the unit is, and the sender resumes from there
n = len(blob)
link.send(js(["buildFrame", 25, [n & 255, (n >> 8) & 255, (n >> 16) & 255, 1, 0xFF]])[0]); a = link.reply(0x64, 2.0)["data"]
check(a[3] == 0 and a[0] | a[1] << 8 | a[2] << 16 == 0, "begin is acknowledged: 0 bytes held, receiving")
off, dropped, resumed = 0, False, False
while off < n:
    part = list(blob[off:off + 200])
    if off == 1000 and not dropped:                      # this frame is lost on the way
        dropped = True
        link.send(js(["buildFrame", 34, []])[0])         # the sender, hearing nothing, asks what the unit holds
    else:
        link.send(js(["buildFrame", 34, part])[0])
    r = link.reply(0x64, 2.0)["data"]
    got = r[0] | r[1] << 8 | r[2] << 16
    if r[3] == 1: off = n; break
    if got != off + len(part): resumed = True
    off = got
check(dropped and resumed, "a lost chunk shows as a shorter count in the answer, and the sender goes on from there")
check(amp_total() == t0 + 2, f"...and the transfer is stored whole (amps {amp_total()})")

# 3. the old failure: only part of it arrives and a commit (cmd 48) is sent
before = amp_total()
link.send(js(["buildFrame", 25, [n & 255, (n >> 8) & 255, (n >> 16) & 255, 1, 0xFF]])[0]); link.reply(0x64, 2.0)
for off in range(0, 6704, 200):
    link.send(js(["buildFrame", 34, list(blob[off:min(off + 200, 6704)])])[0]); link.reply(0x64, 2.0)
link.send(js(["buildFrame", 48, []])[0]); link._tick(40)
check(amp_total() == before, f"a transfer that stopped at 6,704 of 7,504 bytes is NOT stored (amps stay {amp_total()})")
late = ask(34, [], 0x64)
check(late is not None and late[3] == 2, f"...and a later question is told it was refused (state {late[3] if late else None})")
check(P.upload_reliable(link, blob, 1, 0xFF, 200) is True and amp_total() == before + 1, "a fresh, whole upload after that works")

# 4. the editor's presets use the same path
rec = P.obj_to_rec(P.rec_to_obj(4, P.read_preset(link, 4)[1]))
check(P.upload_reliable(link, rec, 4, 4, 100) is True, "a preset restore is acknowledged and stored")
# 5. a cabinet (raw Q18 taps) goes the same way
def cab_total():
    d = ask(81, [1, 0], 0x61); return d[1] if d else None
taps = struct.pack("<512i", *([int(0.8 * (1 << 18))] + [(i * 37) % 2000 - 1000 for i in range(511)]))
c0 = cab_total()
check(P.upload_reliable(link, taps, 2, 0xFF, 200) is True and cab_total() == c0 + 1, f"a 512-tap cabinet is stored through the same acknowledged path (cabs {c0} -> {cab_total()})")
# 6. a live cabinet (target 2, slot 0xFE) is refused at the BEGIN while the audio core still holds a swap, and accepted once the swap is done
import ctypes
dll = link.d
dll.emu_process_audio_f32.argtypes = [ctypes.POINTER(ctypes.c_float)] * 3 + [ctypes.c_int]
dll.emu_midi_event.argtypes = [ctypes.c_int] * 3
_i, _L, _R = (ctypes.c_float * 64)(), (ctypes.c_float * 64)(), (ctypes.c_float * 64)()
def blocks(k):
    for _ in range(k): dll.emu_process_audio_f32(_i, _L, _R, 64)
blocks(400)
n_live = len(taps)
begin_live = [n_live & 255, (n_live >> 8) & 255, (n_live >> 16) & 255, 2, 0xFE]
for c_, v_ in ((99, 1), (98, 0x40), (6, 7)): dll.emu_midi_event(0xB0, c_, v_)      # an amp pick (NRPN): its swap waits for the mute and for the knob to be quiet (24 blocks)
dll.emu_swap_pending.restype = ctypes.c_int
for _ in range(120):                                                  # the pick reaches the audio core on the next firmware tick
    if dll.emu_swap_pending(): break
    link._tick(1)
check(dll.emu_swap_pending() == 1, "the amp pick is waiting for the mute")
a = ask(25, begin_live, 0x64)
check(a is not None and a[3] == 2, "a live cabinet begin is REFUSED while an amp swap is pending (acknowledged with state 2)")
b = ask(25, [n_live & 255, (n_live >> 8) & 255, (n_live >> 16) & 255, 2, 0xFF], 0x64)
check(b is not None and b[3] == 0, "a stored cabinet begin is not affected (it goes to the flash, not into the slot the swap rewrites)")
blocks(600)                                                           # the swap is applied
a = ask(25, begin_live, 0x64)
check(a is not None and a[3] == 0, "the same begin is accepted once the swap is done")
print("RESULT:", "FAILED" if fails else "ALL PASSED"); sys.exit(1 if fails else 0)
