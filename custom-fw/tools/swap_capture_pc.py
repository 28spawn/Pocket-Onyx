#!/usr/bin/env python3
"""
custom-fw/tools/swap_capture_pc.py - what does the unit's output do around a preset change? Records the unit's USB recording endpoint (the amp's output, the same samples the firmware hands the codec)
while this tool changes presets by MIDI Program Change at known times, then measures every swap and prints statistics. READ-ONLY on the unit apart from the Program Changes (and the one back to the
last preset at the end).

A steady signal must go through the unit: cable an output of a PC audio interface to its input jack and let --tone-device play a quiet guitar-like tone, or plug in any steady source.

Per swap it measures, from the 10 ms RMS envelope: the DIP (the level between 50 % of the signal before the swap and 50 % of the signal after it), the mute gap inside it, how long the level takes
to come back to 90 % and to stay over 80 % (the "kicking in"), and the DISCONTINUITIES: the largest second difference  x[n] - 2 x[n-1] + x[n-2]  in the dip and its recovery against the 99.9th percentile of the
same measure on the steady signal before and after (a swap is "glitched" over --limit times that), with the length of the glitched stretch and where it falls relative to the end of the dip, so a fade
can be sized from the numbers.

    python custom-fw/tools/swap_capture_pc.py --swaps 40 --every 1.6 --tone-device "Line 1/2 (HeadRush" [--presets 0-39] [--save qa/run.npz] [--wav qa/run.wav] [--limit 6]
    python custom-fw/tools/swap_capture_pc.py --analyse qa/run.npz                 (analyse a saved recording again)
"""
import argparse, random, sys, time
from pathlib import Path
import numpy as np
import sounddevice as sd
sys.path.insert(0, str(Path(__file__).resolve().parent))
import ble_matrix_pc as m

SR = 44100


def runs(mask):
    """(start, length) of every run of True"""
    d = np.diff(np.concatenate(([0], mask.view(np.int8), [0])))
    st = np.where(d == 1)[0]; en = np.where(d == -1)[0]
    return list(zip(st, en - st))


def analyse(x, t0_host, swaps, limit, sr):
    """x: mono float array, t0_host: host time of sample 0, swaps: [(host time of the Program Change, preset)]"""
    win, hop = int(0.010 * sr), int(0.001 * sr)
    c = np.concatenate(([0.0], np.cumsum(x * x)))
    idx = np.arange(0, len(x) - win, hop)
    env = np.sqrt((c[idx + win] - c[idx]) / win)                  # env[j] covers samples idx[j] .. idx[j] + win
    tenv = (idx + win // 2) / sr
    d2 = np.abs(x[2:] - 2 * x[1:-1] + x[:-2]); d2 = np.concatenate(([0.0, 0.0], d2))
    rows = []
    for k, (ts, pr) in enumerate(swaps):
        t = ts - t0_host
        j0 = int(np.searchsorted(tenv, t - 0.35)); j1 = int(np.searchsorted(tenv, t + 1.35))
        if j0 < 0 or j1 >= len(env): continue
        e = env[j0:j1]; te = tenv[j0:j1] - t                      # time relative to the Program Change, seconds
        pre = float(np.median(e[(te > -0.30) & (te < -0.05)])); post = float(np.median(e[(te > 0.95) & (te < 1.30)]))
        if pre < 1e-4 or post < 1e-4:
            rows.append(dict(k=k, preset=pr, gap=None)); continue
        mwin = (te > -0.10) & (te < 0.70)
        im = int(np.argmin(np.where(mwin, e, 9.0)))               # the deepest point of the dip
        lowv = float(e[im])
        ia = im
        while ia > 0 and e[ia] < 0.5 * pre: ia -= 1               # the dip starts where the level last was over 50 % of the signal before
        ib = im
        while ib < len(e) - 1 and e[ib] < 0.5 * post: ib += 1     # and ends where it is back over 50 % of the signal after
        ic = ib
        while ic < len(e) - 1 and e[ic:ic + 40].min() < 0.8 * post: ic += 1       # settled: 40 ms in a row over 80 %
        i90 = ib
        while i90 < len(e) - 1 and e[i90] < 0.9 * post: i90 += 1
        sa, sb = int((t + te[ia] - 0.02) * sr), int((t + te[ic] + 0.02) * sr)
        pre_d2 = d2[int((t - 0.30) * sr): int((t - 0.05) * sr)]; post_d2 = d2[int((t + 0.95) * sr): int((t + 1.30) * sr)]
        ref = max(float(np.percentile(pre_d2, 99.9)), float(np.percentile(post_d2, 99.9)), 1e-7)
        w = d2[sa:sb]
        over = np.where(w > limit * ref)[0]
        end_ib = int((t + te[ib]) * sr)
        low = np.abs(x[int((t + te[ia]) * sr): int((t + te[ib]) * sr)]) < 0.03 * pre * 1.41
        gl = 0
        for st, ln in runs(low): gl = max(gl, ln)
        rows.append(dict(k=k, preset=pr, gap=gl / sr * 1000.0, dip=(te[ib] - te[ia]) * 1000.0, start=te[ia] * 1000.0, depth=lowv / min(pre, post),
                         rec90=(te[i90] - te[ib]) * 1000.0, settled=(te[ic] - te[ib]) * 1000.0, pre=pre, post=post, ratio=(float(w.max()) / ref if len(w) else 0.0),
                         at_ms=((int(np.argmax(w)) + sa - end_ib) / sr * 1000.0 if len(w) else 0.0), n_over=len(over),
                         span_ms=((over.max() - over.min()) / sr * 1000.0 if len(over) else 0.0),
                         first_ms=((over.min() + sa - end_ib) / sr * 1000.0 if len(over) else None), last_ms=((over.max() + sa - end_ib) / sr * 1000.0 if len(over) else None)))
    return rows


def report(rows, limit):
    nogap = [r for r in rows if r["gap"] is None]
    rows = [r for r in rows if r["gap"] is not None]
    print(f"{len(rows)} swaps analysed, {len(nogap)} without a usable signal")
    if not rows: return

    def q(name, vals, unit="ms"):
        v = np.array(vals, dtype=float)
        print(f"  {name:48s} median {np.median(v):7.1f}  p10 {np.percentile(v, 10):7.1f}  p90 {np.percentile(v, 90):7.1f}  max {v.max():7.1f} {unit}")
    print("the dip (the level between 50 % of the signal before and 50 % of the signal after):")
    q("Program Change -> start of the dip", [r["start"] for r in rows])
    q("dip length", [r["dip"] for r in rows])
    q("mute gap (samples under 3 % of the signal)", [r["gap"] for r in rows])
    q("deepest point, as a fraction of the signal", [r["depth"] * 100 for r in rows], "%")
    print("the recovery after the dip ends (level back over 50 %):")
    q("to 90 % of the signal after the swap", [r["rec90"] for r in rows])
    q("until it stays over 80 % for 40 ms (settled)", [r["settled"] for r in rows])
    q("level after the swap vs before (rms)", [100 * r["post"] / r["pre"] for r in rows], "%")
    rt = np.array([r["ratio"] for r in rows])
    print(f"the discontinuities (worst step vs the steady level): median {np.median(rt):.1f}x, p90 {np.percentile(rt, 90):.1f}x, max {rt.max():.1f}x")
    bad = [r for r in rows if r["n_over"]]
    print(f"glitched swaps (over {limit:.0f}x): {len(bad)} of {len(rows)} ({100 * len(bad) / len(rows):.0f} %)")
    if bad:
        sp = np.array([r["span_ms"] for r in bad]); fi = np.array([r["first_ms"] for r in bad]); la = np.array([r["last_ms"] for r in bad]); at = np.array([r["at_ms"] for r in bad])
        print(f"  glitched stretch: median {np.median(sp):.1f} ms, p90 {np.percentile(sp, 90):.1f}, max {sp.max():.1f}; worst step at {np.median(at):+.1f} ms from the END of the dip (median)")
        print(f"  first over-limit sample {np.median(fi):+.1f} ms (p10 {np.percentile(fi, 10):+.1f}), last {np.median(la):+.1f} ms (p90 {np.percentile(la, 90):+.1f}) relative to the end of the dip (negative = before it)")
        print("  worst ten:")
        for r in sorted(bad, key=lambda r: -r["ratio"])[:10]:
            print(f"    P{r['preset'] + 1:02d} {r['ratio']:6.1f}x  at {r['at_ms']:+7.1f} ms  stretch {r['span_ms']:5.1f} ms  dip {r['dip']:5.1f} ms  recovery to 90 % {r['rec90']:6.1f} ms")
    print("slowest recoveries (to 90 %):")
    for r in sorted(rows, key=lambda r: -r["rec90"])[:8]:
        print(f"    P{r['preset'] + 1:02d} recovery {r['rec90']:6.1f} ms, settled {r['settled']:6.1f} ms, dip {r['dip']:5.1f} ms, level after {100 * r['post'] / r['pre']:.0f} % of before")


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--swaps", type=int, default=40)
    ap.add_argument("--every", type=float, default=1.6)
    ap.add_argument("--presets", default="0-39")
    ap.add_argument("--limit", type=float, default=6.0, help="a swap is glitched when its worst step is over this many times the steady level")
    ap.add_argument("--device", default="Custom MIDI")
    ap.add_argument("--wav", default="")
    ap.add_argument("--analyse", default="", help="analyse a recording made earlier (a .npz saved with --save) instead of recording")
    ap.add_argument("--save", default="", help="save the raw recording and the swap times (.npz)")
    ap.add_argument("--tone-device", default="", help="play a steady guitar-like test tone (110 Hz and two harmonics) on the output device whose name contains this (cable it to the unit's input jack)")
    ap.add_argument("--tone-level", type=float, default=0.05, help="peak level of the tone, 0..1 (0.05 = -26 dBFS; an instrument input is hot)")
    ap.add_argument("--analog-device", default="", help="ALSO record this input device (cable the unit's headphone / line OUT into an input of a PC audio interface): the sound after the DMA and the codec, to see whether the cracks are there and not in the samples the firmware computes")
    ap.add_argument("--list", action="store_true", help="list the audio devices and stop")
    ap.add_argument("--seed", type=int, default=1)
    a = ap.parse_args()
    random.seed(a.seed)
    if a.list:
        for i, d in enumerate(sd.query_devices()):
            if sd.query_hostapis(d["hostapi"])["name"] == "MME": print(i, d["name"], "in", d["max_input_channels"], "out", d["max_output_channels"])
        return 0
    lo, _, hi = a.presets.partition("-"); plist = list(range(int(lo), int(hi or lo) + 1))
    cur = None; u = None; stat = []
    if a.analyse:
        z = np.load(a.analyse)
        x2 = z["x"]; t_first = float(z["t0"]); swaps = [(float(t), int(p)) for t, p in z["swaps"]]
    else:
        cand = [(i, d) for i, d in enumerate(sd.query_devices()) if a.device.lower() in d["name"].lower() and d["max_input_channels"] > 0 and sd.query_hostapis(d["hostapi"])["name"] == "MME"]
        if not cand:
            print("no recording device containing", a.device); return 2
        dev = cand[0][0]
        u = m.Unit("Custom MIDI")
        print("BUILD", u.build_id(), flush=True)
        chunks = []
        t_first = [None]

        def cb(indata, frames, tinfo, status):
            if t_first[0] is None: t_first[0] = time.time() - frames / SR
            if status: stat.append(str(status))
            chunks.append(indata.copy())
        swaps = []
        tone = None
        achunks = []; at_first = [None]; astream = None
        if a.analog_device:
            ad = [(i, d) for i, d in enumerate(sd.query_devices()) if a.analog_device.lower() in d["name"].lower() and d["max_input_channels"] > 0 and sd.query_hostapis(d["hostapi"])["name"] == "MME"]
            if not ad:
                print("no input device containing", a.analog_device); return 2

            def acb(indata, frames, tinfo, status):
                if at_first[0] is None: at_first[0] = time.time() - frames / SR
                achunks.append(indata.copy())
            astream = sd.InputStream(device=ad[0][0], channels=2, samplerate=SR, callback=acb, dtype="float32")
            astream.start(); print("analog capture on", ad[0][1]["name"], flush=True)
        if a.tone_device:
            od = [(i, d) for i, d in enumerate(sd.query_devices()) if a.tone_device.lower() in d["name"].lower() and d["max_output_channels"] > 0 and sd.query_hostapis(d["hostapi"])["name"] == "MME"]
            if not od:
                print("no output device containing", a.tone_device); return 2
            ph = [0]

            def tcb(outdata, frames, tinfo, status):
                t = (ph[0] + np.arange(frames)) / SR
                v = a.tone_level * (np.sin(2 * np.pi * 110 * t) + 0.5 * np.sin(2 * np.pi * 220 * t + 0.7) + 0.3 * np.sin(2 * np.pi * 330 * t + 1.9)) / 1.8
                outdata[:, :] = v[:, None]
                ph[0] += frames
            tone = sd.OutputStream(device=od[0][0], channels=2, samplerate=SR, callback=tcb, dtype="float32")
            tone.start(); print("test tone on", od[0][1]["name"], "level", a.tone_level, flush=True); time.sleep(1.0)
        with sd.InputStream(device=dev, channels=2, samplerate=SR, callback=cb, dtype="float32"):
            time.sleep(1.5)
            cur = random.choice(plist)
            u.mo.send_message([0xC0, cur]); time.sleep(2.0)
            for k in range(a.swaps):
                p = random.choice(plist)
                while p == cur and len(plist) > 1: p = random.choice(plist)
                swaps.append((time.time(), p)); u.mo.send_message([0xC0, p]); cur = p
                time.sleep(a.every)
            time.sleep(1.0)
        if tone: tone.stop()
        if astream: astream.stop()
        x2 = np.concatenate(chunks)
        t_first = t_first[0]
        if a.save:
            extra = {}
            if astream: extra = dict(xa=np.concatenate(achunks), ta=at_first[0])
            np.savez(a.save, x=x2, t0=t_first, swaps=np.array(swaps), **extra)
        if astream:
            xa = np.concatenate(achunks)[:, 0].astype(np.float64)
            print(f"ANALOG capture: {len(xa) / SR:.1f} s, peak {np.abs(xa).max():.4f}, rms {np.sqrt(np.mean(xa ** 2)):.5f}", flush=True)
            print("=== ANALOG (after the DMA and the codec) ===", flush=True)
            report(analyse(xa, at_first[0], swaps, a.limit, SR), a.limit)
            print("=== DIGITAL (the samples the firmware computes) ===", flush=True)
    x = x2[:, 0].astype(np.float64)
    print(f"recorded {len(x) / SR:.1f} s, peak {np.abs(x).max():.4f}, rms {np.sqrt(np.mean(x ** 2)):.5f}, stream flags: {sorted(set(stat)) or 'none'}", flush=True)
    if a.wav and not a.analyse:
        import wave
        with wave.open(a.wav, "wb") as w:
            w.setnchannels(2); w.setsampwidth(2); w.setframerate(SR); w.writeframes((np.clip(x2, -1, 1) * 32767).astype("<i2").tobytes())
    report(analyse(x, t_first, swaps, a.limit, SR), a.limit)
    if u is not None and cur is not None: u.mo.send_message([0xC0, cur])
    return 0


if __name__ == "__main__":
    sys.exit(main())
