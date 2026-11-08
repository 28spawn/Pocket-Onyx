# SPDX-License-Identifier: GPL-3.0-only
# Copyright (C) 2026 The Onyx Authors (@28spawn)
"""custom-fw/test/test_ble_diag_decode.py - the host decoder (tools/ble_diag_decode.py) against the C layout (include/pa_ble_diag.h) and a synthetic block."""
import os
import re
import struct
import sys

here = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, os.path.join(here, "..", "tools"))
import ble_diag_decode as bd

hdr = open(os.path.join(here, "..", "include", "pa_ble_diag.h"), encoding="utf-8").read()
assert int(re.search(r"#define PA_BLE_DIAG_SIZE (\d+)u", hdr).group(1)) == bd.SIZE, "SIZE differs from the header"
assert int(re.search(r"#define PA_BLE_RING_N\s+(\d+)u", hdr).group(1)) == bd.RING_N
assert int(re.search(r"#define PA_BLE_DIAG_MAGIC\s+(0x[0-9A-Fa-f]+)u", hdr).group(1), 16) == bd.MAGIC
# the layout offsets in the struct comment lines
assert "28" in re.search(r"seen0\[37\].*?/\* (\d+)", hdr).group(1) and "324" in re.search(r"hist\[2\]\[3\].*?/\* (\d+)", hdr).group(1) and "408" in re.search(r"ring\[PA_BLE_RING_N\].*?/\* (\d+)", hdr).group(1)
for bit, name in enumerate(bd.PROFILE_BITS):
    assert re.search(r"PA_BLE_PROF_%s\s+\(1u << %d\)" % (name, bit), hdr), name

# a synthetic block: 300 events, channel map with 15 channels used, every 4th event lost, one CRC error
buf = bytearray(bd.SIZE)
used = [1, 2, 3, 6, 7, 8, 9, 10, 11, 13, 14, 15, 34, 35, 36]
chm = sum(1 << c for c in used)
struct.pack_into("<7I", buf, 0, bd.MAGIC, bd.SIZE, 0x12, 300, 11, chm & 0xFFFFFFFF, chm >> 32)
seen0 = [0] * 37
heard0 = [0] * 37
for n in range(300):
    ch = used[n % len(used)]
    heard = n % 4 != 0
    seen0[ch] += 1
    heard0[ch] += heard
    struct.pack_into("<HBBhBB", buf, bd.OFF_RING + 8 * (n % bd.RING_N), n, (1 if heard else 0) | (2 if n == 299 else 0) | 4, ch, (n % 7) - 3, ch, len(used))
struct.pack_into("<37H", buf, bd.OFF_TABLES, *seen0)
struct.pack_into("<37H", buf, bd.OFF_TABLES + 74, *heard0)
struct.pack_into("<7H", buf, bd.OFF_HIST + 2 * 7 * (0 * 3 + 1), 10, 5, 3, 1, 0, 0, 0)
d = bd.decode(bytes(buf))
assert d["profile"] == 0x12 and d["wr"] == 300 and d["hop"] == 11 and d["chm"] == chm
assert len(d["events"]) == 256 and d["events"][0]["evt"] == 44 and d["events"][-1]["evt"] == 299
assert d["events"][-1]["crc"] and d["events"][-1]["heard"]
assert d["hist"][0][1][:4] == [10, 5, 3, 1]
text = bd.report(d)
assert "15 channels used" in text and "CHM_LATE, UPD_LATE" in text and "IRQ 45 event" in text
# a wrong magic and a wrong size are refused
bad = bytearray(buf); bad[0] ^= 1
try:
    bd.decode(bytes(bad)); raise SystemExit("bad magic accepted")
except ValueError:
    pass
bad = bytearray(buf); struct.pack_into("<I", bad, 4, 100)
try:
    bd.decode(bytes(bad)); raise SystemExit("bad size accepted")
except ValueError:
    pass
print("ble diag decode: all passed")
