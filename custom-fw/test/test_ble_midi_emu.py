# SPDX-License-Identifier: GPL-3.0-only
# Copyright (C) 2026 The Onyx Authors (@28spawn)
"""The BLE-MIDI path, end to end, through the firmware.

The custom firmware has one MIDI ingress: a 512-byte ring that both the USB
MIDI driver and the BLE GATT writer push into, and that
`pa_usb_ble_poll()` drains into the SysEx assembler. The BLE writer used to
forward the raw bytes, but a real BLE-MIDI peer sends BLE-MIDI 1.0 packets
([header][timestamp][message], and SysEx continuation packets with raw data),
so the framing has to be stripped first.

This drives the emulator's DLL exactly as a BLE write would arrive - through
`emu_feed_ble_midi()`, which is `pa_ble_gatt_rx_write()` + the ring drain -
and checks that a command sent over BLE produces the SAME reply as the same
command sent over the raw USB MIDI path.

    python custom-fw/test/test_ble_midi_emu.py [path/to/pocket_amp_emu.dll]
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
d.emu_feed_ble_midi.argtypes = [ctypes.POINTER(ctypes.c_uint8), ctypes.c_uint32]
d.emu_next_sysex_reply.argtypes = [ctypes.c_char_p, ctypes.c_int]
d.emu_next_sysex_reply.restype = ctypes.c_int
d.emu_next_ble_tx.argtypes = [ctypes.c_char_p, ctypes.c_int]
d.emu_next_ble_tx.restype = ctypes.c_int

sys.path.insert(0, os.path.join(ROOT, "tools"))
import mvave_device as M          # noqa: E402

fails = 0


def check(ok, msg):
    global fails
    print(("  ok   " if ok else "  FAIL ") + msg)
    if not ok:
        fails += 1


def take_reply():
    buf = ctypes.create_string_buffer(600)
    n = d.emu_next_sysex_reply(buf, 600)
    return bytes(buf.raw[:n]) if n > 0 else None


def take_ble_packets():
    """The BLE-MIDI packets the device notified, one per call."""
    packets = []
    while True:
        buf = ctypes.create_string_buffer(600)
        n = d.emu_next_ble_tx(buf, 600)
        if n <= 0:
            break
        packets.append(bytes(buf.raw[:n]))
    return packets


def ask_over_usb(cmd, payload=b""):
    frame = M.build_frame(cmd, payload)
    b = (ctypes.c_uint8 * len(frame)).from_buffer_copy(frame)
    d.emu_feed_sysex_bytes(b, len(frame))
    return take_reply()


def ask_over_ble(cmd, payload=b"", mtu=20):
    """The same command, framed the way a BLE-MIDI peer sends it."""
    frame = M.build_frame(cmd, payload)
    packets = M.ble_midi_packets(frame, mtu)
    for pkt in packets:
        b = (ctypes.c_uint8 * len(pkt)).from_buffer_copy(pkt)
        d.emu_feed_ble_midi(b, len(pkt))
    return take_reply(), packets


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
    length = l0 | (l1 << 8) | (l2 << 16)
    data = inner[6:6 + length]
    chk = inner[6 + length]
    total = (inner[2] + l0 + l1 + l2 + sum(data)) & 0xFF
    return {"type": inner[2], "length": length, "data": data,
            "checksum_ok": chk == ((~total) & 0xFF)}


print("=== the BLE-MIDI framing, host side (the tool) ===")
frame = M.build_frame(17)
packets = M.ble_midi_packets(frame, 23)
print(f"    a {len(frame)} byte SysEx frame -> {len(packets)} BLE packets")
check(packets[0][0] == 0x80 and packets[0][1] == 0x80,
      "the first packet is 0x80 0x80 …")
check(all(p[0] == 0x80 for p in packets), "every packet starts with the header 0x80")
check(packets[-1][-2] == 0x80 and packets[-1][-1] == 0xF7,
      "the closing F7 is preceded by a timestamp byte")
back = M.ble_midi_unframe(packets)
check(back == frame, "the framing round trips back to the same frame")

print("\n=== the device info over the raw USB MIDI path ===")
usb = ask_over_usb(17)
r = unpack_reply(usb)
check(r is not None, "the firmware answered")
if r:
    print(f"    the reply: type {r['type']} (0x{r['type']:02x}), {r['length']} bytes")
    check(r["checksum_ok"], "the reply's checksum is right")
    check(b"Onyx" in r["data"],
          "the reply carries the name the editor matches on")

print("\n=== the same command over BLE-MIDI ===")
ble, packets = ask_over_ble(17)
print(f"    {len(packets)} packets, {sum(len(p) for p in packets)} bytes on the air")
check(ble is not None, "the firmware answered over BLE")
if ble is not None:
    print(f"    the reply: {len(ble)} bytes")
    check(ble == usb, "the BLE reply is byte-identical to the USB reply")
    rb = unpack_reply(ble)
    check(rb is not None and rb["checksum_ok"], "the BLE reply's checksum is right")
    check(rb and b"Onyx" in rb["data"],
          "the BLE reply carries the device name")

print("\n=== a command over BLE with a negotiated MTU ===")
ble_small, packets_small = ask_over_ble(17, mtu=40)
print(f"    {len(packets_small)} packets")
check(ble_small == usb, "the reply is the same with a bigger MTU")

print("\n=== the read commands over BLE ===")
for cmd, name in ((18, "the descriptor"), (21, "the drain"), (35, "the read")):
    b, _ = ask_over_ble(cmd)
    rr = unpack_reply(b)
    check(rr is not None, f"{name} (cmd {cmd}) answered over BLE")
    if rr:
        print(f"    cmd {cmd}: type {rr['type']} (0x{rr['type']:02x}), {rr['length']} bytes")

print("\n=== the reply leaves over BLE, framed ===")
take_ble_packets()          # drain the replies of the checks above
ble, _ = ask_over_ble(17)
packets = take_ble_packets()
print(f"    the device notified {len(packets)} BLE-MIDI packets")
check(len(packets) >= 1, "the device sent the reply over BLE")
if packets:
    check(packets[0][0] == 0x80 and packets[0][1] == 0x80,
          "the first packet is 0x80 0x80 …")
    check(packets[-1][-2] == 0x80 and packets[-1][-1] == 0xF7,
          "the closing F7 is preceded by a timestamp byte")
    back = M.ble_midi_unframe(packets)
    check(back == ble,
          "unframing the notified packets gives back the same reply")

print("\n" + ("THE BLE PATH WORKS" if fails == 0 else "SOME CHECKS FAILED"))
sys.exit(0 if fails == 0 else 1)
