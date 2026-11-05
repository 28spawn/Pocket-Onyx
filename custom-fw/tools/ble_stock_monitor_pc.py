"""Host-side BLE monitor for a baseline unit (measured from the PC).

Read-only towards the unit: it only scans, connects, discovers services, subscribes to the BLE-MIDI characteristic and disconnects.

  python tools/ble_stock_monitor_pc.py --adv 30                       # advertising only: packets/s, RSSI, gaps between packets
  python tools/ble_stock_monitor_pc.py --attempts 6 --hold 30 --out qa/ble-gate/stock_windows.json

Per attempt it records: scan time, connect time, service-discovery time, whether the MIDI characteristic was found, notifications received (count, first, longest silence),
whether the link dropped before the hold ended (when, and the error), the disconnect time, and how long until the unit advertises again.
The unit uses a standard resolvable private address, so the unit is found by name / MIDI service UUID.
Limits: the PC's Bluetooth stack does not expose the connection interval, channel map or CRC errors.
"""
from __future__ import annotations

import argparse
import asyncio
import json
import statistics
import time
from pathlib import Path

from bleak import BleakClient, BleakScanner

MIDI_SVC = "03b80e5a-ede8-4b33-a751-6ce34ec4c700"
MIDI_CHR = "7772e5db-3868-4112-a1a9-f2669d106bf3"


def match(dev, adv, name: str) -> bool:
    return MIDI_SVC in [u.lower() for u in (adv.service_uuids or [])] or (name and name.lower() in ((dev.name or adv.local_name or "").lower()))


async def find_unit(name: str, timeout: float):
    """scan until the unit is seen: (device, rssi, seconds) or None"""
    found = asyncio.Event()
    hit = {}
    t0 = time.monotonic()

    def cb(dev, adv):
        if not found.is_set() and match(dev, adv, name):
            hit.update(dev=dev, rssi=adv.rssi, name=dev.name or adv.local_name)
            found.set()

    async with BleakScanner(cb):
        try:
            await asyncio.wait_for(found.wait(), timeout)
        except asyncio.TimeoutError:
            return None
    return hit["dev"], hit["rssi"], time.monotonic() - t0, hit["name"]


async def adv_watch(name: str, secs: float) -> dict:
    stamps, rssis, addrs = [], [], set()

    def cb(dev, adv):
        if match(dev, adv, name):
            stamps.append(time.monotonic())
            rssis.append(adv.rssi)
            addrs.add(dev.address)

    async with BleakScanner(cb):
        await asyncio.sleep(secs)
    gaps = [(b - a) * 1000 for a, b in zip(stamps, stamps[1:])]
    out = dict(seconds=secs, reports=len(stamps), per_second=round(len(stamps) / secs, 2), addresses=sorted(addrs),
               rssi_min=min(rssis) if rssis else None, rssi_median=statistics.median(rssis) if rssis else None, rssi_max=max(rssis) if rssis else None,
               gap_ms_median=round(statistics.median(gaps), 1) if gaps else None, gap_ms_max=round(max(gaps), 1) if gaps else None)
    print("ADVERTISING:", json.dumps(out))
    return out


async def attempt(n: int, name: str, hold: float, timeout: float, scan_t: float) -> dict:
    r: dict = dict(attempt=n)
    t_start = time.monotonic()
    el = lambda: round(time.monotonic() - t_start, 2)   # noqa: E731
    f = await find_unit(name, scan_t)
    if not f:
        r.update(result="unit not seen in %.0f s" % scan_t)
        return r
    dev, rssi, t_scan, shown = f
    r.update(address=dev.address, name=shown, rssi=rssi, scan_s=round(t_scan, 2))
    dropped = {}
    notes: list[float] = []

    def on_disc(_c):
        if "t" not in dropped:
            dropped["t"] = el()

    try:
        async with BleakClient(dev, disconnected_callback=on_disc, timeout=timeout) as c:
            r["connect_s"] = el()
            midi = c.services.get_characteristic(MIDI_CHR)
            r["services_s"] = el()
            r["services"] = len(list(c.services))
            r["midi_char"] = bool(midi)
            if midi:
                def on_note(_h, data):
                    notes.append(time.monotonic())
                await c.start_notify(midi, on_note)
                r["notify_s"] = el()
            t_hold = time.monotonic()
            while time.monotonic() - t_hold < hold and "t" not in dropped:
                await asyncio.sleep(0.2)
            r["held_s"] = round(time.monotonic() - t_hold, 1)
            r["link_up_at_end"] = "t" not in dropped
            if "t" not in dropped:
                t_d = time.monotonic()
                await c.disconnect()
                r["disconnect_s"] = round(time.monotonic() - t_d, 2)
        r["result"] = "held" if r.get("link_up_at_end") else "dropped"
    except Exception as e:   # connect failures are data, not errors
        r["result"] = "connect failed" if "connect_s" not in r else "error"
        r["error"] = f"{type(e).__name__}: {e}"
    if "t" in dropped and r.get("result") in ("dropped", "error"):
        r["dropped_at_s"] = dropped["t"]
    r["notifications"] = len(notes)
    if len(notes) > 1:
        r["notify_longest_gap_s"] = round(max(b - a for a, b in zip(notes, notes[1:])), 2)
    t_back = time.monotonic()
    again = await find_unit(name, 60.0)
    r["advertising_again_s"] = round(time.monotonic() - t_back, 1) if again else None
    return r


def table(rows: list[dict]) -> None:
    print("\nattempt  result          scan  connect  services  notes  held   advertising again   detail")
    for r in rows:
        print("%-8s %-15s %-5s %-8s %-9s %-6s %-6s %-18s %s" % (r["attempt"], r.get("result"), r.get("scan_s", "-"), r.get("connect_s", "-"), r.get("services_s", "-"),
              r.get("notifications", "-"), r.get("held_s", "-"), r.get("advertising_again_s", "-"), r.get("error", r.get("dropped_at_s", ""))))
    ok = sum(r.get("result") == "held" for r in rows)
    print(f"\n{ok}/{len(rows)} attempts connected and held for the full hold time")


async def main() -> None:
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--name", default="Pocket AMP")
    ap.add_argument("--adv", type=float, default=0.0, help="advertising-only measurement for this many seconds")
    ap.add_argument("--attempts", type=int, default=0)
    ap.add_argument("--hold", type=float, default=30.0)
    ap.add_argument("--timeout", type=float, default=20.0)
    ap.add_argument("--scan", type=float, default=20.0)
    ap.add_argument("--out", default="")
    a = ap.parse_args()
    out: dict = {}
    if a.adv:
        out["advertising"] = await adv_watch(a.name, a.adv)
    rows = []
    for n in range(1, a.attempts + 1):
        print(f"--- attempt {n}/{a.attempts}", flush=True)
        r = await attempt(n, a.name, a.hold, a.timeout, a.scan)
        print(json.dumps(r), flush=True)
        rows.append(r)
        await asyncio.sleep(2.0)
    if rows:
        table(rows)
        out["attempts"] = rows
    if a.out:
        Path(a.out).write_text(json.dumps(out, indent=1))
        print("wrote", a.out)


if __name__ == "__main__":
    asyncio.run(main())
