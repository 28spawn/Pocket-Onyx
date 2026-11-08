#!/usr/bin/env python3
"""
custom-fw/tools/ui_watch_pc.py - watch what the unit's UI state does while someone presses its buttons. READ-ONLY.
Samples the UI context (g_ui, 256 bytes) and the test-state block (button mask) over the USB diagnostic channel at about 8 Hz and prints every change with a timestamp: which bytes of
the UI context changed (offset: old -> new), and the button mask going down / up. Use it to see, for example, whether a footswitch press in STOMP mode changed the state or not.

    python custom-fw/tools/ui_watch_pc.py [--seconds 60] [--ui 0x01C0D840]
"""
import argparse
import struct
import sys
import time
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
import ble_matrix_pc as m

NAMES = {0: "view", 8: "preset_index", 17: "cursor_slot_idx"}
NOISE = {30, 31, 47, 48}      # the input / output level meters change all the time


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--seconds", type=float, default=60.0)
    ap.add_argument("--ui", default="0x01C0D840")
    a = ap.parse_args()
    ui = int(a.ui, 16)
    u = m.Unit("Custom MIDI")
    print("BUILD", u.build_id(), flush=True)
    t0 = time.time()
    prev = bytes(u.ram(ui, 192))
    pb = struct.unpack("<I", bytes(u.ram(0x01C7C390 + 32, 4)))[0]
    print(f"{0.0:6.1f}s watching g_ui[0:192] at {ui:#x}; button mask now {pb:#x}", flush=True)
    while time.time() - t0 < a.seconds:
        try:
            cur = bytes(u.ram(ui, 192))
            bm = struct.unpack("<I", bytes(u.ram(0x01C7C390 + 32, 4)))[0]
        except Exception as e:
            print(f"{time.time() - t0:6.1f}s read error {e}", flush=True)
            time.sleep(0.3)
            continue
        t = time.time() - t0
        if bm != pb:
            print(f"{t:6.1f}s buttons {pb:#04x} -> {bm:#04x}  (bits: 0 HOME, 1 VOL-, 2 VOL+, 3 <, 4 >, 5 PARA)", flush=True)
            pb = bm
        ch = [f"{i}{'(' + NAMES[i] + ')' if i in NAMES else ''}:{prev[i]}->{cur[i]}" for i in range(len(cur)) if cur[i] != prev[i] and i not in NOISE]
        if ch:
            print(f"{t:6.1f}s ui  " + "  ".join(ch[:14]) + (" ..." if len(ch) > 14 else ""), flush=True)
            prev = cur
        time.sleep(0.06)


if __name__ == "__main__":
    main()

# SPDX-License-Identifier: GPL-3.0-only
# Copyright (C) 2026 The Onyx Authors (@28spawn)
