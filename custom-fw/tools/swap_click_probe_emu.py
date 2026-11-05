#!/usr/bin/env python3
"""
custom-fw/tools/swap_click_probe_emu.py - click probe for EVERY swap the firmware does under live audio, on the real firmware path (the emulator DLL: input layer, UI, the audio core's
fade-out / swap-when-muted / fade-in, the whole chain). Host timing is not the unit's (a swap that takes 3 ms on the unit takes microseconds here), so this measures the ALGORITHMIC
click only: a discontinuity the fades and the swap order let through. What the unit's timing does is measured by tools/preset_perf_pc.py / swap_probe_pc.py.

For each event (a preset change, or a model change of one slot) a steady guitar-like tone plays; the metric is the largest SECOND difference of the output  x[n] - 2x[n-1] + x[n-2]  (a
tone's own curvature is small and a click is large) in the event window, divided by the largest second difference of the steady output before and after the event (floor 1e-4 FS).
The mute dip (the output at zero between the fades) is reported in ms, and where in the window the worst step fell.

    python custom-fw/tools/swap_click_probe_emu.py [--limit 4.0] [--presets all|N] [--models] [-v]

Exit 1 when any event exceeds the limit. build: powershell -File custom-fw/emu/build_dll.ps1 first.
"""
import argparse, ctypes, math, os, sys, tempfile
import numpy as np

FW = os.path.abspath(os.path.join(os.path.dirname(__file__), ".."))
os.environ["PA_FLASH_FILE"] = os.path.join(tempfile.mkdtemp(prefix="pa_swapclick_"), "flash.bin")
N = 64
DYN, DRIVE, AMP, CAB, EQ, MOD, DLY, REV = range(8)
SLOT_MODULE = {DYN: 0, AMP: 1, MOD: 2, DLY: 3, REV: 4, CAB: 5, DRIVE: 6, EQ: 7}      # the NRPN module numbers (MIDI-MAP-CUSTOM.md)
SLOT_NAME = ["DYN", "DRIVE", "AMP", "CAB", "EQ", "MOD", "DLY", "REV"]


class Rig:
    def __init__(self, dll):
        self.d = ctypes.CDLL(dll)
        d = self.d
        d.emu_init()
        d.emu_midi_event.argtypes = [ctypes.c_int] * 3
        d.emu_get_ui.argtypes = [ctypes.c_int]
        d.emu_get_slot_type.argtypes = [ctypes.c_int]
        F = ctypes.POINTER(ctypes.c_float)
        d.emu_process_audio_f32.argtypes = [F, F, F, ctypes.c_int]
        self.inp = (ctypes.c_float * N)(); self.oL = (ctypes.c_float * N)(); self.oR = (ctypes.c_float * N)()
        self.t = 0

    def run(self, blocks, level=0.25):
        out = np.empty(blocks * N, dtype=np.float64)
        k = 0
        for _ in range(blocks):
            for i in range(N):
                tt = (self.t + i) / 44100.0
                self.inp[i] = level * (math.sin(2 * math.pi * 110 * tt) + 0.5 * math.sin(2 * math.pi * 220 * tt + 0.7) + 0.3 * math.sin(2 * math.pi * 330 * tt + 1.9)) / 1.8
            self.t += N
            self.d.emu_process_audio_f32(self.inp, self.oL, self.oR, N)
            out[k:k + N] = np.frombuffer(self.oL, dtype=np.float32)
            k += N
        return out

    def pc(self, p):
        self.d.emu_midi_event(0xC0, p, 0)

    def nrpn(self, mod, par, hi):
        for c, v in ((99, mod), (98, par), (6, hi)):
            self.d.emu_midi_event(0xB0, c, v)


def d2max(x):
    return float(np.max(np.abs(x[2:] - 2 * x[1:-1] + x[:-2]))) if len(x) > 3 else 0.0


def measure(rig, event, settle=250, pre=300, window=120, tail=300, post=300, floor=1e-4):
    """returns (ratio in the swap window, ms of its worst step, longest dip ms, steady, ratio in the tail after the swap window)"""
    rig.run(settle)
    before = rig.run(pre)
    event()
    ev = rig.run(window)                                                # the request, the fade-out, the swap, the fade-in (about 25 blocks on the unit's clock)
    late = rig.run(tail)
    after = rig.run(post)
    steady = max(d2max(before), d2max(after), floor)
    worst = d2max(ev)
    dd = np.abs(ev[2:] - 2 * ev[1:-1] + ev[:-2])
    where = int(np.argmax(dd)) / 44.1                                   # ms after the event
    env = np.array([np.sqrt(np.mean(ev[b * N:(b + 1) * N] ** 2)) for b in range(window)])
    dip = float(np.sum(env < 0.1 * max(float(np.sqrt(np.mean(before ** 2))), 1e-6))) * N / 44.1     # ms with the level under 10 % of the steady level
    return worst / steady, where, dip, steady, d2max(late) / steady


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--limit", type=float, default=4.0)
    ap.add_argument("--presets", default="all", help="all, or a single index 0..79 to switch to P01 from")
    ap.add_argument("--silent-limit", type=float, default=0.005, help="the largest output peak (full scale) allowed after a preset change with a SILENT input: the amp's resting DC through a blocker that restarted at zero put 0.01-0.05 there (pack 186)")
    ap.add_argument("--noise-limit", type=int, default=8, help="most swaps (of 40) allowed an early burst with a NOISY input (0.0003 rms, what a real input jack gives): over 0.01 FS and over 5x the preset's own steady level. Pack 187: 10 before the gate restarted shut, 5 after")
    ap.add_argument("--governor", action="store_true", help="also every step of the load governor (drive oversampling, cabinet partitions)")
    ap.add_argument("--models", action="store_true", help="also every model of every slot (default: presets only)")
    ap.add_argument("--dll", default=os.path.join(FW, "emu", "pocket_amp_emu.dll"))
    ap.add_argument("-v", action="store_true")
    a = ap.parse_args()
    rig = Rig(a.dll)
    rig.run(300)
    rows = []

    def add(name, r):
        rows.append((name,) + r)
        if a.v:
            print(f"  {name:28s} ratio {r[0]:6.2f}x  worst at +{r[1]:6.1f} ms  dip {r[2]:5.1f} ms  steady {r[3]:.5f}  later {r[4]:.2f}x", flush=True)

    seq = range(40) if a.presets == "all" else [int(a.presets)]
    prev = 0
    rig.pc(0); rig.run(400)
    for p in seq:
        to = (p + 1) % 40 if a.presets == "all" else p
        frm = p if a.presets == "all" else 0
        rig.pc(frm); rig.run(400)
        add(f"preset P{frm + 1:02d} -> P{to + 1:02d}", measure(rig, lambda to=to: rig.pc(to)))
    if a.models:
        rig.pc(9); rig.run(400)
        for slot in (AMP, CAB, DRIVE, MOD, DLY, REV, EQ, DYN):
            t = 0
            cur = rig.d.emu_get_slot_type(slot)
            tried = 0
            while t < 48:
                def ev(t=t, slot=slot):
                    rig.nrpn(SLOT_MODULE[slot], 0x40, t + 0x20)
                rig.nrpn(SLOT_MODULE[slot], 0x41, 127)                      # the slot on
                rig.run(60)
                r = measure(rig, ev, settle=120)
                if rig.d.emu_get_slot_type(slot) != t:
                    break                                                   # past the last type of this slot
                add(f"{SLOT_NAME[slot]} type {cur if False else t:2d}", r)
                t += 1
                tried += 1
    if a.governor:
        # the load governor changes the drive's oversampling (4x -> 2x) and the cabinet's partitions (16 -> 8) in the middle of the signal, with no fade (audio_alnk.c); a high-gain preset with a long cabinet
        rig.d.emu_set_governor.argtypes = [ctypes.c_int]
        for pset in (9, 5, 24, 0):                                          # P10 metal, P06, P25, P01 clean
            for lv_from, lv_to in ((0, 1), (1, 2), (2, 1), (1, 0), (0, 2), (2, 0)):
                rig.d.emu_set_governor(lv_from); rig.pc(pset); rig.run(400)
                add(f"governor L{lv_from}->L{lv_to} on P{pset + 1:02d}", measure(rig, lambda lv=lv_to: rig.d.emu_set_governor(lv), settle=120))
        rig.d.emu_set_governor(0)
    # the burst on a silent input: a restarted amp must not make a sound by itself
    quiet = []
    for p in range(40):
        to = (p + 1) % 40
        rig.pc(p); rig.run(600, level=0.0)
        rig.pc(to); ev = rig.run(300, level=0.0)
        quiet.append((p + 1, to + 1, float(np.abs(ev).max())))
    qbad = [q for q in quiet if q[2] > a.silent_limit]
    rng = np.random.default_rng(1)

    def run_noise(blocks, rms):
        out = np.empty(blocks * 64)
        for b in range(blocks):
            v = rms * rng.standard_normal(64)
            for i in range(64): rig.inp[i] = float(v[i])
            rig.d.emu_process_audio_f32(rig.inp, rig.oL, rig.oR, 64)
            out[b * 64:(b + 1) * 64] = np.frombuffer(rig.oL, dtype=np.float32)
        return out
    nb = []
    for p in range(40):
        to = (p + 1) % 40
        rig.pc(p); run_noise(600, 0.0003)
        rig.pc(to); ev = run_noise(700, 0.0003)
        e = np.array([np.sqrt(np.mean(ev[i:i + 220] ** 2)) for i in range(0, len(ev) - 220, 220)])
        early = float(e[6:60].max()); late = float(np.median(e[-60:]))
        if float(np.abs(ev[:64 * 200]).max()) > 0.01 and early > 5 * max(late, 1e-6): nb.append((p + 1, to + 1, early))
    print(f"noisy input (0.0003 rms): swaps with an early burst: {len(nb)} of 40 (limit {a.noise_limit})" + (", worst P%02d>P%02d %.4f" % max(nb, key=lambda r: r[2]) if nb else ""))
    print(f"silent input: output peak after a preset change, median {np.median([q[2] for q in quiet]):.4f}, max {max(q[2] for q in quiet):.4f} FS (limit {a.silent_limit}); over the limit: {len(qbad)} of {len(quiet)}")
    rows.sort(key=lambda r: -r[1])
    print(f"{len(rows)} swaps measured (limit {a.limit:.1f}x steady). Worst first:")
    for name, ratio, where, dip, steady, later in rows[:12]:
        print(f"  {name:28s} {ratio:6.2f}x   worst step at +{where:6.1f} ms   dip {dip:5.1f} ms   (after the swap window: {later:.2f}x)")
    ratios = np.array([r[1] for r in rows])
    dips = np.array([r[3] for r in rows])
    lates = np.array([r[5] for r in rows])
    print(f"ratio: median {np.median(ratios):.2f}x  p90 {np.percentile(ratios, 90):.2f}x  max {ratios.max():.2f}x    mute dip: median {np.median(dips):.1f} ms  max {dips.max():.1f} ms    after the swap window: max {lates.max():.2f}x")
    bad = [r for r in rows if r[1] > a.limit]
    nfail = len(nb) > a.noise_limit
    print(f"RESULT: {len(bad)} swap(s) over {a.limit:.1f}x, {len(qbad)} silent-input burst(s) over {a.silent_limit}, {len(nb)} noisy-input burst(s) -> {'FAIL' if (bad or qbad or nfail) else 'PASS'}")
    return 1 if (bad or qbad or nfail) else 0


if __name__ == "__main__":
    sys.exit(main())
