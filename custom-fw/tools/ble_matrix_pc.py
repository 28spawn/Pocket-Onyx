#!/usr/bin/env python3
"""
custom-fw/tools/ble_matrix_pc.py - the Bluetooth experiment matrix, run from the PC with the unit on USB.

For each profile (a set of run-time switches in the firmware, include/pa_ble_diag.h) it makes N connection attempts to the unit over BLE (bleak) and, around every attempt, reads
the unit's own link instrumentation live over the USB-MIDI diagnostic channel (RAM read 7D 02; profile write 7D 03). Nothing is flashed between attempts and the unit is never put
into download mode, so the state being measured survives.

Each attempt: connect, read the service list, turn the MIDI notifications on, send the identity query, wait for the reply, hold the link a few seconds, disconnect.
Output: one CSV row per attempt, a success summary per profile, and per profile the per-channel heard/seen table gained during its attempts.

    python custom-fw/tools/ble_matrix_pc.py                       # profiles default,no_enc,hw_remap,widen2; 10 attempts each
    python custom-fw/tools/ble_matrix_pc.py --profiles default --attempts 3
    python custom-fw/tools/ble_matrix_pc.py --profiles 0x12,0x13  --csv matrix.csv
    python custom-fw/tools/ble_matrix_pc.py --report              # just print the unit's current instrumentation (no BLE)
    python custom-fw/tools/ble_matrix_pc.py --set-profile 0x12    # set the profile and exit

Needs: pip install bleak python-rtmidi. The unit must be running pack 149 or later, USB connected ("Custom MIDI" port), and not connected to another central.
"""
from __future__ import annotations

import argparse
import asyncio
import csv
import queue
import re
import statistics
import struct
import sys
import time
from pathlib import Path

HERE = Path(__file__).resolve().parent
sys.path.insert(0, str(HERE))
import ble_diag_decode as bd  # noqa: E402

PTR_ADDR = 0x01C7D5D0
MIDI_CHAR = "7772e5db-3868-4112-a1a9-f2669d106bf3"
IDENTITY = bytes.fromhex("F000324500000040" "7F" "F7")
PROFILES = {"default": 0x72, "vendor_win": 0x72, "soft_remap": 0x12, "no_enc": 0x73, "hw_remap": 0x32, "widen2": 0x3A, "strict": 0x00}   # pack 153: the default has the vendor window (bit 6); hw_remap = pack 152 behaviour (no vendor window); soft_remap is the pre-152 default


def status_index(name: str) -> int:
    hdr = (HERE.parent / "include" / "pa_ble_radio.h").read_text(encoding="utf-8")
    enum = re.search(r"enum \{\s*(PA_BLE_ST_MAGIC.*?PA_BLE_ST_WORDS)", hdr, re.S).group(1)
    names = re.findall(r"PA_BLE_ST_([A-Z0-9_]+)", enum)
    return names.index(name)


def pack_8to7(data: bytes) -> bytes:
    bits = nb = 0
    out = bytearray()
    for b in data:
        bits |= b << nb
        nb += 8
        while nb >= 7:
            out.append(bits & 0x7F)
            bits >>= 7
            nb -= 7
    if nb:
        out.append(bits & 0x7F)
    return bytes(out)


def unpack_7to8(data: bytes) -> bytes:
    bits = nb = 0
    out = bytearray()
    for b in data:
        bits |= (b & 0x7F) << nb
        nb += 7
        if nb >= 8:
            out.append(bits & 0xFF)
            bits >>= 8
            nb -= 8
    return bytes(out)


class Unit:
    """the unit's diagnostic channel over USB-MIDI"""

    def __init__(self, port: str):
        import rtmidi
        self.reads = self.retries = 0
        self.mo, self.mi = rtmidi.MidiOut(), rtmidi.MidiIn()
        self.mi.ignore_types(sysex=False)
        oi = next((i for i, n in enumerate(self.mo.get_ports()) if port.lower() in n.lower()), None)
        ii = next((i for i, n in enumerate(self.mi.get_ports()) if port.lower() in n.lower()), None)
        if oi is None or ii is None:
            raise SystemExit(f"no MIDI port containing '{port}': outputs {self.mo.get_ports()}, inputs {self.mi.get_ports()} (is the unit on USB and the pack 149 or later?)")
        self.mo.open_port(oi)
        self.mi.open_port(ii)
        self.q: "queue.Queue[list[int]]" = queue.Queue()
        self.mi.set_callback(lambda ev, _d=None: self.q.put(ev[0]))

    def _call(self, inner: bytes, rep: int, timeout: float = 1.5) -> bytes:
        while not self.q.empty():
            self.q.get_nowait()
        self.mo.send_message([0xF0] + list(pack_8to7(inner)) + [0xF7])
        end = time.time() + timeout
        while time.time() < end:
            try:
                m = self.q.get(timeout=0.1)
            except queue.Empty:
                continue
            if m and m[0] == 0xF0 and m[-1] == 0xF7:
                body = unpack_7to8(bytes(m[1:-1]))
                if len(body) >= 2 and body[0] == 0x7D and body[1] == rep:
                    return body
        raise TimeoutError(f"no reply {rep:#x} from the unit")

    def ram(self, addr: int, n: int) -> bytes:
        out = bytearray()
        while n:
            k = min(n, self.CHUNK)
            for attempt in range(self.RETRIES):
                try:
                    body = self._call(bytes([0x7D, 0x02]) + struct.pack("<IH", addr, k), 0x82, timeout=0.6)
                    break
                except TimeoutError:
                    self.retries += 1
                    if attempt == self.RETRIES - 1:
                        raise
            if body[2:4] == b"\xff\xff":
                raise RuntimeError(f"the unit refused the RAM read at {addr:#x}")
            got = struct.unpack_from("<H", body, 2)[0]
            out += body[8:8 + got]
            addr += got
            n -= got
            self.reads += 1
            if not got:
                break
        return bytes(out)

    CHUNK = 16            # replies of 8 and 16 bytes came back, 32 and more often did not (pack 149A bench, 2026-11-03): small, idempotent reads, retried
    RETRIES = 6

    def build_id(self) -> str:
        w = struct.unpack("<6I", self.ram(0x01C7D5B0, 24))
        if w[0] != 0x49444C42:
            return "none (firmware older than the build-id feature)"
        return str(w[1]) + "  (" + b"".join(x.to_bytes(4, "little") for x in w[2:6]).split(b"\0")[0].decode("ascii", "replace") + ")"

    def profile(self, value: int | None = None) -> int:
        body = self._call(bytes([0x7D, 0x03, 0 if value is None else 1]) + struct.pack("<I", value or 0), 0x83)
        return struct.unpack_from("<I", body, 2)[0]

    def diag(self) -> dict:
        p = struct.unpack("<8I", self.ram(PTR_ADDR, 32))
        if p[0] != 0x50454C42:
            raise SystemExit("no Bluetooth pointer block in the unit's RAM (a build without PA_BLE?)")
        n_fw = (p[6] >> 8) & 0xFF
        i = status_index("DIAG_PTR")
        if not n_fw or n_fw <= i:
            raise SystemExit("this firmware has no link instrumentation block (older than pack 149)")
        ptr = struct.unpack("<I", self.ram(p[1] + 4 * i, 4))[0]
        return bd.decode(self.ram(ptr, bd.SIZE))


def delta_tables(a: dict, b: dict) -> dict:
    return {k: [(y - x) & 0xFFFF for x, y in zip(a[k], b[k])] for k in ("seen0", "heard0", "seen1", "heard1")}


async def attempt(unit: Unit, name_part: str, hold: float, timeout: float, scan: float) -> dict:
    from bleak import BleakClient, BleakScanner
    r = dict(ok=False, connect_s="", services_s="", n_services="", notify=False, reply=False, error="")
    before = unit.diag()
    t0 = time.time()
    got: list[bytes] = []
    try:
        dev = await BleakScanner.find_device_by_filter(lambda d, adv: name_part.lower() in (adv.local_name or d.name or "").lower(), timeout=scan)
        if not dev:
            raise RuntimeError("not found while scanning (advertising? held by another central?)")
        try:
            client = BleakClient(dev, timeout=timeout, winrt=dict(use_cached_services=False))
        except TypeError:
            client = BleakClient(dev, timeout=timeout)
        async with client as c:
            r["connect_s"] = f"{time.time() - t0:.1f}"
            svcs = list(c.services)                      # (bleak has read the whole list by now)
            r["services_s"] = f"{time.time() - t0:.1f}"
            r["n_services"] = len(svcs)
            await c.start_notify(MIDI_CHAR, lambda _h, data: got.append(bytes(data)))
            r["notify"] = True
            ts = int(time.monotonic() * 1000) & 0x1FFF
            pkt = bytes([0x80 | (ts >> 7), 0x80 | (ts & 0x7F)]) + IDENTITY[:-1] + bytes([0x80 | (ts & 0x7F), 0xF7])
            await c.write_gatt_char(MIDI_CHAR, pkt, response=False)
            end = time.time() + hold
            while time.time() < end:
                await asyncio.sleep(0.2)
                if any(b"\x50\x6f\x63\x6b\x65\x74" in g for g in got):
                    r["reply"] = True
            r["ok"] = True
    except Exception as e:  # noqa: BLE001 - every failure is a result here
        r["error"] = f"{type(e).__name__}: {e}"[:100]
    await asyncio.sleep(1.0)
    try:
        after = unit.diag()
        new = after["wr"] - before["wr"]
        ev = after["events"][-min(new, len(after["events"])):] if new > 0 else []
        r["events"] = new
        r["heard_pct"] = f"{100 * sum(e['heard'] for e in ev) // len(ev)}" if ev else ""
        r["tables"] = delta_tables(before, after)
    except Exception as e:  # noqa: BLE001
        r["error"] += f" | diag: {e}"
        r["tables"] = None
    return r


def summarize(name: str, rows: list[dict]) -> None:
    ok = [r for r in rows if r["ok"]]
    full = [r for r in rows if r["ok"] and r["notify"]]
    rep = [r for r in rows if r["reply"]]
    print(f"\n=== profile {name}: {len(rows)} attempts: connected+discovered {len(ok)}, notifications on {len(full)}, identity reply {len(rep)} ===")
    if ok:
        print(f"    time to connect (s): median {statistics.median(float(r['connect_s']) for r in ok):.1f};  to the service list: median {statistics.median(float(r['services_s']) for r in ok):.1f}")
    hp = [int(r["heard_pct"]) for r in rows if r.get("heard_pct")]
    if hp:
        print(f"    share of connection events heard during the attempts: median {statistics.median(hp)}%")
    errs = sorted({r["error"] for r in rows if r["error"]})
    for e in errs[:4]:
        print(f"    error: {e}")
    tabs = [r["tables"] for r in rows if r.get("tables")]
    if tabs:
        tot = {k: [sum(t[k][c] for t in tabs) for c in range(37)] for k in ("seen0", "heard0", "seen1", "heard1")}
        print("    per-channel heard/seen gained (alignment 0 | alignment 1):")
        line = []
        for c in range(37):
            if tot["seen0"][c] or tot["seen1"][c]:
                line.append(f"ch{c:02d} {tot['heard0'][c]}/{tot['seen0'][c]} | {tot['heard1'][c]}/{tot['seen1'][c]}")
        for i in range(0, len(line), 3):
            print("      " + "    ".join(f"{x:28s}" for x in line[i:i + 3]))


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--port", default="Custom MIDI")
    ap.add_argument("--name", default="Pocket")
    ap.add_argument("--profiles", default="default,no_enc,hw_remap,widen2", help="names (default,no_enc,hw_remap,widen2,strict) or hex words, comma separated")
    ap.add_argument("--attempts", type=int, default=10)
    ap.add_argument("--hold", type=float, default=5.0, help="seconds to keep each link after the identity query")
    ap.add_argument("--timeout", type=float, default=20.0)
    ap.add_argument("--scan", type=float, default=10.0)
    ap.add_argument("--csv", default="ble_matrix.csv")
    ap.add_argument("--report", action="store_true")
    ap.add_argument("--set-profile")
    a = ap.parse_args()
    unit = Unit(a.port)
    print("BUILD", unit.build_id(), flush=True)
    if a.set_profile is not None:
        print("profile now", bd.profile_text(unit.profile(int(a.set_profile, 0))))
        return 0
    if a.report:
        d = unit.diag()
        print(bd.report(d))
        return 0
    start_profile = unit.profile()
    all_rows = []
    try:
        for tok in a.profiles.split(","):
            value = PROFILES[tok] if tok in PROFILES else int(tok, 0)
            print(f"\n--- profile {tok}: {bd.profile_text(unit.profile(value))}")
            rows = []
            for i in range(a.attempts):
                r = asyncio.run(attempt(unit, a.name, a.hold, a.timeout, a.scan))
                r.update(profile=tok, attempt=i + 1)
                rows.append(r)
                print(f"  #{i + 1:2d} {'OK  ' if r['ok'] else 'FAIL'} connect {r['connect_s'] or '-':>5}s services {r['services_s'] or '-':>5}s ({r['n_services'] or '-'}) notify {'Y' if r['notify'] else 'n'} reply {'Y' if r['reply'] else 'n'} "
                      f"events {r.get('events', '?')} heard {r.get('heard_pct', '?')}%  {r['error']}")
            summarize(tok, rows)
            all_rows += rows
    finally:
        unit.profile(start_profile)
    print("USB diagnostic channel:", unit.reads, "reads,", unit.retries, "retries")
    with open(a.csv, "w", newline="") as f:
        w = csv.writer(f)
        w.writerow(["profile", "attempt", "ok", "connect_s", "services_s", "n_services", "notify", "reply", "events", "heard_pct", "error"])
        for r in all_rows:
            w.writerow([r["profile"], r["attempt"], int(r["ok"]), r["connect_s"], r["services_s"], r["n_services"], int(r["notify"]), int(r["reply"]), r.get("events", ""), r.get("heard_pct", ""), r["error"]])
    print(f"\nwrote {a.csv}; the unit's profile was put back to {bd.profile_text(start_profile)}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
