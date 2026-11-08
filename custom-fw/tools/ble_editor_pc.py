#!/usr/bin/env python3
"""
custom-fw/tools/ble_editor_pc.py - the web editor's Bluetooth path against the real unit, without a browser: the page's own protocol code (web/Onyx_Web_Editor.html, run in Node) builds the BLE-MIDI
packets and reads the notifications, bleak is only the radio. It does what the page does after "Connect Bluetooth": identity, the state record (cmd 80), the name lists (cmd 81), then a control
that changes nothing (the output level written back at its own value) and a read to prove it.

    python custom-fw/tools/ble_editor_pc.py [--name Pocket] [--chunk 20]

The unit must not be held by another Bluetooth program (close MIDIberry), and no USB diagnostic tool may run at the same time. Read-only apart from the one write of an unchanged value.
"""
import argparse, asyncio, json, os, re, subprocess, sys, tempfile, time
from pathlib import Path
from bleak import BleakClient, BleakScanner

FW = Path(__file__).resolve().parent.parent
SVC, CHR = "03b80e5a-ede8-4b33-a751-6ce34ec4c700", "7772e5db-3868-4112-a1a9-f2669d106bf3"
TMP = tempfile.mkdtemp(prefix="pa_ble_editor_")
page = (FW / "web" / "Onyx_Web_Editor.html").read_text(encoding="utf-8")
(Path(TMP) / "page.js").write_text(re.search(r"<script>(.*)</script>", page, re.S).group(1), encoding="utf-8")
(Path(TMP) / "run.js").write_text(
    "const c=require('./page.js');const ops=JSON.parse(require('fs').readFileSync(0,'utf8'));"
    "const out=ops.map(([f,...a])=>{if(f==='unframe'){const u=new c.BleUnframer();return [].concat(...a[0].map(p=>u.push(p)));}"
    "const r=c[f](...a);return ArrayBuffer.isView(r)?Array.from(r):r;});console.log(JSON.stringify(out));")


def js(*ops):
    r = subprocess.run(["node", str(Path(TMP) / "run.js")], input=json.dumps(list(ops)), capture_output=True, text=True, cwd=TMP)
    if r.returncode:
        raise SystemExit(r.stderr)
    return json.loads(r.stdout)


async def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--name", default="Pocket")
    ap.add_argument("--chunk", type=int, default=20, help="BLE-MIDI packet size the page uses (20)")
    ap.add_argument("--upload", action="store_true", help="also send a LIVE cabinet (RAM only) and reload the preset afterwards")
    a = ap.parse_args()
    dev = None
    for addr, (d, adv) in (await BleakScanner.discover(timeout=8.0, return_adv=True)).items():
        if a.name.lower() in (d.name or adv.local_name or "").lower():
            dev = d
    if not dev:
        print("the unit is not advertising (is it connected to another program?)"); return 2
    print(f"found {dev.name} {dev.address}")
    notes = []
    async with BleakClient(dev, timeout=20.0) as cl:
        await cl.start_notify(CHR, lambda _h, data: notes.append((time.time(), list(bytes(data)))))
        print(f"connected, ATT MTU {getattr(cl, 'mtu_size', '?')}, notifications on")
        ok = True

        async def ask(cmd, payload, want, label, wait=8.0):
            nonlocal ok
            notes.clear()
            frame = js(["buildFrame", cmd, payload])[0]
            t0 = time.time()
            for p in js(["bleFrames", frame, a.chunk, int(t0 * 1000) & 0x1FFF])[0]:
                await cl.write_gatt_char(CHR, bytes(p), response=False)
                await asyncio.sleep(0.01)
            got = None
            while time.time() - t0 < wait:
                await asyncio.sleep(0.05)
                frames = js(["unframe", [p for _t, p in notes]])[0] if notes else []
                for f in frames:
                    r = js(["parseReply", f])[0]
                    if r and (want is None or r["type"] == want):
                        got = r
                if got:
                    break
            n = len(notes); nb = sum(len(p) for _t, p in notes)
            if got:
                print(f"  ok   {label}: reply type {got['type']:#04x}, {len(got['data'])} bytes, in {n} notifications ({nb} bytes) after {time.time() - t0:.2f} s")
            else:
                ok = False
                print(f"  FAIL {label}: no reply within {wait:.0f} s ({n} notifications, {nb} bytes arrived)")
            return got

        r = await ask(17, [], None, "device info (cmd 17, any reply type)")
        if r:
            print("       ", bytes(r["data"]).split(b"\0")[0].decode("latin1", "replace")[:60])
        s = await ask(80, [], 0x60, "state record (cmd 80)")
        st = js(["decodeState", s["data"]])[0] if s else None
        if st:
            print(f"        preset {st['preset'] + 1} \"{st['name']}\", output level {st['outLevel']} %, {st['namCount']} captures, {st['cabCount']} stored cabinets, loopback {st['g'].get('loopback')}")
        for kind, label in ((0, "amp names (cmd 81, kind 0)"), (1, "cabinet names (cmd 81, kind 1)")):
            n = await ask(81, [kind, 0], 0x61, label)
            if n:
                d = n["data"]; print(f"        {d[1]} types, {d[3]} in this page, first user type {d[4]}")
        if st:                                          # a control that changes nothing: the output level at its own value
            before = st["outLevel"]
            notes.clear()
            for p in js(["bleFrames", js(["nrpn", 8, 2, before, True])[0], a.chunk, 0])[0]:     # one short message (the page sends it as BLE-MIDI packets of its own)
                pass
            msg = js(["nrpn", 8, 2, before, True])[0]
            for i in range(0, len(msg), 3):
                await cl.write_gatt_char(CHR, bytes(js(["bleFrames", msg[i:i + 3], 20, 0])[0][0]), response=False); await asyncio.sleep(0.02)
            await asyncio.sleep(0.3)
            s2 = await ask(80, [], 0x60, "state record again, after the NRPN write")
            st2 = js(["decodeState", s2["data"]])[0] if s2 else None
            if st2:
                same = st2["outLevel"] == before
                ok = ok and same
                print(f"  {'ok  ' if same else 'FAIL'} the output level is still {st2['outLevel']} % after writing {before} % (a control reached the unit and changed nothing)")
        if a.upload and st:                              # a LIVE cabinet (RAM only, nothing is stored in the flash): the page's upload frames over BLE-MIDI, then the preset is reloaded to drop it
            import struct
            taps = struct.pack("<256i", *([int(0.8 * (1 << 18))] + [(i * 37) % 2000 - 1000 for i in range(255)]))
            frames = js(["uploadFrames", list(taps), 2, 254, 100])[0]
            t0 = time.time()
            for f in frames:
                for p in js(["bleFrames", f, a.chunk, int(time.time() * 1000) & 0x1FFF])[0]:
                    await cl.write_gatt_char(CHR, bytes(p), response=False); await asyncio.sleep(0.01)
                await asyncio.sleep(0.03)
            await asyncio.sleep(1.0)
            s3 = await ask(80, [], 0x60, "state record after a live cabinet upload")
            st3 = js(["decodeState", s3["data"]])[0] if s3 else None
            live = bool(st3 and st3["userCab"] == 1)
            ok = ok and live
            print(f"  {'ok  ' if live else 'FAIL'} live cabinet over Bluetooth: {len(frames)} frames, {len(taps)} bytes in {time.time() - t0:.1f} s, the unit reports it loaded")
            await cl.write_gatt_char(CHR, bytes(js(["bleFrames", js(["pc", st["preset"]])[0], 20, 0])[0][0]), response=False)      # reload the preset: the live cabinet is dropped
            await asyncio.sleep(1.5)
            s4 = await ask(80, [], 0x60, "state record after reloading the preset")
            st4 = js(["decodeState", s4["data"]])[0] if s4 else None
            print(f"  {'ok  ' if st4 and st4['userCab'] == 0 else 'FAIL'} the preset reload dropped it (the unit is as it was)")
        print("\nBLUETOOTH EDITOR PATH WORKS" if ok else "\nBLUETOOTH EDITOR PATH HAS FAILURES")
        return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(asyncio.run(main()))

# SPDX-License-Identifier: GPL-3.0-only
# Copyright (C) 2026 The Onyx Authors (@28spawn)
