/* SPDX-License-Identifier: GPL-3.0-only */
/* Copyright (C) 2026 The Onyx Authors (@28spawn) */
/* custom-fw/src/fx/fx_eq.c — the EQ block: three types
 *
 *   0 5-BAND        five peaking bands, 100 Hz / 400 Hz / 1 kHz / 3 kHz / 8 kHz, Q 1.2,
 *                   +/-12 dB (the original EQ, unchanged - its coefficient code is below)
 *   1 LOW/HIGH CUT  24 dB/oct Butterworth high-pass and low-pass + LEVEL
 *                     knob 0 LOWCUT  20 .. 500 Hz   (exponential; 0 % = 20 Hz, i.e. off)
 *                     knob 1 HICUT   2 .. 20 kHz    (exponential; 100 % = 20 kHz, i.e. off)
 *                     knob 2 LEVEL   -12 .. +12 dB  (50 % = 0 dB)
 *                   the most-used EQ on a guitar: boom out, fizz out (NAM full-rig captures
 *                   already include their cab, so the CAB block's cuts are often off)
 *   2 PARAMETRIC-3  low shelf 120 Hz, a sweepable mid (Q 1.0) and a high shelf 4 kHz
 *                     knob 0 LOW  +/-12 dB, knob 1 MID +/-12 dB, knob 2 FREQ 200 Hz .. 5 kHz
 *                     (exponential), knob 3 HIGH +/-12 dB
 *
 * set_param ids: 0..4 the knobs, 10 the type. The filters run as Q21 DF2T biquads (the
 * 5-band's own path); the new types compute their RBJ coefficients in float (the AC791N
 * has an FPU) and quantize them. pa_eq_freq_hz() / pa_eq_gain_tenth_db() give the knobs'
 * real units for the screen.
 */

#include "pa_fx.h"
#include "pa_config.h"
#include "../dsp/fixedpoint.h"
#include <math.h>
#include <string.h>

#define EQ_NUM_BANDS 5
#define EQ_FS        PA_SAMPLE_RATE_F

enum { EQ_TYPE_5BAND = 0, EQ_TYPE_CUT = 1, EQ_TYPE_PARAM3 = 2, EQ_TYPE_COUNT = 3 };

typedef struct {
    int32_t b0, b1, b2, a1, a2;       /* Q21 DF2T coefficients */
} eq_coef_t;

typedef struct {
    eq_coef_t coef[2][EQ_NUM_BANDS];  /* Double-buffered coefficient sets (ping-pong) */
    volatile uint8_t buf_idx;         /* Active buffer index read atomically by audio ISR */
    float     s1[EQ_NUM_BANDS], s2[EQ_NUM_BANDS]; /* DF2T state registers (float: the FPU does a section in a third of the time of the 64-bit integer maths) */
    float     r1[EQ_NUM_BANDS], r2[EQ_NUM_BANDS]; /* the same for the right channel behind a stereo cabinet (idle while the output is mono) */
    int       type;
    int32_t   knob[EQ_NUM_BANDS];     /* the knobs (Q21), kept so a type change recomputes */
    int32_t   gain_q21;               /* output level (LOW/HIGH CUT) */
} eq_state_t;

extern void *pa_alloc(uint32_t);
extern void  pa_free(void *);

/* ---- type 0: the 5-band (unchanged) ------------------------------------------------ */
static const int32_t k_band_cos_w0[EQ_NUM_BANDS] = {   /* IMP-04/13: match UI labels */
    2096939, /* Band 0:  100 Hz */
    2093747, /* Band 1:  400 Hz */
    2075903, /* Band 2: 1000 Hz */
    1908482, /* Band 3: 3000 Hz */
     876127  /* Band 4: 8000 Hz */
};

static const int32_t k_band_alpha[EQ_NUM_BANDS] = {    /* Q = 1.2 */
     12449,  /* 100 Hz */
     49772,  /* 400 Hz */
    124077,  /* 1 kHz  */
    362223,  /* 3 kHz  */
    793906   /* 8 kHz  */
};

static void eq_calc_5band(eq_state_t *s, uint8_t buf, int b, int32_t val_q21) {
    if (val_q21 < 0) val_q21 = 0;
    if (val_q21 > PA_Q21_ONE) val_q21 = PA_Q21_ONE;

    /* Knob 0..1 -> -12..+12 dB. RBJ peaking: A = 10^(dB/40) (the old 0.25..4.0 mapping
     * was A^2 = +/-24 dB, i.e. twice what the UI shows). */
    float db = ((float)val_q21 / (float)PA_Q21_ONE - 0.5f) * 24.0f;
    int32_t A_q21 = (int32_t)(powf(10.0f, db / 40.0f) * (float)PA_Q21_ONE);

    int32_t alpha   = k_band_alpha[b];
    int32_t cos_w0  = k_band_cos_w0[b];

    int32_t alpha_mul_A = q21_mul(alpha, A_q21);
    int32_t alpha_div_A = (int32_t)(((int64_t)alpha << 21) / (A_q21 > 262144 ? A_q21 : 262144));

    int32_t a0 = PA_Q21_ONE + alpha_div_A;
    int32_t inv_a0 = (int32_t)((((int64_t)PA_Q21_ONE) << 21) / (a0 > 1000 ? a0 : 1000));

    eq_coef_t *c = &s->coef[buf][b];
    c->b0 = q21_mul(PA_Q21_ONE + alpha_mul_A, inv_a0);
    c->b1 = q21_mul(-2 * cos_w0, inv_a0);
    c->b2 = q21_mul(PA_Q21_ONE - alpha_mul_A, inv_a0);
    c->a1 = q21_mul(-2 * cos_w0, inv_a0);
    c->a2 = q21_mul(PA_Q21_ONE - alpha_div_A, inv_a0);
}

/* ---- types 1 and 2: float RBJ biquads, quantized --------------------------------- */
enum { RBJ_PEAK, RBJ_LOWSHELF, RBJ_HIGHSHELF, RBJ_HP, RBJ_LP, RBJ_FLAT };

static inline int32_t q21f(float x) { return (int32_t)lrintf(x * 2097152.0f); }

static void eq_set_rbj(eq_state_t *s, uint8_t buf, int b, int kind, float f0, float q, float db) {
    float b0 = 1, b1 = 0, b2 = 0, a0 = 1, a1 = 0, a2 = 0;
    if (kind != RBJ_FLAT) {
        const float w = 6.2831853f * f0 / EQ_FS, c = cosf(w), sn = sinf(w);
        const float al = sn / (2.0f * q), A = powf(10.0f, db / 40.0f);
        const float sq = 2.0f * sqrtf(A) * al;
        switch (kind) {
            case RBJ_PEAK:
                b0 = 1 + al * A; b1 = -2 * c; b2 = 1 - al * A; a0 = 1 + al / A; a1 = -2 * c; a2 = 1 - al / A;
                break;
            case RBJ_LOWSHELF:
                b0 = A * ((A + 1) - (A - 1) * c + sq); b1 = 2 * A * ((A - 1) - (A + 1) * c);
                b2 = A * ((A + 1) - (A - 1) * c - sq);
                a0 = (A + 1) + (A - 1) * c + sq; a1 = -2 * ((A - 1) + (A + 1) * c); a2 = (A + 1) + (A - 1) * c - sq;
                break;
            case RBJ_HIGHSHELF:
                b0 = A * ((A + 1) + (A - 1) * c + sq); b1 = -2 * A * ((A - 1) + (A + 1) * c);
                b2 = A * ((A + 1) + (A - 1) * c - sq);
                a0 = (A + 1) - (A - 1) * c + sq; a1 = 2 * ((A - 1) - (A + 1) * c); a2 = (A + 1) - (A - 1) * c - sq;
                break;
            case RBJ_HP:
                b0 = (1 + c) / 2; b1 = -(1 + c); b2 = (1 + c) / 2; a0 = 1 + al; a1 = -2 * c; a2 = 1 - al;
                break;
            case RBJ_LP:
                b0 = (1 - c) / 2; b1 = 1 - c; b2 = (1 - c) / 2; a0 = 1 + al; a1 = -2 * c; a2 = 1 - al;
                break;
        }
    }
    eq_coef_t *cf = &s->coef[buf][b];
    cf->b0 = q21f(b0 / a0);
    cf->b1 = q21f(b1 / a0);
    cf->b2 = q21f(b2 / a0);
    cf->a1 = q21f(a1 / a0);
    cf->a2 = q21f(a2 / a0);
}

static float knob01(int32_t v) {
    float k = (float)v * (1.0f / 2097152.0f);
    return k < 0.0f ? 0.0f : (k > 1.0f ? 1.0f : k);
}

/* the knobs' real units (also what the screen shows) */
static float eq_lowcut_hz(int32_t v)  { return 20.0f * powf(25.0f, knob01(v)); }    /* 20 .. 500 */
static float eq_hicut_hz(int32_t v)   { return 2000.0f * powf(10.0f, knob01(v)); }  /* 2k .. 20k */
static float eq_midfreq_hz(int32_t v) { return 200.0f * powf(25.0f, knob01(v)); }   /* 200 .. 5k */
static float eq_db(int32_t v)         { return (knob01(v) - 0.5f) * 24.0f; }        /* +/-12 */

static void eq_configure(eq_state_t *s) {
    uint8_t nb = (uint8_t)((s->buf_idx & 1u) ^ 1u);
    s->gain_q21 = PA_Q21_ONE;
    switch (s->type) {
        case EQ_TYPE_CUT:
            /* 4th-order Butterworth = two 2nd-order sections at Q 0.5412 and 1.3066 */
            eq_set_rbj(s, nb, 0, RBJ_HP, eq_lowcut_hz(s->knob[0]), 0.5412f, 0);
            eq_set_rbj(s, nb, 1, RBJ_HP, eq_lowcut_hz(s->knob[0]), 1.3066f, 0);
            eq_set_rbj(s, nb, 2, RBJ_LP, fminf(eq_hicut_hz(s->knob[1]), 20000.0f), 0.5412f, 0);
            eq_set_rbj(s, nb, 3, RBJ_LP, fminf(eq_hicut_hz(s->knob[1]), 20000.0f), 1.3066f, 0);
            eq_set_rbj(s, nb, 4, RBJ_FLAT, 0, 1, 0);
            s->gain_q21 = q21f(powf(10.0f, eq_db(s->knob[2]) / 20.0f));
            break;
        case EQ_TYPE_PARAM3:
            eq_set_rbj(s, nb, 0, RBJ_LOWSHELF, 120.0f, 0.707f, eq_db(s->knob[0]));
            eq_set_rbj(s, nb, 1, RBJ_PEAK, eq_midfreq_hz(s->knob[2]), 1.0f, eq_db(s->knob[1]));
            eq_set_rbj(s, nb, 2, RBJ_HIGHSHELF, 4000.0f, 0.707f, eq_db(s->knob[3]));
            eq_set_rbj(s, nb, 3, RBJ_FLAT, 0, 1, 0);
            eq_set_rbj(s, nb, 4, RBJ_FLAT, 0, 1, 0);
            break;
        default:
            for (int b = 0; b < EQ_NUM_BANDS; b++) eq_calc_5band(s, nb, b, s->knob[b]);
            break;
    }
    s->buf_idx = nb;
}

/* For the screen: the frequency (Hz) a knob of `type` sets, or 0 if it is not a frequency */
uint32_t pa_eq_freq_hz(int type, int p, int32_t knob_q21) {
    if (type == EQ_TYPE_CUT && p == 0) return (uint32_t)lrintf(eq_lowcut_hz(knob_q21));
    if (type == EQ_TYPE_CUT && p == 1) return (uint32_t)lrintf(eq_hicut_hz(knob_q21));
    if (type == EQ_TYPE_PARAM3 && p == 2) return (uint32_t)lrintf(eq_midfreq_hz(knob_q21));
    return 0;
}
/* ... and the gain in tenths of a dB, or INT32_MIN if it is not a gain */
int32_t pa_eq_gain_tenth_db(int type, int p, int32_t knob_q21) {
    if ((type == EQ_TYPE_CUT && p == 2) || (type == EQ_TYPE_PARAM3 && p != 2 && p < 4))
        return (int32_t)lrintf(eq_db(knob_q21) * 10.0f);
    return INT32_MIN;
}

/* One channel through the sections; st1/st2 are that channel's state registers. */
/* PA_RAMFN (pa_config.h): the three EQ run functions run from SRAM; they make no call out of the window (eq_channel is a leaf, the two others call only it) */
PA_RAMFN static void eq_channel(eq_state_t *s, int32_t *buf, int n, float *st1, float *st2) {
    /* Capture active buffer index ONCE per block: lockless, zero-spin, atomic */
    const uint8_t nb = (uint8_t)(s->buf_idx & 1u);

    for (int b = 0; b < EQ_NUM_BANDS; b++) {
        const eq_coef_t *cf = &s->coef[nb][b];
        const int32_t b0 = cf->b0;
        const int32_t b1 = cf->b1;
        const int32_t b2 = cf->b2;
        const int32_t a1 = cf->a1;
        const int32_t a2 = cf->a2;

        if (b0 == PA_Q21_ONE && b1 == 0 && b2 == 0 && a1 == 0 && a2 == 0)
            continue;                                        /* a flat section: skip it */
        const float k = 1.0f / 2097152.0f;                   /* the coefficients are Q21 */
        const float fb0 = (float)b0 * k, fb1 = (float)b1 * k, fb2 = (float)b2 * k, fa1 = (float)a1 * k, fa2 = (float)a2 * k;
        float s1 = st1[b], s2 = st2[b];
        if (!(s1 > -4.0e9f && s1 < 4.0e9f)) s1 = 0.0f;       /* NaN / infinity: never let a corrupt state stay */
        if (!(s2 > -4.0e9f && s2 < 4.0e9f)) s2 = 0.0f;

        for (int i = 0; i < n; i++) {
            const float x = (float)buf[i];
            float y = fb0 * x + s1;
            s1 = fb1 * x - fa1 * y + s2;
            s2 = fb2 * x - fa2 * y;
            if (y > 2.0e9f) y = 2.0e9f; else if (y < -2.0e9f) y = -2.0e9f;
            buf[i] = (int32_t)y;
        }
        if (s1 < 1.0e-6f && s1 > -1.0e-6f) s1 = 0.0f;        /* flush the denormals the decaying state would reach in silence */
        if (s2 < 1.0e-6f && s2 > -1.0e-6f) s2 = 0.0f;
        st1[b] = s1;
        st2[b] = s2;
    }
    if (s->gain_q21 != PA_Q21_ONE)
        for (int i = 0; i < n; i++) buf[i] = sat32(((int64_t)buf[i] * s->gain_q21) >> 21);
}

PA_RAMFN static void eq_run(fx_node_t *nd, int32_t *buf, int n) {
    eq_state_t *s = (eq_state_t *)nd->state;
    if (!s) return;
    eq_channel(s, buf, n, s->s1, s->s2);
}

/* Behind a stereo cabinet the EQ keeps the two channels apart. Both channels use the same coefficients (one buffer index for the block). */
PA_RAMFN static void eq_run_stereo(fx_node_t *nd, int32_t *bl, int32_t *br, int n) {
    eq_state_t *s = (eq_state_t *)nd->state;
    if (!s) return;
    eq_channel(s, bl, n, s->s1, s->s2);
    eq_channel(s, br, n, s->r1, s->r2);
}

static void eq_create(fx_node_t *nd) {
    eq_state_t *s = (eq_state_t *)pa_alloc(sizeof(eq_state_t));
    if (s) {
        memset(s, 0, sizeof(eq_state_t));
        s->type = EQ_TYPE_5BAND;
        for (int b = 0; b < EQ_NUM_BANDS; b++) {
            s->knob[b] = PA_Q21_ONE / 2;
        }
        s->gain_q21 = PA_Q21_ONE;
        s->buf_idx = 1;
        eq_configure(s); /* fills buf 0, sets s->buf_idx = 0 */
        s->buf_idx = 0;
        eq_configure(s); /* fills buf 1, sets s->buf_idx = 1 */
    }
    nd->state = s;
}

static void eq_destroy(fx_node_t *nd) {
    if (nd->state) {
        pa_free(nd->state);
        nd->state = 0;
    }
}

static void eq_set_param(fx_node_t *nd, int id, int32_t val) {
    eq_state_t *s = (eq_state_t *)nd->state;
    if (!s) return;
    if (id >= 0 && id < EQ_NUM_BANDS) {
        s->knob[id] = val;
        eq_configure(s);
    } else if (id == 10) {
        int t = (int)((uint32_t)val % EQ_TYPE_COUNT);
        if (t != s->type) {
            s->type = t;
            for (int b = 0; b < EQ_NUM_BANDS; b++) s->s1[b] = s->s2[b] = s->r1[b] = s->r2[b] = 0;
            eq_configure(s);
        }
    }
}

static void eq_reset(fx_node_t *nd) {
    eq_state_t *s = (eq_state_t *)nd->state;
    if (!s) return;
    for (int b = 0; b < EQ_NUM_BANDS; b++) {
        s->s1[b] = 0;
        s->s2[b] = 0;
        s->r1[b] = 0;
        s->r2[b] = 0;
    }
}

fx_node_t *fx_eq_new(void) {
    static fx_node_t n;
    n.run       = eq_run;
    n.run_stereo = eq_run_stereo;
    n.stereo_opt = 2;                 /* stereo only behind a stereo cabinet; otherwise exactly the mono EQ */
    n.create    = eq_create;
    n.destroy   = eq_destroy;
    n.set_param = eq_set_param;
    n.reset     = eq_reset;
    n.active    = 1;
    n.frame     = PA_BLOCK_N;
    n.tag       = "eq";
    return &n;
}
