#!/usr/bin/env python3
"""
custom-fw/tools/usb_midi_probe.py - talk to the running Pocket Amp firmware over its USB-MIDI port.

Sends one SysEx request and prints EVERYTHING the unit sends back, with timestamps, so a missing
reply can be told apart from a reply that the tool did not show. SysEx is not ignored here (the
default of python-rtmidi is to drop it).

    python custom-fw/tools/usb_midi_probe.py                 # the OTA "who are you?" query (read only)
    python custom-fw/tools/usb_midi_probe.py --listen 5      # just listen for 5 s
    python custom-fw/tools/usb_midi_probe.py --hex "F0 00 32 45 00 00 00 40 7F F7"

The "enter upgrade" frame (F0 22 24 35 7F F7) is NOT sent by this tool: it resets the unit into the
ROM download mode, so it is only ever sent on purpose (enter_download_mode.py --enter).
"""
from __future__ import annotations

import argparse
import sys
import time

QUERY = [0xF0, 0x00, 0x32, 0x45, 0x00, 0x00, 0x00, 0x40, 0x7F, 0xF7]   # the OTA query: the device answers with its ID block
UPGRADE = [0xF0, 0x22, 0x24, 0x35, 0x7F, 0xF7]


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--port", default="Custom MIDI", help="part of the MIDI port name (default: Custom MIDI)")
    ap.add_argument("--listen", type=float, default=3.0, help="seconds to listen after sending")
    ap.add_argument("--hex", help="a message to send instead of the query, e.g. 'F0 7E 7F 06 01 F7'")
    a = ap.parse_args()

    import rtmidi
    mo, mi = rtmidi.MidiOut(), rtmidi.MidiIn()
    outs, ins = mo.get_ports(), mi.get_ports()
    print("MIDI outputs:", outs)
    print("MIDI inputs :", ins)
    oi = next((i for i, n in enumerate(outs) if a.port.lower() in n.lower()), None)
    ii = next((i for i, n in enumerate(ins) if a.port.lower() in n.lower()), None)
    if oi is None or ii is None:
        print(f"no MIDI port with '{a.port}' in its name (is the unit running and connected?)")
        return 1
    mi.ignore_types(sysex=False, timing=True, active_sense=True)      # do NOT drop SysEx
    mi.open_port(ii)
    mo.open_port(oi)
    time.sleep(0.2)
    while mi.get_message():                                           # drop anything stale
        pass

    msg = [int(x, 16) for x in a.hex.split()] if a.hex else QUERY
    if msg[:6] == UPGRADE:
        print("refusing to send the enter-upgrade frame from this tool")
        return 2
    print(f"sending on '{outs[oi]}': {' '.join(f'{b:02X}' for b in msg)}")
    t0 = time.time()
    mo.send_message(msg)
    n = 0
    while time.time() - t0 < a.listen:
        m = mi.get_message()
        if m:
            data, _dt = m
            n += 1
            print(f"  +{time.time() - t0:6.3f}s  from '{ins[ii]}': {len(data)} bytes: {' '.join(f'{b:02X}' for b in data)}")
        else:
            time.sleep(0.002)
    print(f"{n} message(s) received in {a.listen:.1f} s")
    mo.close_port()
    mi.close_port()
    return 0 if n else 3


if __name__ == "__main__":
    sys.exit(main())
