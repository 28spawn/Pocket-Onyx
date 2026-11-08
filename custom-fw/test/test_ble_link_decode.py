#!/usr/bin/env python3
"""custom-fw/test/test_ble_link_decode.py - tools/ble_link_decode.py on a synthetic ble_dg block built from tools/ble_diag_layout.json."""
import json
import struct
import sys
from pathlib import Path

TOOLS = Path(__file__).resolve().parent.parent / "tools"
sys.path.insert(0, str(TOOLS))
import ble_link_decode as d  # noqa: E402

lay = json.load(open(TOOLS / "ble_diag_layout.json"))
raw = bytearray(lay["sizeof"])


def put(name, v, fmt):
    struct.pack_into(fmt, raw, lay[name], v)


put("magic", 0x4C454C42, "<I"); put("now_us", 50_000_000, "<I"); put("cind_rx", 3, "<I"); put("cind_ok", 3, "<I"); put("closes", 2, "<I"); put("peer_terms", 1, "<I")
put("close_reason", 0x13, "<B"); put("close_by", 1, "<B"); put("close_evt", 656, "<H")
put("ev_n", 2, "<I")
struct.pack_into("<IBBH", raw, lay["ev"], 49_000_000, 6, 0, 0)
struct.pack_into("<IBBH", raw, lay["ev"] + lay["ev_size"], 49_500_000, 17, 0, 0x13)
put("pdu_n", 2, "<I")
struct.pack_into("<IHBB8s", raw, lay["pdu"], 49_100_000, 3, 4, 2, bytes([0x0C, 0x09, 0, 0, 0, 0, 0, 0]))
struct.pack_into("<IHBB8s", raw, lay["pdu"] + lay["pdu_size"], 49_200_000, 5, 0x80 | 1, 3, bytes([0x03, 0x01, 0, 0, 0, 0, 0, 0]))
text = "\n".join(d.decode(bytes(raw), lay))
for want in ("remote user terminated", "by the peer", "RX ll VERSION_IND", "TX att MTU_RSP", "event close 0x0013", "closed 2"):
    assert want in text, want
# oldest first
assert text.index("cind_ok") < text.index("VERSION_IND") < text.index("MTU_RSP") < text.index("event close")
bad = bytearray(raw); struct.pack_into("<I", bad, lay["magic"], 0)
assert "magic" in d.decode(bytes(bad), lay)[0]
print("ble link decode: all passed")

# SPDX-License-Identifier: GPL-3.0-only
# Copyright (C) 2026 The Onyx Authors (@28spawn)
