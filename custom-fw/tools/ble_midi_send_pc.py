#!/usr/bin/env python3
"""
custom-fw/tools/ble_midi_send_pc.py - connect to the Pocket Amp over BLE from the PC, turn the BLE-MIDI notifications on and send it MIDI messages.

Needs `bleak` (pip install bleak). The unit accepts one central at a time: if a phone or another program holds the link, the connect fails.
From pack 143 the firmware routes BLE-MIDI to the amp's MIDI router like USB MIDI: a Program Change (C0 n) jumps to preset n+1, and the identity query below is answered with a SysEx
notification (printed here). Counters: python custom-fw/tools/jl_ram_read.py ble  ("BLE-MIDI messages received", "event packets queued for the central").

    python custom-fw/tools/ble_midi_send_pc.py [--name Pocket] [--status 0xB0 --d1 20 --d2 64 | --hex "F0 00 32 45 00 00 00 40 7F F7"] [--count 1] [--listen 3]
"""
import argparse
import asyncio
import time

from bleak import BleakClient, BleakScanner

MIDI_CHAR = "7772e5db-3868-4112-a1a9-f2669d106bf3"


def ble_midi_packet(msg):
    """one BLE-MIDI packet: header (0x80 | timestamp bits 12..7), timestamp low (0x80 | bits 6..0), then the message. A SysEx (F0 .. F7) gets a timestamp byte before the F7."""
    ts = int(time.monotonic() * 1000) & 0x1FFF
    hdr, lo = 0x80 | (ts >> 7), 0x80 | (ts & 0x7F)
    msg = bytes(msg)
    if msg and msg[0] == 0xF0 and msg[-1] == 0xF7:
        return bytes([hdr, lo]) + msg[:-1] + bytes([lo, 0xF7])
    return bytes([hdr, lo]) + msg


async def main(a):
    print("scanning for %r ..." % a.name)
    dev = await BleakScanner.find_device_by_filter(lambda d, adv: a.name.lower() in (adv.local_name or d.name or "").lower(), timeout=a.scan)
    if not dev:
        print("not found: is it advertising? (a phone or the PC may already hold the connection)")
        return 1
    print("found", dev.address, dev.name)
    got = []
    async with BleakClient(dev, timeout=a.timeout) as c:
        print("connected:", c.is_connected)
        svc = [s.uuid for s in c.services]
        print("services:", ", ".join(svc))
        await c.start_notify(MIDI_CHAR, lambda _h, data: got.append(bytes(data)))
        print("notifications on (CCCD written)")
        for i in range(a.count):
            msg = bytes.fromhex(a.hex) if a.hex else [a.status, a.d1, a.d2]
            pkt = ble_midi_packet(msg)
            await c.write_gatt_char(MIDI_CHAR, pkt, response=False)
            print("sent", pkt.hex(" "), "(BLE-MIDI packet: header, timestamp, then the message)")
            await asyncio.sleep(0.2)
        await asyncio.sleep(a.listen)
        await c.stop_notify(MIDI_CHAR)
    print("notifications received from the unit: %d %s" % (len(got), [g.hex(" ") for g in got]))
    return 0


if __name__ == "__main__":
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--name", default="Pocket")
    ap.add_argument("--status", type=lambda x: int(x, 0), default=0xB0)
    ap.add_argument("--d1", type=int, default=20)
    ap.add_argument("--d2", type=int, default=64)
    ap.add_argument("--hex", help="send these MIDI bytes instead, e.g. 'F0 00 32 45 00 00 00 40 7F F7' (the unit's read-only identity query) or 'C0 02' (program change: preset 3)")
    ap.add_argument("--count", type=int, default=1)
    ap.add_argument("--listen", type=float, default=2.0)
    ap.add_argument("--scan", type=float, default=10.0)
    ap.add_argument("--timeout", type=float, default=20.0)
    raise SystemExit(asyncio.run(main(ap.parse_args())))
