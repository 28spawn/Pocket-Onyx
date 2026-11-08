/* SPDX-License-Identifier: GPL-3.0-only */
/* Copyright (C) 2026 The Onyx Authors (@28spawn) */
/* custom-fw/src/fx/fx_dynamics.c — Pocket Amp "6+1" FX Module (Noise Gate++ / AI Gate MS + Boost / Compress)
 *
 * Matches User Manual Pages 7–8 ("FX module can select Noise Gate or NG+Boost or NG+Compress,
 * three collocation ways, you can use 6+1 effect at the same time"):
 *   - The AI Noise Gate runs FIRST whenever `Gate > 0` (`thresh_open > 0`), even when `Boost` or
 *     `Compress` is selected! Setting `Gate = 0` turns off the Noise Gate.
 *   - Mode 3 (`DYN_TYPE_AI_GATE_MS`) adds noise-floor tracking: while the gate is shut it learns
 *     the floor (hiss, hum), and the opening threshold stays a margin above it (`Bias`).
 *
 * The gate (rewritten 2026-10-22, measured by custom-fw/test/test_gate.c):
 *   detector  2nd-order 50 Hz high-pass (DC, handling rumble) -> |x| -> peak follower,
 *             instant attack, 25 ms release. Flat from low E to 5 kHz. The old detector put a
 *             "120 Hz" one-pole high-pass that was really ~740 Hz in front of |x| + 2|dx|, so it
 *             was 24.5 dB more sensitive at 5 kHz than at 82 Hz: hiss held the gate open and low
 *             notes were chopped. (Mains hum cannot be filtered out of the detector without
 *             taking the low E with it; AI GATE MS's floor tracking is what rejects hum.)
 *   decision  open at THRESH, close 6 dB lower after a 50 ms hold - real hysteresis (it was
 *             1.2 dB, and a decaying note chattered).
 *   gain      exactly 1.0 while open (a held note passes bit-for-bit; the old gain followed the
 *             detector and distorted low notes: 6.6 % THD at low E), exponential fades, never
 *             above unity (the old "soft knee" reached 1.22x, a +1.7 dB bump while closing).
 *   knobs     GATE / AI GATE MS: THRESH (same knob law as before), RATIO = release 20 ms .. 1 s
 *             (it was inverted, 137 -> 73 ms), ATTACK = gate attack 0.1 .. 5 ms,
 *             LEVEL = output volume 0..1.0 (100 % = unity; it did nothing on these types).
 *             BOOST+GATE: RATIO = gate release, ATTACK unused by the gate (fast).
 *             COMP+GATE: RATIO and ATTACK belong to the compressor only (the shared attack
 *             used to make the GATE open in 53 ms at ATTACK 100 %); the gate uses fast fixed
 *             times.
 *
 * The compressor (rewritten 2026-10-23, measured by custom-fw/test/test_comp.c). The old one
 * computed "reduction = over / (env + thr) * RATIO", which is not a ratio: 1.2:1 at RATIO 50 %,
 * 5.8:1 at 100 %; RATIO also set its release; its 1 ms detector rode a low E's waveform
 * (3.4 % THD); there was no makeup. Now, in float, per 8-sample step:
 *   detector  the peak over the last 11.6..23 ms (a running max of two 512-sample windows):
 *             steady on a held low E (a peak lands in every window), so no ripple
 *   computer  dB domain, a real RATIO 1:1 .. 20:1 (20^knob), 6 dB soft knee
 *   smoothing on the gain reduction in dB: ATTACK 0.5 .. 30 ms (to 90 %), release follows
 *             ATTACK (80 ms + 12 x attack, to 90 %, after the detector's hold) - never RATIO
 *   makeup    automatic: a note peaking at -15 dBFS (-18 dBFS rms) keeps its level (<= +20 dB)
 *   LEVEL     output volume 0..1.0 on top (100 % = unity)
 * COMP+GATE: threshold -30 dBFS; knob 0 is the gate's. COMP (type 4, no gate): knob 0 is the
 * compressor's THRESH, -50 .. -10 dBFS.
 * The gate runs in float: the AC791N has a single-precision FPU, and a float biquad is ~10x
 * cheaper on it than the saturating Q21 form.
 */
#include <math.h>

#include "pa_fx.h"
#include "pa_config.h"
#include "../dsp/fixedpoint.h"

typedef enum {
    DYN_TYPE_GATE_PLUS  = 0, /* 1. Noise Gate++ */
    DYN_TYPE_BOOST      = 1, /* 2. Boost (with stackable Noise Gate when Gate > 0) */
    DYN_TYPE_COMPRESS   = 2, /* 3. Compress (with stackable Noise Gate when Gate > 0) */
    DYN_TYPE_AI_GATE_MS = 3, /* 4. AI Gate MS (Gate threshold + Bias trend prediction) */
    DYN_TYPE_COMP       = 4, /* 5. Compressor alone, with its own THRESH (no gate) */
    DYN_TYPE_COUNT      = 5
} dyn_type_t;

enum {
    DYN_PARAM_TYPE    = 0,
    DYN_PARAM_GATE    = 1, /* Gate threshold / intensity (0 = Noise Gate OFF, >0 = stacked ON) */
    DYN_PARAM_DECAY   = 2, /* Compress Sustain / Gate release */
    DYN_PARAM_ATTACK  = 3, /* Compress Attack (larger value = slower attack / more prominent sound-head, Page 8) */
    DYN_PARAM_LEVEL   = 4, /* Boost Gain / Compress Level */
    DYN_PARAM_BIAS    = 5, /* AI Gate MS "Bias" lead prediction (Page 8) */
    DYN_PARAM_RANGE   = 6, /* Expander range limit (0 = full mute / -100 dB, >0 = -60 .. -6 dB floor) */
    DYN_PARAM_MAKEUP  = 7, /* Manual makeup gain (-12 .. +24 dB) */
    DYN_PARAM_AUTO_MAKEUP = 8 /* 1 = auto makeup (default), 0 = manual makeup */
};

typedef struct {
    dyn_type_t type;
    int32_t    thresh_open;
    int32_t    thresh_close;
    int32_t    sustain;
    int32_t    level;
    int32_t    ai_bias_lead;  /* Manual Page 8: Bias trend lead factor in Q21 */

    uint32_t   hold_counter;
    uint32_t   hold_samples;
    uint8_t    gate_open;

    /* the gate (float, see the header) */
    float      g_hb0, g_hb1, g_hb2, g_ha1, g_ha2;   /* 50 Hz 2nd-order high-pass (sidechain) */
    float      g_hx1, g_hx2, g_hy1, g_hy2;
    float      g_env;            /* peak follower of the sidechain */
    float      g_env_rel;        /* per-sample release coefficient of the follower (25 ms) */
    float      g_open_thr;       /* opening threshold (linear peak); 0 = gate off */
    float      g_gain;           /* applied gain, 0..1 */
    float      g_att;            /* per-sample attack coefficient */
    float      g_rel;            /* per-sample release coefficient */
    float      g_floor;          /* AI GATE MS: learned noise floor (peak) */
    uint16_t   g_fast;           /* samples left, after a reset, in which the floor learns at the fast rate both ways: it restarted at 0 and took 0.8 s to rise to the input's noise (pack 186) */
    float      g_margin;         /* AI GATE MS: threshold margin over the floor (x) */
    float      g_range_floor;    /* minimum gain floor when shut (0.0 = full gate -100 dB, >0 = expander floor) */
    float      g_range_db;       /* expander range in dB (-100 dB mute or -60 .. -6 dB) */
    float      g_knob_ratio;     /* last RATIO and ATTACK knob values, 0..1, so a type change */
    float      g_knob_attack;    /* can re-derive the gate times */
    float      g_knob_thresh;

    /* the compressor (float, see the header) */
    float      c_thr_db;         /* threshold, dBFS peak */
    float      c_slope;          /* 1 - 1/ratio */
    float      c_makeup;         /* active makeup gain, linear */
    uint8_t    c_auto_makeup;    /* 1 = automatic makeup (default), 0 = manual makeup */
    float      c_manual_makeup_db; /* manual makeup gain in dB */
    float      c_att, c_rel;     /* per-step (8 samples) smoothing coefficients */
    float      c_rms_sq;         /* smooth hybrid detector: running mean-square energy */
    float      c_rms_coeff;      /* RMS integration 1-pole coefficient */
    float      c_peak;           /* smooth hybrid detector: fast peak follower */
    float      c_peak_rel;       /* peak follower decay coefficient */
    float      c_win_max, c_prev_max;   /* legacy window fields retained for struct compatibility */
    uint32_t   c_win_n;
    float      c_gr_db;          /* smoothed gain reduction, dB (<= 0) */
    float      c_gain;           /* gain applied at the end of the last step (incl. makeup) */
} dyn_state_t;

#define GATE_FS          44100.0f
#define GATE_HOLD_S      0.050f      /* hold after the detector drops below the close threshold */
#define GATE_CLOSE_RATIO 0.5f        /* close 6 dB under open */
#define GATE_DET_REL_S   0.025f      /* detector release */
#define GATE_FAST_ATT_S  0.0003f     /* fixed attack for BOOST+GATE / COMP+GATE */
#define GATE_FIXED_REL_S 0.080f      /* fixed release (to -40 dB) for COMP+GATE */

#define COMP_STEP        8           /* control rate: the gain computer runs every 8 samples */
#define COMP_WIN         512         /* detector window (11.6 ms); the peak spans 2 windows */
#define COMP_KNEE_DB     6.0f
#define COMP_REF_DB      (-15.0f)    /* makeup: a note peaking here keeps its level */
#define COMP_MAKEUP_MAX  20.0f
#define COMP_FIXED_THR   (-30.0f)    /* COMP+GATE threshold (knob 0 is the gate's) */

static float clamp01(float v) { return v < 0.0f ? 0.0f : (v > 1.0f ? 1.0f : v); }

/* the knob laws (shared with the screen through pa_dyn_display) */
static float comp_ratio(float k)    { return powf(20.0f, clamp01(k)); }            /* 1 .. 20 :1 */
static float comp_att90_ms(float k) { return 0.5f * powf(60.0f, clamp01(k)); }     /* 0.5 .. 30 ms */
static float comp_rel90_ms(float k) { return 80.0f + 12.0f * comp_att90_ms(k); }   /* 86 .. 440 ms */
static float comp_thr_db(float k)   { return -50.0f + 40.0f * clamp01(k); }        /* COMP only */
static float gate_att_ms(float k)   { return 0.1f * powf(50.0f, clamp01(k)); }
static float gate_rel_ms(float k)   { return 20.0f * powf(50.0f, clamp01(k)); }

/* static gain (dB, <= 0) for a peak level L (dBFS): threshold T, 6 dB soft knee */
static float comp_curve(float L, float T, float slope) {
    float o = L - T;
    if (2.0f * o <= -COMP_KNEE_DB) return 0.0f;
    if (2.0f * o >=  COMP_KNEE_DB) return -slope * o;
    o += 0.5f * COMP_KNEE_DB;
    return -slope * o * o / (2.0f * COMP_KNEE_DB);
}

/* per-sample one-pole coefficient for a time constant tau (seconds) */
static float tau_coeff(float tau_s) {
    if (tau_s < 1e-6f) return 1.0f;
    return 1.0f - expf(-1.0f / (tau_s * GATE_FS));
}

/* Re-derive the gate's attack/release from the knobs and the type. */
static void gate_times(dyn_state_t *s) {
    const int gate_knobs = (s->type == DYN_TYPE_GATE_PLUS || s->type == DYN_TYPE_AI_GATE_MS);
    /* ATTACK: 0.1 ms .. 5 ms (log) on the gate types, fixed fast otherwise */
    float att_s = gate_knobs ? 0.001f * gate_att_ms(s->g_knob_attack) : GATE_FAST_ATT_S;
    /* RATIO: release to -40 dB, 20 ms .. 1 s (log); COMP+GATE: fixed (the knob is the comp's) */
    float rel40 = (s->type == DYN_TYPE_COMPRESS) ? GATE_FIXED_REL_S : 0.001f * gate_rel_ms(s->g_knob_ratio);
    s->g_att = tau_coeff(att_s);
    s->g_rel = tau_coeff(rel40 / 4.60517f);       /* exp decay: -40 dB after 4.6 tau */

    /* the compressor: RATIO, ATTACK (release follows it), threshold, makeup */
    const float ratio = comp_ratio(s->g_knob_ratio);
    s->c_slope  = 1.0f - 1.0f / ratio;
    s->c_thr_db = (s->type == DYN_TYPE_COMP) ? comp_thr_db(s->g_knob_thresh) : COMP_FIXED_THR;
    {
        const float a90 = 0.001f * comp_att90_ms(s->g_knob_attack) / 2.302585f;   /* 90 % = 2.3 tau */
        const float r90 = 0.001f * comp_rel90_ms(s->g_knob_attack) / 2.302585f;
        s->c_att = expf(-(float)COMP_STEP / (a90 * GATE_FS));
        s->c_rel = expf(-(float)COMP_STEP / (r90 * GATE_FS));
    }
    /* Detector coefficients: ~8 ms RMS tau, ~6 ms peak decay */
    s->c_rms_coeff = 1.0f - expf(-(float)COMP_STEP / (0.008f * GATE_FS));
    s->c_peak_rel  = 1.0f - expf(-(float)COMP_STEP / (0.006f * GATE_FS));

    /* Makeup gain: automatic (-15 dBFS ref peak) or user-specified manual */
    if (s->c_auto_makeup) {
        float mk = -comp_curve(COMP_REF_DB, s->c_thr_db, s->c_slope);
        if (mk > COMP_MAKEUP_MAX) mk = COMP_MAKEUP_MAX;
        s->c_makeup = powf(10.0f, mk / 20.0f);
    } else {
        s->c_makeup = powf(10.0f, s->c_manual_makeup_db / 20.0f);
    }
}

extern void *pa_alloc(uint32_t);
extern void  pa_free(void *);


/* the compressor, after the gate (see the header) */
static void comp_run(dyn_state_t *s, int32_t *buf, int n) {
    const float lvl = (float)s->level * (1.0f / 2097152.0f);
    if (s->c_slope <= 0.0f && s->level == PA_Q21_ONE && (!s->c_auto_makeup ? (__builtin_fabsf(s->c_manual_makeup_db) < 0.01f) : 1)) return;      /* 1:1, unity: bit-exact */
    for (int i0 = 0; i0 + COMP_STEP <= n; i0 += COMP_STEP) {
        float pk = 0.0f;
        float sum_sq = 0.0f;
        for (int k = 0; k < COMP_STEP; k++) {
            float a = (float)buf[i0 + k] * (1.0f / 2097152.0f);
            float a_abs = a < 0.0f ? -a : a;
            if (a_abs > pk) pk = a_abs;
            sum_sq += a * a;
        }

        /* Smooth Peak/RMS Hybrid Detector:
         * 1. Continuous RMS energy follower (tau ~8 ms) provides ripple-free tracking of low frequencies (low-E 82 Hz).
         * 2. Leaky peak follower (tau ~6 ms) with instant attack captures fast transients and pick attack edges.
         * 3. Peak/RMS hybrid takes the envelope ceiling, preserving transients while eliminating LF waveform ripple. */
        float step_ms = sum_sq * (1.0f / (float)COMP_STEP);
        s->c_rms_sq += (step_ms - s->c_rms_sq) * s->c_rms_coeff;
        float rms = sqrtf(s->c_rms_sq) * 1.41421356f;

        if (pk > s->c_peak) {
            s->c_peak = pk;
        } else {
            s->c_peak += (pk - s->c_peak) * s->c_peak_rel;
        }

        float env = (s->c_peak > rms) ? s->c_peak : rms;

        const float L = 20.0f * log10f(env + 1e-9f);
        const float target = comp_curve(L, s->c_thr_db, s->c_slope);
        const float c = (target < s->c_gr_db) ? s->c_att : s->c_rel;
        s->c_gr_db = target + (s->c_gr_db - target) * c;

        const float g1 = powf(10.0f, s->c_gr_db * 0.05f) * s->c_makeup * lvl;
        const float g0 = s->c_gain, dg = (g1 - g0) * (1.0f / COMP_STEP);
        for (int k = 0; k < COMP_STEP; k++) {
            float y = (float)buf[i0 + k] * (g0 + dg * (float)(k + 1));
            if (y >  2147483520.0f) y =  2147483520.0f;
            if (y < -2147483520.0f) y = -2147483520.0f;
            buf[i0 + k] = (int32_t)y;
        }
        s->c_gain = g1;
    }
}

static void dynamics_run(fx_node_t *nd, int32_t *buf, int n) {
    dyn_state_t *s = (dyn_state_t *)nd->state;
    if (!s) return;

    for (int i = 0; i < n; i++) {
        int32_t x = buf[i];

        int32_t y = x;

        /* Stage 1 ("+1" of 6+1): the noise gate (Manual Pages 7-8) - see the header. */
        if (s->g_open_thr > 0.0f && s->type != DYN_TYPE_COMP) {
            const float xf = (float)x * (1.0f / 2097152.0f);
            /* sidechain: 50 Hz high-pass, then a peak follower */
            float hp = s->g_hb0 * xf + s->g_hb1 * s->g_hx1 + s->g_hb2 * s->g_hx2
                     - s->g_ha1 * s->g_hy1 - s->g_ha2 * s->g_hy2;
            s->g_hx2 = s->g_hx1; s->g_hx1 = xf;
            s->g_hy2 = s->g_hy1; s->g_hy1 = hp;
            float a = hp < 0.0f ? -hp : hp;
            s->g_env = (a > s->g_env) ? a : s->g_env + (a - s->g_env) * s->g_env_rel;

            float open_thr = s->g_open_thr;
            if (s->type == DYN_TYPE_AI_GATE_MS) {
                /* learn the floor only while shut: slow to rise (0.8 s), quick to fall (50 ms) */
                if (!s->gate_open) {
                    const float rise = s->g_fast ? 4.534e-4f : 2.834e-5f;
                    if (s->g_fast) s->g_fast--;
                    s->g_floor += (s->g_env - s->g_floor) * ((s->g_env > s->g_floor) ? rise : 4.534e-4f);
                }
                float adaptive = s->g_floor * s->g_margin;
                if (adaptive > open_thr) open_thr = adaptive;
            }
            const float close_thr = open_thr * GATE_CLOSE_RATIO;

            if (s->g_env >= open_thr) {
                s->gate_open = 1;
                s->hold_counter = s->hold_samples;
            } else if (s->gate_open) {
                if (s->g_env >= close_thr) {
                    s->hold_counter = s->hold_samples;
                } else if (s->hold_counter > 0) {
                    s->hold_counter--;
                } else {
                    s->gate_open = 0;
                }
            }

            if (s->gate_open) {
                s->g_gain += (1.0f - s->g_gain) * s->g_att;
                if (s->g_gain > 0.99999f) s->g_gain = 1.0f;
            } else {
                /* Expander range limit: smoothly decay down to s->g_range_floor instead of hard cut */
                s->g_gain -= (s->g_gain - s->g_range_floor) * s->g_rel;
                if (s->g_gain < s->g_range_floor + 1e-5f) s->g_gain = s->g_range_floor;
            }

            if (s->g_gain < 1.0f)
                y = (int32_t)((float)y * s->g_gain);             /* |gain| <= 1: no overflow */
        }

        /* Stage 2: Stacked Boost or Compress (when selected) */
        if (s->type == DYN_TYPE_BOOST) {
            y = sat32(q21_mul(y, s->level));
        } else if (s->type == DYN_TYPE_GATE_PLUS || s->type == DYN_TYPE_AI_GATE_MS) {
            /* LEVEL = output volume, 0..100 % -> 0..1.0 (100 % = unity, the preset default).
             * At unity the sample is left alone, so an open gate stays bit-exact. */
            if (s->level != PA_Q21_ONE)
                y = sat32(q21_mul(y, s->level));
        }

        buf[i] = sat32(y);
    }

    if (s->type == DYN_TYPE_COMPRESS || s->type == DYN_TYPE_COMP) comp_run(s, buf, n);
}

static void dynamics_create(fx_node_t *nd) {
    dyn_state_t *s = (dyn_state_t *)pa_alloc(sizeof(dyn_state_t));
    if (s) {
        s->type          = DYN_TYPE_AI_GATE_MS;
        s->thresh_open   = 4200;       /* ~-54 dBFS default studio gate threshold */
        s->thresh_close  = (4200 * 7) >> 3;
        s->sustain       = 1468006;
        s->level         = PA_Q21_ONE;
        s->ai_bias_lead  = 524288;
        s->hold_counter  = 0;
        s->hold_samples  = (uint32_t)(GATE_HOLD_S * GATE_FS);   /* 50 ms */
        s->gate_open     = 1;          /* Open by default */

        /* the gate: 50 Hz 2nd-order Butterworth high-pass for the sidechain (RBJ) */
        {
            const float w = 6.28318530718f * 50.0f / GATE_FS, c = cosf(w);
            const float al = sinf(w) / (2.0f * 0.70710678f), a0 = 1.0f + al;
            s->g_hb0 = (1.0f + c) / 2.0f / a0;
            s->g_hb1 = -(1.0f + c) / a0;
            s->g_hb2 = (1.0f + c) / 2.0f / a0;
            s->g_ha1 = -2.0f * c / a0;
            s->g_ha2 = (1.0f - al) / a0;
        }
        s->g_hx1 = s->g_hx2 = s->g_hy1 = s->g_hy2 = 0.0f;
        s->g_env        = 0.0f;
        s->g_env_rel    = tau_coeff(GATE_DET_REL_S);
        s->g_open_thr   = 4200.0f / 2097152.0f;      /* the old default, ~-54 dBFS */
        s->g_gain       = 0.0f;                       /* created SHUT (as after a reset): an open gate passed the input's noise, amplified by the amp, until it closed */
        s->g_floor      = 0.0f;
        s->g_margin     = 2.0f + 2.0f * 0.25f;       /* bias 0.25 -> +8 dB over the floor */
        s->g_knob_ratio = 0.3f;
        s->g_knob_attack= 0.0f;
        s->g_knob_thresh= 0.5f;
        s->g_range_floor = 0.0f;
        s->g_range_db    = -100.0f;
        s->c_rms_sq      = 0.0f;
        s->c_peak        = 0.0f;
        s->c_auto_makeup = 1;
        s->c_manual_makeup_db = 0.0f;
        s->c_win_max = s->c_prev_max = 0.0f;
        s->c_win_n   = 0;
        s->c_gr_db   = 0.0f;
        gate_times(s);
        s->c_gain    = s->c_makeup;
    }
    nd->state = s;
}

static void dynamics_destroy(fx_node_t *nd) {
    if (nd->state) {
        pa_free(nd->state);
        nd->state = 0;
    }
}

static void dynamics_set_param(fx_node_t *nd, int id, int32_t val) {
    dyn_state_t *s = (dyn_state_t *)nd->state;
    if (!s) return;
    switch (id) {
        case 0: /* Knob 0: THRESH / GATE (0 = Noise Gate OFF, >0 = stacked ON) */
            if (val <= 0) {
                s->thresh_open  = 0;
                s->thresh_close = 0;
            } else {
                /* Calibrated logarithmic threshold mapping from -68 dBFS to -24 dBFS */
                int64_t v = val;
                int32_t t = (int32_t)(800 + ((v * v) >> 25) + ((v * 65536) >> 21));
                s->thresh_open  = t;
                s->thresh_close = (t * 7) >> 3;
            }
            /* the gate reads the same threshold, as a linear peak level */
            s->g_open_thr = (float)s->thresh_open * (1.0f / 2097152.0f);
            /* COMP: the same knob is the compressor's THRESH */
            s->g_knob_thresh = clamp01((float)val * (1.0f / 2097152.0f));
            gate_times(s);
            break;
        case 1: /* Knob 1: RATIO / DECAY / SUSTAIN */
            s->sustain       = val;
            s->g_knob_ratio  = clamp01((float)val * (1.0f / 2097152.0f));
            gate_times(s);
            break;
        case 2: /* Knob 2: ATTACK */
            /* Manual Page 8: "larger value = slower compression starting time" */
            s->g_knob_attack = clamp01((float)val * (1.0f / 2097152.0f));
            gate_times(s);
            break;
        case 3: /* Knob 3: LEVEL */
            s->level         = val;
            break;
        case 4: /* LEVEL alias */
            s->level         = val;
            break;
        case 5: /* Bias: AI GATE MS margin over the learned floor, 0..1 -> +6..+12 dB */
            s->ai_bias_lead  = val;
            {
                float b = (float)val * (1.0f / 2097152.0f);
                if (b < 0.0f) b = 0.0f;
                if (b > 1.0f) b = 1.0f;
                s->g_margin = 2.0f + 2.0f * b;
            }
            break;
        case 6: /* Knob 6 / Param 6: Expander Range Limit (0 = full mute / -100 dB, >0 = -60 dB .. -6 dB floor) */
            if (val <= 0) {
                s->g_range_floor = 0.0f;
                s->g_range_db    = -100.0f;
            } else {
                float k = clamp01((float)val * (1.0f / 2097152.0f));
                s->g_range_db    = -60.0f + 54.0f * k; /* -60 dB to -6 dB */
                s->g_range_floor = powf(10.0f, s->g_range_db / 20.0f);
            }
            break;
        case 7: /* Knob 7 / Param 7: Manual Makeup Gain (val < 0: auto; val >= 0: manual -12 dB .. +24 dB) */
            if (val < 0) {
                s->c_auto_makeup = 1;
            } else {
                s->c_auto_makeup = 0;
                float k = clamp01((float)val * (1.0f / 2097152.0f));
                s->c_manual_makeup_db = -12.0f + 36.0f * k; /* -12 dB to +24 dB */
            }
            gate_times(s);
            s->c_gain = s->c_makeup * (float)s->level * (1.0f / 2097152.0f);
            break;
        case 8: /* Param 8: Auto Makeup Toggle (1 = auto, 0 = manual) */
            s->c_auto_makeup = (val != 0) ? 1 : 0;
            gate_times(s);
            s->c_gain = s->c_makeup * (float)s->level * (1.0f / 2097152.0f);
            break;
        case 10: /* Explicit Model Type selection from UI dropdown */
            {
                dyn_type_t nt = (val >= 0 && val < DYN_TYPE_COUNT) ? (dyn_type_t)val : DYN_TYPE_GATE_PLUS;
                if (nt != s->type) {            /* the compressor starts from rest */
                    s->c_win_max = s->c_prev_max = 0.0f;
                    s->c_win_n = 0;
                    s->c_gr_db = 0.0f;
                }
                s->type = nt;
                gate_times(s);
                s->c_gain = s->c_makeup * (float)s->level * (1.0f / 2097152.0f);
            }
            break;
        default: break;
    }
}

/* What the DYN knobs mean in real units, for the screen (the same laws as above).
 * *unit: PA_DYN_U_DB (0.1 dB), PA_DYN_U_RATIO (0.1, "x:1"), PA_DYN_U_MS (0.1 ms),
 * PA_DYN_U_OFF (the gate is off); PA_DYN_U_NONE = no unit, show 0..10. */
int32_t pa_dyn_display(int type, int p, int32_t knob_q21, int *unit) {
    const float k = clamp01((float)knob_q21 * (1.0f / 2097152.0f));
    const int comp = (type == DYN_TYPE_COMPRESS || type == DYN_TYPE_COMP);
    *unit = PA_DYN_U_NONE;
    if (p == 0) {
        if (type == DYN_TYPE_COMP) { *unit = PA_DYN_U_DB; return (int32_t)lrintf(10.0f * comp_thr_db(k)); }
        if (knob_q21 <= 0) { *unit = PA_DYN_U_OFF; return 0; }
        {
            int64_t v = knob_q21;
            int32_t t = (int32_t)(800 + ((v * v) >> 25) + ((v * 65536) >> 21));
            *unit = PA_DYN_U_DB;
            return (int32_t)lrintf(200.0f * log10f((float)t / 2097152.0f));
        }
    }
    if (p == 1) {
        if (comp) { *unit = PA_DYN_U_RATIO; return (int32_t)lrintf(10.0f * comp_ratio(k)); }
        *unit = PA_DYN_U_MS;
        return (int32_t)lrintf(10.0f * gate_rel_ms(k));
    }
    if (p == 2) {
        if (comp) { *unit = PA_DYN_U_MS; return (int32_t)lrintf(10.0f * comp_att90_ms(k)); }
        if (type == DYN_TYPE_BOOST) return 0;
        *unit = PA_DYN_U_MS;
        return (int32_t)lrintf(10.0f * gate_att_ms(k));
    }
    if (p == 6) {
        *unit = PA_DYN_U_DB;
        if (knob_q21 <= 0) return -1000;
        float db_val = -60.0f + 54.0f * k;
        return (int32_t)lrintf(10.0f * db_val);
    }
    if (p == 7) {
        *unit = PA_DYN_U_DB;
        float db_val = -12.0f + 36.0f * k;
        return (int32_t)lrintf(10.0f * db_val);
    }
    return 0;
}

/* test hook: the compressor's current gain reduction (dB) */
float pa_dyn_comp_gr_db(const fx_node_t *nd) {
    const dyn_state_t *s = (const dyn_state_t *)nd->state;
    return s ? s->c_gr_db : 0.0f;
}

static void dynamics_reset(fx_node_t *nd) {
    dyn_state_t *s = (dyn_state_t *)nd->state;
    if (!s) return;
    s->hold_counter = 0;
    s->gate_open    = 0;
    s->g_hx1 = s->g_hx2 = s->g_hy1 = s->g_hy2 = 0.0f;
    s->g_env        = 0.0f;
    /* The gate restarts SHUT, not open: at gain 1 it passed the input's hum and noise, amplified by the amp, until its release closed it (up to a second): after a preset swap the owner's unit
     * made a burst of 0.01-0.05 of full scale at +70..+110 ms for every preset with a gate (pack 186 recordings, reproduced in the emulator with 0.0003 rms of input noise). A played note opens it
     * within its attack time (g_env follows the input from the first sample). */
    s->g_gain       = (s->g_open_thr > 0.0f && s->type != DYN_TYPE_COMP) ? s->g_range_floor : 1.0f;
    s->g_floor      = 0.0f;
    s->g_fast       = 8820u;                                      /* 0.2 s of fast floor learning after a reset */
    s->c_rms_sq  = 0.0f;
    s->c_peak    = 0.0f;
    s->c_win_max = s->c_prev_max = 0.0f;
    s->c_win_n   = 0;
    s->c_gr_db   = 0.0f;
    s->c_gain    = s->c_makeup * (float)s->level * (1.0f / 2097152.0f);
}

fx_node_t *fx_dynamics_new(void) {
    static fx_node_t n;
    n.run          = dynamics_run;
    n.run_stereo   = 0;
    n.create       = dynamics_create;
    n.destroy      = dynamics_destroy;
    n.set_param    = dynamics_set_param;
    n.reset        = dynamics_reset;
    n.active       = 1;
    n.frame        = PA_BLOCK_N;
    n.tag          = "dyn";
    n.tail_enabled = 0;
    n.tail_draining= 0;
    return &n;
}

void pa_dyn_set_expander_range_db(fx_node_t *nd, float range_db) {
    if (!nd || !nd->state) return;
    dyn_state_t *s = (dyn_state_t *)nd->state;
    s->g_range_db = range_db;
    if (range_db <= -80.0f) {
        s->g_range_floor = 0.0f;
        s->g_range_db    = -100.0f;
    } else {
        if (range_db > 0.0f) range_db = 0.0f;
        s->g_range_floor = powf(10.0f, range_db / 20.0f);
    }
}

float pa_dyn_get_expander_range_db(const fx_node_t *nd) {
    if (!nd || !nd->state) return -100.0f;
    const dyn_state_t *s = (const dyn_state_t *)nd->state;
    return s->g_range_db;
}

void pa_dyn_set_manual_makeup_db(fx_node_t *nd, float makeup_db, int auto_makeup) {
    if (!nd || !nd->state) return;
    dyn_state_t *s = (dyn_state_t *)nd->state;
    s->c_auto_makeup = auto_makeup ? 1 : 0;
    s->c_manual_makeup_db = makeup_db;
    gate_times(s);
    s->c_gain = s->c_makeup * (float)s->level * (1.0f / 2097152.0f);
}

float pa_dyn_get_makeup_gain_db(const fx_node_t *nd) {
    if (!nd || !nd->state) return 0.0f;
    const dyn_state_t *s = (const dyn_state_t *)nd->state;
    return 20.0f * log10f(s->c_makeup > 1e-6f ? s->c_makeup : 1e-6f);
}

int pa_dyn_is_auto_makeup(const fx_node_t *nd) {
    if (!nd || !nd->state) return 1;
    const dyn_state_t *s = (const dyn_state_t *)nd->state;
    return (int)s->c_auto_makeup;
}
