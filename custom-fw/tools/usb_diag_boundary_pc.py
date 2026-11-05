#!/usr/bin/env python3
"""
custom-fw/tools/usb_diag_boundary_pc.py - does the unit's USB reply path lose replies of an exact multiple of 64 bytes? (pa_usb_chunk.h)

Sends RAM reads (diagnostic channel, 7D 02) of different data sizes and counts how many come back. The prediction, from the arithmetic of the reply size:
    8, 16, 24, 40, 48, 64, 96 data bytes   reply is not a multiple of 64 USB bytes   -> every read comes back
    32 and 240 data bytes                  reply is exactly 64 / 384 USB bytes       -> no short packet ends the transfer: the read does not come back
A firmware with the padding rule (pack 149A + USB fix) must return all of them. Run it on the pack that is on the unit:

    python custom-fw/tools/usb_diag_boundary_pc.py [--trials 8]

The order puts the sizes expected to fail last, and re-reads 8 and 16 afterwards to see whether the failure leaves the channel stuck.
"""
import argparse
import struct
import sys
import time
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
import ble_matrix_pc as m  # noqa: E402

PREDICT_FAIL = {32, 240}
ORDER = [8, 16, 24, 40, 48, 64, 96, 32, 8, 16, 240, 8]


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--trials", type=int, default=8)
    ap.add_argument("--port", default="Custom MIDI")
    ap.add_argument("--expect-fixed", action="store_true", help="the firmware has the padding rule: every size must come back")
    a = ap.parse_args()
    u = m.Unit(a.port)
    print("BUILD", u.build_id(), flush=True)
    bad = 0
    for size in ORDER:
        ok = 0
        for _ in range(a.trials):
            try:
                body = u._call(bytes([0x7D, 0x02]) + struct.pack("<IH", 0x01C7D5D0, size), 0x82, timeout=0.8)
                ok += body[2:4] != bytes([0xFF, 0xFF])
            except TimeoutError:
                pass
            time.sleep(0.3)
        want_none = size in PREDICT_FAIL and not a.expect_fixed
        good = (ok == 0) if want_none else (ok == a.trials)
        bad += not good
        print(f"  read of {size:3d} data bytes: {ok}/{a.trials} came back   expected: {'none' if want_none else 'all'}   {'OK' if good else 'DIFFERENT'}", flush=True)
    print("RESULT:", "matches the expectation for every size" if not bad else f"{bad} size(s) differ from the expectation")
    return 1 if bad else 0


if __name__ == "__main__":
    raise SystemExit(main())
