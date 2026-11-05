#!/usr/bin/env python3
"""
custom-fw/tools/preset_latency_pc.py - where does the time go between a preset-change signal and the new preset on the screen? READ-ONLY apart from the signals it injects.

The unit already stamps its own events with its 24 MHz clock (the event log, SRAM 0x01C7D600, include/pa_evlog.h): PA_EV_BUTTONS (button mask), PA_EV_ENCODER (PARA steps), PA_EV_VIEW,
PA_EV_PRESET (preset now / before), PA_EV_SWAP_REQ (handed to the audio thread), PA_EV_SWAP_DONE (the audio thread applied it, with the microseconds it took), PA_EV_SLOW_TICK. Around them
this tool polls, over the diag channel, the screen's frame counter (SRAM 0x01C7D5A0: full redraws, last frame microseconds), the UI's selected preset, view and bank, and prints one timeline per trial:

    t(host)   the PC's clock from the moment the signal was sent (resolution = the poll period, printed)
    t(unit)   the unit's own clock from the first event the signal caused (exact, 1/24 us)

Stages: signal -> input seen (BUTTONS / ENCODER) -> UI preset changed (PRESET) -> swap requested -> swap done -> first screen frame drawn after the change -> screen quiet.
The screen stage is by host polling (no event marks a drawn frame yet); the others are the unit's own stamps.

Signals (only messages the firmware handler acts on: Program Change, and CC 64 HOME / 65 PARA click / 66 PARA CCW / 67 PARA CW, see usb_ble.c handle_midi_pedal_event):
    --inject remote --to 79      a Program Change to preset index 79 (P80); with --trials N it alternates --to and --back (default 0)
    --inject para --steps 10     CC 67 (PARA CW) ten times, 40 ms apart, then --click for a CC 65; --ccw uses CC 66. Put the unit in the view you use (Bank Mode, preset list ...) first
    --watch                      send nothing: YOU press / turn the real PARA or buttons; the tool waits for the first event and records everything after it
Compare --watch (the physical path through the button / encoder scan) with --inject para (the USB path into the same input layer) and --inject remote (straight to the preset loader).

    python custom-fw/tools/preset_latency_pc.py --inject remote --to 79 --trials 4
    python custom-fw/tools/preset_latency_pc.py --watch --trials 3 --window 4
"""
import argparse, struct, sys, time
from pathlib import Path
sys.path.insert(0, str(Path(__file__).resolve().parent))
import ble_matrix_pc as m

EVLOG, UI, FRAMES = 0x01C7D600, 0x01C0D840, 0x01C7D5A0
CAP = 64
NAMES = {1: "BUTTONS", 2: "ENCODER", 3: "VIEW", 4: "PRESET", 5: "MODEL", 6: "SLOW_TICK", 7: "SWAP_REQ", 8: "SWAP_DONE", 9: "ISR_LONG", 10: "BYPASS", 11: "SMP", 15: "GOVERNOR", 16: "AMP_SLOW", 17: "FREEZE"}
QUIET = {9, 15, 16}            # block-time events that fire all the time on an overloaded unit: noted, not a stage

def ev_text(code, a, b):
    if code == 1: return f"button mask {a:#04x} (was {b:#04x})  [bits: 0 HOME 1 VOL- 2 VOL+ 3 < 4 > 5 PARA]"
    if code == 2: return f"PARA encoder {a:+d} step(s)" if a < 0x80000000 else f"PARA encoder {a - (1 << 32):+d} step(s)"
    if code == 3: return f"view {a} (was {b})"
    if code == 4: return f"preset P{a + 1:02d} (was P{b + 1:02d})"
    if code == 6: return f"one UI tick took {a / 1000:.1f} ms (last marker {b:#x})"
    if code == 7: return f"swap requested ({'preset' if a == 0xFF else 'slot ' + str(a)}, {b})"
    if code == 8: return f"swap applied in {a / 1000:.2f} ms ({'preset' if b == 0xFF else 'slot ' + str(b)})"
    return f"{NAMES.get(code, code)} a={a} b={b}"

class Probe:
    def __init__(self):
        self.u = m.Unit("Custom MIDI")
        self.n_seen = None
    def head(self):
        w = struct.unpack("<4I", self.u.ram(EVLOG, 16))
        return w[0], w[1]
    def entry(self, i):
        return struct.unpack("<4I", self.u.ram(EVLOG + 16 + (i % CAP) * 16, 16))
    def frames(self):
        w = struct.unpack("<4I", self.u.ram(FRAMES, 16)); return w[0], w[1]
    def ui(self):
        b = self.u.ram(UI, 12); return b[0], b[8], b[11]          # view, preset index, bank group

def trial(p, label, send, window, wait_first):
    magic, n0 = p.head()
    if magic != 0x474C5645: raise SystemExit("no event log in this build")
    f0, _ = p.frames(); v0, pr0, bk0 = p.ui()
    seen = n0; events = []; frames = []; uichg = []; polls = 0
    t0 = time.perf_counter()
    if send: send()
    t_send = time.perf_counter()
    tfirst = None; tlast = None; fprev = f0; uprev = (v0, pr0, bk0)
    deadline = t_send + (60.0 if wait_first else window)
    started = not wait_first
    while time.perf_counter() < deadline:
        _, n = p.head(); polls += 1
        tnow = time.perf_counter()
        if n != seen:
            if tfirst is None: tfirst = tnow
            tlast = tnow; seen = n
            if wait_first and not started:
                tk, code, a_, b_ = p.entry(n0)              # one read, only to see whether the first thing is real activity (not a block-time event)
                if code not in QUIET: started = True; t_send = tnow; deadline = t_send + window; tfirst = tnow
        fc, fl = p.frames()
        if fc != fprev:
            x = struct.unpack("<4I", p.u.ram(0x01C7D590, 16))       # the last frame: drawing ms, panel send us, bands sent, bands unchanged and skipped
            frames.append((time.perf_counter(), fc - fprev, fl, x)); fprev = fc
        us = p.ui()
        if us != uprev: uichg.append((time.perf_counter(), us)); uprev = us
    if wait_first and not started:
        print(f"[{label}] nothing happened in 60 s (no event) - was the signal made?"); return None
    lost = max(0, seen - n0 - CAP)
    for i in range(max(n0, seen - CAP), seen):             # read the log after the window: reading it during the window slowed the polling by ~10 ms per event
        tk, code, a_, b_ = p.entry(i)
        events.append((None, tk, code, a_, b_))
    if lost: print(f"  (the log wrapped: {lost} oldest events lost)")
    if events and tfirst is not None:                      # place each event on the host clock: the first count change is the first event (+- one poll)
        t0u = events[0][1]
        events = [(tfirst + ((tk - t0u) & 0xFFFFFFFF) / 24e6, tk, code, a_, b_) for (_, tk, code, a_, b_) in events]
    ev = [e for e in events if e[2] not in QUIET]
    quiet = [e for e in events if e[2] in QUIET]
    base = ev[0][1] if ev else None
    print(f"\n[{label}]  polling every {1000.0 * (time.perf_counter() - t0) / max(1, polls):.0f} ms (events are read after the window; their host times are placed from the unit clock, +- one poll); {len(ev)} events, {len(frames)} screen-frame changes, {len(quiet)} block-time events ignored")
    rows = []
    for tnow, tk, code, a, b in ev:
        rows.append((tnow - t_send, ((tk - base) & 0xFFFFFFFF) / 24000.0 if base is not None else 0.0, ev_text(code, a, b), code))
    for tnow, nfr, fl, x in frames:
        rows.append((tnow - t_send, None, f"SCREEN: {nfr} frame(s) drawn (last frame {fl / 1000:.1f} ms; {x[2]} bands sent, {x[3]} unchanged and skipped; drawing {x[0] / 1000:.1f} ms, panel {x[1] / 1000:.1f} ms)", 100))
    for tnow, us in uichg:
        rows.append((tnow - t_send, None, f"ui view {us[0]} preset P{us[1] + 1:02d} bank {us[2]}", 101))
    for th, tu, txt, code in sorted(rows):
        print(f"  t(host) {1000 * th:7.0f} ms   t(unit) {('%7.1f ms' % tu) if tu is not None else '     -   '}   {txt}")
    first = lambda c: next((r for r in sorted(rows) if r[3] == c), None)
    pr, rq, dn = first(4), first(7), first(8)
    scr = [r for r in sorted(rows) if r[3] == 100 and pr and r[0] >= pr[0] - 0.01]
    if pr and rq and dn:
        print(f"  -> preset change {pr[1]:.1f} ms after the first event; swap requested +{rq[1] - pr[1]:.1f} ms, applied +{dn[1] - pr[1]:.1f} ms")
    if pr and scr:
        print(f"  -> first screen frame after the change at t(host) {1000 * scr[0][0]:.0f} ms (preset change seen at {1000 * pr[0]:.0f} ms); last frame at {1000 * scr[-1][0]:.0f} ms")
    elif pr:
        print("  -> NO screen frame was drawn after the preset change within the window")
    big = [tn - t_send for tn, nfr, fl, x in frames if x[2] >= 10]          # the frame that carries the change: 10 or more of the 20 bands sent
    lag = (big[0] if big else None)
    print("  -> the full redraw (10+ bands) came " + (f"{1000 * lag:.0f} ms after the signal" if lag is not None else "NEVER within the window"))
    return lag

def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--inject", choices=["remote", "para"]); ap.add_argument("--watch", action="store_true")
    ap.add_argument("--to", type=int, default=79); ap.add_argument("--back", type=int, default=0)
    ap.add_argument("--steps", type=int, default=10); ap.add_argument("--click", action="store_true"); ap.add_argument("--ccw", action="store_true")
    ap.add_argument("--trials", type=int, default=3); ap.add_argument("--window", type=float, default=3.0); ap.add_argument("--gap", type=float, default=3.0)
    a = ap.parse_args()
    if not a.inject and not a.watch: ap.error("choose --inject remote|para or --watch")
    p = Probe()
    print("BUILD", p.u.build_id())
    lags = []
    for t in range(a.trials):
        if a.watch:
            print(f"\n>>> trial {t + 1}/{a.trials}: now press / turn the real PARA or buttons (waiting up to 60 s) ...", flush=True)
            lags.append(trial(p, f"watch {t + 1}", None, a.window, True))
        elif a.inject == "remote":
            tgt = a.to if t % 2 == 0 else a.back
            lags.append(trial(p, f"remote: Program Change {tgt} (P{tgt + 1:02d})", lambda tgt=tgt: p.u.mo.send_message([0xC0, tgt]), a.window, False))
        else:
            cc = 66 if a.ccw else 67
            def send():
                for _ in range(a.steps):
                    p.u.mo.send_message([0xB0, cc, 127]); time.sleep(0.04)
                if a.click: p.u.mo.send_message([0xB0, 65, 127])
            lags.append(trial(p, f"para: CC {cc} x{a.steps}" + (" + click" if a.click else ""), send, a.window, False))
        time.sleep(a.gap)
    lags = [x for x in lags if x is not None]
    LIMIT = 0.25
    ok = len(lags) == a.trials and all(x <= LIMIT for x in lags)
    print(f"\nRESULT: full redraw after the signal in {', '.join('%.0f ms' % (1000 * x) for x in lags) or 'none'} (limit {1000 * LIMIT:.0f} ms) -> {'PASS' if ok else 'FAIL'}")
    return 0 if ok else 1

if __name__ == "__main__":
    sys.exit(main())
