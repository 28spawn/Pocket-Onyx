#!/usr/bin/env python3
"""Read a region of the Pocket Amp's SPI NOR flash over USB MIDI.

The device answers on a private diagnostic SysEx (manufacturer 0x7D):

    F0 7D 01 <addr u32 LE> <len u16 LE> F7      ->  F0 7D 81 <len u16 LE> <addr u32 LE> <data> F7

The payload travels packed 8-to-7 (USB MIDI carries 7-bit data bytes), the same bit
stream the editor's frames use.

Diagnostic reader utility for SPI NOR flash over USB MIDI. READ ONLY: changes nothing on the device.

    python custom-fw/tools/dump_flash.py --list
    python custom-fw/tools/dump_flash.py 0x0B5000:32768 -o vm_partition.bin
    python custom-fw/tools/dump_flash.py 0x0B5000:32768 -o vm_partition.bin --strings
"""
import argparse
import os
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from mvave_device import UsbMidiLink, pack_8to7, unpack_7to8   # noqa: E402

MFG_ID = 0x7D
CMD_FLASH_READ = 0x01
REP_FLASH_READ = 0x81


def read_region(link, addr, length, chunk=256):
    """Read `length` bytes at `addr`, in chunks, and return them."""
    out = bytearray()
    while len(out) < length:
        want = min(chunk, length - len(out))
        inner = bytes([MFG_ID, CMD_FLASH_READ]) + addr.to_bytes(4, "little") + \
            want.to_bytes(2, "little")
        frame = bytes([0xF0]) + pack_8to7(inner) + bytes([0xF7])
        link.send_frame(frame)
        reply = None
        for raw in link.read_frames(2.0):
            if len(raw) >= 3 and raw[0] == 0xF0 and raw[-1] == 0xF7:
                body = unpack_7to8(raw[1:-1])
                if body and body[0] == MFG_ID and body[1] == REP_FLASH_READ:
                    reply = body
                    break
        if reply is None:
            raise RuntimeError("the device did not answer the flash read at 0x%08X" % addr)
        if len(reply) < 4:
            raise RuntimeError("short reply %r" % (bytes(reply[:8]),))
        if reply[2] == 0xFF and reply[3] == 0xFF:
            raise RuntimeError("the device could not read 0x%08X" % addr)
        rlen = reply[2] | (reply[3] << 8)
        raddr = int.from_bytes(reply[4:8], "little")
        data = reply[8:8 + rlen]
        if raddr != addr + len(out):
            raise RuntimeError("the reply is for 0x%08X, expected 0x%08X"
                               % (raddr, addr + len(out)))
        out += data
    return bytes(out)


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[1])
    ap.add_argument("region", nargs="?", help="ADDR:LEN in hex, e.g. 0x0B5000:32768")
    ap.add_argument("-o", "--out", help="write the bytes here (default: stdout)")
    ap.add_argument("--strings", action="store_true",
                    help="print the printable strings in the region")
    ap.add_argument("--list", action="store_true", help="list the MIDI ports and exit")
    args = ap.parse_args()

    if args.list:
        outs, ins = UsbMidiLink.ports()
        print("outputs:", outs)
        print("inputs: ", ins)
        return 0

    if not args.region:
        ap.print_help()
        return 2

    addr_s, len_s = args.region.split(":")
    addr = int(addr_s, 0)
    length = int(len_s, 0)

    link = UsbMidiLink()
    link.open()
    try:
        data = read_region(link, addr, length)
    finally:
        link.close()

    if args.out:
        with open(args.out, "wb") as fh:
            fh.write(data)
        print("wrote %s (%d bytes from 0x%08X)" % (args.out, len(data), addr))
    else:
        sys.stdout.buffer.write(data)

    if args.strings:
        cur = bytearray()
        base = addr
        for i, b in enumerate(data):
            if 32 <= b < 127:
                if not cur:
                    cur.append(b)
                else:
                    cur.append(b)
            else:
                if len(cur) >= 4:
                    print("  0x%08X  %s" % (base + i - len(cur), cur.decode("latin1")))
                cur = bytearray()
        if len(cur) >= 4:
            print("  0x%08X  %s" % (base + len(data) - len(cur), cur.decode("latin1")))
    return 0


if __name__ == "__main__":
    sys.exit(main())

# SPDX-License-Identifier: GPL-3.0-only
# Copyright (C) 2026 The Onyx Authors (@28spawn)
