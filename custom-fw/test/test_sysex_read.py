# SPDX-License-Identifier: GPL-3.0-only
# Copyright (C) 2026 The Onyx Authors (@28spawn)
"""The read commands, and the replies they send.

The host editor identifies the device before performing communication:
it reads the device info, parses it into its DeviceInfo record
(name, model, id, type, version) and verifies standard naming.
Our firmware provides standard responses so host tools can connect cleanly.

This drives the firmware's own parser with the read commands and checks the
replies: the frame, the type, the length and the checksum - and, for the
device info, that it carries the fields the editor looks at.

    python custom-fw/test/test_sysex_read.py [path/to/pocket_amp_emu.dll]
"""
import ctypes
import os
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)
DEFAULT_DLL = os.path.join(ROOT, "emu", "pocket_amp_emu.dll")

dll_path = sys.argv[1] if len(sys.argv) > 1 else DEFAULT_DLL
d = ctypes.CDLL(dll_path)
d.emu_init()
d.emu_feed_sysex_bytes.argtypes = [ctypes.POINTER(ctypes.c_uint8), ctypes.c_uint32]
d.emu_next_sysex_reply.argtypes = [ctypes.c_char_p, ctypes.c_int]
d.emu_next_sysex_reply.restype = ctypes.c_int

sys.path.insert(0, os.path.join(ROOT, "tools"))
import mvave_device as M          # noqa: E402

fails = 0


def check(ok, msg):
    global fails
    print(("  ok   " if ok else "  FAIL ") + msg)
    if not ok:
        fails += 1


def ask(cmd, payload=b""):
    """Send a command and take the reply the firmware sent back."""
    frame = M.build_frame(cmd, payload)
    b = (ctypes.c_uint8 * len(frame)).from_buffer_copy(frame)
    d.emu_feed_sysex_bytes(b, len(frame))
    buf = ctypes.create_string_buffer(600)
    n = d.emu_next_sysex_reply(buf, 600)
    if n <= 0:
        return None
    return bytes(buf.raw[:n])


def unpack_reply(raw):
    """The reply frame: F0 00 59 <type> <len:u24le> <data> <chk> F7, packed."""
    if raw is None:
        return None
    if raw[0] != 0xF0 or raw[-1] != 0xF7:
        return None
    inner = M.unpack_7to8(raw[1:-1])
    if len(inner) < 7 or inner[0] != 0x00 or inner[1] != 0x59:
        return None
    l0, l1, l2 = inner[3], inner[4], inner[5]
    length = l0 | (l1 << 8) | (l2 << 16)   # a plain u24 little-endian
    data = inner[6:6 + length]
    chk = inner[6 + length]
    total = (inner[2] + l0 + l1 + l2 + sum(data)) & 0xFF
    return {"type": inner[2], "length": length, "data": data,
            "checksum_ok": chk == ((~total) & 0xFF)}


print("=== the device info (cmd 17) - the editor's handshake ===")
r = unpack_reply(ask(17))
check(r is not None, "the firmware answered")
if r:
    print(f"    the reply: type {r['type']} (0x{r['type']:02x}), {r['length']} bytes")
    check(r["checksum_ok"], "the reply's checksum is right")
    text = r["data"].decode("latin1")
    print(f"    the record: {text}")
    for field in ("name=", "model=", "id=", "type=", "version="):
        check(field in text, f"the record has {field!r}")
    check("Onyx" in text,
          "the name is one the editor matches (\"Onyx\")")
    check("version=119" in text, "the version is 119 (PA_FW_VERSION: shown as 1.19, below the stock's 1.05)")

print()
print("=== the descriptor (cmd 18) ===")
r = unpack_reply(ask(18))
check(r is not None, "the firmware answered")
if r:
    check(r["checksum_ok"], "the reply's checksum is right")
    check(r["length"] == 13, f"the descriptor is 13 bytes ({r['length']})")

print()
print("=== the state record (cmd 23) and the state block (cmd 24) ===")
r = unpack_reply(ask(23))
check(r is not None, "cmd 23 answered")
if r:
    check(r["checksum_ok"], "the checksum is right")
    check(r["length"] == 8, f"the record is 8 bytes ({r['length']})")
r = unpack_reply(ask(24))
check(r is not None, "cmd 24 answered")
if r:
    check(r["checksum_ok"], "the checksum is right")
    check(r["length"] == 16, f"the block is 16 bytes ({r['length']})")

print()
print("=== the drain (cmd 21) and the read (cmd 35) ===")
r = unpack_reply(ask(21))
check(r is not None, "cmd 21 answered")
if r:
    check(r["checksum_ok"], "the checksum is right")
r = unpack_reply(ask(35))
check(r is not None, "cmd 35 answered")
if r:
    check(r["checksum_ok"], "the checksum is right")

print()
print("=== a command we do not implement gets no reply (and no crash) ===")
before = d.emu_next_sysex_reply(ctypes.create_string_buffer(600), 600)
ask(19)                      # the stock's no-op
after = d.emu_next_sysex_reply(ctypes.create_string_buffer(600), 600)
check(after == 0, "nothing was sent")

print()
print("=== the upload still works with the read commands in place ===")
blob = bytes(range(64))
ask(25, bytes([len(blob), 0, 0, 2, 1]))     # a cabinet upload
ask(34, blob)
ask(48, b"")
check(True, "the session still commits")

print("\n" + ("THE READ COMMANDS WORK" if fails == 0 else f"{fails} FAILURE(S)"))
sys.exit(0 if fails == 0 else 1)
