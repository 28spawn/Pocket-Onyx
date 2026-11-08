#!/usr/bin/env python3
"""custom-fw/tools/stress_effects.py - the effect-matrix stress test.

Runs every effect on its own, in combination, and in different ORDERS, over a
sine and a real recording, and checks two things:

  1. SANITY - the chain always produces a sane signal: no NaN, no silence, no
     runaway, no DC offset, no over-clipping, and a sane stereo image.

  2. BEHAVIOUR - each effect actually does the job it is supposed to do. A
     drive adds gain and distortion, a cabinet rolls off the top, a noise
     gate closes on a quiet signal, a delay leaves a tail, a reverb leaves a
     longer one, a modulator moves the amplitude. A block that does nothing
     would pass the sanity check and still be broken.

The order is the point. A drive after the cabinet runs on the cabinet's
high-passed output, a reverb before the amp gets distorted by it, and a delay
before the drive smears the drive's attack. This is what finds the
combinations that "bug the audio".

    python custom-fw/tools/stress_effects.py --dll custom-fw/emu/pocket_amp_emu.dll

The method, the signals and the pass criteria are written up in
custom-fw/test/EFFECT_MATRIX.md - read that for the reasoning.
"""
import argparse
import ctypes
import math
import os
import random
import sys
import wave

# --------------------------------------------------------------- the chain --
# The slot ids, in the firmware's order.
DYN, DRIVE, AMP, CAB, EQ, MOD, DLY, REV = range(8)
SLOT_NAMES = ["DYN", "DRIVE", "AMP", "CAB", "EQ", "MOD", "DLY", "REV"]

# How many knobs each block shows. The knob index IS the DSP's set_param id.
SLOT_KNOBS = {
    DYN:   ["THRESH", "RATIO", "ATTACK", "LEVEL"],
    DRIVE: ["DRIVE", "TONE", "LEVEL"],
    AMP:   ["GAIN", "BASS", "MID", "TREBLE", "PRES", "MASTER"],
    CAB:   ["LEVEL", "LOWCUT", "HICUT"],
    EQ:    ["100Hz", "400Hz", "1kHz", "3kHz", "8kHz"],
    MOD:   ["RATE", "DEPTH", "MIX", "FDBK"],
    DLY:   ["TIME", "FDBK", "MIX", "TONE"],
    REV:   ["DECAY", "LOWCUT", "TONE", "MIX"],
}

# The model list of each block (the type index the UI's dropdown uses).
SLOT_TYPES = {
    DYN:   ["GATE", "BOOST+GATE", "COMP+GATE", "AI GATE MS", "COMP"],
    DRIVE: ["TS-808", "KLON", "OCD", "RAT", "BIG MUFF", "FUZZ FACE"],
    AMP:   None,     # the 20 factory amps
    CAB:   None,     # the 20 factory IRs
    EQ:    None,
    MOD:   ["CHORUS", "PHASER", "FLANGER", "TREMOLO", "VIBRATO", "UNIVIBE", "AUTO FILTER"],
    DLY:   ["ANALOG", "DUCKING", "TAPE", "DUAL", "LO-FI"],
    REV:   ["ROOM", "HALL", "SWELL", "SPRING", "SHIMMER", "CLOUD"],
}
TYPE_COUNT = {DYN: 4, DRIVE: 6, AMP: 20, CAB: 20, EQ: 1, MOD: 7, DLY: 5, REV: 6}

# A representative, musical default for each block: the knob values (0..100)
# the UI would show for a sensible sound.
DEFAULT_PARAMS = {
    DYN:   [50, 50, 50, 100],
    DRIVE: [60, 50, 80],
    AMP:   [45, 50, 50, 55, 50, 50],
    CAB:   [100, 50, 50],
    EQ:    [50, 50, 50, 50, 50],
    MOD:   [50, 50, 30, 10],
    DLY:   [50, 15, 10, 50],
    REV:   [30, 45, 50, 10],
}

# --------------------------------------------------------- the pass criteria
# Every one of these has a reason; see EFFECT_MATRIX.md.
SILENCE_FLOOR_DB = -60.0   # the output may not be quieter than this
MAX_GAIN_DB      = 24.0    # and not louder than this
MAX_CLIP_FRAC    = 0.05    # at most 5 % of the samples may be at the rail
MAX_DC_FRAC      = 0.02    # the DC offset may not exceed 2 % of the peak
MAX_RUNAWAY      = 4.0     # the gain may not grow by more than this, relative
                           # to the INPUT's own trend (a recording that fades
                           # in must not be mistaken for a runaway)
MIN_EFFECT       = 0.02    # an effect must change the signal by at least this
MAX_LR_SPILL     = 0.02    # a MONO input must come out of a MONO chain with
                           # the left and right channels equal. The global EQ
                           # used to be applied to the left channel only, so
                           # the right channel came out with no EQ at all.

# The MASTER volume the matrix measures the chain at.
#
# The matrix measures the EFFECTS, not the output stage's overload handling.
# The chain ends in a limiter and a soft ceiling (pa_soft_ceiling_q21), and
# the high-gain metal models - 3USMetal1, 9USMetal2, 10ModMetal - have so much
# gain and so much internal squash that they drive that limiter into its clamp
# for 35..43 % of the signal at a -14 dBFS input, at ANY input level: their
# own nonlinearity flattens the wave (a crest factor of 1.09..1.16 instead of
# 1.41), so they sit at the rail from -14 dBFS down to -26 dBFS. That is the
# amp's character, not a fault - but it would drown every other case in the
# matrix.
#
# So the measurement runs with the master backed off 20 dB (the pot at 2048
# = 50 %, i.e. -20 dB on the -40..0 dB taper). The chain keeps 20 dB of
# headroom, so the "no more than 5 % at the rail" criterion means what it
# says again: a chain that is far louder than it should be. The level law and
# the output stage are measured on their own, in custom-fw/emu/test_level.py.
#
# This used to be accidental: the emulator's MASTER pot started at the middle
# of its range (2048) and the firmware's tick reads that pot every tick, so
# the matrix silently ran 20 dB down whatever it asked for. The pot now starts
# at full scale, and the matrix sets it explicitly.
MATRIX_MASTER_Q12 = 2048    # 50 % = -20 dB on the firmware's taper

# The behavioural thresholds.
MIN_DRIVE_GAIN   = 1.5     # a drive must add at least this much gain
MIN_AMP_GAIN     = 1.5     # an amp must add at least this much gain (or distortion: MIN_THD)
MIN_THD          = 0.02    # a drive must add at least this much distortion
MAX_CAB_HF       = 0.35    # a cabinet must pass at most this much of 8 kHz
                             # relative to 1 kHz
MAX_EQ_DEVIATION = 0.15    # a flat EQ must stay within this of unity
MIN_TAIL         = 1e-4    # a delay/reverb must leave a tail
MIN_MOD_DEPTH    = 0.02    # a modulator must move the amplitude
MIN_GATE_ATTEN   = 12.0    # a gate must attenuate a quiet signal by this much
TOGGLE_TOLERANCE = 0.15    # a block must keep its level across a bypass
                             # (a delay is exempt: it restarts in phase)

# The frequencies the transfer function is measured at.
TF_FREQS = [100, 200, 400, 800, 1600, 3200, 6400, 10000]


class Telemetry(ctypes.Structure):
    _fields_ = [
        ("view", ctypes.c_uint32), ("preset_index", ctypes.c_uint32),
        ("preset_volume", ctypes.c_uint32), ("master_volume", ctypes.c_uint32),
        ("selected_slot", ctypes.c_uint32), ("selected_param", ctypes.c_uint32),
        ("slot_active_mask", ctypes.c_uint32), ("sram_used_bytes", ctypes.c_uint32),
        ("sram_peak_bytes", ctypes.c_uint32), ("cpu_total_permille", ctypes.c_uint32),
        ("cpu_stage_permille", ctypes.c_uint32 * 8),
        ("tuner_active", ctypes.c_uint32), ("tuner_locked", ctypes.c_uint32),
        ("tuner_note", ctypes.c_uint32), ("tuner_octave", ctypes.c_int32),
        ("tuner_cents", ctypes.c_int32), ("tuner_freq_tenths", ctypes.c_uint32),
        ("tuner_dots", ctypes.c_uint32), ("drum_playing", ctypes.c_uint32),
        ("drum_bpm", ctypes.c_uint32), ("drum_pattern", ctypes.c_uint32),
        ("in_peak", ctypes.c_float), ("out_peak", ctypes.c_float),
        ("bt_state", ctypes.c_uint32), ("cursor_slot", ctypes.c_uint32),
        ("editor_cursor", ctypes.c_uint32),
    ]


class Emu:
    """The emulator DLL, driven the way the Python server drives it."""

    N = 64
    SR = 44100

    def __init__(self, dll_path):
        self.d = ctypes.CDLL(dll_path)
        d = self.d
        d.emu_process_audio_f32.argtypes = [ctypes.POINTER(ctypes.c_float),
                                             ctypes.POINTER(ctypes.c_float),
                                             ctypes.POINTER(ctypes.c_float),
                                             ctypes.c_int]
        d.emu_ui_input.argtypes = [ctypes.c_uint32, ctypes.c_int32, ctypes.c_uint16]
        d.emu_set_slot_enabled.argtypes = [ctypes.c_int, ctypes.c_int]
        d.emu_set_slot_param.argtypes = [ctypes.c_int, ctypes.c_int, ctypes.c_int]
        d.emu_set_slot_type.argtypes = [ctypes.c_int, ctypes.c_int]
        d.emu_set_slot_order.argtypes = [ctypes.POINTER(ctypes.c_uint8)]
        d.emu_get_slot_order.argtypes = [ctypes.POINTER(ctypes.c_uint8)]
        d.emu_set_master_volume.argtypes = [ctypes.c_int]
        d.emu_ui_pot.argtypes = [ctypes.c_uint16]
        d.emu_ui_pot.restype = None
        d.emu_select_preset.argtypes = [ctypes.c_int]
        d.emu_get_telemetry.argtypes = [ctypes.POINTER(Telemetry)]
        self.inp = (ctypes.c_float * self.N)()
        self.outL = (ctypes.c_float * self.N)()
        self.outR = (ctypes.c_float * self.N)()
        self.tel = Telemetry()

    def reset(self):
        """A clean chain. emu_init() rebuilds everything, so each case starts
        from the same state - otherwise one case would inherit the previous
        case's delay lines and reverb tails."""
        self.d.emu_init()
        self.d.emu_set_master_volume(100)
        # The MASTER the matrix measures at - see MATRIX_MASTER_Q12. It has to
        # be set through the pot, because the firmware's tick reads the pot
        # every tick and would otherwise overwrite whatever was asked for.
        self.d.emu_ui_pot(MATRIX_MASTER_Q12)
        self.order(*range(8))

    def order(self, *slots):
        assert len(slots) == 8 and sorted(slots) == list(range(8)), slots
        buf = (ctypes.c_uint8 * 8)(*slots)
        self.d.emu_set_slot_order(buf)

    def get_order(self):
        buf = (ctypes.c_uint8 * 8)()
        self.d.emu_get_slot_order(buf)
        return list(buf)

    def enable(self, *slots):
        for s in range(8):
            self.d.emu_set_slot_enabled(s, 1 if s in slots else 0)

    def params(self, slot, values):
        for i, v in enumerate(values):
            self.d.emu_set_slot_param(slot, i, v)

    def set_type(self, slot, t):
        self.d.emu_set_slot_type(slot, t)

    def cpu_permille(self):
        self.d.emu_get_telemetry(ctypes.byref(self.tel))
        return self.tel.cpu_total_permille

    def run(self, signal, collect=True):
        """Feed a mono float list through the chain. Returns (left, right)."""
        n = self.N
        gotL, gotR = [], []
        for off in range(0, len(signal) - n + 1, n):
            for i in range(n):
                self.inp[i] = signal[off + i]
            self.d.emu_process_audio_f32(self.inp, self.outL, self.outR, n)
            if collect:
                gotL.extend(self.outL[i] for i in range(n))
                gotR.extend(self.outR[i] for i in range(n))
        return gotL, gotR


# ------------------------------------------------------------- the signals --
def sine(freq=220.0, amp=0.2, seconds=1.0, sr=44100):
    """A sine. The DC blockers in the chain remove a constant input over
    time, so a DC test signal would make a later measurement window read
    quieter and fake a level change that is not there."""
    n = int(seconds * sr)
    return [amp * math.sin(2 * math.pi * freq * i / sr) for i in range(n)]


def noise(amp=0.25, seconds=1.0, seed=1234):
    """White noise, for the transfer function. A single sine has no content
    above its own frequency, so it cannot show whether a cabinet filters the
    top or an EQ moves a band."""
    rnd = random.Random(seed)
    n = int(seconds * 44100)
    return [rnd.uniform(-amp, amp) for _ in range(n)]


def read_wav(path, seconds=1.0, sr=44100, target_peak=None):
    w = wave.open(path, "rb")
    nch, rate, nframes, width = (w.getnchannels(), w.getframerate(),
                                 w.getnframes(), w.getsampwidth())
    raw = w.readframes(min(nframes, int(seconds * rate)))
    w.close()
    import array
    if width == 2:
        a = array.array("h"); a.frombytes(raw)
        if sys.byteorder == "big": a.byteswap()
        s = [x / 32768.0 for x in a]
    else:
        a = array.array("f"); a.frombytes(raw)
        s = list(a)
    if nch > 1:
        s = [sum(s[i:i + nch]) / nch for i in range(0, len(s) - nch + 1, nch)]
    if target_peak:
        pk = max(abs(x) for x in s) or 1.0
        s = [x * target_peak / pk for x in s]
    return s


# ------------------------------------------------------------- the measuring --
def rms_of(x):
    return math.sqrt(sum(v * v for v in x) / len(x)) if x else 0.0


def band_mag(sig, freqs, sr=44100):
    """The magnitude at a set of frequencies (a plain DFT - slow but exact,
    and the test does not need more)."""
    out = {}
    n = len(sig)
    if n == 0:
        return {f: 0.0 for f in freqs}
    for f in freqs:
        re = im = 0.0
        w = 2 * math.pi * f / sr
        for i, x in enumerate(sig):
            re += x * math.cos(w * i)
            im += x * math.sin(w * i)
        out[f] = 2.0 * math.hypot(re, im) / n
    return out


def transfer(out, ref, freqs):
    """The output/input magnitude at each frequency, relative to the bare
    chain at the same frequency. 1.0 = unchanged, 0.5 = -6 dB."""
    mo = band_mag(out, freqs)
    mr = band_mag(ref, freqs)
    return {f: (mo[f] / mr[f]) if mr[f] > 1e-9 else float("nan") for f in freqs}


def thd(sig, f0=220.0, sr=44100):
    """Total harmonic distortion: the harmonic energy over the fundamental.
    This is what tells a DRIVE (which adds distortion) from a clean path."""
    m = band_mag(sig, [f0, 2 * f0, 3 * f0, 4 * f0], sr)
    fund = m[f0]
    if fund < 1e-12:
        return 0.0
    harm = math.sqrt(m[2 * f0] ** 2 + m[3 * f0] ** 2 + m[4 * f0] ** 2)
    return harm / fund


def envelope_depth(sig, win=512):
    """How much the amplitude moves over time (the coefficient of variation of
    the per-window RMS). A MODULATOR moves it; a static filter does not."""
    if len(sig) < win * 2:
        return 0.0
    vals = []
    for off in range(0, len(sig) - win, win):
        r = rms_of(sig[off:off + win])
        if r > 1e-9:
            vals.append(r)
    if len(vals) < 4:
        return 0.0
    mean = sum(vals) / len(vals)
    if mean < 1e-12:
        return 0.0
    var = sum((v - mean) ** 2 for v in vals) / len(vals)
    return math.sqrt(var) / mean


def measure(inp, left, right, mono_chain=True, has_saturation=True):
    """Everything the criteria need, from one measurement window."""
    m = {}
    n = len(left)
    if n == 0:
        return dict(ok=False, why="no output")
    peak = max(abs(x) for x in left)
    rms = rms_of(left)
    mean = sum(left) / n
    bad = sum(1 for x in left if x != x or x in (float("inf"), float("-inf")))
    clipped = sum(1 for x in left if abs(x) > 0.995)
    in_rms = rms_of(inp)
    # the stereo image: how far L and R differ
    diff_rms = math.sqrt(sum((a - b) ** 2 for a, b in zip(left, right)) / n)
    # the runaway check, normalised by the INPUT's own trend: a recording that
    # fades in must not be mistaken for the chain running away.
    k = max(1, n // 10)
    out_first = rms_of(left[:k]);  out_last = rms_of(left[-k:])
    in_first = rms_of(inp[:k]);    in_last = rms_of(inp[-k:])
    g_first = out_first / in_first if in_first > 1e-12 else 1.0
    g_last = out_last / in_last if in_last > 1e-12 else 1.0
    runaway = (g_last / g_first) if g_first > 1e-9 else 1.0
    m.update(peak=peak, rms=rms, mean=mean, bad=bad, clipped=clipped,
             gain_db=20 * math.log10(max(rms, 1e-12) / max(in_rms, 1e-12)),
             runaway=runaway, n=n, diff_rms=diff_rms,
             stereo_spread=(diff_rms / rms) if rms > 1e-9 else 0.0)
    why = []
    if bad:
        why.append("%d NaN/Inf samples" % bad)
    if rms < 10 ** (SILENCE_FLOOR_DB / 20):
        why.append("the output is silent (%.0f dBFS)" % (20 * math.log10(max(rms, 1e-12))))
    if m["gain_db"] > MAX_GAIN_DB:
        why.append("the output is %.1f dB too loud" % m["gain_db"])
    if peak > 0 and abs(mean) / peak > MAX_DC_FRAC:
        why.append("a DC offset of %.1f %% of the peak" % (100 * abs(mean) / peak))
    if has_saturation:
        if clipped / n > MAX_CLIP_FRAC:
            why.append("%.1f %% of the samples are clipped" % (100 * clipped / n))
    else:
        if clipped > 0:
            why.append("%d sample(s) clipped (clean blocks must have 0%% clipping)" % clipped)
    if runaway > MAX_RUNAWAY:
        why.append("the gain ran away (x%.1f)" % runaway)
    # A mono signal through a chain of mono blocks must come out with the
    # left and right channels equal. A stereo block (MOD/DLY/REV) legitimately
    # makes them differ, so the check is only meaningful for a mono chain.
    if mono_chain and rms > 1e-9 and m["stereo_spread"] > MAX_LR_SPILL:
        why.append("the left and right channels differ by %.0f %% on a mono "
                   "signal" % (100 * m["stereo_spread"]))
    m["why"] = ", ".join(why)
    m["ok"] = not why
    return m


# ------------------------------------------------------------------- the runs --
class Runner:
    def __init__(self, emu, report):
        self.emu = emu
        self.report = report

    def configure(self, slots, order, types=None, params=None):
        emu = self.emu
        emu.reset()
        emu.enable(*slots)
        for slot, vals in (params or DEFAULT_PARAMS).items():
            emu.params(slot, vals)
        for slot, t in (types or {}).items():
            emu.set_type(slot, t)
        emu.order(*order)

    def warmup(self, signal):
        """Let the chain settle: the master volume slews, the preset volume
        glides and the DC blockers fill up. Measuring during that would read
        the transient, not the effect."""
        self.emu.run(signal[:len(signal) // 3], collect=False)

    def case(self, name, slots, order, signal, label, types=None, params=None,
             record=True):
        """Measure one configuration.

        `record=False` marks a DIAGNOSTIC run (a probe for the behaviour
        checks, not a case in its own right): a probe deliberately uses a
        signal the chain is meant to reject - a noise gate silencing a quiet
        signal is the gate WORKING, not a failure - so a probe must not be
        judged by the sanity criteria."""
        self.configure(slots, order, types, params)
        self.warmup(signal)
        left, right = self.emu.run(signal)
        # A chain with no stereo block (MOD/DLY/REV) must keep the two
        # channels equal for a mono input.
        mono_chain = not (set(slots) & {MOD, DLY, REV})
        # Clean blocks (EQ, Delay, Reverb, Mod, etc.) must NEVER clip (0% allowed).
        # The relaxed clipping threshold is reserved strictly for Drive and Amp blocks.
        has_saturation = bool(set(slots) & {DRIVE, AMP})
        m = measure(signal, left, right, mono_chain=mono_chain, has_saturation=has_saturation)
        m["cpu"] = self.emu.cpu_permille()
        m["name"] = name
        m["slots"] = " ".join(SLOT_NAMES[s] for s in slots)
        m["order"] = " ".join(SLOT_NAMES[s] for s in order)
        m["signal"] = label
        m["left"], m["right"] = left, right
        if record:
            self.report.append(m)
        return m

    def tail(self, slots, order, signal, types=None, params=None):
        """Feed the signal, then silence, and measure the output during the
        silence. A delay or a reverb rings on; a static filter does not."""
        self.configure(slots, order, types, params)
        self.warmup(signal)
        self.emu.run(signal, collect=False)
        silence = [0.0] * (int(0.6 * Emu.SR))
        left, right = self.emu.run(silence)
        return rms_of(left)

    def reference(self, signal):
        """The bare chain, for the behavioural comparisons."""
        self.configure((), tuple(range(8)))
        self.warmup(signal)
        left, right = self.emu.run(signal)
        return left


def fmt(m):
    if m["ok"]:
        return ("ok   peak %6.3f  rms %6.3f  gain %+6.1f dB  spread %4.2f  cpu %4.1f %%"
                % (m["peak"], m["rms"], m["gain_db"], m["stereo_spread"], m["cpu"] / 10.0))
    return "FAIL " + m["why"]


def check_behaviour(run, slot, sig, label, failures):
    """Does the effect do its job? Appends the complaints to `failures`."""
    bad = []
    ref = run.reference(sig)

    if slot in (DRIVE, AMP, CAB, EQ):
        # the transfer function, on broadband noise
        nz = run.noise_signal
        ref_nz = run.reference(nz)
        out_nz = run.case("tf", (slot,), tuple(range(8)), nz, "noise", record=False)
        tf = transfer(out_nz["left"], ref_nz, TF_FREQS)
        if slot == DRIVE:
            if not (tf[800] > MIN_DRIVE_GAIN):
                bad.append("the drive adds no gain (%.2fx at 800 Hz)" % tf[800])
            t_on, t_off = thd(out_nz["left"]), 0.0
            if t_on < MIN_THD:
                bad.append("the drive adds no distortion (THD %.4f)" % t_on)
        elif slot == AMP:
            # An amp must DO something: add gain, or add distortion. (The stock amps are
            # level-matched, so a clean model at GAIN 45 % adds little level on loud noise:
            # Amp 1 (UK Clean) is +1.3 dB. It still distorts a guitar-level note, and a
            # bypassed or broken amp does neither.)
            if not (tf[400] > MIN_AMP_GAIN):
                s = sine(220.0, 0.2)
                out_s = run.case("thd", (slot,), tuple(range(8)), s, "sine", record=False)
                t_amp, t_ref = thd(out_s["left"]), thd(run.reference(s))
                if not (t_amp > MIN_THD and t_amp > 3 * t_ref):
                    bad.append("the amp adds no gain (%.2fx at 400 Hz) and no distortion (THD %.4f)"
                               % (tf[400], t_amp))
        elif slot == CAB:
            if not (tf[6400] <= MAX_CAB_HF):
                bad.append("the cab does not roll off the top (%.2fx at 6.4 kHz)"
                           % tf[6400])
            if not (tf[100] < 0.9):
                bad.append("the cab does not cut the low end (%.2fx at 100 Hz)"
                           % tf[100])
        elif slot == EQ:
            for f in TF_FREQS:
                if abs(tf[f] - 1.0) > MAX_EQ_DEVIATION:
                    bad.append("the flat EQ is off by %.2fx at %d Hz" % (tf[f] - 1.0, f))
                    break
    elif slot == DYN:
        # a noise gate must close on a quiet signal
        quiet = run.quiet_signal
        out_q = run.case("gate", (DYN,), tuple(range(8)), quiet, "quiet", record=False)
        ref_q = rms_of(run.reference(quiet))
        if ref_q > 1e-9 and rms_of(out_q["left"]) > 0:
            att = 20 * math.log10(ref_q / rms_of(out_q["left"]))
            if att < MIN_GATE_ATTEN:
                bad.append("the gate does not close on a quiet signal (%.1f dB)" % att)
    elif slot == MOD:
        out = run.case("mod", (MOD,), tuple(range(8)), sig, label, record=False)
        if envelope_depth(out["left"]) < MIN_MOD_DEPTH:
            bad.append("the modulator does not move the amplitude")
    elif slot in (DLY, REV):
        tail = run.tail((slot,), tuple(range(8)), sig)
        if tail < MIN_TAIL:
            bad.append("the %s leaves no tail" % SLOT_NAMES[slot])

    for c in bad:
        failures.append((SLOT_NAMES[slot], label, c))
    return bad


def check_toggle(run, failures):
    """Does a block keep its settings when it is bypassed and switched back
    on? A bypass frees the block's state (the C7 SRAM reclaim) and switching
    it on again creates a fresh one, so the knobs and the model have to be
    put back - otherwise the block comes up with the factory defaults."""
    sig = run.noise_signal
    for slot in range(8):
        if slot in (DLY, REV):
            continue    # a delay restarts in phase: its level legitimately differs
        # the reference: the block, on, never toggled
        run.configure((slot,), tuple(range(8)))
        run.warmup(sig)
        ref, _ = run.emu.run(sig)
        # the same block, toggled off and on again
        run.configure((slot,), tuple(range(8)))
        run.emu.d.emu_set_slot_enabled(slot, 0)
        run.emu.run(sig, collect=False)
        run.emu.d.emu_set_slot_enabled(slot, 1)
        run.warmup(sig)
        out, _ = run.emu.run(sig)
        a, b = rms_of(ref), rms_of(out)
        if a > 1e-9:
            dev = abs(b - a) / a
            if dev > TOGGLE_TOLERANCE:
                failures.append((SLOT_NAMES[slot], "bypass toggle",
                                 "the settings were lost across a bypass "
                                 "(%.0f %% of the level)" % (100 * dev)))


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--dll", default=os.path.join(os.path.dirname(os.path.dirname(
        os.path.abspath(__file__))), "emu", "pocket_amp_emu.dll"))
    ap.add_argument("--wav", default=os.path.join(os.path.dirname(os.path.dirname(
        os.path.abspath(__file__))), "emu", "samples", "rock_riff.wav"))
    ap.add_argument("--quick", action="store_true", help="only the single-effect cases")
    ap.add_argument("--types", action="store_true", help="also sweep every model type")
    a = ap.parse_args()

    if not os.path.exists(a.dll):
        print("the emulator DLL is not built: %s" % a.dll)
        print("  powershell -ExecutionPolicy Bypass -File custom-fw/emu/build_dll.ps1")
        return 2

    emu = Emu(a.dll)
    report = []
    run = Runner(emu, report)
    run.noise_signal = noise()
    run.quiet_signal = sine(amp=0.005, seconds=1.0)
    behaviour_fails = []

    signals = [
        ("sine -14 dBFS", sine(amp=0.2)),
        ("sine -6 dBFS", sine(amp=0.5)),
    ]
    if os.path.exists(a.wav):
        signals.append(("rock_riff -6 dBFS", read_wav(a.wav, target_peak=0.5)))
    else:
        print("(no %s - using the sines only)" % a.wav)

    # ---------------------------------------------------------- 1. one effect --
    print("=== 1. every effect on its own, and what it is supposed to do ===")
    for slot in range(8):
        for sname, sig in signals:
            m = run.case(SLOT_NAMES[slot], [slot], tuple(range(8)), sig, sname)
            print("  %-6s %-16s  %s" % (m["slots"], m["signal"], fmt(m)))
            if not m["ok"]:
                continue
            complaints = check_behaviour(run, slot, sig, sname, behaviour_fails)
            for c in complaints:
                print("       FAIL %s" % c)

    # --------------------------------------------------- 2. every model type --
    if a.types:
        print()
        print("=== 2. every MODEL of every block ===")
        for slot, count in sorted(TYPE_COUNT.items()):
            names = SLOT_TYPES.get(slot)
            for t in range(count):
                for sname, sig in signals[:1]:
                    m = run.case("%s/%s" % (SLOT_NAMES[slot],
                                            names[t] if names else str(t)),
                                 [slot], tuple(range(8)), sig, sname,
                                 types={slot: t})
                    print("  %-22s %-16s  %s" % (m["name"], m["signal"], fmt(m)))

    # --------------------------------------------------------- 3. the pairs --
    if not a.quick:
        print()
        print("=== 3. every PAIR, in both orders ===")
        for i in range(8):
            for j in range(8):
                if i == j:
                    continue
                for sname, sig in signals[:1]:
                    rest = tuple(k for k in range(8) if k not in (i, j))
                    m = run.case("%s+%s" % (SLOT_NAMES[i], SLOT_NAMES[j]),
                                 [i, j], (i, j) + rest, sig, sname)
                    if not m["ok"]:
                        print("  FAIL %-14s %-16s  %s" % (m["name"], m["signal"], m["why"]))
        print("  %d pair cases run" % len([m for m in report if "+" in m["name"]]))

        # ------------------------------------------------ 4. the whole chain --
        print()
        print("=== 4. the whole chain, in a few orders ===")
        orders = [
            (DYN, DRIVE, AMP, CAB, EQ, MOD, DLY, REV),   # the default
            (DRIVE, AMP, CAB, EQ, MOD, DLY, REV, DYN),
            (CAB, DRIVE, AMP, EQ, MOD, DLY, REV, DYN),   # a drive AFTER the cab
            (AMP, CAB, DRIVE, EQ, MOD, DLY, REV, DYN),   # a drive after the cab too
            (REV, DLY, MOD, EQ, AMP, CAB, DRIVE, DYN),   # the time effects first
        ]
        for sname, sig in signals:
            for o in orders:
                m = run.case("all", list(range(8)), o, sig, sname)
                print("  %-42s %-16s  %s" % (m["order"], m["signal"], fmt(m)))

    # --------------------------------------------------- 5. the bypass toggle --
    print()
    print("=== 5. does a block keep its settings across a bypass? ===")
    before = len(behaviour_fails)
    check_toggle(run, behaviour_fails)
    for f in behaviour_fails[before:]:
        print("  FAIL %-6s %-16s  %s" % f)
    if len(behaviour_fails) == before:
        print("  every block keeps its knobs and its model across a bypass")

    # ---------------------------------------------------------- the report --
    fails = [m for m in report if not m["ok"]]
    print()
    print("=== summary ===")
    print("  %d cases, %d failed the sanity check" % (len(report), len(fails)))
    if fails:
        print()
        for m in fails:
            print("  FAIL %-16s %-16s order: %-30s  %s"
                  % (m["slots"], m["signal"], m["order"], m["why"]))
    if behaviour_fails:
        print("  %d failed the behaviour check" % len(behaviour_fails))
        for slot, sname, c in behaviour_fails:
            print("  FAIL %-6s %-16s  %s" % (slot, sname, c))
    if not fails and not behaviour_fails:
        print("  every effect, every pair, every order: sane signal, and every")
        print("  effect does the job it is supposed to do.")
        return 0
    return 1


if __name__ == "__main__":
    sys.exit(main())

# SPDX-License-Identifier: GPL-3.0-only
# Copyright (C) 2026 The Onyx Authors (@28spawn)
