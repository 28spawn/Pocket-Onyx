#!/usr/bin/env python3
"""
custom-fw/tools/ble_gate_eval.py - the numbers behind hardware test HW-18 (BLE-MIDI link, per host) of TEST-PROTOCOL.md.

It reads the JSON files that tools/ble_link_trace_pc.py writes (gated attempts, or --watch for a host you connect by hand) and applies the pass criteria below to each
host. It reads, prints and decides; it records nothing (the ledger is written with tools/qa_recovery.py hw-run).

Criteria, per host (each has its number in the output):
  B-1  attempts     at least MIN_ATTEMPTS gated attempts, and every one connected and switched the MIDI notifications on (hosts connected by hand: n/a)
  B-2  supervision  0 established links that died of a supervision timeout
  B-3  reception    every established link (first data seen, at least MIN_EVENTS events) was heard in >= 95 % of its events and had >= 95 % valid packets
  B-4  recovery     after every gated disconnect of ours: the link is closed and the unit advertises again, found by a scan, within 10 s
  B-5  established  at least one established link (a host that never completes a connection passes nothing)
Reported, not gated: the share of CONNECT_INDs that failed to establish (the hosts retry; see the numbers), CRC-bad packets, autozoom counts.

    python custom-fw/tools/ble_gate_eval.py windows:qa/ble-gate/windows_155_vendorwin.json iphone:qa/ble-gate/iphone_152_watch.json android:qa/ble-gate/android_152_watch.json
        host:file[,file...] ...  exit 0 = every host PASS and the required hosts are all present, 1 = a host FAILED, 2 = a required host has no data
    --require windows,iphone,android     the hosts that must be present for HW-18 (default)
"""
import argparse
import json
import sys

MIN_ATTEMPTS = 4
MIN_EVENTS = 100
HEARD_MIN = 95.0
VALID_MIN = 95.0
RECOVERY_MAX_S = 10.0


def evaluate(host, files):
    conns, attempts = [], []
    for f in files:
        d = json.load(open(f, encoding="utf-8"))
        conns += d.get("connections", [])
        attempts += d.get("attempts", [])
    rows = []

    def row(code, name, ok, detail):
        rows.append((code, name, ok, detail))

    gated = [a for a in attempts if a.get("t") is not None]
    if gated:
        bad = [a["attempt"] for a in gated if a.get("error") or "gatt_connected" not in a["t"] or "notify_started" not in a["t"]]
        row("B-1", "attempts", len(gated) >= MIN_ATTEMPTS and not bad, f"{len(gated)} attempts (need {MIN_ATTEMPTS}), {len(gated) - len(bad)} connected with notifications on" + (f"; failed: {bad}" if bad else ""))
    else:
        row("B-1", "attempts", None, "n/a (connected by hand)")

    est = [c for c in conns if c.get("first_data_s") is not None and c.get("events", 0) >= MIN_EVENTS]
    sup = [c["id"] for c in conns if (c.get("close") or {}).get("by") == "supervision timeout" and c.get("first_data_s") is not None]
    row("B-2", "supervision", not sup, f"{len(sup)} established links died of a supervision timeout" + (f" (links {sup})" if sup else ""))

    if est:
        mh = min(c["heard_pct"] for c in est)
        mv = min(c["valid_pct"] for c in est)
        worst = min(est, key=lambda c: c["valid_pct"])
        row("B-3", "reception", mh >= HEARD_MIN and mv >= VALID_MIN,
            f"{len(est)} established links: heard min {mh:.1f} % (need {HEARD_MIN:g}), valid min {mv:.1f} % (need {VALID_MIN:g}); worst link #{worst['id']} at {worst.get('interval_ms', 0):g} ms")
    else:
        row("B-3", "reception", False, f"no established link with {MIN_EVENTS}+ events")
    row("B-5", "established", len(est) >= 1, f"{len(est)} established links")

    if gated:
        late = [(a["attempt"], a.get("link_closed_after_our_disconnect_s"), a.get("rescan_found")) for a in gated
                if a.get("link_closed_after_our_disconnect_s") is None or a["link_closed_after_our_disconnect_s"] > RECOVERY_MAX_S or not a.get("rescan_found")]
        worst_t = max((a.get("link_closed_after_our_disconnect_s") or 0.0) for a in gated)
        row("B-4", "recovery", not late, f"slowest close after our disconnect {worst_t:.1f} s (need <= {RECOVERY_MAX_S:g}), scanner found the unit again in all but {len(late)}" + (f": {late}" if late else ""))
    else:
        row("B-4", "recovery", None, "n/a (connected by hand)")

    fails = sum(1 for c in conns if (c.get("close") or {}).get("by") == "could not establish")
    crc = sum(c.get("crc_bad", 0) for c in est)
    ev = sum(c.get("events", 0) for c in est)
    zoom = sum(c.get("zoom_widened", 0) for c in conns)
    info = (f"{len(conns)} CONNECT_INDs, {fails} failed to establish ({100.0 * fails / max(1, len(conns)):.0f} %), "
            f"CRC-bad {crc} of {ev} events on established links, autozoom widenings {zoom}")
    ok = all(r[2] is not False for r in rows)
    return ok, rows, info


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("hosts", nargs="+", help="host:file[,file...]")
    ap.add_argument("--require", default="windows,iphone,android")
    a = ap.parse_args()
    data = {}
    for tok in a.hosts:
        host, _, files = tok.partition(":")
        data.setdefault(host.lower(), []).extend(f for f in files.split(",") if f)
    required = [h for h in a.require.split(",") if h]
    failed, missing = [], [h for h in required if h not in data]
    for host, files in data.items():
        ok, rows, info = evaluate(host, files)
        print(f"\n== {host}  ({len(files)} file{'s' if len(files) != 1 else ''}) : {'PASS' if ok else 'FAIL'}")
        for code, name, r, detail in rows:
            print(f"   {code} {name:12s} {'PASS' if r else ('n/a ' if r is None else 'FAIL')}  {detail}")
        print(f"   info: {info}")
        if not ok:
            failed.append(host)
    print()
    for h in missing:
        print(f"== {h}: NOT TESTED (HW-18 needs it)")
    verdict = "FAIL" if failed else ("INCOMPLETE" if missing else "PASS")
    print(f"HW-18 {verdict}" + (f": failed hosts {failed}" if failed else "") + (f": no data for {missing}" if missing else ""))
    return 1 if failed else (2 if missing else 0)


if __name__ == "__main__":
    sys.exit(main())

# SPDX-License-Identifier: GPL-3.0-only
# Copyright (C) 2026 The Onyx Authors (@28spawn)
