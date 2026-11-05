#!/usr/bin/env python3
"""Offline test of the Bluetooth readout in tools/jl_ram_read.py: a fake ROM serves a status block laid out as include/pa_ble_radio.h says,
and the decoder must name the right fields. Also checks that the Python field list matches the header's enum, in order."""
import re
import struct
import sys
from io import StringIO
from pathlib import Path
from contextlib import redirect_stdout

HERE = Path(__file__).resolve().parent
sys.path.insert(0, str(HERE.parent / "tools"))
import jl_ram_read as J

hdr = (HERE.parent / "include" / "pa_ble_radio.h").read_text(encoding="utf-8")
enum = re.search(r"enum \{\s*(PA_BLE_ST_MAGIC.*?PA_BLE_ST_WORDS)", hdr, re.S).group(1)
names = [m.group(1) for m in re.finditer(r"PA_BLE_ST_([A-Z0-9_]+)", enum)]
assert names[-1] == "WORDS"
names = names[:-1]
assert names == J.BLE_ST, f"field order differs:\n header {names}\n python {J.BLE_ST}"

PTR, ST, BC = 0x01C7D5D0, 0x01C74000, 0x01C74800
mem = {}
def put(addr, words):
    for i, w in enumerate(words):
        mem[addr + 4 * i] = w
vals = dict.fromkeys(J.BLE_ST, 0)
vals.update(MAGIC=0x31454C42, STATE=5, STEP=0xB10C0000, ERR=0, START_MS=312, START_US=48200, ADV_STARTS=1, ADV_EVENTS=77, IRQS=154, ISR_MAX_US=61, RF_OPS=1700, RF_SECTIONS=0x7FFF, POLLS=999, CONNECTED=0)
put(PTR, [0x50454C42, ST, 0x01C74900, BC, 2380, 1, 1, 0x06])
put(ST, [vals[k] for k in J.BLE_ST])
put(BC, [0xB10C0000, 154] + [0] * 10)

class Fake:
    def read(self, addr, n):
        out = b""
        for a in range(addr, addr + n, 4):
            out += struct.pack("<I", mem.get(a, 0))
        return out[:n]

buf = StringIO()
with redirect_stdout(buf):
    J.show_ble(Fake())
text = buf.getvalue()
print(text)
for need in ("ADVERTISING", "ADVERTISING EVENTS 77", "the radio is transmitting", "took 48200 us", "1700 ops run", "RUNNING", "longest 61 us", "calibration compiled in: YES"):
    assert need in text, f"missing: {need}"
# a build without the radio, and the hang case
mem[PTR] = 0
buf = StringIO()
with redirect_stdout(buf):
    J.show_ble(Fake())
assert "not in this build" in buf.getvalue()
mem[PTR] = 0x50454C42
mem[ST + 4] = 3
buf = StringIO()
with redirect_stdout(buf):
    J.show_ble(Fake())
assert "HUNG" in buf.getvalue()
# the previous run's record (a reset keeps it): shown under its own heading, with the storm error named
PREV = 0x01C74C00
pv = dict(vals, STATE=7, ERR=6, ADV_EVENTS=300, POLLS=1234, CONNECTED=1)
put(PREV, [pv[k] for k in J.BLE_ST] + [0x56455250])
mem[ST + 4] = 5
put(PTR, [0x50454C42, ST, 0x01C74900, BC, 2380, PREV, 1, 0x06])
buf = StringIO()
with redirect_stdout(buf):
    J.show_ble(Fake())
text = buf.getvalue()
assert "THE RUN BEFORE THIS RESET" in text and "INTERRUPT STORM" in text and "FAILED" in text, text
mem[PREV + 4 * len(J.BLE_ST)] = 0
buf = StringIO()
with redirect_stdout(buf):
    J.show_ble(Fake())
assert "no record" in buf.getvalue()
print("ble readout: all passed")
