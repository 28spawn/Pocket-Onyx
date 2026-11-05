"""The stored captures and cabinets (10 + 15), on the firmware's own code: upload, list, delete, overwrite, the counts that follow, a reboot, and the

cases that must be refused. The flash is the emulator's simulated chip, so the windows of pa_flash_map.h apply.


    python custom-fw/test/test_user_store.py [path/to/pocket_amp_emu.dll]

"""

import ctypes

import math

import os

import struct

import sys

import tempfile


HERE = os.path.dirname(os.path.abspath(__file__))

ROOT = os.path.dirname(HERE)

flash = os.path.join(tempfile.mkdtemp(prefix="pa_store_"), "flash.bin")

os.environ["PA_FLASH_FILE"] = flash

dll = sys.argv[1] if len(sys.argv) > 1 else os.path.join(ROOT, "emu", "pocket_amp_emu.dll")

sys.path.insert(0, os.path.join(ROOT, "tools"))

import mvave_device as M  # noqa: E402


d = ctypes.CDLL(dll)

d.emu_init()

d.emu_feed_sysex_bytes.argtypes = [ctypes.POINTER(ctypes.c_uint8), ctypes.c_uint32]

d.emu_next_sysex_reply.argtypes = [ctypes.c_char_p, ctypes.c_int]

d.emu_next_sysex_reply.restype = ctypes.c_int

d.emu_midi_event.argtypes = [ctypes.c_int] * 3

F = ctypes.POINTER(ctypes.c_float)

d.emu_process_audio_f32.argtypes = [F, F, F, ctypes.c_int]

ib, ol, orr = (ctypes.c_float * 64)(), (ctypes.c_float * 64)(), (ctypes.c_float * 64)()

fails = 0


def check(ok, msg):

    global fails

    print(("  ok   " if ok else "  FAIL ") + msg)

    if not ok:

        fails += 1


def run(blocks):

    for _ in range(blocks):

        d.emu_process_audio_f32(ib, ol, orr, 64)


def drain():

    while d.emu_next_sysex_reply(ctypes.create_string_buffer(700), 700) > 0:

        pass


def feed(cmd, payload=b""):

    fr = M.build_frame(cmd, payload)

    d.emu_feed_sysex_bytes((ctypes.c_uint8 * len(fr)).from_buffer_copy(fr), len(fr))


def ask(cmd, payload=b""):

    drain()

    feed(cmd, payload)

    buf = ctypes.create_string_buffer(700)

    n = d.emu_next_sysex_reply(buf, 700)

    if n <= 0:

        return None

    inner = M.unpack_7to8(bytes(buf.raw[:n])[1:-1])

    ln = inner[3] | (inner[4] << 8) | (inner[5] << 16)

    return inner[2], bytes(inner[6:6 + ln])


def upload(blob, target, slot):

    drain()

    feed(25, struct.pack("<I", len(blob))[:3] + bytes([target, slot]))

    for i in range(0, len(blob), 200):

        feed(34, blob[i:i + 200])

    feed(48, b"")

    run(40)


def state():

    return ask(80)[1]


def names(kind):

    out, start, total, first = [], 0, 1, 0

    while start < total:

        t, dd = ask(81, bytes([kind, start]))

        total, n, first = dd[1], dd[3], dd[4]

        for i in range(n):

            out.append(dd[5 + i * 16:21 + i * 16].split(b"\0")[0].decode("latin1"))

        if not n:

            break

        start += n

    return out, first


def user(op, kind, idx, name=b""):

    t, dd = ask(82, bytes([op, kind, idx]) + name)

    assert t == 0x62

    return dd[3]


def cab(i, peak=0.8):

    return struct.pack("<256i", *([int(peak * (1 << 18))] + [i * 100 + k for k in range(255)]))


def gru(i):

    return bytes((j * 7 + i) & 0xFF for j in range(3852))


def wave(i):

    return b"WAVE" + struct.pack("<I", 1) + struct.pack("<f", 1.0) + bytes((j + i) & 0xFF for j in range(7504 - 12))


run(300)

print("=== a clean store ===")

s = state()

a, fa = names(0)

c, fc = names(1)

check(s[115] == 0 and s[125] == 0 and s[126] == 10 and s[127] == 15, f"nothing stored: captures {s[115]}, cabinets {s[125]}, free slots {s[126]} / {s[127]}")

check(len(a) == 20 and fa == 20, f"20 stock amps, the first stored one would be type {fa} ({len(a)} listed)")

check(len(c) >= 20 and fc == len(c), f"{len(c)} cabinets, the first stored one would be type {fc}")

n_img_cab = len(c)


upload(cab(0), 2, 14); user(0, 1, 14)          # the first write creates the simulated chip's file; what is outside the windows is recorded after it
chip0 = open(flash, "rb").read()
chip0 += bytes([0xFF]) * (0x100000 - len(chip0))          # the file grows as the chip is written: what is not there is blank
print("=== 10 captures, 15 cabinets ===")

for i in range(10):

    upload(wave(i) if i % 2 == 0 else gru(i), 1, 0xFF)

s = state()

a, fa = names(0)

check(s[115] == 10 and s[126] == 0, f"10 captures stored ({s[115]}), free slots {s[126]}")

check(len(a) == 30 and all(a[20:]), f"the list has 20 + 10 names ({len(a)})")

upload(gru(99), 1, 0xFF)

check(state()[115] == 10, "an 11th capture is refused: the count stays at 10")

for i in range(15):

    upload(cab(i), 2, 0xFF)

s = state()

c, fc = names(1)

check(s[125] == 15 and s[127] == 0, f"15 cabinets stored ({s[125]}), free slots {s[127]}")

check(len(c) == n_img_cab + 15 and all(c[fc:]), f"the cabinet list has {len(c)} names, all of the stored ones named")

upload(cab(99), 2, 0xFF)

check(state()[125] == 15, "a 16th cabinet is refused")


print("=== delete: the counts follow, the numbers of the others stay ===")

before = names(0)[0]

check(user(0, 0, 3) == 0, "capture 3 deleted")

s = state()

a, _ = names(0)

check(s[115] == 9 and s[126] == 1 and a[20 + 3] == "" and a[20 + 4] == before[20 + 4] and len(a) == 30, f"9 captures exist, one free slot; type {20 + 3} is empty, the next one is unchanged ({a[20 + 4]!r})")

check(user(0, 0, 3) != 0, "deleting it again is refused")

check(user(0, 1, 6) == 0, "cabinet 6 deleted")

s = state()

c, fc = names(1)

check(s[125] == 14 and s[127] == 1 and c[fc + 6] == "" and c[fc + 7], f"14 cabinets exist, one free slot; its type is empty ({c[fc + 6]!r}), the next one is not")

upload(gru(3), 1, 0xFF)

s = state()

a, _ = names(0)

check(s[115] == 10 and a[20 + 3] != "", "a new capture takes the free slot (index 3): 10 again")

upload(cab(77), 2, 0xFF)

s = state()

c, fc = names(1)

check(s[125] == 15 and c[fc + 6] != "", "a new cabinet takes the free slot (index 6): 15 again")


print("=== overwrite in place ===")

nb = names(0)[0]

upload(gru(5), 1, 5)

a, _ = names(0)

check(state()[115] == 10 and a == nb, "a capture sent to slot 5 replaces it: the count and the names are the same")

upload(cab(5), 2, 9)

check(state()[125] == 15, "a cabinet sent to slot 9 replaces it: still 15")


print("=== rename ===")

check(user(1, 1, 2, b"MY CAB") == 0, "cabinet 2 renamed")

c, fc = names(1)

check(c[fc + 2] == "MY CAB", f"the list shows it ({c[fc + 2]!r})")

check(user(1, 0, 0, b"MY AMP") == 0 and names(0)[0][20] == "MY AMP", "capture 0 renamed")


print("=== a slot that plays a deleted cabinet goes back to a stock model ===")

fc_type = fc + 8

for cc, v in ((99, 5), (98, 0x40), (6, 0), (38, 0)):

    pass

d.emu_midi_event(0xB0, 99, 5); d.emu_midi_event(0xB0, 98, 0x40); d.emu_midi_event(0xB0, 6, fc_type + 0x20)

run(400)

s = state()

check(s[22 + 3 * 8 + 1] == fc_type, f"cabinet type {fc_type} (stored slot 8) selected ({s[22 + 3 * 8 + 1]})")

check(user(0, 1, 8) == 0, "that cabinet deleted")

run(600)

s = state()

check(s[22 + 3 * 8 + 1] == 0, f"the cab slot is back on the first stock cabinet ({s[22 + 3 * 8 + 1]})")


print("=== a power cycle keeps the store ===")

cnt = (state()[115], state()[125], names(0)[0], names(1)[0])

d.emu_init()

run(300)

s = state()

check((s[115], s[125], names(0)[0], names(1)[0]) == cnt, f"after a reboot: {s[115]} captures and {s[125]} cabinets, the names are the same")


print("=== the data is where the map says, and nothing else was touched ===")

fl = open(flash, "rb").read()

check(len(fl) == 0x100000, f"the simulated chip file is {len(fl)} bytes")

check(fl[0xA5000:0xA5004] == struct.pack("<i", int(0.8 * (1 << 18))), "cabinet slot 0 holds its taps at 0xA5000")

check(fl[0x91000:0x91000 + 16] == gru(1)[:16] or fl[0x91000:0x91004] == b"WAVE", "NAM blob slot 0 is at 0x91000")

check(fl[0xB3000:0xB3004] != bytes([255]) * 4, "cabinet slot 14 (0xB3000) is written")

outside = [(0, 0x86000), (0xB4000, 0x100000)]       # everything outside our one window: the boot area, the app image, the V1.05 trailer's sector, the stock's VM and log, BTIF / USER / the factory bundle, the stock's saved presets and save record
dirty = [hex(lo) for lo, hi in outside if chip0[lo:hi] != fl[lo:hi]]
check(not dirty, f"everything outside our window (the boot area, the app image, the stock's VM and log, BTIF / USER / the factory bundle, the stock's saved presets and save record) is exactly as it was ({dirty})")
print("=== refused ===")

upload(b"\x01\x02\x03", 2, 0xFF)

check(state()[125] == cnt[1], "a 3-byte cabinet is refused")

check(user(0, 0, 40) != 0 and user(0, 1, 40) != 0, "a delete of a slot that does not exist is refused")

print("=== clear all: the stored items AND the whole area behind them ===")

pre = state()
check(pre[115] > 0 and pre[125] > 0, f"there is something to clear ({pre[115]} captures, {pre[125]} cabinets)")
check(user(2, 1, 0) == 0, "clear all cabinets is accepted")
s = state()
check(s[125] == 0, f"no stored cabinet is left (count {s[125]}, free {s[127]})")
check(s[115] == pre[115], "the captures were not touched by clearing the cabinets")
check(user(2, 0, 0) == 0, "clear all captures is accepted")
s = state()
check(s[115] == 0 and s[126] == 10, f"no stored capture is left (count {s[115]}, free {s[126]})")
fl2 = open(flash, "rb").read()
blank = lambda lo, hi: fl2[lo:hi] == bytes([255]) * (hi - lo)
check(blank(0x86000, 0x88000), "the cabinet registry sectors (0x86000, 0x87000) are blank")
check(blank(0xA5000, 0xB4000), "all 15 cabinet slot sectors are blank")
check(blank(0x91000, 0xA5000), "all the capture blob slots (0x91000..0xA4FFF) are blank")
dirty = [hex(lo) for lo, hi in outside if chip0[lo:hi] != fl2[lo:hi]]
check(not dirty, f"nothing outside the user store changed ({dirty})")
c0 = state()[125]
upload(cab(0), 2, 0xFF)
check(state()[125] == c0 + 1, "a cabinet can be stored again after a clear")


print("\n" + ("THE USER STORE WORKS" if fails == 0 else f"{fails} FAILURE(S)"))

sys.exit(1 if fails else 0)

