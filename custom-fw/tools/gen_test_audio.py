#!/usr/bin/env python3
"""Generate the emulator's test audio and impulse responses, and the firmware's default cab.

Everything here is SYNTHESISED (fixed seeds), so it is the project's own material:

  * guitar / bass DI phrases - a plucked-string model (Karplus-Strong excited by the
    string's triangular pluck shape, with string damping and palm muting), playing
    original phrases;
  * cabinet IRs - a speaker-cabinet magnitude model (resonance, cone peaks and dips,
    roll-off, seeded irregularity) turned into a minimum-phase impulse response;
  * reverb IRs - shaped, frequency-dependent decaying noise (plate) and a dispersive
    echo train (spring);
  * a NAM test capture (emu/test_capture.nam): the open A2 container format with
    deterministic weights - for the parser, the upload path and the engine;
  * the firmware's default cab (src/fx/cab_default_table.inc): the closed 4x12 model,
    512 taps in Q18, scaled to the L2 norm every cab IR is normalised to.

    python custom-fw/tools/gen_test_audio.py               # write the emulator's files
    python custom-fw/tools/gen_test_audio.py --firmware    # also rewrite cab_default_table.inc
    python custom-fw/tools/gen_test_audio.py --check       # exit 1 if cab_default_table.inc differs

The emulator's server calls ensure_emu_assets() at start-up, so the WAVs are generated on
first use.
"""
import argparse
import io
import os
import sys
import wave

import numpy as np

HERE = os.path.dirname(os.path.abspath(__file__))
FW = os.path.dirname(HERE)
EMU = os.path.join(FW, "emu")
CAB_INC = os.path.join(FW, "src", "fx", "cab_default_table.inc")
FS = 44100
CAB_REF_L2_Q18 = 400150.0          # fx_cab_ir.c: the energy every cab IR is normalised to


# ---- small DSP helpers -------------------------------------------------------------------
def onepole_lp(x, hz, fs=FS):
    a = 1.0 - np.exp(-2 * np.pi * hz / fs)
    y = np.empty_like(x)
    s = 0.0
    for i, v in enumerate(x):
        s += a * (v - s)
        y[i] = s
    return y


def write_wav(path, x, fs=FS, peak=0.89):
    x = np.asarray(x, dtype=np.float64)
    x = x / (np.max(np.abs(x)) + 1e-12) * peak
    pcm = np.round(x * 32767).astype(np.int16)
    os.makedirs(os.path.dirname(path), exist_ok=True)
    with wave.open(path, "wb") as w:
        w.setnchannels(1)
        w.setsampwidth(2)
        w.setframerate(fs)
        w.writeframes(pcm.tobytes())


# ---- the plucked string ------------------------------------------------------------------
def pluck(freq, dur, seed, vel=1.0, mute=0.0, bright=0.6, pick=0.18, fs=FS):
    """One plucked note. mute 0..1 = palm mute (faster decay, darker)."""
    n = int(dur * fs)
    N = max(2, int(round(fs / freq)))
    rng = np.random.default_rng(seed)
    # the string's shape when the pick lets go: a triangle peaking at the pick position
    # (harmonics fall as 1/n^2, with the pick-position notches), plus a little noise
    k = np.arange(N) / N
    tri = np.where(k < pick, k / pick, (1.0 - k) / (1.0 - pick))
    exc = (tri - tri.mean()) + 0.08 * rng.uniform(-1, 1, N)
    exc = onepole_lp(exc, 2500 + 6000 * bright)                 # pick hardness
    y = np.zeros(n + N + 1)
    y[:N] = exc * vel
    # loop gain per period: string decay (and the mute)
    t60 = (2.5 if freq < 150 else 1.8) * (1.0 - 0.85 * mute)
    g = 10 ** (-3.0 / (t60 * freq))
    k = N
    while k < len(y):
        hi = min(k + N, len(y))
        prev = y[k - N:hi - N]
        prev1 = y[k - N - 1:hi - N - 1] if k - N - 1 >= 0 else np.concatenate(([0.0], y[k - N:hi - N - 1]))
        y[k:hi] = g * 0.5 * (prev + prev1)
        k = hi
    out = y[:n]
    if mute > 0:
        out = onepole_lp(out, 900 + 2500 * (1 - mute))
    a = np.minimum(1.0, np.arange(n) / (0.002 * fs))            # 2 ms attack
    return out * a


def midi_hz(m):
    return 440.0 * 2 ** ((m - 69) / 12.0)


def render_events(events, total, seed0):
    """events: (time_s, midi, dur_s, vel, mute)."""
    x = np.zeros(int(total * FS) + FS)
    for i, (t0, m, dur, vel, mute) in enumerate(events):
        s = pluck(midi_hz(m), dur + 0.4 * (1 - mute), seed0 + i, vel, mute)
        a = int(t0 * FS)
        x[a:a + len(s)] += s[: len(x) - a]
    x = x - onepole_lp(x, 30)                                    # the input coupling
    return x[: int(total * FS)]


def strum(t0, notes, dur, vel, down=True, spread=0.012, mute=0.0):
    order = notes if down else list(reversed(notes))
    return [(t0 + i * spread, m, dur, vel * (0.85 + 0.15 * (i == 0)), mute) for i, m in enumerate(order)]


# Chord voicings (MIDI): standard tuning E2 A2 D3 G3 B3 E4
CH = {"G": [43, 47, 50, 55, 59, 67], "C": [48, 52, 55, 60, 64], "D": [50, 57, 62, 66],
      "Em": [40, 47, 52, 55, 59, 64], "Am": [45, 52, 57, 60, 64], "F": [41, 48, 53, 57, 60, 65]}


def phrase_clean_chords():
    bpm, ev, t = 90, [], 0.0
    beat = 60 / bpm
    for _ in range(2):
        for c in ("G", "C", "D", "Em"):
            for k, down in enumerate((True, True, False, True, False, True)):
                ev += strum(t + k * beat / 2 * (4 / 6) * 1.5, CH[c], beat * 1.5, 0.9 if down else 0.6, down)
            t += 4 * beat
    return render_events(ev, t + 1.0, 1000)


def phrase_arpeggio():
    bpm, ev, t = 100, [], 0.0
    e8 = 60 / bpm / 2
    for _ in range(2):
        for c in ("Am", "F", "C", "G"):
            notes = CH[c]
            seq = [notes[0], notes[2], notes[3], notes[-1], notes[-2], notes[3], notes[2], notes[1]]
            for k, m in enumerate(seq):
                ev.append((t + k * e8, m, e8 * 3, 0.8 if k % 4 == 0 else 0.65, 0.0))
            t += 8 * e8
    return render_events(ev, t + 1.0, 2000)


def power(root):
    return [root, root + 7, root + 12]


def phrase_rock_riff():
    bpm, ev, t = 120, [], 0.0
    e8 = 60 / bpm / 2
    riff = [(40, 0), (40, 1), (43, 0), (40, 1), (45, 0), (45, 1), (43, 0), (40, 1)]
    for _ in range(5):
        for root, muted in riff:
            ev += strum(t, power(root), e8 * (0.9 if muted else 1.8), 0.95, True, 0.006, 0.7 if muted else 0.0)
            t += e8
    return render_events(ev, t + 1.0, 3000)


def phrase_metal_chug():
    bpm, ev, t = 140, [], 0.0
    s16 = 60 / bpm / 4
    pat = "XxxxXxxx XxxX xxXx".replace(" ", "")
    for bar in range(4):
        for k, ch in enumerate(pat):
            if ch == "X":
                ev += strum(t, power(40 if bar % 2 == 0 else 41), s16 * 2, 1.0, True, 0.004, 0.2)
            else:
                ev.append((t, 40, s16, 0.85, 0.9))
            t += s16
    return render_events(ev, t + 1.0, 4000)


def phrase_blues_lead():
    bpm, ev, t = 80, [], 0.0
    e8 = 60 / bpm / 2
    line = [69, 72, 74, 72, 69, 67, 69, None, 76, 74, 72, 74, 72, 69, None, None,
            64, 67, 69, 72, 69, 67, 64, None, 69, 72, 74, 76, 74, 72, 69, None]
    for m in line:
        if m is not None:
            ev.append((t, m, e8 * 1.8, 0.85, 0.0))
        t += e8
    return render_events(ev, t + 1.5, 5000)


def phrase_bass_line():
    bpm, ev, t = 100, [], 0.0
    e8 = 60 / bpm / 2
    roots = [28, 28, 31, 33]                                     # E1 E1 G1 A1
    for _ in range(3):
        for r in roots:
            for k, step in enumerate((0, 0, 12, 7, 0, 7, 10, 7)):
                ev.append((t, r + step, e8 * 0.95, 0.9 if k == 0 else 0.7, 0.15))
                t += e8
    return render_events(ev, t + 1.0, 6000)


PHRASES = {
    "di_clean_chords.wav": phrase_clean_chords,
    "di_arpeggio.wav": phrase_arpeggio,
    "di_rock_riff.wav": phrase_rock_riff,
    "di_metal_chug.wav": phrase_metal_chug,
    "di_blues_lead.wav": phrase_blues_lead,
    "di_bass_line.wav": phrase_bass_line,
}


# ---- cabinet IRs ---------------------------------------------------------------------
def peak_db(f, f0, gain_db, q):
    """A smooth bell in dB around f0."""
    x = np.log2(np.maximum(f, 1.0) / f0) * q * 2.0
    return gain_db / (1.0 + x * x)


def cab_magnitude(f, p, seed):
    """|H(f)| in dB: a speaker in a cabinet."""
    fl, fh = p["lo"], p["hi"]
    db = -40.0 * np.log10(1.0 + (fl / np.maximum(f, 1.0)) ** 2) / 2.0   # 2nd-order low end
    db += peak_db(f, p["res_f"], p["res_db"], 1.2)                      # cabinet resonance
    for f0, g, q in p["peaks"]:
        db += peak_db(f, f0, g, q)
    db += -60.0 * np.log10(1.0 + (f / fh) ** 4) / 2.0                  # steep top roll-off
    rng = np.random.default_rng(seed)
    knots_f = np.geomspace(200, 12000, 24)
    knots_db = rng.normal(0.0, p["rough"], len(knots_f))
    db += np.interp(np.log(np.maximum(f, 1.0)), np.log(knots_f), knots_db)
    return db


def min_phase_ir(db, n_fft):
    """Minimum-phase impulse response from a magnitude (dB) on rfft bins (real cepstrum)."""
    mag = 10 ** (db / 20.0)
    logm = np.log(np.maximum(mag, 1e-6))
    full = np.concatenate([logm, logm[-2:0:-1]])
    cep = np.fft.ifft(full).real
    w = np.zeros(len(full))
    w[0] = 1.0
    w[1:len(full) // 2] = 2.0
    w[len(full) // 2] = 1.0
    spec = np.exp(np.fft.fft(cep * w))
    return np.fft.ifft(spec).real[:n_fft]


CABS = {
    "cab_4x12_closed": dict(lo=80, hi=5200, res_f=105, res_db=5.0, rough=1.4,
                            peaks=[(700, -3.0, 1.5), (2700, 4.5, 1.6), (4100, 2.5, 3.0)]),
    "cab_2x12_open":   dict(lo=95, hi=6000, res_f=120, res_db=2.0, rough=1.8,
                            peaks=[(900, -2.0, 1.4), (2200, 3.0, 1.4), (3600, 2.0, 2.5)]),
    "cab_1x12_combo":  dict(lo=105, hi=5600, res_f=135, res_db=2.5, rough=1.6,
                            peaks=[(1200, 3.0, 1.3), (2900, 2.0, 2.0), (500, -2.0, 1.5)]),
    "cab_bass_4x10":   dict(lo=45, hi=3800, res_f=70, res_db=4.0, rough=1.0,
                            peaks=[(400, -2.5, 1.2), (1800, 2.5, 1.5)]),
}


def cab_ir(name, taps=1024, seed=900):
    n_fft = 8192
    f = np.fft.rfftfreq(n_fft, 1.0 / FS)
    db = cab_magnitude(f, CABS[name], seed + sum(map(ord, name)))
    ir = min_phase_ir(db, n_fft)[:taps].copy()
    fade = 64
    ir[-fade:] *= 0.5 * (1 + np.cos(np.linspace(0, np.pi, fade)))
    return ir


# ---- reverb IRs ----------------------------------------------------------------------
def plate_ir(sec=1.2, rt60=1.8, seed=950):
    n = int(sec * FS)
    rng = np.random.default_rng(seed)
    t = np.arange(n) / FS
    bands = [(0, 600, 1.0), (600, 3000, 0.8), (3000, 9000, 0.55), (9000, 20000, 0.3)]
    spec_t = np.zeros(n)
    nz = rng.standard_normal(n)
    N = np.fft.rfft(nz)
    fr = np.fft.rfftfreq(n, 1.0 / FS)
    for lo, hi, k in bands:
        band = np.fft.irfft(N * ((fr >= lo) & (fr < hi)), n)
        spec_t += band * np.exp(-6.91 * t / (rt60 * k))         # higher bands die faster
    pre = int(0.008 * FS)
    ir = np.concatenate([np.zeros(pre), spec_t])[:n]
    ir[0] = 0.6 * np.max(np.abs(ir))                              # the direct sound
    return ir


def spring_ir(sec=1.2, seed=960):
    n = int(sec * FS)
    rng = np.random.default_rng(seed)
    ir = np.zeros(n)
    # a dispersive chirp (high frequencies arrive first), repeated with decay
    m = int(0.025 * FS)
    tt = np.arange(m) / FS
    chirp = np.sin(2 * np.pi * (4000 * tt - 60000 * tt * tt)) * np.exp(-tt / 0.012)
    chirp = onepole_lp(chirp, 5000)
    delay = int(0.032 * FS)
    g = 1.0
    pos = int(0.004 * FS)
    while pos + m < n and g > 1e-3:
        ir[pos:pos + m] += g * chirp * (1 + 0.1 * rng.standard_normal())
        pos += delay + int(rng.integers(-40, 40))
        g *= 0.82
    ir[0] = 0.8
    return ir


REVERBS = {"rev_plate": plate_ir, "rev_spring": spring_ir}


# ---- a synthetic NAM test capture -----------------------------------------------------
# The open Neural Amp Modeler "A2" container: a SlimmableContainer with a 3-channel ("Lite",
# 1,871 weights) and an 8-channel ("Full", 12,146 weights) WaveNet, the layer layout the
# firmware's engine runs. The weights are deterministic pseudo-random values (seeded, small,
# finite) - it is a TEST capture for the parser, the upload path and the engine, not a
# model of any amplifier.
A2_KERNELS = [6] * 14 + [15, 15] + [6] * 7
A2_DILATIONS = [1, 3, 7, 17, 41, 101, 239] * 2 + [1, 13] + [1, 3, 7, 17, 41, 101, 239]
A2_WEIGHTS = {3: 1871, 8: 12146}


def _a2_layer(ch):
    off = {"active": False, "shift": True, "groups": 1}
    n = len(A2_KERNELS)
    return {
        "input_size": 1, "condition_size": 1,
        "head": {"out_channels": 1, "kernel_size": 16, "bias": True},
        "channels": ch, "kernel_sizes": A2_KERNELS, "dilations": A2_DILATIONS,
        "activation": [{"type": "LeakyReLU", "negative_slope": 0.01}] * n,
        "bottleneck": ch,
        "head1x1": {"active": False, "out_channels": 1, "groups": 1},
        "layer1x1": {"active": True, "groups": 1},
        "groups_input": 1, "groups_input_mixin": 1,
        "conv_pre_film": off, "conv_post_film": off,
        "input_mixin_pre_film": off, "input_mixin_post_film": off,
        "activation_pre_film": off, "activation_post_film": off,
        "layer1x1_post_film": off, "head1x1_post_film": off,
        "gating_mode": ["none"] * n, "secondary_activation": [None] * n,
        "slimmable": None,
    }


def test_capture(seed=990):
    meta = {"name": "Synthetic test capture", "modeled_by": "pocket-amp-mod (generated)",
            "gear_type": "amp", "tone_type": "test", "loudness": -20.0, "gain": 0.5,
            "input_level_dbu": 12, "output_level_dbu": 12,
            "notes": "Generated by custom-fw/tools/gen_test_audio.py - not a model of any amplifier."}
    subs = []
    for ch, max_value in ((3, 0.5), (8, 1.0)):
        rng = np.random.default_rng(seed + ch)
        w = (0.15 * rng.uniform(-1.0, 1.0, A2_WEIGHTS[ch])).round(6)
        # the format's tail: ... head taps, head bias, head_scale (the LAST weight scales the
        # output; the engine reads it from there - zero would make the capture silent)
        w[-2] = 0.0
        w[-1] = 0.02
        subs.append({"max_value": max_value, "model": {
            "version": "0.7.0", "metadata": meta, "architecture": "WaveNet",
            "config": {"layers": [_a2_layer(ch)], "head": None, "head_scale": 0.02},
            "weights": [float(x) for x in w], "sample_rate": 48000}})
    return {"version": "0.7.0", "architecture": "SlimmableContainer",
            "config": {"submodels": subs}, "sample_rate": 48000, "metadata": meta}


# ---- outputs -------------------------------------------------------------------------
def ensure_emu_assets(emu_dir=EMU, force=False, log=print):
    """Create any missing emulator test file. Returns the number generated."""
    made = 0
    for fname, fn in PHRASES.items():
        p = os.path.join(emu_dir, "samples", fname)
        if force or not os.path.exists(p):
            write_wav(p, fn())
            made += 1
            log(f"[test audio] generated samples/{fname}")
    for name in CABS:
        p = os.path.join(emu_dir, "irs", f"{name}.wav")
        if force or not os.path.exists(p):
            write_wav(p, cab_ir(name), peak=0.9)
            made += 1
            log(f"[test audio] generated irs/{name}.wav")
    for name, fn in REVERBS.items():
        p = os.path.join(emu_dir, "irs", f"{name}.wav")
        if force or not os.path.exists(p):
            write_wav(p, fn(), peak=0.9)
            made += 1
            log(f"[test audio] generated irs/{name}.wav")
    p = os.path.join(emu_dir, "test_capture.nam")
    if force or not os.path.exists(p):
        import json
        with open(p, "w", encoding="utf-8") as f:
            json.dump(test_capture(), f)
        made += 1
        log("[test audio] generated test_capture.nam")
    p = os.path.join(emu_dir, "default_cab_ir.wav")
    if force or not os.path.exists(p):
        write_wav(p, cab_ir("cab_4x12_closed"), peak=0.9)
        made += 1
        log("[test audio] generated default_cab_ir.wav")
    return made


def default_cab_q18():
    ir = cab_ir("cab_4x12_closed", taps=512)
    q = np.round(ir / np.sqrt(np.sum(ir * ir)) * CAB_REF_L2_Q18).astype(np.int64)
    return q


def emit_cab_inc():
    q = default_cab_q18()
    b = io.StringIO()
    b.write("/* custom-fw/src/fx/cab_default_table.inc - the default cabinet (512 taps, Q18)\n"
            " *\n"
            " * GENERATED FILE - do not edit.  Regenerate with\n"
            " *\n"
            " *     python custom-fw/tools/gen_test_audio.py --firmware\n"
            " *\n"
            " * A synthesised closed-back 4x12 (the speaker-cabinet model in that tool, made\n"
            " * minimum-phase), scaled to CAB_REF_L2_Q18 so the default keeps its level.\n"
            " */\n")
    for i in range(0, len(q), 8):
        b.write("    " + ", ".join(str(int(v)) for v in q[i:i + 8]) + ",\n")
    return b.getvalue()


def main():
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("--firmware", action="store_true", help="also rewrite cab_default_table.inc")
    ap.add_argument("--check", action="store_true", help="verify cab_default_table.inc only")
    ap.add_argument("--force", action="store_true", help="regenerate the emulator files even if present")
    a = ap.parse_args()
    text = emit_cab_inc()
    if a.check:
        cur = open(CAB_INC, encoding="utf-8").read() if os.path.exists(CAB_INC) else ""
        print(f"{CAB_INC}: {'matches the generator' if cur == text else 'DIFFERS from the generator'}")
        return 0 if cur == text else 1
    n = ensure_emu_assets(force=a.force)
    print(f"emulator test files: {n} generated")
    if a.firmware:
        with open(CAB_INC, "w", encoding="utf-8", newline="\n") as f:
            f.write(text)
        print(f"wrote {CAB_INC}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
