#!/usr/bin/env python3
"""
custom-fw/tools/usb_editor_pc.py - the web editor's USB-MIDI path against the real unit, without a browser (the page's own protocol code runs in Node, python-rtmidi is only the port):
identity, state record, name lists, 40 repeated state reads (reliability + latency), a control written and read back and RESTORED, a preset change and back, and a live cabinet.
Everything it changes is put back. Close MIDIberry / the browser editor and every other diagnostic tool first.

    python custom-fw/tools/usb_editor_pc.py [--port "Custom MIDI"] [--reads 40]
"""
import argparse, json, re, subprocess, sys, tempfile, time
from pathlib import Path
import rtmidi

FW = Path(__file__).resolve().parent.parent
TMP = tempfile.mkdtemp(prefix="pa_usb_editor_")
page = (FW / "web" / "Onyx_Web_Editor.html").read_text(encoding="utf-8")
(Path(TMP) / "page.js").write_text(re.search(r"<script>(.*)</script>", page, re.S).group(1), encoding="utf-8")
(Path(TMP) / "run.js").write_text("const c=require('./page.js');const ops=JSON.parse(require('fs').readFileSync(0,'utf8'));"
    "console.log(JSON.stringify(ops.map(([f,...a])=>{const r=c[f](...a);return ArrayBuffer.isView(r)?Array.from(r):r;})));")


def js(*ops):
    r = subprocess.run(["node", str(Path(TMP) / "run.js")], input=json.dumps(list(ops)), capture_output=True, text=True, cwd=TMP)
    if r.returncode:
        raise SystemExit(r.stderr)
    return json.loads(r.stdout)


def main():
    ap = argparse.ArgumentParser(); ap.add_argument("--port", default="Custom MIDI"); ap.add_argument("--reads", type=int, default=40)
    a = ap.parse_args()
    mi, mo = rtmidi.MidiIn(), rtmidi.MidiOut(); mi.ignore_types(sysex=False)
    pi = next((i for i, n in enumerate(mi.get_ports()) if a.port.lower() in n.lower()), None)
    po = next((i for i, n in enumerate(mo.get_ports()) if a.port.lower() in n.lower()), None)
    if pi is None or po is None:
        print("no unit port"); return 2
    mi.open_port(pi); mo.open_port(po)
    ok = True

    def drain():
        while mi.get_message():
            pass

    def ask(cmd, payload, want, label, wait=3.0, quiet=False):
        nonlocal ok
        drain(); t0 = time.time(); mo.send_message(js(["buildFrame", cmd, payload])[0]); got = None
        while time.time() - t0 < wait and not got:
            m = mi.get_message()
            if not m:
                time.sleep(0.001); continue
            if m[0][0] == 0xF0:
                r = js(["parseReply", m[0]])[0]
                if r and (want is None or r["type"] == want):
                    got = r
        dt = time.time() - t0
        if not quiet:
            print(f"  {'ok  ' if got else 'FAIL'} {label}" + (f": {len(got['data'])} bytes in {dt * 1000:.0f} ms" if got else f": no reply in {wait:.0f} s"))
        if not got:
            ok = False
        return got, dt

    def state():
        r, _ = ask(80, [], 0x60, "state", quiet=True)
        return js(["decodeState", r["data"]])[0] if r else None

    def send(msg):
        for i in range(0, len(msg), 3):
            mo.send_message(msg[i:i + 3]); time.sleep(0.005)

    r, _ = ask(17, [], None, "device info (cmd 17)")
    st = state(); print("  state:", {k: st[k] for k in ("preset", "name", "outLevel", "namCount", "cabCount")} if st else None, "loopback", st and st["g"].get("loopback"))
    if not st:
        return 1
    for kind in (0, 1):
        n, _ = ask(81, [kind, 0], 0x61, f"names kind {kind}")
        if n:
            d = n["data"]; print(f"        {d[1]} types, {d[3]} in this page, first user {d[4]}")
    lat = []; fails = 0
    for _ in range(a.reads):
        r, dt = ask(80, [], 0x60, "", quiet=True)
        if r: lat.append(dt * 1000)
        else: fails += 1
    ok = ok and fails == 0
    if lat:
        lat.sort(); print(f"  {'ok  ' if not fails else 'FAIL'} {a.reads} state reads: {fails} lost, latency median {lat[len(lat) // 2]:.0f} ms, max {lat[-1]:.0f} ms")
    # a control, written and read back, then restored
    before = st["outLevel"]; test = before - 1 if before > 1 else before + 1
    send(js(["nrpn", 8, 2, test, True])[0]); time.sleep(0.3); s2 = state()
    good = bool(s2 and s2["outLevel"] == test); ok = ok and good
    print(f"  {'ok  ' if good else 'FAIL'} output level {before} -> {test}: unit reports {s2 and s2['outLevel']}")
    send(js(["nrpn", 8, 2, before, True])[0]); time.sleep(0.3); s3 = state()
    good = bool(s3 and s3["outLevel"] == before); ok = ok and good
    print(f"  {'ok  ' if good else 'FAIL'} restored to {before}: unit reports {s3 and s3['outLevel']}")
    # a preset change and back
    other = (st["preset"] + 1) % 80
    mo.send_message(js(["pc", other])[0]); time.sleep(1.2); s4 = state()
    good = bool(s4 and s4["preset"] == other); ok = ok and good
    print(f"  {'ok  ' if good else 'FAIL'} program change to {other + 1}: unit shows {s4 and s4['preset'] + 1} \"{s4 and s4['name']}\"")
    mo.send_message(js(["pc", st["preset"]])[0]); time.sleep(1.2); s5 = state()
    good = bool(s5 and s5["preset"] == st["preset"]); ok = ok and good
    print(f"  {'ok  ' if good else 'FAIL'} back to {st['preset'] + 1}: unit shows {s5 and s5['preset'] + 1}")
    # a live cabinet (RAM only), then the preset reload drops it
    import struct
    taps = struct.pack("<256i", *([int(0.8 * (1 << 18))] + [(i * 37) % 2000 - 1000 for i in range(255)]))
    t0 = time.time()
    for f in js(["uploadFrames", list(taps), 2, 254, 100])[0]:
        mo.send_message(f); time.sleep(0.03)
    time.sleep(1.0); s6 = state(); live = bool(s6 and s6["userCab"] == 1); ok = ok and live
    print(f"  {'ok  ' if live else 'FAIL'} live cabinet over USB in {time.time() - t0:.1f} s: unit reports it loaded")
    mo.send_message(js(["pc", st["preset"]])[0]); time.sleep(1.5); s7 = state()
    good = bool(s7 and s7["userCab"] == 0); ok = ok and good
    print(f"  {'ok  ' if good else 'FAIL'} the preset reload dropped it")
    print("\nUSB EDITOR PATH WORKS" if ok else "\nUSB EDITOR PATH HAS FAILURES")
    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main())

# SPDX-License-Identifier: GPL-3.0-only
# Copyright (C) 2026 The Onyx Authors (@28spawn)
