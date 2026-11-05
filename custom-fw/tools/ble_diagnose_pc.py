#!/usr/bin/env python3
"""
custom-fw/tools/ble_diagnose_pc.py - one read-only report of the unit's Bluetooth: is the radio up and advertising, is a central connected, what do the link counters say, which Bluetooth events did
the unit log (bond restored / saved / forgotten, connects, disconnects, errors), and what is in the bond log in the flash (pa_bond.h). It sends nothing but RAM and flash READS over the USB diag channel.

    python custom-fw/tools/ble_diagnose_pc.py [--watch 30]        --watch N: keep looking for N seconds and print every change of the link state / counters / new Bluetooth events

Run it with the unit on USB. For the bond check: pair (after "Remove device" in Windows), run it, power-cycle the unit, run it again: the bond must still be there and "restored at start-up" must show.
"""
import argparse, re, struct, sys, time, zlib
from pathlib import Path
sys.path.insert(0, str(Path(__file__).resolve().parent))
import ble_matrix_pc as m
import flash_store_pc as F
import perf_monitor_pc as pm

HDR = (Path(__file__).resolve().parent.parent / "include" / "pa_ble_radio.h").read_text(encoding="utf-8")
ENUM = re.search(r"enum \{\s*(PA_BLE_ST_MAGIC.*?PA_BLE_ST_WORDS)", HDR, re.S).group(1)
NAMES = re.findall(r"PA_BLE_ST_([A-Z0-9_]+)", ENUM)
STATE = {0: "idle (not asked)", 1: "requested", 2: "no calibration", 3: "starting", 4: "up", 5: "ADVERTISING", 6: "CONNECTED", 7: "FAILED"}
BLE_EV = {1: "boot decision", 2: "start-up began", 3: "radio up", 4: "advertising enabled", 5: "first advertising event", 6: "a central CONNECTED", 7: "the central LEFT", 8: "ERROR",
          9: "BLE interrupt longer than ever", 10: "MIDI notifications ON", 11: "first BLE-MIDI message", 12: "bond RESTORED at start-up", 13: "bond SAVED to flash", 14: "bond FORGOTTEN"}
BOND_ADDR, BOND_SLOTS, SLOT = 0xBAE00, 8, 64
PA_BOND_MAGIC = 0x444E4F42


def status(u):
    p = struct.unpack("<8I", u.ram(m.PTR_ADDR, 32))
    if p[0] != 0x50454C42:
        return None
    n = min(len(NAMES), 145)
    raw = b"".join(u.ram(p[1] + 4 * i, min(64, 4 * (n - i))) for i in range(0, n, 16))
    w = struct.unpack("<%dI" % n, raw[:4 * n])
    return dict(zip(NAMES, w))


def bond_area(u):
    raw = F.flash(u, BOND_ADDR, BOND_SLOTS * SLOT)
    rows = []
    for i in range(BOND_SLOTS):
        s = raw[i * SLOT:(i + 1) * SLOT]
        if s == b"\xff" * SLOT:
            rows.append((i, "blank", None)); continue
        magic, seq, flags, addr_rand, have_irk, _pad = struct.unpack_from("<IIBBBB", s, 0)
        crc = struct.unpack_from("<I", s, 60)[0]
        ok = magic == PA_BOND_MAGIC and crc == (zlib.crc32(s[:60]) & 0xFFFFFFFF)
        ediv = struct.unpack_from("<H", s, 58)[0]
        rows.append((i, "valid" if ok else "TORN/invalid", dict(seq=seq, bonded=bool(flags & 1), rand=s[12:20].hex(), ediv=ediv, ltk_first=s[20:24].hex(), have_irk=bool(have_irk), addr=s[52:58].hex()[::-1] if have_irk else "-")))
    return rows


def ble_events(u, since=0):
    magic, n = pm.evcount(u)
    out = []
    for k in range(max(since, n - pm.CAP), n):
        try:
            e = pm.evread(u, k)
        except Exception:
            break
        if e["code"] == 20:
            out.append((k, e))
    return out, n


def report(u):
    print("BUILD", u.build_id())
    st = status(u)
    if not st:
        print("no Bluetooth status block: a build without the radio, or the radio did not start")
        return
    state = STATE.get(st["STATE"], st["STATE"])
    print(f"radio: state {state}, error {st['ERR']}, connected {st['CONNECTED']}, MIDI notifications on {st['MIDI_READY']}, central MIDI messages received {st['MIDI_IN']}, MIDI subscriptions {st['MIDI_SUBS']}")
    print(f"advertising: starts {st['ADV_STARTS']}, events {st['ADV_EVENTS']}, heard by a scanner {st['ADV_RX']}, SCAN_REQ {st['SCAN_REQ']}; CONNECT_IND heard {st['CIND_RX']}, accepted {st['CIND_OK']}, rejected {st['CIND_REJ']}")
    # the close history: 8 entries of 6 words (pa_ble_radio.h PA_BLE_ST_HIST0): [0] stack ms at the close, [1] reason | who << 8 | event counter << 16, [2] how long the connection lasted (ms),
    # [3] interval | latency << 16, [4] window offset | size << 16, [5] event IRQs | RX IRQs << 16. who: 1 we closed, 2 the central closed (terminate indication), 3 the establishment rule, 4 supervision timeout
    h0 = NAMES.index("HIST0")
    print("recent link closes (newest first; reason is the HCI code: 0x13 the remote user ended it, 0x08 supervision timeout, 0x3D MIC failure, 0x06 key missing, 0x28 instant passed):")
    rows = []
    for i in range(8):
        w = [st[NAMES[h0 + 6 * i + j]] if h0 + 6 * i + j < len(NAMES) else 0 for j in range(6)]
        if any(w): rows.append(w)
    rows.sort(key=lambda w: -w[0])
    for w in rows:
        why, who, evt = w[1] & 0xFF, (w[1] >> 8) & 0xFF, w[1] >> 16
        print(f"   at stack t={w[0]:8d} ms: reason {why:#04x} who {who} event {evt:5d}, the link lasted {w[2]:6d} ms, interval {w[3] & 0xFFFF} x1.25 ms latency {w[3] >> 16}")
    if not rows: print("   (none)")
    other = {k: st[k] for k in NAMES if any(t in k for t in ("ENC", "SMP", "LINK_", "CLOSE", "PEER", "SUP", "FAIL")) and not k.startswith("HIST") and st[k]}
    print("encryption / link counters that are not zero:", other)
    evs, n = ble_events(u)
    print(f"Bluetooth events in the unit's log ({len(evs)} of the last {min(n, pm.CAP)}):")
    for k, e in evs[-25:]:
        print(f"   #{k:5d}  {BLE_EV.get(e['a'], e['a'])}  (b={e['b']})")
    rows = bond_area(u)
    print("bond log in the flash (0xBAE00, 8 slots):")
    for i, kind, d in rows:
        print(f"   slot {i}: {kind}" + (f"  seq {d['seq']} {'BOND' if d['bonded'] else 'forgotten'} ediv {d['ediv']:#06x} rand {d['rand']} ltk[0:4] {d['ltk_first']} irk {d['have_irk']} peer {d['addr']}" if d else ""))
    valid = [(i, d) for i, kind, d in rows if kind == "valid"]
    cur = max(valid, key=lambda x: x[1]["seq"]) if valid else None
    print("stored bond now:", "none" if not cur or not cur[1]["bonded"] else f"yes (slot {cur[0]}, ediv {cur[1]['ediv']:#06x})")
    restored = [e for _, e in evs if e["a"] == 12]
    saved = [e for _, e in evs if e["a"] == 13]
    print(f"this power-on: bond restored {len(restored)}x, saved {len(saved)}x")
    return st, evs, n


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--watch", type=float, default=0)
    a = ap.parse_args()
    u = m.Unit("Custom MIDI")
    res = report(u)
    if a.watch and res:
        st0, evs, seen = res
        print(f"\nwatching for {a.watch:.0f} s (connect / pair / disconnect from the PC now) ...")
        t0 = time.time(); last = (st0["CONNECTED"], st0["MIDI_READY"])
        while time.time() - t0 < a.watch:
            time.sleep(0.4)
            try:
                st = status(u); new, seen2 = ble_events(u, seen); seen = seen2
            except Exception as e:
                print(f"t={time.time() - t0:5.1f}s read error {e}"); continue
            if st and (st["CONNECTED"], st["MIDI_READY"]) != last:
                last = (st["CONNECTED"], st["MIDI_READY"]); print(f"t={time.time() - t0:5.1f}s connected {st['CONNECTED']}, MIDI on {st['MIDI_READY']}")
            for k, e in new:
                print(f"t={time.time() - t0:5.1f}s event: {BLE_EV.get(e['a'], e['a'])} (b={e['b']})")
        print("\nafter the watch:")
        report(u)


if __name__ == "__main__":
    main()
