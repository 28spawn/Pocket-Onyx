/* SPDX-License-Identifier: GPL-3.0-only */
/* Copyright (C) 2026 The Onyx Authors (@28spawn) */
/* custom-fw/src/dsp/tuner.c — Pocket Amp Chromatic Tuner DSP with Ref 430-450Hz & Bypass Volume
 *
 * PITCH DETECTION (rewritten 2026-11-04; the first version copied the stock's zero-crossing timing on a 938 Hz one-pole low-pass, which jittered and took an overtone's crossings for the
 * fundamental, so the reading wandered and a string had to be played again and again): a YIN detector (de Cheveigne & Kawahara 2002: the difference function d(tau), its cumulative mean
 * normalisation, the first dip under a threshold, parabolic interpolation) on the input decimated by 4 to 11,025 Hz, window 512 samples, periods 11..190 samples (1,002 Hz .. 58 Hz: a seven-string's
 * low B is 61.7 Hz). The first dip, not the deepest one, is what keeps the FUNDAMENTAL when an overtone is stronger. It runs in slices of 12 lags per audio block (about 6 k multiply-adds a
 * block; a detection every ~70 ms), so the interrupt stays light, and the result goes through
 *   - a median of the last five detections (a single wrong one - an octave slip, a pluck transient - never reaches the screen),
 *   - a lock only when two detections in a row agree within 3 %,
 *   - the 62-cent note hysteresis (a string held between two notes does not flicker the name),
 *   - a HOLD: when the signal ends (the string has died away, the pick is off) the last reading stays on the screen for 4 s, marked `held`, so a string can be played once and the peg turned
 *     while reading it.
 * Frequencies are floats (the logarithm for cents and notes is computed once per detection, never per sample). */

#include <math.h>
#include "pa_ui.h"
#include "pa_config.h"
#include "fixedpoint.h"

#define TUNER_DEC      4u
#define TUNER_TAU_MIN  11u
#define TUNER_TAU_MAX  190u
#define TUNER_WIN      512u
#define TUNER_NEED     (TUNER_WIN + TUNER_TAU_MAX)
#define TUNER_SLICE    12u
#define TUNER_GATE_Q21 500        /* a raw block peak under this (-72 dBFS) is "no signal" */
#define TUNER_HOLD_BLOCKS 2756u   /* 4 s of 64-sample blocks at 44.1 kHz */
#define TUNER_YIN_THRESH 0.15f

static const float k_log2_to_semi = 12.0f;

void pa_tuner_init(pa_tuner_state_t *t) {
    if (!t) return;
    t->active           = 0;
    t->ref_a4_hz        = 440u; /* Manual Page 6: 430..450 Hz adjustable */
    t->bypass_volume    = 100u; /* Manual Page 6: Volume 0..100 in Bypass status. It was 0 and nothing ever set it, so LIVE played silence exactly like MUTED (2026-11-04, owner) */
    t->input_level_dots = 0u;
    t->signal_locked    = 0u;
    t->held             = 0u;
    t->note_index       = 4u;
    t->octave           = 2;
    t->cents            = 0;
    t->cents_q4         = 0;
    t->cents_disp       = 0;
    t->freq_tenths_hz   = 824u;
    t->confidence       = 0;
    t->hold_blocks      = 0u;
    t->dec_acc          = 0;
    t->dec_n            = 0u;
    t->ring_w           = 0u;
    t->det_new          = 0u;
    t->det_state        = 0u;
    t->det_tau          = TUNER_TAU_MIN;
    t->hist_n           = 0u;
    t->hist_i           = 0u;
    t->quiet_blocks     = 0u;
    for (uint32_t i = 0; i < PA_TUNER_RING; i++) t->ring[i] = 0;
}

/* ---- from a frequency to a note, cents and the lock state ---- */
static void tuner_show(pa_tuner_state_t *t, float f_hz) {
    const float ref = (t->ref_a4_hz >= 430u && t->ref_a4_hz <= 450u) ? (float)t->ref_a4_hz : 440.0f;
    /* semitones from A4: 12 log2(f / ref) */
    const float semis = k_log2_to_semi * (logf(f_hz / ref) * 1.44269504f);
    int midi = 69 + (int)floorf(semis + 0.5f);
    float cents = (semis - (float)(midi - 69)) * 100.0f;
    /* hysteresis: a note already shown is kept until the pitch is clearly (62 cents, 12 past the halfway point) toward the next one */
    if (t->signal_locked) {
        const int cur_midi = ((int)t->octave + 1) * 12 + (int)t->note_index;
        if (midi != cur_midi) {
            const float c_cur = (semis - (float)(cur_midi - 69)) * 100.0f;
            if (c_cur > -62.0f && c_cur < 62.0f) { midi = cur_midi; cents = c_cur; }
        }
    }
    if (cents >  50.0f) cents =  50.0f;
    if (cents < -50.0f) cents = -50.0f;
    if (midi < 0) midi = 0;
    t->note_index    = (uint8_t)(midi % 12);
    t->octave        = (int8_t)(midi / 12 - 1);
    t->cents         = (int8_t)(cents >= 0.0f ? cents + 0.5f : cents - 0.5f);
    t->freq_tenths_hz = (uint16_t)(f_hz * 10.0f + 0.5f);
    t->signal_locked = 1u;
    t->held          = 0u;
}

static float median5(const float *h, uint8_t n) {
    float a[5];
    for (uint8_t i = 0; i < n; i++) a[i] = h[i];
    for (uint8_t i = 1; i < n; i++) {                     /* insertion sort */
        float v = a[i]; int j = (int)i - 1;
        while (j >= 0 && a[j] > v) { a[j + 1] = a[j]; j--; }
        a[j + 1] = v;
    }
    return a[n / 2u];
}

/* a detection finished: f_hz > 0 is a voiced result, 0 means the window was not periodic */
static void tuner_commit(pa_tuner_state_t *t, float f_hz) {
    if (f_hz <= 0.0f) {
        if (t->confidence > 0u) t->confidence--;
        return;
    }
    t->hist[t->hist_i] = f_hz;
    t->hist_i = (uint8_t)((t->hist_i + 1u) % 5u);
    if (t->hist_n < 5u) t->hist_n++;
    /* two detections in a row that agree (3 %) before a lock; once locked a wrong one is outvoted by the median */
    const uint8_t prev_i = (uint8_t)((t->hist_i + 4u) % 5u), prev2_i = (uint8_t)((t->hist_i + 3u) % 5u);
    int agree = 0;
    if (t->hist_n >= 2u) {
        const float a = t->hist[prev_i], b = t->hist[prev2_i];
        const float r = a > b ? a / b : b / a;
        agree = (r < 1.03f);
    }
    if (agree) { if (t->confidence < 32u) t->confidence++; } else if (t->confidence > 0u) t->confidence = (t->confidence > 1u) ? 1u : 0u;
    if (t->confidence >= 2u || (t->signal_locked && t->confidence >= 1u)) {
        const float f = median5(t->hist, t->hist_n);
        tuner_show(t, f);
    }
}

/* ---- the YIN detector, one slice of lags per block ---- */
static inline int32_t ring_at(const pa_tuner_state_t *t, uint32_t i) { return t->ring[i & (PA_TUNER_RING - 1u)]; }

static void tuner_det_start(pa_tuner_state_t *t) {
    t->det_w0 = (uint32_t)t->ring_w;
    const uint32_t base = t->det_w0 - TUNER_NEED;
    int32_t pk = 1;
    for (uint32_t i = 0; i < TUNER_NEED; i++) { int32_t v = ring_at(t, base + i); if (v < 0) v = -v; if (v > pk) pk = v; }
    /* scale the window so its peak sits in 512..1023: the squared differences then fit 32 bits over 512 terms */
    int sh = 0;
    while ((pk >> sh) > 1023 && sh < 10) sh++;
    int lsh = 0;
    while (sh == 0 && (pk << (lsh + 1)) <= 1023 && lsh < 4) lsh++;
    t->det_rsh = (int8_t)sh; t->det_lsh = (int8_t)lsh;
    t->det_tau = 1u;                                       /* d(1) .. d(TAU_MIN - 1) are needed for the cumulative mean: start at 1 */
    t->det_state = 1u;
}

static float tuner_det_finish(pa_tuner_state_t *t) {
    /* d' (cumulative mean normalised) and the first dip under the threshold */
    float run = 0.0f;
    float best_v = 1e9f; uint32_t best_tau = 0u;
    uint32_t pick = 0u;
    float dn[TUNER_TAU_MAX + 2u];
    for (uint32_t tau = 1u; tau <= TUNER_TAU_MAX; tau++) {
        run += (float)t->det_d[tau];
        dn[tau] = (run > 0.0f) ? (float)t->det_d[tau] * (float)tau / run : 1.0f;
    }
    for (uint32_t tau = TUNER_TAU_MIN; tau <= TUNER_TAU_MAX; tau++) {
        if (dn[tau] < best_v) { best_v = dn[tau]; best_tau = tau; }
    }
    for (uint32_t tau = TUNER_TAU_MIN; tau <= TUNER_TAU_MAX - 1u; tau++) {
        if (dn[tau] < TUNER_YIN_THRESH) {
            while (tau + 1u <= TUNER_TAU_MAX && dn[tau + 1u] < dn[tau]) tau++;      /* down to the bottom of that dip */
            pick = tau;
            break;
        }
    }
    if (pick == 0u) {
        if (best_v < 0.35f) pick = best_tau;               /* no dip under the threshold: the deepest one if it is clear enough */
        else return 0.0f;
    }
    if (pick <= TUNER_TAU_MIN || pick >= TUNER_TAU_MAX) return 0.0f;     /* at the edge of the range: no neighbour for the interpolation */
    /* parabolic interpolation over d(tau - 1), d(tau), d(tau + 1) */
    const float y0 = (float)t->det_d[pick - 1u], y1 = (float)t->det_d[pick], y2 = (float)t->det_d[pick + 1u];
    const float den = y0 - 2.0f * y1 + y2;
    float tau_f = (float)pick;
    if (den > 1e-9f) {
        float off = 0.5f * (y0 - y2) / den;
        if (off > 1.0f) off = 1.0f;
        if (off < -1.0f) off = -1.0f;
        tau_f += off;
    }
    return (44100.0f / (float)TUNER_DEC) / tau_f;
}

static void tuner_det_slice(pa_tuner_state_t *t) {
    const uint32_t base = t->det_w0 - TUNER_NEED;
    const int rsh = t->det_rsh, lsh = t->det_lsh;
    uint32_t tau = t->det_tau;
    for (uint32_t n = 0; n < TUNER_SLICE && tau <= TUNER_TAU_MAX; n++, tau++) {
        uint32_t acc = 0u;
        for (uint32_t j = 0; j < TUNER_WIN; j++) {
            const int32_t a = (int32_t)((uint32_t)ring_at(t, base + j) << lsh) >> rsh;       /* the shift up is unsigned: shifting a negative value left is undefined */
            const int32_t b = (int32_t)((uint32_t)ring_at(t, base + j + tau) << lsh) >> rsh;
            const int32_t d = a - b;
            acc += (uint32_t)(d * d);
        }
        t->det_d[tau] = acc;
    }
    t->det_tau = (uint16_t)tau;
    if (tau > TUNER_TAU_MAX) {
        const float f = tuner_det_finish(t);
        t->det_state = 0u;
        tuner_commit(t, f);
    }
}

void pa_tuner_process_block(pa_tuner_state_t *t, int32_t *buf, int n) {
    if (!t || !t->active) return;

    int32_t peak = 0;
    int32_t bypass_gain_q21 = (int32_t)(((uint64_t)t->bypass_volume * PA_Q21_ONE) / 100u);

    for (int i = 0; i < n; i++) {
        int32_t x = buf[i];
        int32_t abs_x = (x < 0) ? -x : x;
        if (abs_x > peak) peak = abs_x;

        /* decimate by 4: the mean of four samples (a crude but sufficient low-pass for pitch), 16-bit */
        t->dec_acc += x;
        if (++t->dec_n >= TUNER_DEC) {
            int32_t v = t->dec_acc >> 8;                     /* four Q21 samples summed: full scale 2^23, >> 8 = 2^15 */
            if (v > 32767) v = 32767;
            if (v < -32768) v = -32768;
            t->ring[t->ring_w & (PA_TUNER_RING - 1u)] = (int16_t)v;
            t->ring_w++;
            t->det_new++;
            t->dec_acc = 0; t->dec_n = 0u;
        }
        /* Manual Page 6: apply Bypass Volume (0 = mute, 100 = unity dry guitar pass-through) */
        buf[i] = q21_mul(x, bypass_gain_q21);
    }

    /* Compute 0..7 dots for IN jack input volume real-time display bar (Manual Page 6) */
    uint32_t dots = (uint32_t)peak >> 16;
    if (dots > 7u) dots = 7u;
    t->input_level_dots = (uint8_t)dots;

    const int active_sig = (peak >= TUNER_GATE_Q21);
    if (active_sig) { t->quiet_blocks = 0u; t->held = 0u; t->hold_blocks = 0u; }
    else if (t->quiet_blocks < 65535u) t->quiet_blocks++;

    /* the detector: only on a live signal; a detection starts when 512 new decimated samples have come in (and the window is full once) */
    if (active_sig) {
        if (t->det_state == 0u && t->det_new >= TUNER_WIN && t->ring_w >= TUNER_NEED) {
            t->det_new = 0u;
            tuner_det_start(t);
        }
        if (t->det_state == 1u) tuner_det_slice(t);
    } else {
        t->det_state = 0u;                                   /* a detection over a dying signal would read noise: drop it */
        if (t->det_new > TUNER_WIN) t->det_new = TUNER_WIN;
    }

    /* the end of the signal: ~100 ms of quiet and the reading goes to HOLD (it stays on the screen, marked held) for 4 s, then the lock is dropped */
    if (!active_sig && t->quiet_blocks > 70u && t->signal_locked) {
        if (!t->held) { t->held = 1u; t->hold_blocks = 0u; }
        if (t->hold_blocks < 65535u) t->hold_blocks++;
        if (t->hold_blocks >= TUNER_HOLD_BLOCKS) { t->signal_locked = 0u; t->held = 0u; t->confidence = 0u; t->hist_n = 0u; t->hist_i = 0u; }
    } else if (!t->signal_locked && !active_sig && t->quiet_blocks > 70u) {
        t->confidence = 0u; t->hist_n = 0u; t->hist_i = 0u;
    }
}

/* The needle the screen draws. The measurement is already steadied by the median, but a few cents of detection-to-detection movement remain (2 pixels a cent on the scale). This runs once per
 * UI tick: a move of more than 10 cents is real (the string is being tuned) and is followed at once, so the response time does not suffer; under that it is a one-pole filter, 1/4 a tick (about
 * 55 ms), and a first lock or a new note starts from the measurement. */
void pa_tuner_ui_tick(pa_tuner_state_t *t) {
    if (!t) return;
    static uint8_t s_prev_locked, s_prev_note;
    const int32_t dq = (int32_t)t->cents * 16 - t->cents_q4;
    if (!t->signal_locked || !s_prev_locked || t->note_index != s_prev_note || dq > 160 || dq < -160) t->cents_q4 = (int16_t)((int32_t)t->cents * 16);
    else t->cents_q4 = (int16_t)(t->cents_q4 + (dq >> 2));
    s_prev_locked = t->signal_locked; s_prev_note = t->note_index;
    t->cents_disp = (int8_t)((t->cents_q4 + (t->cents_q4 >= 0 ? 8 : -8)) / 16);
}
