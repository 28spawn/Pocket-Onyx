#!/usr/bin/env python3
"""Generate the Groove Station's drum kit: custom-fw/src/dsp/drum_kit.inc.

Every voice is SYNTHESISED here - oscillators, noise and filters, with fixed random seeds -
so the kit is original, reproducible, and contains no recording of anyone's instrument.

    python custom-fw/tools/gen_drum_kit.py            # writes src/dsp/drum_kit.inc
    python custom-fw/tools/gen_drum_kit.py --check    # exit 1 if the file is not what this makes
    python custom-fw/tools/gen_drum_kit.py --wav DIR  # also write one WAV per voice, to listen

Twelve drum voices at 22.05 kHz (the engine runs at that rate and brings its bus up to the 44.1 kHz
output) plus the metronome click, also at 22.05 kHz:

    kick     a sine whose pitch falls 158 -> 48 Hz with a two-stage decay, a beater
             click, gentle saturation
    snare    two drum-head modes (185 / 330 Hz) + band-passed noise for the wires
    rim      side stick: three short wooden modes + a click
    hat      closed hi-hat: six band-limited square oscillators at inharmonic ratios
             + noise, high-passed
    pedal    the foot "chick": the same metal, darker and shorter
    open     open hi-hat: the hat with a long decay
    crash    noise + a lower metal cluster, band-passed, long decay
    ride     inharmonic bell partials + a quiet noise wash
    bell     the ride bell: fewer, stronger partials
    toms     three pitched membranes (95 / 130 / 175 Hz) with a short pitch drop
    click    a woodblock-like ping (two modes), for the metronome styles

Each voice is normalised to the same peak and trimmed where it falls 60 dB below its
peak (with a 10 ms fade); the engine's per-voice trims (drums_synth.c) set the balance.

The recipes below are the REFERENCE.  The firmware does not store them as 16-bit PCM (86 KB): the kick and the
toms are re-made on the device by a small synthesiser (drum_codec.h), the noisy voices are stored as IMA-ADPCM or
mu-law, and this tool measures each stored form against its reference.
"""
import argparse
import hashlib
import io
import math
import os
import sys
import wave

import numpy as np

HERE = os.path.dirname(os.path.abspath(__file__))
FW = os.path.dirname(HERE)
OUT = os.path.join(FW, "src", "dsp", "drum_kit.inc")

FS = 22050          # the kit's rate (played at half speed into 44.1 kHz)
FS_CLICK = 22050    # the click is at the kit's rate too: the whole drum engine runs at 22.05 kHz
PEAK = 29000        # every voice normalised to this int16 peak


# ---- building blocks ----------------------------------------------------------------
def t_axis(sec, fs=FS):
    return np.arange(int(round(sec * fs))) / fs


def env(t, tau, attack=0.0):
    e = np.exp(-t / tau)
    if attack > 0:
        e *= np.clip(t / attack, 0.0, 1.0)
    return e


def onepole_lp(x, hz, fs=FS):
    a = 1.0 - np.exp(-2 * np.pi * hz / fs)
    y = np.empty_like(x)
    s = 0.0
    for i, v in enumerate(x):
        s += a * (v - s)
        y[i] = s
    return y


def onepole_hp(x, hz, fs=FS):
    return x - onepole_lp(x, hz, fs)


def hp2(x, hz, fs=FS):
    return onepole_hp(onepole_hp(x, hz, fs), hz, fs)


def lp2(x, hz, fs=FS):
    return onepole_lp(onepole_lp(x, hz, fs), hz, fs)


def chirp_sine(t, f_of_t, fs=FS):
    phase = 2 * np.pi * np.cumsum(f_of_t) / fs
    return np.sin(phase)


def bl_square(t, f, fs=FS):
    """A band-limited square (odd harmonics up to Nyquist)."""
    y = np.zeros_like(t)
    k = 1
    while k * f < 0.45 * fs:
        y += np.sin(2 * np.pi * k * f * t) / k
        k += 2
    return y


def metal(t, freqs, fs=FS):
    return sum(bl_square(t, f, fs) for f in freqs) / len(freqs)


def noise(n, seed):
    return np.random.default_rng(seed).standard_normal(n)


# An inharmonic cluster of our own choosing for the hi-hats / crash (ratios picked so no
# two partials share a low-order relation).
HAT_FREQS = [263.0, 397.0, 481.0, 559.0, 727.0, 947.0]


# ---- the voices ----------------------------------------------------------------------
def kick():
    t = t_axis(0.45)
    f = 48.0 + 110.0 * np.exp(-t / 0.035)
    # a fast thump and a quieter low tail (two-stage decay, as a real kick drum has)
    amp = 0.75 * env(t, 0.05, 0.0015) + 0.25 * env(t, 0.25, 0.0015)
    body = chirp_sine(t, f) * amp
    click = onepole_lp(noise(len(t), 11), 4000) * env(t, 0.004) * 0.6
    x = body + click
    return np.tanh(1.2 * x) / np.tanh(1.2)


def snare():
    t = t_axis(0.35)
    tone = 0.6 * np.sin(2 * np.pi * 185 * t) * env(t, 0.09, 0.001) \
        + 0.35 * np.sin(2 * np.pi * 330 * t) * env(t, 0.06, 0.001)
    wires = onepole_lp(hp2(noise(len(t), 12), 1500), 9000) * env(t, 0.16, 0.001) * 1.1
    snap = hp2(noise(len(t), 13), 3000) * env(t, 0.006) * 0.8
    return tone + wires + snap


def rim():
    t = t_axis(0.20)
    modes = 0.7 * np.sin(2 * np.pi * 450 * t) * env(t, 0.05) \
        + 0.5 * np.sin(2 * np.pi * 1650 * t) * env(t, 0.03) \
        + 0.3 * np.sin(2 * np.pi * 2900 * t) * env(t, 0.02)
    click = hp2(noise(len(t), 21), 2000) * env(t, 0.002) * 0.9
    return modes + click


def hat_core(sec, tau, hp_hz, seed, mix_noise=0.4, freqs=HAT_FREQS, scale=1.0):
    t = t_axis(sec)
    m = metal(t, [f * scale for f in freqs])
    n = noise(len(t), seed)
    x = (1.0 - mix_noise) * m + mix_noise * n
    return hp2(x, hp_hz) * env(t, tau, 0.0005)


def hat():
    return hat_core(0.15, 0.035, 6500, 31)


def pedal():
    x = hat_core(0.20, 0.030, 4000, 32, mix_noise=0.5)
    t = t_axis(0.20)
    chick = onepole_lp(hp2(noise(len(t), 33), 800), 3000) * env(t, 0.012) * 0.5
    return x + chick


def open_hat():
    return hat_core(0.60, 0.17, 6000, 34)


def crash():
    t = t_axis(1.10)
    m = metal(t, [f * 0.62 for f in HAT_FREQS])
    n = noise(len(t), 41)
    x = 0.45 * m + 0.55 * n
    x = onepole_lp(hp2(x, 3000), 10000)
    return x * env(t, 0.30, 0.004)


def ride():
    t = t_axis(0.90)
    parts = [(420, 0.9, 0.39), (1130, 0.7, 0.53), (1790, 0.6, 0.46),
             (2470, 0.5, 0.56), (3330, 0.4, 0.42), (4480, 0.35, 0.35)]
    x = sum(a * np.sin(2 * np.pi * f * t + i) * env(t, tau, 0.001)
            for i, (f, a, tau) in enumerate(parts))
    wash = hp2(noise(len(t), 51), 5000) * env(t, 0.42, 0.002) * 0.5
    ping = hp2(noise(len(t), 52), 3000) * env(t, 0.004) * 0.8
    return x + wash + ping


def bell():
    t = t_axis(0.60)
    parts = [(620, 1.0, 0.35), (1480, 0.8, 0.32), (2330, 0.6, 0.28), (3410, 0.4, 0.21)]
    x = sum(a * np.sin(2 * np.pi * f * t + i) * env(t, tau, 0.0008)
            for i, (f, a, tau) in enumerate(parts))
    ping = hp2(noise(len(t), 61), 4000) * env(t, 0.003) * 0.6
    return x + ping


def tom(f0, tau, seed):
    t = t_axis(0.45)
    f = f0 * (1.0 + 0.22 * np.exp(-t / 0.05))
    body = chirp_sine(t, f) * env(t, tau, 0.001)
    over = 0.25 * chirp_sine(t, 1.58 * f) * env(t, tau * 0.5, 0.001)
    attack = onepole_lp(hp2(noise(len(t), seed), 500), 5000) * env(t, 0.008) * 0.5
    return np.tanh(1.3 * (body + over + attack))


def click():
    t = t_axis(400 / FS_CLICK, FS_CLICK)
    x = 0.8 * np.sin(2 * np.pi * 1900 * t) * env(t, 0.006, 0.0003) \
        + 0.5 * np.sin(2 * np.pi * 3100 * t) * env(t, 0.004, 0.0003)
    x += hp2(noise(len(t), 71), 3000, FS_CLICK) * env(t, 0.001) * 0.3
    return x


# name in the engine, generator, rate, trim?
VOICES = [
    ("kick", kick), ("snare", snare), ("rim", rim), ("hat", hat), ("pedal", pedal),
    ("open", open_hat), ("crash", crash), ("ride", ride), ("bell", bell),
    ("tom_lo", lambda: tom(95, 0.17, 81)), ("tom_mid", lambda: tom(130, 0.14, 82)),
    ("tom_hi", lambda: tom(175, 0.12, 83)),
]


def finish(x, fs, trim=True):
    """Normalise to PEAK and trim where the voice is 60 dB under its peak (+ 10 ms fade)."""
    x = np.asarray(x, dtype=np.float64)
    pk = np.max(np.abs(x))
    x = x / pk
    if trim:
        above = np.nonzero(np.abs(x) >= 1e-3)[0]
        end = min(len(x), int(above[-1]) + 1 + int(0.010 * fs))
        x = x[:end]
        fade = int(0.010 * fs)
        x[-fade:] *= np.linspace(1.0, 0.0, fade)
    return np.round(x * PEAK).astype(np.int16)


def render():
    """The reference voices: what the recipes above make, as 16-bit PCM.  The firmware does not store these (see
    below); they are what the stored forms are measured against, and what fixes the length of every voice."""
    out = {}
    for name, fn in VOICES:
        out[name] = finish(fn(), FS)
    out["click"] = finish(click(), FS_CLICK, trim=False)
    return out


# ---- how each voice is stored in the firmware ---------------------------------------------
# The PCM kit was 86 KB.  The kick and the toms are tonal and short, so the firmware synthesises them (a pitch-gliding sine, a
# decaying overtone, a burst of filtered noise, a saturator - a few multiplies a sample, no table but a sine); the noisy voices
# are stored compressed: IMA-ADPCM (4 bits a sample) where they are short, 8-bit mu-law (G.711, 2:1, much cleaner) for the two
# long cymbals; the metronome click stays 16-bit PCM.  src/dsp/drum_codec.h is the decoder / synthesiser.
STORE = {
    "kick": "kick", "tom_lo": "tom", "tom_mid": "tom", "tom_hi": "tom",
    "snare": "adpcm", "rim": "adpcm", "hat": "adpcm", "pedal": "adpcm", "ride": "adpcm", "bell": "adpcm",
    "open": "ulaw", "crash": "ulaw",
    "click": "pcm",
}
TOM_PARAMS = {"tom_lo": (95.0, 0.17, 81), "tom_mid": (130.0, 0.14, 82), "tom_hi": (175.0, 0.12, 83)}
SYN_RAW = int(0.45 * FS)            # the synthesised voices are computed for 0.45 s, then cut to the reference voice's length

IMA_INDEX = [-1, -1, -1, -1, 2, 4, 6, 8]
IMA_STEP = [7, 8, 9, 10, 11, 12, 13, 14, 16, 17, 19, 21, 23, 25, 28, 31, 34, 37, 41, 45, 50, 55, 60, 66, 73, 80, 88, 97, 107,
            118, 130, 143, 157, 173, 190, 209, 230, 253, 279, 307, 337, 371, 408, 449, 494, 544, 598, 658, 724, 796, 876, 963,
            1060, 1166, 1282, 1411, 1552, 1707, 1878, 2066, 2272, 2499, 2749, 3024, 3327, 3660, 4026, 4428, 4871, 5358, 5894,
            6484, 7132, 7845, 8630, 9493, 10442, 11487, 12635, 13899, 15289, 16818, 18500, 20350, 22385, 24623, 27086, 29794,
            32767]


def ima_step_update(pred, idx, code):
    step = IMA_STEP[idx]
    vp = step >> 3
    if code & 4: vp += step
    if code & 2: vp += step >> 1
    if code & 1: vp += step >> 2
    pred += -vp if code & 8 else vp
    pred = max(-32768, min(32767, pred))
    idx = max(0, min(88, idx + IMA_INDEX[code & 7]))
    return pred, idx


def ima_encode(x):
    """4 bits a sample, the low nibble first; the predictor and the step index start at 0."""
    pred, idx, codes = 0, 0, []
    for s in x:
        step = IMA_STEP[idx]
        diff = int(s) - pred
        code = 0
        if diff < 0:
            code, diff = 8, -diff
        if diff >= step: code |= 4; diff -= step
        if diff >= step >> 1: code |= 2; diff -= step >> 1
        if diff >= step >> 2: code |= 1
        pred, idx = ima_step_update(pred, idx, code)
        codes.append(code)
    if len(codes) & 1:
        codes.append(0)
    return bytes(codes[i] | (codes[i + 1] << 4) for i in range(0, len(codes), 2))


def ima_decode(data, n):
    pred, idx, out = 0, 0, []
    for i in range(n):
        code = (data[i >> 1] >> (4 * (i & 1))) & 15
        pred, idx = ima_step_update(pred, idx, code)
        out.append(pred)
    return np.array(out, dtype=np.int16)


def ulaw_encode(x):
    """G.711 mu-law, one byte a sample."""
    out = bytearray()
    for s in x:
        s = int(s)
        sign = 0x80 if s < 0 else 0
        s = min(abs(s), 32635) + 0x84
        e, mask = 7, 0x4000
        while e > 0 and not (s & mask):
            e -= 1
            mask >>= 1
        out.append(~(sign | (e << 4) | ((s >> (e + 3)) & 15)) & 0xFF)
    return bytes(out)


def ulaw_decode(data):
    out = []
    for b in data:
        u = ~b & 0xFF
        t = (((u & 15) << 3) + 0x84) << ((u >> 4) & 7)
        t -= 0x84
        out.append(-t if u & 0x80 else t)
    return np.array(out, dtype=np.int16)


# The synthesised voices, in the arithmetic the firmware uses (src/dsp/drum_codec.h): exponential envelopes as a multiply a
# sample, a 256-entry sine table with linear interpolation, a 32-bit LCG for the noise, one-pole filters, a rational tanh.  This is
# a mirror in doubles; the gain that brings each voice to PEAK is measured here and written into the file.
SIN_TAB = [float(np.float32(math.sin(2 * math.pi * i / 256.0))) for i in range(257)]


def lut_sin(turns):
    x = turns * 256.0
    i = int(x)
    return SIN_TAB[i] + (SIN_TAB[i + 1] - SIN_TAB[i]) * (x - i)


def tanh_pade(u):
    u2 = u * u
    if u2 < 0.25: return u * (1.0 - u2 * (1.0 / 3.0 - u2 * (2.0 / 15.0)))
    if u > 3.0: return 1.0
    if u < -3.0: return -1.0
    u2 = u * u
    return u * (27.0 + u2) / (27.0 + 9.0 * u2)


def decay_mul(tau):
    return math.exp(-1.0 / (tau * FS))


def pole_coef(hz):
    return 1.0 - math.exp(-2.0 * math.pi * hz / FS)


class Lcg:
    def __init__(self, seed):
        self.s = (seed * 2654435761 + 12345) & 0xFFFFFFFF

    def next(self):                  # uniform on +-sqrt(3): unit variance
        self.s = (self.s * 1664525 + 1013904223) & 0xFFFFFFFF
        return ((self.s >> 8) / 8388608.0 - 1.0) * 1.7320508


def synth_params(kind, name):
    """The constants of one synthesised voice (floats the file carries)."""
    if kind == "kick":
        return dict(pinc0=48.0 / FS, g0=110.0 / FS, kg=decay_mul(0.035),
                    a1=0.75, k1=decay_mul(0.05), a2=0.25, k2=decay_mul(0.25), a3=0.6, k3=decay_mul(0.004),
                    a_hp=0.0, a_lp=pole_coef(4000), att_inv=1.0 / (0.0015 * FS), drive=1.2, seed=11)
    f0, tau, seed = TOM_PARAMS[name]
    return dict(pinc0=f0 / FS, g0=0.22 * f0 / FS, kg=decay_mul(0.05),
                a1=1.0, k1=decay_mul(tau), a2=0.25, k2=decay_mul(tau * 0.5), a3=0.5, k3=decay_mul(0.008),
                a_hp=pole_coef(500), a_lp=pole_coef(5000), att_inv=1.0 / (0.001 * FS), drive=1.3, seed=seed)


def synth_raw(kind, p, n):
    out = np.zeros(n)
    ph, g = 0.0, p["g0"]
    e1, e2, e3 = p["a1"], p["a2"], p["a3"]
    lp1 = lp2 = lp3 = 0.0
    rng = Lcg(p["seed"])
    for i in range(n):
        ph += p["pinc0"] + g
        if ph >= 1.0: ph -= 1.0
        ramp = min(1.0, i * p["att_inv"])
        if kind == "kick":
            if e3 > 1.0e-3: lp1 += p["a_lp"] * (rng.next() - lp1)
            x = lut_sin(ph) * (e1 + e2) * ramp + lp1 * e3
        else:
            x = lut_sin(ph) * e1 * ramp
            if e2 > 4.0e-3:
                ph2 = ph * 1.58
                if ph2 >= 1.0: ph2 -= 1.0
                x += 0.25 * lut_sin(ph2) * e2 * ramp
            if e3 > 1.0e-3:
                v = rng.next()
                lp1 += p["a_hp"] * (v - lp1); h1 = v - lp1
                lp2 += p["a_hp"] * (h1 - lp2); h2 = h1 - lp2
                lp3 += p["a_lp"] * (h2 - lp3)
                x += lp3 * e3
        out[i] = tanh_pade(p["drive"] * x)
        g *= p["kg"]; e1 *= p["k1"]; e2 *= p["k2"]; e3 *= p["k3"]
    return out


def synth_voice(kind, name, n_target):
    """-> (the int16 the firmware plays, the parameter dict with its gain)"""
    p = synth_params(kind, name)
    raw = synth_raw(kind, p, SYN_RAW)
    p["gain"] = PEAK / float(np.max(np.abs(raw)))
    fade = int(0.010 * FS)
    y = raw[:n_target] * p["gain"]
    y[-fade:] *= np.linspace(1.0, 0.0, fade)
    return np.round(y).astype(np.int16), p


def snr_db(ref, y):
    e = ref.astype(np.float64) - y.astype(np.float64)
    return 10 * math.log10(float((ref.astype(np.float64) ** 2).sum()) / max(1e-9, float((e ** 2).sum())))


def build():
    """-> (voices in file order, the PCM the firmware plays).  A voice is a dict: name, store, n, plus its data or parameters."""
    ref = render()
    voices, played = [], {}
    for name, arr in ref.items():
        how = STORE[name]
        v = dict(name=name, store=how, n=len(arr), snr=None)
        if how == "pcm":
            v["data"] = arr
            played[name] = arr
        elif how == "adpcm":
            v["data"] = ima_encode(arr)
            played[name] = ima_decode(v["data"], len(arr))
        elif how == "ulaw":
            v["data"] = ulaw_encode(arr)
            played[name] = ulaw_decode(v["data"])
        else:
            played[name], v["params"] = synth_voice(how, name, len(arr))
        v["snr"] = snr_db(arr, played[name])
        voices.append(v)
    return voices, played, ref


def c_float(x):
    t = "%.9g" % float(np.float32(x))
    if not any(c in t for c in ".eEn"):
        t += ".0"
    return t + "f"


def emit(voices):
    b = io.StringIO()
    b.write("/* custom-fw/src/dsp/drum_kit.inc - the Groove Station's drum kit\n"
            " *\n"
            " * GENERATED FILE - do not edit.  Regenerate with\n"
            " *\n"
            " *     python custom-fw/tools/gen_drum_kit.py\n"
            " *\n"
            " * Every voice is synthesised by that tool (oscillators, noise and filters, fixed\n"
            " * seeds): original material, no recordings.  The twelve drum voices are 22.05 kHz,\n"
            " * brought up to the 44.1 kHz output by the engine; the click is at the same rate.\n"
            " *\n"
            " * Stored three ways (see the tool): the kick and the toms as the constants of a small\n"
            " * synthesiser, the noisy voices as IMA-ADPCM or G.711 mu-law, the click as 16-bit PCM.\n"
            " * Needs src/dsp/drum_codec.h before it.\n"
            " */\n\n")
    b.write("/* the synthesised voices' sine: 256 steps and the wrap point */\n"
            "static const float k_kit_sin[257] = {\n")
    for i in range(0, 257, 8):
        b.write("    " + ", ".join(c_float(v) for v in SIN_TAB[i:i + 8]) + ",\n")
    b.write("};\n\n")
    for v in voices:
        name, n = v["name"], v["n"]
        up = name.upper()
        b.write(f"#define KIT_{up}_N {n}u\n")
        if v["store"] in ("kick", "tom"):
            p = v["params"]
            b.write(f"static const pa_dsynth_t k_kit_{name} = {{\n"
                    f"    {c_float(p['gain'])}, {c_float(p['pinc0'])}, {c_float(p['g0'])}, {c_float(p['kg'])},\n"
                    f"    {c_float(p['a1'])}, {c_float(p['k1'])}, {c_float(p['a2'])}, {c_float(p['k2'])},\n"
                    f"    {c_float(p['a3'])}, {c_float(p['k3'])}, {c_float(p['a_hp'])}, {c_float(p['a_lp'])},\n"
                    f"    {c_float(p['att_inv'])}, {c_float(p['drive'])}, {p['seed']}u, k_kit_sin,\n"
                    "};\n\n")
        elif v["store"] == "pcm":
            b.write(f"static const int16_t k_kit_{name}[KIT_{up}_N] = {{\n")
            arr = v["data"]
            for i in range(0, len(arr), 12):
                b.write("    " + ", ".join(str(int(x)) for x in arr[i:i + 12]) + ",\n")
            b.write("};\n\n")
        else:
            what = "IMA-ADPCM, 4 bits a sample" if v["store"] == "adpcm" else "G.711 mu-law, 8 bits a sample"
            size = f"(KIT_{up}_N + 1u) / 2u" if v["store"] == "adpcm" else f"KIT_{up}_N"
            b.write(f"/* {what} */\n")
            b.write(f"static const uint8_t k_kit_{name}[{size}] = {{\n")
            data = v["data"]
            for i in range(0, len(data), 24):
                b.write("    " + ", ".join("0x%02x" % x for x in data[i:i + 24]) + ",\n")
            b.write("};\n\n")
    return b.getvalue()


def write_wavs(played, folder):
    os.makedirs(folder, exist_ok=True)
    for name, arr in played.items():
        with wave.open(os.path.join(folder, f"{name}.wav"), "wb") as w:
            w.setnchannels(1)
            w.setsampwidth(2)
            w.setframerate(FS_CLICK if name == "click" else FS)
            w.writeframes(arr.tobytes())


def stored_bytes(v):
    if v["store"] in ("kick", "tom"):
        return 64                        # the constants (struct pa_dsynth_t)
    return len(v["data"]) * (2 if v["store"] == "pcm" else 1)


def main():
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("--check", action="store_true", help="verify the file instead of writing it")
    ap.add_argument("--out", default=OUT)
    ap.add_argument("--wav", help="also write one WAV per voice (as the firmware plays it) into this folder")
    a = ap.parse_args()
    voices, played, ref = build()
    text = emit(voices)
    if a.wav:
        write_wavs(played, a.wav)
    if a.check:
        cur = open(a.out, encoding="utf-8").read() if os.path.exists(a.out) else ""
        same = cur == text
        print(f"{a.out}: {'matches the generator' if same else 'DIFFERS from the generator'}")
        return 0 if same else 1
    with open(a.out, "w", encoding="utf-8", newline="\n") as f:
        f.write(text)
    pcm_total = sum(len(v) * 2 for v in ref.values())
    total = sum(stored_bytes(v) for v in voices)
    print(f"wrote {a.out}: {len(voices)} voices, {total:,} bytes stored (as 16-bit PCM: {pcm_total:,}), "
          f"sha256 {hashlib.sha256(text.encode()).hexdigest()[:16]}")
    for v in voices:
        fs = FS_CLICK if v["name"] == "click" else FS
        print(f"   {v['name']:8s} {v['n']:6d} frames ({1000 * v['n'] / fs:6.0f} ms)  {v['store']:6s} {stored_bytes(v):6d} B  "
              f"SNR vs the reference {v['snr']:5.1f} dB")
    return 0


if __name__ == "__main__":
    sys.exit(main())

# SPDX-License-Identifier: GPL-3.0-only
# Copyright (C) 2026 The Onyx Authors (@28spawn)
