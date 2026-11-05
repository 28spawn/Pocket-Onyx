"""The web editor's protocol on the firmware's own parser: the state record (cmd 80), the name lists (cmd 81),
and the live cabinet upload (cmd 25 / 34 / 48, target 2). MIDI changes made through the MIDI handler must show in the record.

    python custom-fw/test/test_editor_protocol.py [path/to/pocket_amp_emu.dll]
"""
import ctypes
import math
import os
import struct
import sys
import tempfile

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)
os.environ["PA_FLASH_FILE"] = os.path.join(tempfile.mkdtemp(prefix="pa_editor_"), "flash.bin")
dll_path = sys.argv[1] if len(sys.argv) > 1 else os.path.join(ROOT, "emu", "pocket_amp_emu.dll")
d = ctypes.CDLL(dll_path)
d.emu_init()
d.emu_feed_sysex_bytes.argtypes = [ctypes.POINTER(ctypes.c_uint8), ctypes.c_uint32]
d.emu_next_sysex_reply.argtypes = [ctypes.c_char_p, ctypes.c_int]
d.emu_next_sysex_reply.restype = ctypes.c_int
d.emu_midi_event.argtypes = [ctypes.c_int] * 3
F = ctypes.POINTER(ctypes.c_float)
d.emu_process_audio_f32.argtypes = [F, F, F, ctypes.c_int]
sys.path.insert(0, os.path.join(ROOT, "tools"))
import mvave_device as M  # noqa: E402

fails = 0
inp = (ctypes.c_float * 64)()
oL = (ctypes.c_float * 64)()
oR = (ctypes.c_float * 64)()
t = 0


def check(ok, msg):
    global fails
    print(("  ok   " if ok else "  FAIL ") + msg)
    if not ok:
        fails += 1


def run(blocks):
    global t
    for _ in range(blocks):
        for i in range(64):
            inp[i] = 0.2 * math.sin(2 * math.pi * 220 * (t + i) / 44100.0)
        t += 64
        d.emu_process_audio_f32(inp, oL, oR, 64)


def ask(cmd, payload=b""):
    frame = M.build_frame(cmd, payload)
    d.emu_feed_sysex_bytes((ctypes.c_uint8 * len(frame)).from_buffer_copy(frame), len(frame))
    buf = ctypes.create_string_buffer(600)
    n = d.emu_next_sysex_reply(buf, 600)
    if n <= 0:
        return None
    raw = bytes(buf.raw[:n])
    inner = M.unpack_7to8(raw[1:-1])
    ln = inner[3] | (inner[4] << 8) | (inner[5] << 16)
    data = inner[6:6 + ln]
    ok = inner[6 + ln] == (~((inner[2] + inner[3] + inner[4] + inner[5] + sum(data)) & 0xFF)) & 0xFF
    return inner[2], data, ok


def state():
    r = ask(80)
    assert r and r[0] == 0x60 and r[2], "no state reply"
    return r[1]


def cc(c, v):
    d.emu_midi_event(0xB0, c, v)


run(300)
print("=== the state record (cmd 80) ===")
s = state()
check(len(s) == 128 and s[0] == 1, f"128 bytes, version 1 ({len(s)} B, v{s[0]})")
d.emu_midi_event(0xC0, 4, 0); run(400)
s = state()
check(s[2] == 4, f"program change 4 shows as preset {s[2]}")
cc(102, 20); cc(110, 90); cc(109, 64); cc(113, 100); run(20)
s = state()
amp = 22 + 2 * 8
check(s[amp + 2] in range(15, 26), f"CC 102 = 20/127 -> amp knob 0 near 16% ({s[amp + 2]})")
cc(28 + 1, 0); run(20)
check(state()[22 + 2 * 8] == 0, "CC 29 (AMP off) clears the amp's on flag")
cc(28 + 1, 127); run(20)
check(state()[22 + 2 * 8] == 1, "CC 29 = 127 sets it again")
for c, v, idx, name in ((8, 0, 0, "x"),):
    pass
print()
print("=== the name lists (cmd 81) ===")
for kind in (0, 1):
    r = ask(81, bytes([kind, 0]))
    check(r is not None and r[0] == 0x61 and r[2], f"kind {kind} answered")
    if r:
        k, total, start, n = r[1][0], r[1][1], r[1][2], r[1][3]
        check(k == kind and start == 0 and len(r[1]) == 5 + 16 * n and n > 0 and total >= 20, f"kind {kind}: {total} total, {n} in this reply")
        nm = r[1][5:21].split(b"\0")[0].decode("latin1")
        check(len(nm) > 0, f"kind {kind} first name {nm!r}")
        r2 = ask(81, bytes([kind, n]))
        check(r2 is not None and r2[1][2] == n and r2[1][3] > 0, f"kind {kind}: the next page starts at {n}")
print()
print("=== a live cabinet upload (target 2) ===")
taps = [int(0.9 * (1 << 18))] + [0] * 31
taps = [int(x) for x in taps]
blob = b"".join(struct.pack("<i", x) for x in taps)
check(state()[116] == 0, "no user cabinet at first")
r = ask(25, struct.pack("<I", len(blob))[:3] + bytes([2, 0xFE]))          # 0xFE: a live cabinet in RAM only
ask(34, blob)
r = ask(48, b"")
run(600)
check(state()[116] == 1, "after the commit the record says a user cabinet is live")
d.emu_midi_event(0xC0, 7, 0); run(600)
check(state()[116] == 0, "a preset change drops it")
# bad: too short / not a multiple of 4
ask(25, struct.pack("<I", 10)[:3] + bytes([2, 0])); ask(34, bytes(10)); ask(48, b"")
run(600)
check(state()[116] == 0, "a 10-byte cabinet is refused")
print("\n" + ("THE EDITOR PROTOCOL WORKS" if fails == 0 else f"{fails} FAILURE(S)"))
sys.exit(0 if fails == 0 else 1)
