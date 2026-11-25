/* SPDX-License-Identifier: GPL-3.0-only */
/* Copyright (C) 2026 The Onyx Authors (@28spawn) */
/* custom-fw/src/fx/fx_mod.c - the MOD block: seven stereo modulation effects.
 *
 *   0 CHORUS       two modulated taps (15 / 16 ms +-5 ms), L/R in quadrature
 *   1 PHASER       four allpass stages per side, sweeping 200 Hz .. 2 kHz (Phase 90 range)
 *   2 FLANGER      1 .. 6.8 ms through-zero-free flanger with feedback
 *   3 TREMOLO      amplitude, L/R in quadrature            (MIX and FDBK unused)
 *   4 VIBRATO      pitch, 100 % wet                         (MIX and FDBK unused)
 *   5 WAH          LFO-swept resonant band-pass (the wah pedal rocked by RATE), 380 Hz .. 2.2 kHz
 *   6 AUTO WAH     the same filter swept by the playing: hard picking opens it, RATE = how fast it closes
 *
 * (1.13: UNIVIBE and AUTO FILTER were replaced: next to PHASER and VIBRATO the univibe was hard to tell apart, and the auto filter was the same sweep with a duller voice.)
 *
 * Knobs (set_param ids): 0 RATE (0.10 .. 8.10 Hz), 1 DEPTH, 2 MIX, 3 FDBK (resonance on
 * the WAH types), 10 type. pa_mod_rate_mhz() gives RATE in real units for the screen.
 *
 * What changed (custom-fw/test/test_mod.c measures each):
 *   - the delay line is float: 1024 x 4 B, the same 4 KB as the old 2048 x int16, but it
 *     no longer clips above 0 dBFS (15 % THD at +3.5 dBFS before) or rounds to 16 bits;
 *   - MIX is equal-power (dry cos, wet sin): engaging the chorus no longer drops the
 *     level 3..6 dB;
 *   - the phaser / univibe right channel is its own allpass chain on the quadrature LFO.
 *     It used to be the left wet signal inverted, which cancelled completely in mono;
 *   - the phaser sweeps 200 Hz .. 2 kHz (it was 1.1 .. 8 kHz);
 *   - the auto filter follows the playing (3 ms attack, 150 ms release, -42 .. -6 dBFS
 *     mapped onto the sweep); it used to sit almost wide open, a ~1 dB effect.
 *
 * Float: the AC791N has a single-precision FPU. In and out are Q21.
 */

#include <math.h>
#include <string.h>
#include "pa_fx.h"
#include "pa_config.h"
#include "../dsp/fixedpoint.h"

#define MOD_FS        PA_SAMPLE_RATE_F
#define MOD_BUF_LEN   1024u            /* longest tap: chorus 720 + 224 = 944 samples */
#define MOD_BUF_MASK  (MOD_BUF_LEN - 1u)
#define MOD_AP_STAGES 4
#define MOD_CTL       16               /* sweep coefficients are updated every 16 samples */

typedef enum {
    MOD_TYPE_CHORUS     = 0,
    MOD_TYPE_PHASER     = 1,
    MOD_TYPE_FLANGER    = 2,
    MOD_TYPE_TREMOLO    = 3,
    MOD_TYPE_VIBRATO    = 4,
    MOD_TYPE_WAH        = 5,
    MOD_TYPE_AUTOWAH    = 6,
    MOD_TYPE_COUNT      = 7
} mod_type_t;

#define MOD_WARM_FILL 1024u          /* samples: longer than the longest tap (660 + 224 = 884) */
#define MOD_WARM_END  (MOD_WARM_FILL + 882u)

typedef struct {
    float     *buf;
    uint32_t   w;
    mod_type_t type;
    uint32_t   lfo_phase, lfo_inc;
    float      depth, fb, mix, dry_g, wet_g;

    /* phaser / univibe: one chain per side */
    float      apx[2][MOD_AP_STAGES], apy[2][MOD_AP_STAGES], apa[2][MOD_AP_STAGES];
    float      ph_fb[2];
    int        ctl;
    uint32_t   warm;              /* samples run since the delay line was cleared (type change, reset): the delay-line types are bypassed, then crossfaded in, once it holds a signal */

    /* auto filter */
    float      env, rel, svf_low, svf_band, svf_f, svf_damp;
} mod_state_t;

extern void *pa_alloc(uint32_t);
extern void  pa_free(void *);

/* RATE knob -> LFO phase increment per sample (2^32 = one cycle): 0.10 .. 8.10 Hz */
static uint32_t mod_inc_from_knob(int32_t val) {
    if (val < 0) val = 0;
    if (val > PA_Q21_ONE) val = PA_Q21_ONE;
    const uint32_t inc = 9739u + (uint32_t)(((uint64_t)(uint32_t)val * 779134u) >> 21);     /* the phase step per sample at 44.1 kHz */
#if PA_SAMPLE_RATE_HZ == 44100
    return inc;
#else
    return (uint32_t)(((uint64_t)inc * 44100u) / PA_SAMPLE_RATE_HZ);                  /* the same rate in Hz at this sample rate */
#endif
}
uint32_t pa_mod_rate_mhz(int32_t knob_q21) {
    return (uint32_t)(((uint64_t)mod_inc_from_knob(knob_q21) * ((uint64_t)PA_SAMPLE_RATE_HZ * 1000ull) + (1ull << 31)) >> 32);
}

/* sine-shaped LFO from a 32-bit phase: a triangle bent by (3t - t^3) / 2, in -1..1 */
static inline float lfo_sine(uint32_t phase) {
    float tri = (phase < 0x80000000u) ? ((float)phase * (2.0f / 2147483648.0f) - 1.0f)
                                      : (1.0f - (float)(phase - 0x80000000u) * (2.0f / 2147483648.0f));
    return 0.5f * tri * (3.0f - tri * tri);
}

/* fractional read, `d` samples behind the write position (4-point cubic Hermite interpolation)
 * Provides C1-smooth continuous derivatives and flat high-frequency response (<0.05 dB at 5 kHz),
 * eliminating linear interpolation dulling and modulation corner noise at negligible cycle cost. */
static inline float tap(const mod_state_t *s, float d) {
    if (d < 2.0f) d = 2.0f;
    if (d > (float)(MOD_BUF_LEN - 3u)) d = (float)(MOD_BUF_LEN - 3u);
    uint32_t i = (uint32_t)d;
    float fr = d - (float)i;
    uint32_t w = s->w;
    float ym1 = s->buf[(w - i + 1u) & MOD_BUF_MASK];
    float y0  = s->buf[(w - i)      & MOD_BUF_MASK];
    float y1  = s->buf[(w - i - 1u) & MOD_BUF_MASK];
    float y2  = s->buf[(w - i - 2u) & MOD_BUF_MASK];

    float c0 = y0;
    float c1 = 0.5f * (y1 - ym1);
    float c2 = ym1 - 2.5f * y0 + 2.0f * y1 - 0.5f * y2;
    float c3 = 0.5f * (y2 - ym1) + 1.5f * (y0 - y1);
    return ((c3 * fr + c2) * fr + c1) * fr + c0;
}

/* first-order allpass coefficient whose 90-degree point is f Hz:
 * y = a (x - y1) + x1 is (a + z^-1) / (1 + a z^-1); with a = (t - 1) / (t + 1),
 * t = tan(pi f / fs), its phase is -90 degrees at f. */
static inline float ap_coef(float f) {
    float x = 3.14159265f * f / MOD_FS;
    float t = x + x * x * x * (1.0f / 3.0f) + x * x * x * x * x * (2.0f / 15.0f);
    return (t - 1.0f) / (t + 1.0f);
}

static void mod_update_sweep(mod_state_t *s, float uni_l, float uni_r) {
    float u[2] = { uni_l, uni_r };
    for (int c = 0; c < 2; c++) {
        const float f = 630.0f * expf(2.3025851f * s->depth * (u[c] - 0.5f)); /* 200 Hz .. 2 kHz */
        for (int st = 0; st < MOD_AP_STAGES; st++)
            s->apa[c][st] = ap_coef(f);
    }
}

static inline float allpass_chain(mod_state_t *s, int c, float x) {
    float v = x + s->ph_fb[c] * s->fb;
    for (int st = 0; st < MOD_AP_STAGES; st++) {
        float y = s->apa[c][st] * (v - s->apy[c][st]) + s->apx[c][st];
        s->apx[c][st] = v; s->apy[c][st] = y; v = y;
    }
    s->ph_fb[c] = v;
    return v;
}

PA_HOT static void mod_run_stereo(fx_node_t *nd, int32_t *buf_L, int32_t *buf_R, int n) {
    mod_state_t *s = (mod_state_t *)nd->state;
    if (!s || !s->buf) return;
    const float q = 1.0f / 2097152.0f;

    for (int i = 0; i < n; i++) {
        float xl = (float)buf_L[i] * q;
        float xr = buf_R ? (float)buf_R[i] * q : xl;

        s->lfo_phase += s->lfo_inc;
        float lb = lfo_sine(s->lfo_phase);                    /* bipolar, left */
        float rb = lfo_sine(s->lfo_phase + 0x40000000u);      /* right: 90 degrees on */
        float lu = 0.5f * (lb + 1.0f), ru = 0.5f * (rb + 1.0f);

        float wl = xl, wr = xr;
        int   mixed = 1;

        switch (s->type) {
            case MOD_TYPE_CHORUS:
                wl = tap(s, 660.0f + 224.0f * s->depth * lb);
                wr = tap(s, 720.0f + 224.0f * s->depth * rb);
                s->buf[s->w] = xl;
                s->w = (s->w + 1u) & MOD_BUF_MASK;
                break;
            case MOD_TYPE_FLANGER:
                wl = tap(s, 44.0f + 256.0f * s->depth * lu);
                wr = tap(s, 44.0f + 256.0f * s->depth * ru);
                s->buf[s->w] = xl + s->fb * wl;
                s->w = (s->w + 1u) & MOD_BUF_MASK;
                break;
            case MOD_TYPE_VIBRATO:
                wl = tap(s, 220.0f + 96.0f * s->depth * lb);
                wr = tap(s, 220.0f + 96.0f * s->depth * rb);
                s->buf[s->w] = xl;
                s->w = (s->w + 1u) & MOD_BUF_MASK;
                mixed = 0;
                break;
            case MOD_TYPE_PHASER:
                if (s->ctl == 0) mod_update_sweep(s, lu, ru);
                wl = allpass_chain(s, 0, xl);
                wr = allpass_chain(s, 1, xr);
                break;
            case MOD_TYPE_TREMOLO:
                wl = xl * (1.0f - s->depth * lu);
                wr = xr * (1.0f - s->depth * ru);
                mixed = 0;
                break;
            case MOD_TYPE_WAH:
            case MOD_TYPE_AUTOWAH: {
                const int autow = (s->type == MOD_TYPE_AUTOWAH);
                const float r = fabsf(xl);
                s->env += (r > s->env ? 0.00753f : s->rel) * (r - s->env);       /* attack 3 ms; release: WAH 150 ms, AUTO WAH from the RATE knob */
                if (s->ctl == 0) {
                    const float rr = s->fb > 0.0f ? s->fb : 0.0f;
                    s->svf_damp = 0.8f - 0.65f * rr;                              /* band-pass Q about 1.2 .. 5 */
                    float sweep;
                    if (autow) {
                        /* the playing opens the filter: DEPTH is the sensitivity: the 30 dB window that opens the filter sits at -6 .. +24 dBFS of envelope at DEPTH 0 and at -42 .. -12 dBFS at DEPTH 100 %, the squared curve keeps the low end of the range for soft playing */
                        const float e = s->env > 1e-6f ? s->env : 1e-6f;
                        float nrm = (8.6858896f * logf(e) + 6.0f + 36.0f * s->depth) * (1.0f / 30.0f);
                        if (nrm < 0.0f) nrm = 0.0f;
                        if (nrm > 1.0f) nrm = 1.0f;
                        sweep = nrm;
                        const float frac = (float)(s->lfo_inc - mod_inc_from_knob(0)) * (1.0f / (float)(mod_inc_from_knob(PA_Q21_ONE) - mod_inc_from_knob(0)));
                        s->rel = 1.0f / (PA_SAMPLE_RATE_F * (0.45f - 0.40f * (frac > 1.0f ? 1.0f : frac)));   /* RATE: 450 ms (slow) .. 50 ms (fast) */
                    } else {
                        sweep = lu;                                               /* the foot rocking: RATE is the speed, DEPTH how far it travels */
                        s->rel = 0.000151f;
                    }
                    const float span = autow ? 1.9f : 1.75f * (0.35f + 0.65f * s->depth);
                    const float fc = (autow ? 520.0f : 380.0f) * expf(span * sweep);   /* WAH 380 Hz .. 2.2 kHz; AUTO WAH 520 Hz .. 3.5 kHz (1.14: 1.5 dB brighter on DI playing) */
                    s->svf_f = 2.0f * sinf(3.14159265f * fc / MOD_FS);
                }
                s->svf_low += s->svf_f * s->svf_band;
                const float high = xl - s->svf_low - s->svf_damp * s->svf_band;
                s->svf_band += s->svf_f * high;
                wl = wr = 1.6f * s->svf_damp * s->svf_band;
                break;
            }
            default: break;
        }
        if (++s->ctl >= MOD_CTL) s->ctl = 0;

        float ol = mixed ? s->dry_g * xl + s->wet_g * wl : wl;
        float orr = mixed ? s->dry_g * xr + s->wet_g * wr : wr;
        if (s->warm < MOD_WARM_END && (s->type == MOD_TYPE_CHORUS || s->type == MOD_TYPE_FLANGER || s->type == MOD_TYPE_VIBRATO)) {
            /* A cleared delay line returns silence for its first 15-20 ms, then the wet path starts all at once in the middle of the signal (the emulator click probe: a one-sample step of
             * 30 % of the level, 13x the steady step, on chorus, flanger and vibrato after a type change or a preset swap). Bypass until the line is full, then crossfade over 20 ms. */
            const float r = s->warm < MOD_WARM_FILL ? 0.0f : (float)(s->warm - MOD_WARM_FILL) * (1.0f / (float)(MOD_WARM_END - MOD_WARM_FILL));
            ol = r * ol + (1.0f - r) * xl;
            orr = r * orr + (1.0f - r) * xr;
            s->warm++;
        }
        float a = ol * 2097152.0f, b = orr * 2097152.0f;
        if (a >  2147483000.0f) a =  2147483000.0f;
        if (a < -2147483000.0f) a = -2147483000.0f;
        if (b >  2147483000.0f) b =  2147483000.0f;
        if (b < -2147483000.0f) b = -2147483000.0f;
        buf_L[i] = (int32_t)a;
        if (buf_R) buf_R[i] = (int32_t)b;
    }
}

static void mod_run(fx_node_t *nd, int32_t *buf, int n) {
    mod_run_stereo(nd, buf, 0, n);
}

static void mod_set_mix(mod_state_t *s) {
    /* equal power: a decorrelated wet keeps the level, MIX 50 % = -3 dB each */
    s->dry_g = cosf(1.5707963f * s->mix);
    s->wet_g = sinf(1.5707963f * s->mix);
}

static void mod_create(fx_node_t *nd) {
    mod_state_t *s = (mod_state_t *)pa_alloc(sizeof(mod_state_t));
    if (s) {
        s->buf = (float *)pa_alloc(MOD_BUF_LEN * sizeof(float));
        if (!s->buf) { pa_free(s); nd->state = 0; return; }   /* IMP-13: all-or-nothing */
        for (uint32_t i = 0; i < MOD_BUF_LEN; i++) s->buf[i] = 0.0f;
        /* the rest of the state is zeroed by pa_alloc */
        s->type     = MOD_TYPE_CHORUS;
        s->lfo_inc  = 146000u;
        s->depth    = 0.5f;
        s->fb       = 0.3f;
        s->mix      = 0.5f;
        s->svf_damp = 0.8f - 0.65f * s->fb;
        s->rel      = 0.000151f;
        s->svf_f    = 0.05f;
        mod_set_mix(s);
        mod_update_sweep(s, 0.5f, 0.5f);
    }
    nd->state = s;
}

static void mod_destroy(fx_node_t *nd) {
    mod_state_t *s = (mod_state_t *)nd->state;
    if (s) {
        if (s->buf) pa_free(s->buf);
        pa_free(s);
    }
    nd->state = 0;
}

static float knob01(int32_t val) {
    float v = (float)val * (1.0f / 2097152.0f);
    return v < 0.0f ? 0.0f : (v > 1.0f ? 1.0f : v);
}

static void mod_set_param(fx_node_t *nd, int id, int32_t val) {
    mod_state_t *s = (mod_state_t *)nd->state;
    if (!s) return;
    switch (id) {
        case 0: s->lfo_inc = mod_inc_from_knob(val); break;          /* RATE */
        case 1: s->depth = knob01(val); break;                        /* DEPTH */
        case 2: s->mix = knob01(val); mod_set_mix(s); break;          /* MIX */
        case 3: {                                                     /* FDBK, capped at 0.85 */
            float f = (float)val * (1.0f / 2097152.0f);
            if (f >  0.85f) f =  0.85f;
            if (f < -0.85f) f = -0.85f;
            s->fb = f;
            break;
        }
        case 10: {
            s->type = (mod_type_t)((uint32_t)val % MOD_TYPE_COUNT);
            s->ctl = 0;
            if (s->buf) memset(s->buf, 0, MOD_BUF_LEN * sizeof(float));
            memset(s->apx, 0, sizeof(s->apx));
            memset(s->apy, 0, sizeof(s->apy));
            memset(s->ph_fb, 0, sizeof(s->ph_fb));
            s->svf_low = 0.0f;
            s->svf_band = 0.0f;
            s->warm = 0;
            break;
        }
        default: break;
    }
}

static void mod_reset(fx_node_t *nd) {
    mod_state_t *s = (mod_state_t *)nd->state;
    if (!s) return;
    if (s->buf) memset(s->buf, 0, MOD_BUF_LEN * sizeof(float));
    s->w = 0;
    s->lfo_phase = 0;
    memset(s->apx, 0, sizeof(s->apx));
    memset(s->apy, 0, sizeof(s->apy));
    memset(s->ph_fb, 0, sizeof(s->ph_fb));
    s->env = 0.0f;
    s->rel = 0.000151f;
    s->svf_low = 0.0f;
    s->svf_band = 0.0f;
    s->ctl = 0;
    s->warm = 0;
}

fx_node_t *fx_mod_new(void) {
    static fx_node_t n;
    n.run          = mod_run;
    n.run_stereo   = mod_run_stereo;
    n.create       = mod_create;
    n.destroy      = mod_destroy;
    n.set_param    = mod_set_param;
    n.reset        = mod_reset;
    n.active       = 1;
    n.frame        = PA_BLOCK_N;
    n.tag          = "mod";
    n.tail_enabled = 0;
    n.tail_draining= 0;
    return &n;
}
