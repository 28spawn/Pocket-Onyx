#!/usr/bin/env python3
"""
custom-fw/tools/core1_watch_pc.py - READ-ONLY: is the amp core (core 1) staying online, and how long do the audio blocks take? (the "metallic after the USB test" investigation, 2026-11-03)

Once a second it reads, over the diag channel (RAM reads, nothing written): the second core's status block (SMPS 0x01C7D200: online flag, the reason code, hand-overs, longest job), the load governor
(0x01C7D4C0: level | average block us << 8), the audio-interrupt stats (0x01C7C250: entries, longest cycle count, overruns) and the USB recording counters (0x01C7C420).
It prints a line on every change of the core's state or the governor level, and a one-line summary every 30 s. Optionally it runs the USB recording check (usb_rec_check_pc.py) at given
seconds, so a core-1 timeout can be placed in time against the stream.

    python custom-fw/tools/core1_watch_pc.py --seconds 300 [--stream-at 120,160,200]
"""
import argparse, struct, subprocess, sys, time
from pathlib import Path
sys.path.insert(0, str(Path(__file__).resolve().parent))
import ble_matrix_pc as m
import swap_probe_pc as sp

WHY = {0: "running", 1: "not started", 2: "skipped by buttons", 3: "start FAILED", 4: "TIMED OUT (missed a block), switched off", 5: "refused a job", 6: "stack nearly full, switched off"}

def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--seconds", type=int, default=300)
    ap.add_argument("--stream-at", default="")
    ap.add_argument("--presets", default="", help="after --presets-from seconds, send MIDI Program Change for these presets (e.g. 0-39), one every --presets-every seconds (changes the preset on the unit, nothing is stored)")
    ap.add_argument("--presets-from", type=int, default=30)
    ap.add_argument("--presets-every", type=float, default=3.0)
    a = ap.parse_args()
    streams = sorted(int(x) for x in a.stream_at.split(",") if x)
    u = m.Unit("Custom MIDI")
    print("BUILD", u.build_id(), flush=True)
    t0 = time.time(); last = None; procs = []; n = 0
    plist = []
    if a.presets:
        lo, _, hi = a.presets.partition("-"); plist = list(range(int(lo), int(hi or lo) + 1))
    next_pc = a.presets_from; swaps_seen = 0
    while time.time() - t0 < a.seconds:
        t = time.time() - t0
        while streams and t >= streams[0]:
            print(f"t={t:6.1f}s  >>> recording stream starts (10 s)", flush=True)
            procs.append(subprocess.Popen([sys.executable, str(Path(__file__).resolve().parent / "usb_rec_check_pc.py"), "--seconds", "10"], stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True))
            streams.pop(0)
        while plist and t >= next_pc:
            p = plist.pop(0); u.mo.send_message([0xC0, p]); next_pc += a.presets_every
            print(f"t={t:6.1f}s  >>> Program Change {p}", flush=True)
        try:
            w = struct.unpack("<16I", u.ram(0x01C7D200, 64)) if False else struct.unpack("<8I", u.ram(0x01C7D200, 32))
            g = struct.unpack("<I", u.ram(0x01C7D4C0, 4))[0]
            s = struct.unpack("<6I", u.ram(0x01C7C250, 24))
            i = struct.unpack("<8I", u.ram(0x01C7C420, 32))
        except Exception as e:
            print(f"t={t:6.1f}s  read error: {e}", flush=True); time.sleep(0.5); continue
        try:      # pack 158 swap probe: print the records of any new swap (fine if the build is older: the magic is missing)
            sw = struct.unpack("<40I", u.ram(sp.ADDR, 160))
            if sw[0] == 0x44575053 and (sw[1] != swaps_seen or any((sw[4 + 6 * k] >> 24) & 1 and not (sw[4 + 6 * k] >> 25) & 7 for k in range(sp.NREC) if False)):
                for line in sp.decode(sw)[1:]:
                    pass
                print(f"t={t:6.1f}s  swap probe:
" + "
".join(sp.decode(sw)), flush=True)
                swaps_seen = sw[1]
        except Exception:
            pass
        state = (w[1], w[6], g & 0xFF)
        if state != last:
            print(f"t={t:6.1f}s  core1 {'ONLINE' if w[1] else 'OFF'} ({WHY.get(w[6], w[6])}), governor level {g & 0xFF}, avg block {g >> 8} us, hand-overs {w[2]}, longest core-1 job {w[4] / 24:.0f} us, isr max {s[2] / 24:.0f} us, overruns {s[3]}", flush=True)
            last = state
        n += 1
        if n % 30 == 0:
            print(f"t={t:6.1f}s  . core1 {'on' if w[1] else 'OFF'} level {g & 0xFF} avg {g >> 8} us overruns {s[3]}/{s[0]} longest core-1 job {w[4] / 24:.0f} us  iso pkts {i[0]} alt {i[4]:#x}", flush=True)
        time.sleep(1.0)
    for p in procs:
        print(p.communicate()[0].strip().splitlines()[-4:], flush=True)

if __name__ == "__main__":
    main()
