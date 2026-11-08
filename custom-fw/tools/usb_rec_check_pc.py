#!/usr/bin/env python3
"""
custom-fw/tools/usb_rec_check_pc.py - does Windows get a continuous recording from the unit's USB microphone? (USB-AUDIO-PLAN.md step B; the pack 157 pass gate)

Opens the "Custom MIDI" input device (the unit's recording endpoint) for SECONDS seconds and reports what the capture saw: whether the stream opened, how many frames arrived against
the wall clock (a stream that is fed continuously delivers frames x 1 / rate = elapsed time, within a percent), the callback status flags (input overflow / underflow), the peak level
(pack 157 sends silence: 0 is expected; later packs send the amp), and the longest gap between callbacks. It only records; nothing is written to the unit.

    python custom-fw/tools/usb_rec_check_pc.py [--seconds 10] [--device "Custom MIDI"] [--list]       exit 0 = continuous, 1 = not, 2 = no device
"""
import argparse
import sys
import time

import numpy as np
import sounddevice as sd


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--seconds", type=float, default=10.0)
    ap.add_argument("--device", default="Custom MIDI")
    ap.add_argument("--list", action="store_true"); ap.add_argument("--wav", help="also save the recording here (16-bit WAV) for listening")
    a = ap.parse_args()
    devs = sd.query_devices()
    if a.list:
        for i, d in enumerate(devs):
            print(i, sd.query_hostapis(d["hostapi"])["name"], d["name"], "in", d["max_input_channels"], "out", d["max_output_channels"])
        return 0
    cand = [(i, d) for i, d in enumerate(devs) if a.device.lower() in d["name"].lower() and d["max_input_channels"] > 0]
    if not cand:
        print(f"no input device with '{a.device}' in its name: the unit's microphone is not there")
        return 2
    wasapi = [c for c in cand if "WASAPI" in sd.query_hostapis(c[1]["hostapi"])["name"]]
    i, d = (wasapi or cand)[0]
    rate = int(d["default_samplerate"])
    ch = min(2, d["max_input_channels"])
    print(f"device {i}: {d['name']} ({sd.query_hostapis(d['hostapi'])['name']}), {ch} ch at {rate} Hz")
    frames, flags, peak, stamps, chunks = 0, [], 0.0, [], []

    def cb(indata, n, t, status):
        nonlocal frames, peak
        frames += n
        peak = max(peak, float(np.max(np.abs(indata))) if n else 0.0)
        stamps.append(time.perf_counter()); chunks.append(indata.copy())
        if status:
            flags.append(str(status))

    t0 = time.perf_counter()
    try:
        with sd.InputStream(device=i, channels=ch, samplerate=rate, callback=cb, blocksize=0):
            time.sleep(a.seconds)
    except Exception as e:
        print(f"the stream did not open or died: {e}")
        return 1
    wall = time.perf_counter() - t0
    secs = frames / rate
    gaps = np.diff(stamps) if len(stamps) > 1 else np.array([0.0])
    print(f"wall {wall:.2f} s, frames {frames} = {secs:.2f} s of audio ({100.0 * secs / wall:.1f} % of the wall clock)")
    print(f"callbacks {len(stamps)}, longest gap between callbacks {1000 * gaps.max():.0f} ms, status flags {len(flags)} {sorted(set(flags))[:3]}")
    print(f"peak {peak:.6f} ({'silence' if peak == 0 else '%.1f dBFS' % (20 * np.log10(peak))})")
    if chunks and peak > 0:                                     # what is in the recording: level, dropouts (a run of exact zeros inside the sound), clicks (a jump of more than half the scale)
        x = np.concatenate(chunks, axis=0)[:, 0].astype(np.float64)
        live = np.nonzero(np.abs(x) > 1e-4)[0]
        rms = float(np.sqrt(np.mean(x[live[0]:] ** 2))) if len(live) else 0.0
        jumps = int(np.sum(np.abs(np.diff(x)) > 0.5))
        run, worst = 0, 0
        for v in (x[live[0]:] if len(live) else []):
            run = run + 1 if v == 0.0 else 0
            worst = max(worst, run)
        print(f"content: rms {20 * np.log10(rms + 1e-12):.1f} dBFS after the first sound, clicks (jumps over half scale) {jumps}, longest run of exact zeros inside the sound {worst} samples ({1000.0 * worst / rate:.1f} ms)")
    if a.wav and chunks:
        import wave
        y = np.clip(np.concatenate(chunks, axis=0), -1, 1)
        w = wave.open(a.wav, "wb"); w.setnchannels(y.shape[1]); w.setsampwidth(2); w.setframerate(rate); w.writeframes((y * 32767).astype("<i2").tobytes()); w.close()
        print(f"saved {a.wav}")
    ok = frames > 0 and 0.97 <= secs / wall <= 1.03 and not flags and gaps.max() < 0.25
    print("RESULT: " + ("continuous" if ok else "NOT continuous"))
    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main())

# SPDX-License-Identifier: GPL-3.0-only
# Copyright (C) 2026 The Onyx Authors (@28spawn)
