#!/usr/bin/env python3
"""
custom-fw/tools/stress_load_pc.py - does the unit hang or lose its amp core under load? (pack 158 bench)

What the firmware ACCEPTS over MIDI today (src/board/usb_ble.c handle_midi_pedal_event): Program Change 0..79, CC 20..27 as module TOGGLES (value >= 64) and CC 64..67 as the HOME / PARA buttons. Nothing else:
the "set module" CCs 28..35, the Groove Station CC 86, the knob CCs 102..119 and NRPN are only PROPOSED in MIDI-MAP-CUSTOM.md (section 9, not implemented). An earlier version of this script sent them and
the unit ignored them, so its first run (2026-11-03) loaded nothing but the preset changes. This version sends only what is implemented.

Load it adds: a USB recording stream running all the time (usb_rec_check_pc.py, 10 s runs), a preset change from --presets (default: the heaviest measured ones) or a random module toggle every --every seconds.
What YOU add by hand before it starts, so the load is real: start the Groove Station (drums) on the unit, and pick a preset with as many modules on as possible; the preset changes in the run re-apply each preset's own module set.
Every 0.5 s it reads, over the diag channel, the second core's state, the governor, the audio interrupt's overruns and longest time, and the USB state. A diag read that fails for --hang-s seconds is a HANG
(the diag channel is served by the main loop). It changes the selected preset and the module switches (nothing is stored).

    python custom-fw/tools/stress_load_pc.py --minutes 10 [--every 2.5] [--presets 25,12,0,1] [--seed 1]
"""
import argparse, random, struct, subprocess, sys, time
from pathlib import Path
sys.path.insert(0, str(Path(__file__).resolve().parent))
import ble_matrix_pc as m

def snap(u):
    w = struct.unpack("<8I", u.ram(0x01C7D200, 32)); g = struct.unpack("<I", u.ram(0x01C7D4C0, 4))[0]; s = struct.unpack("<6I", u.ram(0x01C7C250, 24))
    r = struct.unpack("<4I", u.ram(0x01C7C268, 16)); d = struct.unpack("<4I", u.ram(0x01C7C3C0, 16)); late = struct.unpack("<I", u.ram(0x01C7D24C, 4))[0]
    return dict(on=w[1], why=w[6], level=g & 0xFF, avg=g >> 8, entries=s[0], over=s[3], isrmax=s[2] / 24, loops=r[3], late=late, flags=d[0], resets=d[1])

def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--minutes", type=float, default=10); ap.add_argument("--every", type=float, default=2.5); ap.add_argument("--seed", type=int, default=1)
    ap.add_argument("--hang-s", type=float, default=4.0); ap.add_argument("--presets", default="25,12,24,0,1,3,15,31")
    a = ap.parse_args(); random.seed(a.seed)
    u = m.Unit("Custom MIDI"); print("BUILD", u.build_id(), flush=True)
    cc = lambda n, v: u.mo.send_message([0xB0, n, v])
    plist = [int(x) for x in a.presets.split(",")]
    t0 = time.time(); next_change = 0.0; last_ok = t0; procs = []; stream_end = 0.0
    worst = dict(avg=0, isrmax=0, late=0); first = None; events = []; n = 0; prev = None; hung = False
    while time.time() - t0 < a.minutes * 60:
        t = time.time() - t0
        if time.time() > stream_end:
            procs.append(subprocess.Popen([sys.executable, str(Path(__file__).resolve().parent / "usb_rec_check_pc.py"), "--seconds", "10"], stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True))
            stream_end = time.time() + 11
        if t >= next_change:
            kind = random.random()
            if kind < 0.6: p = random.choice(plist); u.mo.send_message([0xC0, p]); events.append((t, f"PC {p}"))
            else: cc(random.choice(range(20, 28)), 127); events.append((t, "module toggle"))
            next_change = t + a.every
        try:
            s = snap(u); last_ok = time.time()
        except Exception as e:
            if time.time() - last_ok > a.hang_s:
                print(f"t={t:6.1f}s  HANG: no diag reply for {time.time() - last_ok:.1f} s ({e})", flush=True); hung = True; break
            time.sleep(0.2); continue
        if first is None: first = s
        for k in worst:
            worst[k] = max(worst[k], s[k])
        if prev and (s["on"] != prev["on"] or s["level"] != prev["level"] or s["flags"] != prev["flags"] or s["resets"] != prev["resets"]):
            print(f"t={t:6.1f}s  CHANGE core1 {'on' if s['on'] else 'OFF'} L{s['level']} avg {s['avg']} us usb flags {s['flags']:#x} resets {s['resets']}", flush=True)
        if prev and s["entries"] == prev["entries"]:
            print(f"t={t:6.1f}s  audio interrupt not counting", flush=True)
        prev = s; n += 1
        if n % 40 == 0:
            print(f"t={t:6.1f}s  core1 {'on' if s['on'] else 'OFF'} L{s['level']} avg {s['avg']:4d} us overruns {s['over'] - first['over']:5d} isrmax {s['isrmax']:6.0f} us late blocks {s['late']}", flush=True)
        time.sleep(0.5)
    for p in procs:
        try: out = p.communicate(timeout=15)[0].strip().splitlines()
        except Exception: p.kill(); out = ["(stream check killed)"]
        print("stream:", " | ".join(out[-3:])[:230], flush=True)
    s = prev or first
    ok = (not hung) and s and s["on"] and s["level"] == 0
    print(f"SUMMARY: {'HANG' if hung else 'no hang'}; core 1 {'on' if s and s['on'] else 'OFF'}; governor L{s['level'] if s else '?'}; worst avg block {worst['avg']} us, worst isr {worst['isrmax']:.0f} us, "
          f"late blocks {worst['late']}, overruns +{(s['over'] - first['over']) if s and first else '?'} -> {'PASS' if ok else 'FAIL'}", flush=True)
    return 0 if ok else 1

if __name__ == "__main__":
    sys.exit(main())
