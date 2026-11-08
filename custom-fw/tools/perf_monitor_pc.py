#!/usr/bin/env python3
"""
custom-fw/tools/perf_monitor_pc.py - USB performance monitor: what makes the unit overload when effects are switched on and off fast? READ-ONLY on the unit (RAM reads over the USB diag channel);
the only thing it sends is the load it is asked to generate (MIDI "set module on / off", CC 28..35), or nothing at all in --mode watch, where YOU press the buttons.

What it reads (fixed SRAM blocks, the same on every build): the second core's status (0x01C7D200: online flag, why-off code, the longest job), the load governor (0x01C7D4C0: level | average
block us << 8), the audio interrupt's stats (0x01C7C250: entries, longest, overruns), the swap probe (0x01C7D240) and the event log (0x01C7D600, 64 events: ISR_LONG with the audio-interrupt
step it was in, AMP_SLOW, BYPASS, GOVERNOR, SMP, SWAP_REQ/DONE, SLOW_TICK, ...). The OVERLOAD popup shows when the amp was auto-bypassed (8 blocks in a row over 1.43 ms), the stereo governor
cut the second channel, the load governor reached level 2, or the codec reported 4 overruns in a second: this tool says which of those happened, and after which effect switch.

Timeline: one line per event / change. Summary: counts, the cause of each overload episode, the last switch before every long block, and the COST OF EACH EFFECT, a least-squares fit of the
average block time and the amp core's job time on which effects were on (needs --mode toggle or sweep: the on / off states are known because the tool set them).

    python custom-fw/tools/perf_monitor_pc.py --seconds 60 --mode toggle --every 0.35 [--seed 1] [--restore-preset N]
    python custom-fw/tools/perf_monitor_pc.py --seconds 90 --mode watch          (you switch effects by hand, as fast as you like)
    python custom-fw/tools/perf_monitor_pc.py --seconds 40 --mode sweep          (all eight on one by one, then off, repeated)

The selected preset keeps whatever effects the run left on: --restore-preset (or the last preset the event log saw) is sent as a Program Change at the end to bring the saved sound back.
"""
import argparse, random, struct, sys, time
from pathlib import Path
sys.path.insert(0, str(Path(__file__).resolve().parent))
import ble_matrix_pc as m

SLOT = ["DYN", "DRV", "AMP", "CAB", "EQ", "MOD", "DLY", "REV"]
# MIDI "set module" CC 28..35 follow the STOCK module order (MIDI-MAP-CUSTOM.md): module -> slot
MODULE_OF_SLOT = {0: 0, 2: 1, 5: 2, 6: 3, 7: 4, 3: 5, 1: 6, 4: 7}
EV = {1: "BUTTONS", 2: "ENCODER", 3: "VIEW", 4: "PRESET", 5: "MODEL", 6: "SLOW_TICK", 7: "SWAP_REQ", 8: "SWAP_DONE", 9: "ISR_LONG", 10: "BYPASS", 11: "SMP", 12: "BOOT", 13: "RUNAWAY",
      14: "AMP_RESET", 15: "GOVERNOR", 16: "AMP_SLOW", 17: "FREEZE", 18: "POOL_FAIL", 19: "LOAD_FAIL", 20: "BLE", 21: "MASTER"}
STEP = {0x21: "pending swap", 0x22: "after the swap", 0x23: "before the chain", 0x24: "after the chain", 0x25: "after the chain / before the drums"}
WHY = {0: "running", 1: "not started", 2: "skipped by buttons", 3: "start FAILED", 4: "TIMED OUT, switched off", 5: "refused a job", 6: "stack nearly full, switched off"}
EVLOG, CAP = 0x01C7D600, 64
NM = os.environ.get("PI32_NM") or shutil.which("pi32v2-nm") or str(Path.home() / "tools/jieli/C$/JL/pi32/bin/pi32v2-nm.exe")


def load_syms(elf):
    """addresses of the globals the OVERLOAD popup looks at, from the build's ELF (they move from build to build: the caller validates them against the unit)"""
    import subprocess
    out = subprocess.run([NM, elf], capture_output=True, text=True).stdout
    want = {"g_pa_audio", "g_pa_stereo_allow", "g_pa_gov_level"}
    got = {}
    for line in out.splitlines():
        f = line.split()
        if len(f) == 3 and f[2] in want: got[f[2]] = int(f[0], 16)
    return got if len(got) == 3 else None


def sym_snap(u, y):
    a = struct.unpack("<IIIIHHHBB", u.ram(y["g_pa_audio"], 24))
    return dict(irq=a[0], xrun=a[1], bypass=a[8], stereo=u.ram(y["g_pa_stereo_allow"], 1)[0], gov=u.ram(y["g_pa_gov_level"], 1)[0])


def snap(u):
    w = struct.unpack("<8I", u.ram(0x01C7D200, 32))
    g = struct.unpack("<I", u.ram(0x01C7D4C0, 4))[0]
    s = struct.unpack("<6I", u.ram(0x01C7C250, 24))
    return dict(on=w[1], why=w[6], job=w[4] / 24.0, level=g & 0xFF, avg=g >> 8, entries=s[0], isrmax=s[2] / 24.0, over=s[3])


def evcount(u):
    h = struct.unpack("<4I", u.ram(EVLOG, 16))
    return h[0], h[1]


def evread(u, k):
    e = struct.unpack("<4I", u.ram(EVLOG + 16 + (k % CAP) * 16, 16))
    return dict(k=k, tick=e[0], code=e[1], a=e[2], b=e[3])


def describe(e):
    c = EV.get(e["code"], f"#{e['code']}")
    a, b = e["a"], e["b"]
    if c == "ISR_LONG": return f"ISR_LONG    block took {a} us (budget 1451), in step {b:#x} ({STEP.get(b, '?')})"
    if c == "AMP_SLOW": return f"AMP_SLOW    the amp took {a} us for one block (input {b >> 16} %, output {b & 0xFFFF} %)"
    if c == "BYPASS": return f"BYPASS      8 blocks in a row over budget: the amp was switched to pass-through (last block {a} us)  -> OVERLOAD popup"
    if c == "GOVERNOR": return f"GOVERNOR    load governor level {a} (average block {b} us)" + ("  -> OVERLOAD popup" if a >= 2 else "")
    if c == "SMP": return f"SMP         amp core status {WHY.get(a, a)} (was {WHY.get(b, b)})"
    if c == "SWAP_REQ": return f"SWAP_REQ    {'preset' if a == 0xFF else SLOT[a] if a < 8 else a} -> {b}"
    if c == "SWAP_DONE": return f"SWAP_DONE   {'preset' if b == 0xFF else SLOT[b] if b < 8 else b} swapped in {a} us on core 0"
    if c == "SLOW_TICK": return f"SLOW_TICK   a UI tick took {a} us (last main-loop step {b:#x})"
    if c == "PRESET": return f"PRESET      P{a + 1} (was P{b + 1})"
    if c == "MODEL": return f"MODEL       {SLOT[a] if a < 8 else a} model {b}"
    return f"{c:11s} a={a} b={b}"


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--seconds", type=float, default=60)
    ap.add_argument("--mode", choices=["watch", "toggle", "sweep", "presets"], default="toggle")
    ap.add_argument("--every", type=float, default=0.35, help="seconds between switches (toggle / sweep)")
    ap.add_argument("--seed", type=int, default=1)
    ap.add_argument("--presets", default="0-39", help="--mode presets: the presets to jump between at random (e.g. 0-39 or 5,9,25)")
    ap.add_argument("--elf", default=str(Path(__file__).resolve().parent.parent / "build_pi32" / "pocket_amp_custom.elf"), help="the build's ELF: the popup's own variables (xrun count, stereo governor) are read from it when its addresses check out against the unit")
    ap.add_argument("--drums", action="store_true", help="start the Groove Station (CC 86) for the run and stop it at the end: the drum machine is an extra load")
    ap.add_argument("--restore-preset", type=int, default=-1, help="Program Change sent at the end (0..79); default: the last preset the event log saw")
    a = ap.parse_args()
    random.seed(a.seed)
    u = m.Unit("Custom MIDI")
    print("BUILD", u.build_id(), flush=True)
    magic, n0 = evcount(u)
    if magic != 0x474C5645:
        print("no event log on this build (magic missing)")
        return 2
    restore = a.restore_preset
    if restore < 0:
        for k in range(max(0, n0 - CAP), n0):
            e = evread(u, k)
            if e["code"] == 4: restore = e["a"]
    cc = lambda n, v: u.mo.send_message([0xB0, n, v])
    plist = []
    if a.mode == "presets":
        for part in a.presets.split(","):
            lo, _, hi = part.partition("-"); plist += list(range(int(lo), int(hi or lo) + 1))
    state = {s: 1 for s in range(8)}                   # unknown at the start: the load below sets them all, so it is known from then on
    if a.mode in ("toggle", "sweep"):
        for s in range(8):
            cc(28 + MODULE_OF_SLOT[s], 127); state[s] = 1
        time.sleep(1.0)
    if a.drums:
        cc(86, 127); print("drum machine started (CC 86)", flush=True); time.sleep(1.5)
    base = snap(u)
    syms = None
    try:
        syms = load_syms(a.elf) if Path(a.elf).exists() else None
        if syms:                                                    # the check: the audio interrupt counter must run at the block rate (689 / s) and the governor level must agree
            c0 = sym_snap(u, syms); ta = time.time(); time.sleep(0.3); c1 = sym_snap(u, syms); rate = (c1["irq"] - c0["irq"]) / (time.time() - ta)
            if not (300 < rate < 1500 and c1["gov"] == snap(u)["level"]): syms = None
            else: print(f"popup variables read from {Path(a.elf).name} (audio interrupts {rate:.0f} / s, as expected)")
    except Exception as ex:
        print("popup variables not available:", ex); syms = None
    if not syms: print("popup variables NOT read (ELF missing or not this build): xrun burst and stereo governor are not checked")
    xr_hist = []; stereo_off = 0; byp = 0
    print(f"start: core1 {'on' if base['on'] else 'OFF'} ({WHY.get(base['why'], base['why'])}), governor L{base['level']}, average block {base['avg']} us, overruns so far {base['over']}, events so far {n0}", flush=True)
    t0 = time.time(); seen = n0; prev = base; next_sw = 0.5; swlog = []; samples = []; events = []; sweep_i = 0; sweep_up = True; last_line = 0.0; nsw = 0
    while time.time() - t0 < a.seconds:
        t = time.time() - t0
        if a.mode == "presets" and t >= next_sw:
            p = random.choice(plist)
            while len(plist) > 1 and swlog and swlog[-1][1] == f"P{p + 1}": p = random.choice(plist)
            u.mo.send_message([0xC0, p])
            swlog.append((t, f"P{p + 1}", None)); nsw += 1
            print(f"t={t:6.2f}s  >>> Program Change P{p + 1}", flush=True)
            next_sw = t + a.every
        elif a.mode in ("toggle", "sweep") and t >= next_sw:
            if a.mode == "toggle":
                s = random.randrange(8); state[s] ^= 1
            else:
                s = sweep_i; state[s] = 1 if sweep_up else 0
                sweep_i += 1
                if sweep_i == 8: sweep_i = 0; sweep_up = not sweep_up
            cc(28 + MODULE_OF_SLOT[s], 127 if state[s] else 0)
            swlog.append((t, SLOT[s], state[s])); nsw += 1
            print(f"t={t:6.2f}s  >>> {SLOT[s]} {'ON ' if state[s] else 'OFF'}", flush=True)
            next_sw = t + a.every
        try:
            cur = snap(u)
            magic, n = evcount(u)
        except Exception as ex:
            print(f"t={t:6.2f}s  read error: {ex}", flush=True); time.sleep(0.2); continue
        tr = time.time() - t0
        for k in range(max(seen, n - CAP), n):
            try: e = evread(u, k)
            except Exception: break
            e["t"] = tr; events.append(e)
            if e["code"] in (1, 2, 3, 6) and e["code"] != 6: continue                 # buttons / encoder / view noise
            print(f"t={tr:6.2f}s  {describe(e)}", flush=True)
        seen = n
        if cur["level"] != prev["level"] or cur["on"] != prev["on"] or cur["over"] > prev["over"]:
            print(f"t={tr:6.2f}s  state: core1 {'on' if cur['on'] else 'OFF'} L{cur['level']} avg {cur['avg']} us overruns +{cur['over'] - prev['over']} (total {cur['over'] - base['over']})", flush=True)
        if syms:
            try:
                ys = sym_snap(u, syms)
                xr_hist.append((tr, ys["xrun"]))
                if not ys["stereo"]:
                    stereo_off += 1
                    if stereo_off == 1: print(f"t={tr:6.2f}s  STEREO     the stereo governor switched the second channel off  -> OVERLOAD popup", flush=True)
                if ys["bypass"] and not byp: byp = 1; print(f"t={tr:6.2f}s  BYPASS     g_pa_audio.bypass is set  -> OVERLOAD popup", flush=True)
            except Exception: pass
        samples.append((tr, cur["avg"], cur["job"], dict(state)))
        prev = cur
        if a.mode == "watch" and tr - last_line > 5: last_line = tr; print(f"t={tr:6.2f}s  avg block {cur['avg']} us, core1 job {cur['job']:.0f} us, L{cur['level']}", flush=True)
    end = snap(u)
    if a.drums:
        cc(86, 0); print("drum machine stopped (CC 86)", flush=True)
    if restore >= 0:
        u.mo.send_message([0xC0, restore]); print(f"restored P{restore + 1} (Program Change)", flush=True)

    print("\n=== SUMMARY ===")
    print(f"{nsw} switches in {a.seconds:.0f} s; overruns +{end['over'] - base['over']}, longest audio interrupt since boot {end['isrmax']:.0f} us, governor L{end['level']}, core 1 {'on' if end['on'] else 'OFF'} ({WHY.get(end['why'], end['why'])})")
    cnt = {}
    for e in events: cnt[EV.get(e["code"], e["code"])] = cnt.get(EV.get(e["code"], e["code"]), 0) + 1
    print("events:", ", ".join(f"{k} x{v}" for k, v in sorted(cnt.items(), key=lambda kv: str(kv[0])) if k not in ("BUTTONS", "ENCODER", "VIEW")) or "none")
    pops = [e for e in events if EV.get(e["code"]) == "BYPASS" or (EV.get(e["code"]) == "GOVERNOR" and e["a"] >= 2)]
    xr = end["over"] - base["over"]
    print(f"OVERLOAD causes seen: {len([e for e in pops if EV.get(e['code']) == 'BYPASS'])} amp auto-bypass, {len([e for e in pops if EV.get(e['code']) == 'GOVERNOR'])} governor level 2+; "
          f"codec overruns +{xr} (the popup needs 4 in one second); the stereo governor is not in the log")
    if syms and xr_hist:
        worst = 0
        for i in range(len(xr_hist)):
            j = i
            while j + 1 < len(xr_hist) and xr_hist[j + 1][0] - xr_hist[i][0] <= 1.0: j += 1
            worst = max(worst, xr_hist[j][1] - xr_hist[i][1])
        print(f"codec xruns (the popup's own counter): +{xr_hist[-1][1] - xr_hist[0][1]} in the run, most in one second {worst} (the popup needs 4) -> {'POPUP by xrun burst' if worst >= 4 else 'no burst'}; stereo governor off in {stereo_off} of {len(xr_hist)} samples")
    longs = [e for e in events if EV.get(e["code"]) == "ISR_LONG"]
    if longs:
        steps = {}
        for e in longs: steps[e["b"]] = steps.get(e["b"], 0) + 1
        print("long audio blocks by the step they were in:", ", ".join(f"{STEP.get(k, hex(k))} x{v}" for k, v in sorted(steps.items(), key=lambda kv: -kv[1])))
        print("the last switch before each long block (the first 12):")
        for e in longs[:12]:
            before = [x for x in swlog if x[0] <= e["t"]]
            tag = f"{before[-1][1]} {'' if before[-1][2] is None else ('ON' if before[-1][2] else 'OFF')} {e['t'] - before[-1][0]:.2f} s earlier" if before else "no switch before it"
            print(f"  t={e['t']:6.2f}s  {e['a']} us in {STEP.get(e['b'], hex(e['b']))}   <- {tag}")
    if a.mode == "presets":
        print("\nper preset change (the events in the 0.8 s after the Program Change): swap time on core 0, long blocks, overload causes")
        rows = []
        for i, (ts, name, _) in enumerate(swlog):
            te = swlog[i + 1][0] if i + 1 < len(swlog) else ts + a.every
            win = [e for e in events if ts <= e["t"] < ts + 0.8]
            done = [e["a"] for e in win if EV.get(e["code"]) == "SWAP_DONE" and e["b"] == 0xFF]
            nl = len([e for e in win if EV.get(e["code"]) == "ISR_LONG"])
            lmax = max([e["a"] for e in win if EV.get(e["code"]) == "ISR_LONG"] or [0])
            by = len([e for e in win if EV.get(e["code"]) == "BYPASS"])
            gv = len([e for e in win if EV.get(e["code"]) == "GOVERNOR" and e["a"] >= 2])
            rows.append((name, done[0] if done else -1, nl, lmax, by, gv, len([e for e in win if EV.get(e["code"]) == "AMP_SLOW"])))
        print("  preset  swap_us  long_blocks  longest_us  bypass  gov2+  amp_slow")
        for r in sorted(rows, key=lambda r: (-r[4], -r[5], -r[3]))[:15]:
            print(f"  {r[0]:6s} {r[1]:8d} {r[2]:12d} {r[3]:11d} {r[4]:7d} {r[5]:6d} {r[6]:9d}")
        sw = [r[1] for r in rows if r[1] >= 0]
        if sw: print(f"swap time on core 0: min {min(sw)} us, median {sorted(sw)[len(sw) // 2]} us, max {max(sw)} us over {len(sw)} swaps; swaps with a long block: {len([r for r in rows if r[2]])}, with an auto-bypass: {len([r for r in rows if r[4]])}, with governor 2+: {len([r for r in rows if r[5]])}")
    if a.mode in ("toggle", "sweep") and len(samples) > 30:
        try:
            import numpy as np
            X = np.array([[1.0] + [float(st[s]) for s in range(8)] for _, _, _, st in samples])
            for name, col in (("average block (core 0)", 1), ("amp core job (core 1)", 2)):
                y = np.array([row[col] for row in samples], dtype=float)
                coef = np.linalg.lstsq(X, y, rcond=None)[0]
                print(f"cost of each effect on the {name}, us (least squares over {len(samples)} samples; base {coef[0]:.0f}):  " + "  ".join(f"{SLOT[s]} {coef[1 + s]:+.0f}" for s in range(8)))
        except ImportError:
            print("(numpy missing: no per-effect cost fit)")
    ok = (end["over"] - base["over"]) < 4 and not pops
    print(f"RESULT: {'PASS: no overload cause seen' if ok else 'OVERLOAD CAUSE SEEN, see the timeline above'}")
    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main())


# SPDX-License-Identifier: GPL-3.0-only

# Copyright (C) 2026 The Onyx Authors (@28spawn)

