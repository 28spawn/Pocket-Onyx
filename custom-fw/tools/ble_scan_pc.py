#!/usr/bin/env python3
"""
custom-fw/tools/ble_scan_pc.py - what does the PC's Bluetooth radio hear from the Pocket Amp?

Needs the `bleak` package (pip install bleak). Scans for N seconds and lists every advertiser, strongest first,
with the advertising data it carried; the unit (name "Pocket AMP_BLE", see BLE_DEVICE_NAME in ble_radio.c) is marked.
If it is not heard while `jl_ram_read.py ble` says the advertising events keep growing, the radio is transmitting
but not on a frequency / with a power / in a format the PC accepts (or the PC's adapter is off).

    python custom-fw/tools/ble_scan_pc.py [--seconds 15] [--name Pocket]
"""
import argparse
import asyncio

from bleak import BleakScanner


async def main(seconds: float, needle: str) -> int:
    seen = {}

    def cb(dev, adv):
        seen[dev.address] = (dev, adv)

    async with BleakScanner(cb, scanning_mode="active"):
        await asyncio.sleep(seconds)
    rows = sorted(seen.values(), key=lambda r: -(r[1].rssi if r[1].rssi is not None else -127))
    print(f"heard {len(rows)} advertisers in {seconds:g} s")
    hit = 0
    for dev, adv in rows:
        name = adv.local_name or dev.name or ""
        mark = ""
        if needle.lower() in name.lower():
            mark, hit = "   <== the Pocket Amp", hit + 1
        print(f"  {dev.address}  rssi {adv.rssi:>4}  {name!r}  services {list(adv.service_uuids)}{mark}")
        if mark:
            print(f"      manufacturer {dict(adv.manufacturer_data)}  service data {dict(adv.service_data)}")
    print("FOUND" if hit else "NOT HEARD")
    return 0 if hit else 1


if __name__ == "__main__":
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--seconds", type=float, default=15)
    ap.add_argument("--name", default="Pocket")
    a = ap.parse_args()
    raise SystemExit(asyncio.run(main(a.seconds, a.name)))

# SPDX-License-Identifier: GPL-3.0-only
# Copyright (C) 2026 The Onyx Authors (@28spawn)
