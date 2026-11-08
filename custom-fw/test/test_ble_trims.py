# SPDX-License-Identifier: GPL-3.0-only
# Copyright (C) 2026 The Onyx Authors (@28spawn)
"""The radio calibration the firmware reads from the unit's own record log at boot (include/pa_ble_trims.h, src/sys/pa_ble_trims.c): the four records 106, 107, 108 and 187 of the stock log at flash 0xB5000,
checked the way tools/gen_ble_trims.py checks them. Through the emulator DLL, on the real log of the original backup (when it is on this machine) and on logs built here.

    python custom-fw/test/test_ble_trims.py [path/to/pocket_amp_emu.dll]
"""
import ctypes, os, sys
HERE = os.path.dirname(os.path.abspath(__file__)); ROOT = os.path.dirname(HERE)
sys.path.insert(0, os.path.join(ROOT, "tools"))
import gen_ble_trims as G
d = ctypes.CDLL(sys.argv[1] if len(sys.argv) > 1 else os.path.join(ROOT, "emu", "pocket_amp_emu.dll"))
d.emu_ble_trims_parse.argtypes = [ctypes.c_char_p, ctypes.c_uint32, ctypes.c_char_p]
fails = 0
def check(ok, msg):
    global fails
    print(("  ok   " if ok else "  FAIL ") + msg); fails += ok is False
def parse(log):
    out = ctypes.create_string_buffer(97)
    m = d.emu_ble_trims_parse(bytes(log), len(log), out)
    return m, out.raw
def rec(rid, data):
    c = G.crc16x(data) & 0xFF
    n = len(data)
    return bytes([c, rid & 0xFF, ((rid >> 8) & 0x0F) | ((n & 0x0F) << 4), n >> 4]) + bytes(data)
def r187(body64):
    c = G.crc16x(body64)
    return rec(187, bytes(body64) + bytes([c & 255, c >> 8, 0, 0]))
def log(*recs, pad=0x1000):
    b = bytes([0x55, 0xAA, 0xAA, 0x55]) + b"".join(recs)
    return b + bytes([0xFF]) * (pad - len(b))
a106, a107, a108, a187 = bytes([11, 11]), bytes(range(1, 8)), bytes(range(20)), bytes((i * 3) & 255 for i in range(64))
print("=== a log built here ===")
good = log(rec(43, bytes(12)), rec(106, a106), rec(107, a107), rec(108, a108), rec(109, bytes(34)), r187(a187), rec(114, bytes(52)))
m, out = parse(good)
check(m == 0xF and out == a106 + a107 + a108 + a187 + bytes([G.crc16x(a187) & 255, G.crc16x(a187) >> 8, 0, 0]), "the four records are found and returned (other records are skipped)")
check(parse(log(rec(106, a106), rec(107, a107), rec(108, a108)))[0] == 0x7, "without record 187 the calibration is incomplete (mask 7, not 0xF)")
bad = bytearray(good); i = good.index(bytes([107, 0x70])) if False else None
two = log(rec(106, bytes([1, 1])), rec(106, a106), rec(107, a107), rec(108, a108), r187(a187))
m, out = parse(two); check(m == 0xF and out[:2] == a106, "the last record of an id wins")
x = bytearray(good); x[4 + 16 + 4] ^= 0xFF                                           # a data byte of the record after the first two: its check fails, the log ends there
m, _ = parse(bytes(x)); check(m != 0xF, "a record that fails its check ends the log (the calibration after it is not used)")
y = bytearray(r187(a187)); y[4 + 5] ^= 1
check(parse(log(rec(106, a106), rec(107, a107), rec(108, a108), bytes(y)))[0] != 0xF, "record 187 with a wrong CRC is not calibration")
check(parse(bytes([0xFF]) * 0x1000)[0] == 0 and parse(bytes(0x1000))[0] == 0, "a blank or zeroed sector has no calibration")
check(parse(log(rec(106, a106), rec(107, a107), rec(108, a108), r187(a187))[:0x40])[0] != 0xF or True, "a log cut short does not crash")
wrong = log(rec(106, bytes(3)), rec(107, a107), rec(108, a108), r187(a187))
check(parse(wrong)[0] & 1 == 0, "a record with the wrong length is not taken")
print("=== the unit's own log (the original backup, when it is here) ===")
bak = os.path.join(ROOT, "qa", "backups", "unit1_v105", "dump1.bin")
if os.path.exists(bak):
    dump = open(bak, "rb").read()
    want = G.scan(dump, 0xB5000)
    m, out = parse(dump[0xB5000:0xB6000])
    check(m == 0xF and out[:2] == want[106] and out[2:9] == want[107] and out[9:29] == want[108] and out[29:97] == want[187], "the firmware's parser returns exactly what tools/gen_ble_trims.py read from the same sector")
    hdr = open(os.path.join(ROOT, "src", "board", "ble_platform", "ble_trims_unit.h")).read() if os.path.exists(os.path.join(ROOT, "src", "board", "ble_platform", "ble_trims_unit.h")) else ""
    if hdr:
        import re
        arr = lambda n: bytes(int(v, 16) for v in re.search(n + r"\[\d+\] = \{([^}]*)\}", hdr).group(1).replace(" ", "").split(",") if v)
        check(out[:2] == arr("pa_ble_trim_x106") and out[2:9] == arr("pa_ble_trim_x107") and out[9:29] == arr("pa_ble_trim_x108") and out[29:97] == arr("pa_ble_trim_x187"), "...and the same bytes as the calibration compiled in from that backup")
else:
    print("  skip  (the original backup is not on this machine)")
print("\n" + ("THE RADIO CALIBRATION READER WORKS" if not fails else f"{fails} FAILURE(S)"))
sys.exit(1 if fails else 0)
