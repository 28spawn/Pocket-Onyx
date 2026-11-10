/* SPDX-License-Identifier: GPL-3.0-only */
/* Copyright (C) 2026 The Onyx Authors (@28spawn) */
#include "../../include/pa_config.h"
/* src/sys/pa_audio_core.c - the portable audio core (see pa_audio_core.h).
 *
 * This is the DEVICE's amp node and the DEVICE's block callback, lifted out of
 * src/board/main.c so that emu/emu_bridge.c and wasm/pa_wasm.c can run the exact
 * same code. The emulator, the WASM build and the device therefore produce the
 * same audio, including the Q21 fixed-point arithmetic, the 512-entry
 * activation LUTs and the Q18 cabinet convolution.
 */
#include "../../include/pa_evlog.h"
#include <math.h>
#include <string.h>
#include "../../include/pa_audio_core.h"
#include "../../include/pa_audio.h"
#include "../../include/pa_stock_assets.h"
#include "../../include/pa_preset.h"
#include "../../include/pa_settings.h"
#include "../../include/pa_user_nam.h"
#include "../../include/pa_input.h"
#include "../../include/pa_overload.h"
#include "../../include/pa_sysex.h"
#include "../../include/pa_model.h"
#include "../dsp/fixedpoint.h"
#include "pa_mix_limiter.h"
#include "../dsp/pa_stereo_spread.h"
/* Master level hold (device only): from power-on the output is SILENT until the front-panel MASTER knob has been read, then it goes to the knob's level (or, if the knob cannot be read
 * within 5 s, to the saved OUTPUT LEVEL). Before this the master started at the saved level, which is 100 % when nothing is saved (settings are not written to the flash yet), and the knob
 * was first read only after 4 s: the first seconds after every power-on were the input noise at full volume (bench 2026-11-03: the input peaked at 17 % with nothing played). */
volatile uint8_t g_pa_master_hold;
volatile uint8_t g_pa_gov_level;           /* set by the audio interrupt's load governor (audio_alnk.c); 0 on the host */
#define PA_IN_TRACE_N 192u
volatile uint16_t g_pa_in_trace[PA_IN_TRACE_N];        /* the raw input's peak per 46 ms since the audio started (>> 8 of the 24-bit value) */
volatile int16_t  g_pa_in_trace_mean[PA_IN_TRACE_N];  /* ... and its mean */
volatile uint8_t g_pa_overload_force;     /* tests: 1 = behave as overloaded (the popup) */
#define PA_SETTINGS_QUIET_MS   5000u    /* the settings are written this long after the last change... */
#define PA_SETTINGS_MIN_GAP_MS 8000u    /* ...and never more often than this */
static volatile uint32_t s_flash_ops;      /* flash operations that quiesced the audio (pa_audio_core_flash_end): each costs the overruns a swap does, and the overload rule must not read them as overload */
static volatile uint32_t s_swap_done_seq;   /* counts the swaps the audio thread has applied: the main loop redraws the screen for each one (the screen is drawn from the chain, which a swap changes) */
static uint32_t pa_perf_permille(uint32_t modelled);   /* the measured CPU load on the device, the model elsewhere (below) */
#if !defined(PA_HOST_BUILD) && !defined(PA_HOST_SIM)
#include "../../include/pa_plat.h"       /* PA_CP: the bench step markers */
#endif
#ifndef PA_CP
#define PA_CP(n) ((void)0)
#endif
#ifndef PA_ISR_CP
#define PA_ISR_CP(n) ((void)0)
#endif

#define AMP_NBANDS 4

typedef struct { int32_t b0, b1, b2, a1, a2; } amp_biquad_t;

typedef struct {
    int32_t      gain_q21;
    int32_t      out_trim_q21;     /* per-model loudness normalisation */
    int32_t      master_q21;
    amp_biquad_t bq[AMP_NBANDS];
} amp_coeffs_t;

typedef struct { float f, min_db, max_db; } amp_band_def_t;

/* Coeff hand-over: the UI thread fills g_amp_next and bumps g_amp_next_seq; the
 * ISR copies it at the next block boundary, so the audio thread never sees a
 * half-written biquad. */
static amp_coeffs_t      g_amp_live;
static amp_coeffs_t      g_amp_next;
static volatile uint32_t g_amp_next_seq = 0, g_amp_live_seq = 0;
static float             g_amp_s1[AMP_NBANDS], g_amp_s2[AMP_NBANDS];   /* tone stack state, float: 7x cheaper than Q21 on this core */


/* Current knob state (Q21 0..1) and model voicing */
static int32_t g_amp_knob[6] = { PA_Q21_ONE / 2, PA_Q21_ONE / 2, PA_Q21_ONE / 2,
                                 PA_Q21_ONE / 2, PA_Q21_ONE / 2, PA_Q21_ONE / 2 };
static amp_band_def_t g_amp_bands[AMP_NBANDS] = {
    {  300.0f, -8.0f, 8.0f }, { 2000.0f, -8.0f, 8.0f },
    { 4000.0f, -8.0f, 8.0f }, { 5000.0f, -6.0f, 6.0f }
};
static float g_amp_trim_db = 0.0f;

/* Output trim (dB) per stock amp slot (GAIN 50 %, flat, matching cab) so all 20 slots
 * land at ~-18 dBFS RMS with the reference DIs
 * (emu/samples/di_clean_chords, di_rock_riff, di_blues_lead; di_bass_line for 16..20).
 * Range -8..+24 dB: slot 2 (US Clean) is a lower-output profile (-42 dBFS). */
static const int8_t k_amp_out_trim_db[PA_STOCK_AMP_COUNT] = {
     2, 24, 15,  2,  2, -3,  5, -8, -8, -7,   /* 1 UK Clean .. 10 US Crunch Gain   */
    -1,  1, -5, -7, -4,  2,  4, -4,  6,  9    /* 11 US High Gain .. 20 Euro Clean  */
};

static inline int32_t f_to_q21(float x) { return (int32_t)lrintf(x * (float)PA_Q21_ONE); }
static inline float   knob01(int32_t q21) { float v = (float)q21 / (float)PA_Q21_ONE; return v < 0.f ? 0.f : (v > 1.f ? 1.f : v); }

/* RBJ cookbook: type 0 = low shelf, 1 = peak, 2 = high shelf */
static amp_biquad_t rbj(int type, float f0, float db, float q) {
    const float A = powf(10.0f, db / 40.0f), w = 2.0f * 3.14159265f * f0 / (float)PA_SAMPLE_RATE;
    const float c = cosf(w), sn = sinf(w), al = sn / (2.0f * q), sq = 2.0f * sqrtf(A) * al;
    float b0, b1, b2, a0, a1, a2;
    if (type == 1) {
        b0 = 1 + al * A; b1 = -2 * c; b2 = 1 - al * A; a0 = 1 + al / A; a1 = -2 * c; a2 = 1 - al / A;
    } else if (type == 0) {
        b0 = A * ((A + 1) - (A - 1) * c + sq); b1 = 2 * A * ((A - 1) - (A + 1) * c); b2 = A * ((A + 1) - (A - 1) * c - sq);
        a0 = (A + 1) + (A - 1) * c + sq;      a1 = -2 * ((A - 1) + (A + 1) * c);    a2 = (A + 1) + (A - 1) * c - sq;
    } else {
        b0 = A * ((A + 1) + (A - 1) * c + sq); b1 = -2 * A * ((A - 1) + (A + 1) * c); b2 = A * ((A + 1) + (A - 1) * c - sq);
        a0 = (A + 1) - (A - 1) * c + sq;      a1 = 2 * ((A - 1) - (A + 1) * c);      a2 = (A + 1) - (A - 1) * c - sq;
    }
    amp_biquad_t r = { f_to_q21(b0 / a0), f_to_q21(b1 / a0), f_to_q21(b2 / a0), f_to_q21(a1 / a0), f_to_q21(a2 / a0) };
    return r;
}

static void amp_recompute(void) {
    amp_coeffs_t c;
    c.gain_q21     = f_to_q21(powf(10.0f, (-15.0f + 30.0f * knob01(g_amp_knob[0])) / 20.0f));
    c.out_trim_q21 = f_to_q21(powf(10.0f, g_amp_trim_db / 20.0f));
    c.master_q21   = f_to_q21(2.0f * knob01(g_amp_knob[5]));
    static const int   k_type[AMP_NBANDS] = { 0, 1, 2, 1 };
    static const float k_q[AMP_NBANDS]    = { 0.707f, 0.7f, 0.707f, 0.9f };
    for (int b = 0; b < AMP_NBANDS; b++) {
        const amp_band_def_t *d = &g_amp_bands[b];
        float db = d->min_db + (d->max_db - d->min_db) * knob01(g_amp_knob[1 + b]);
        c.bq[b] = rbj(k_type[b], d->f, db, k_q[b]);
    }
    g_amp_next = c;
    g_amp_next_seq++;                 /* publish; ISR copies at next block start */
}

void pa_amp_set_model_profile(uint8_t amp_idx) {
    if (amp_idx < PA_STOCK_AMP_COUNT) {
        const pa_stock_amp_info_t *ai = &g_pa_stock_amps[amp_idx];
        g_amp_bands[0] = (amp_band_def_t){ ai->bass_freq,   ai->bass_min_db,   ai->bass_max_db };
        g_amp_bands[1] = (amp_band_def_t){ ai->mid_freq,    ai->mid_min_db,    ai->mid_max_db };
        g_amp_bands[2] = (amp_band_def_t){ ai->treble_freq, ai->treble_min_db, ai->treble_max_db };
#ifndef PA_AMP_TRIM_DISABLE
        g_amp_trim_db  = pa_stock_has_factory_assets() ? (float)k_amp_out_trim_db[amp_idx] : 0.0f;
#else
        g_amp_trim_db  = 0.0f;                          /* calibration builds only */
#endif
    } else {
        /* User NAM captures: generic guitar voicing; the loudness is handled by
         * the capture's own makeup gain. */
        g_amp_bands[0] = (amp_band_def_t){  300.0f, -8.0f, 8.0f };
        g_amp_bands[1] = (amp_band_def_t){ 2000.0f, -8.0f, 8.0f };
        g_amp_bands[2] = (amp_band_def_t){ 4000.0f, -8.0f, 8.0f };
        g_amp_trim_db  = 0.0f;
    }
    amp_recompute();
}

/* ---- the AMP node --------------------------------------------------------- */

static pa_nam_model_t     *s_gru;
static pa_wavenet_model_t *s_wn;
static int                *s_engine;

static void amp_slot_set_param(fx_node_t *nd, int id, int32_t val) {
    (void)nd;
    if (id < 0 || id > 5) return;
    if (val < 0) val = 0;
    if (val > PA_Q21_ONE) val = PA_Q21_ONE;
    g_amp_knob[id] = val;             /* 0 GAIN, 1 BASS, 2 MID, 3 TREBLE, 4 PRES, 5 MASTER */
    amp_recompute();
}

static void amp_slot_reset(fx_node_t *nd) {
    (void)nd;
    for (int b = 0; b < AMP_NBANDS; b++) { g_amp_s1[b] = 0.0f; g_amp_s2[b] = 0.0f; }
}

/* The part of the amp after the engine: the tone stack and the loudness trim x master. It runs on core 1 inside the amp slot, or - when core 0 asks for it
 * (pa_amp_defer_post) - on core 0 right after the hand-over, on the block that comes back: about 90 us a block that core 1 does not have to spend (the WaveNet
 * engine needs every microsecond of its block). c is the coefficient set the block was run with. */
static void amp_post_run(const amp_coeffs_t *c, int32_t *buf, int n) {
    /* 3. Tone stack: the same four biquads and the same Q21 coefficients, run in float (transposed direct form II). The four stages run in ONE pass over
     *    the samples with the signal kept in float between them: the old four passes converted to int32 and saturated after every stage (about 40 % of this
     *    block's cost, on core 1 for every amp). The samples are used as plain numbers (the filter is linear, so the scale does not matter) and go back to
     *    int32 with saturation once. */
    {
        const float k_q21 = 1.0f / 2097152.0f;
        float cb0[AMP_NBANDS], cb1[AMP_NBANDS], cb2[AMP_NBANDS], ca1[AMP_NBANDS], ca2[AMP_NBANDS], s1[AMP_NBANDS], s2[AMP_NBANDS];
        for (int b = 0; b < AMP_NBANDS; b++) {
            const amp_biquad_t *q = &c->bq[b];
            cb0[b] = (float)q->b0 * k_q21; cb1[b] = (float)q->b1 * k_q21; cb2[b] = (float)q->b2 * k_q21;
            ca1[b] = (float)q->a1 * k_q21; ca2[b] = (float)q->a2 * k_q21;
            s1[b] = g_amp_s1[b]; s2[b] = g_amp_s2[b];
        }
        for (int i = 0; i < n; i++) {
            float x = (float)buf[i];
            for (int b = 0; b < AMP_NBANDS; b++) {
                const float y = cb0[b] * x + s1[b];
                s1[b] = cb1[b] * x - ca1[b] * y + s2[b];
                s2[b] = cb2[b] * x - ca2[b] * y;
                x = y;
            }
            buf[i] = (x > 2147483520.0f) ? 2147483647 : (x < -2147483520.0f) ? (-2147483647 - 1) : (int32_t)x;
        }
        for (int b = 0; b < AMP_NBANDS; b++) { g_amp_s1[b] = s1[b]; g_amp_s2[b] = s2[b]; }
    }

    /* 4. Model loudness trim x master */
    int32_t g = q21_mul(c->out_trim_q21, c->master_q21);
    if (g != PA_Q21_ONE) {
        for (int i = 0; i < n; i++) buf[i] = sat32(((int64_t)buf[i] * g) >> 21);
    }
}

/* Deferral: core 0 sets g_amp_defer_ok before it hands a block over; the amp slot on core 1 then snapshots its coefficients into one of two slots and leaves the
 * tone stack and trim to core 0, which applies them to the block it gets back. Two slots, so core 1 writing the snapshot of block N+1 never touches the one
 * core 0 is reading for block N. */
static amp_coeffs_t         g_amp_snap[2];
static volatile uint8_t     g_amp_defer_ok, g_amp_post_pending, g_amp_post_slot, g_amp_job_slot;

void pa_amp_defer_post(int on) {
    g_amp_defer_ok = on ? 1u : 0u; if (!on) g_amp_post_pending = 0u;
    if (s_wn) pa_wavenet_set_defer(on);
}

/* core 0, before the hand-over: condition the input for the WaveNet block engine if that is what the amp slot will run */
void pa_amp_pre_core0(const int32_t *buf, int n) {
    if (s_wn && s_engine && *s_engine == 1 && s_wn->active) pa_wavenet_pre_core0(s_wn, buf, n, (float)g_amp_next.gain_q21 * (1.0f / (float)PA_Q21_ONE));
    else if (s_wn) pa_wavenet_pre_core0((pa_wavenet_model_t *)0, buf, n, 1.0f);
}

void pa_amp_apply_post(int32_t *buf, int n) {
    if (s_wn) pa_wavenet_post_core0(s_wn, buf, n);                 /* the WaveNet's output stage first, then the tone stack */
    if (!g_amp_post_pending) return;
    g_amp_post_pending = 0u;
    amp_post_run(&g_amp_snap[g_amp_post_slot], buf, n);
}

extern volatile uint8_t g_pa_wn_inline;                  /* pa_wavenet.c */
static void amp_slot_run(fx_node_t *nd, int32_t *buf, int n) {
    /* The slot is fading in or out (the on/off button): the wet/dry crossfade runs right after this function, on this core, so the tone stack, the trim and the WaveNet's
     * output stage must be finished here, not left to core 0 - otherwise the fade mixes the dry signal with an unfinished wet one, or (WaveNet) is lost altogether. */
    const int fading_now = (nd && nd->fade_dir != 0);
    g_pa_wn_inline = (uint8_t)fading_now;
    if (g_amp_live_seq != g_amp_next_seq) {          /* atomic hand-over at block boundary */
        g_amp_live_seq = g_amp_next_seq;
        g_amp_live     = g_amp_next;
    }
    const amp_coeffs_t *c = &g_amp_live;

    /* 1. Pre-amp gain */
    const int wn_pre = s_engine && *s_engine == 1 && s_wn && s_wn->active && pa_wavenet_pre_valid();   /* core 0 conditioned this block, the gain included */
    if (c->gain_q21 != PA_Q21_ONE && !wn_pre) {
        for (int i = 0; i < n; i++) buf[i] = sat32(((int64_t)buf[i] * c->gain_q21) >> 21);
    }

    /* 2. Non-linear amp core: the Q21 NAM GRU with the 512-entry activation
     *    LUTs, or the float WaveNet. */
    if (s_engine && *s_engine == 1 && s_wn && s_wn->active) {
        pa_wavenet_process_block_q21(s_wn, buf, n);
    } else if (s_gru && s_gru->active) {
        nam_gru_run_block(s_gru, buf, n);
    }

    if (g_amp_defer_ok && !fading_now) {             /* core 0 applies the tone stack and the trim to the block that comes back */
        const uint8_t slot = g_amp_job_slot;
        g_amp_job_slot = (uint8_t)(slot ^ 1u);
        g_amp_snap[slot] = *c;
        g_amp_post_slot = slot;
        g_amp_post_pending = 1u;
        return;
    }
    amp_post_run(c, buf, n);
}

fx_node_t *pa_amp_node_create(pa_nam_model_t *gru, pa_wavenet_model_t *wn, int *engine_flag) {
    static fx_node_t node;
    s_gru    = gru;
    s_wn     = wn;
    s_engine = engine_flag;

    nam_model_init(gru);
    pa_wavenet_init_default(wn);

    node.next        = 0;
    node.active      = 1;
    node.frame       = PA_BLOCK_N;
    node.state       = gru;
    node.tag         = "amp";
    node.run         = amp_slot_run;
    node.run_stereo  = 0;
    node.create      = 0;
    node.destroy     = 0;
    node.set_param   = amp_slot_set_param;
    node.reset       = amp_slot_reset;
    node.tail_enabled   = 0;
    node.tail_draining  = 0;
    node.fade_dir       = 0;
    node.pending_destroy = 0;
    node.wet_q21        = PA_Q21_ONE;

    pa_amp_set_model_profile(0);
    g_amp_live = g_amp_next; g_amp_live_seq = g_amp_next_seq;
    return &node;
}

/* ---- the click-free mute crossfade (Blk-02) --------------------------------
 * The UI thread must never rewrite chain state while the audio thread is
 * running it, so a preset/model change is done like this:
 *   UI: request mute -> ISR ramps the gain to 0 over 4 blocks -> ISR stops
 *       touching the chain -> UI applies the preset -> UI releases -> ISR ramps
 *       the gain back in. Without this every preset change clicks.
 * The state machine lives here so the device, the emulator and the WASM build
 * all crossfade identically. */
#define PA_XF_BLOCKS   12                                  /* 12 x 64 = 768 samples = 17.4 ms */
#define PA_XF_STEP_Q21 (PA_Q21_ONE / (PA_XF_BLOCKS * PA_BLOCK_N))
/* PA_XF_WARM: after the swap the chain RUNS again but the gain stays 0 for PA_DC_HOLD_SAMPLES (the amp's output DC blocker only tracks in that time, include/pa_model.h): the amp settles on the live
 * input under the mute and the fade-in then starts from a warm amp. Measured on the unit and in the emulator: with a silent input every swap put a burst of 0.01-0.05 of full scale (the amp's
 * resting DC through a blocker that had restarted at zero), echoed by the delay and the reverb for 400 ms; with the hold it is under 0.002. */
enum { PA_XF_IDLE = 0, PA_XF_FADE_OUT, PA_XF_MUTED, PA_XF_FADE_IN, PA_XF_WARM };

static volatile uint8_t s_xf_state = PA_XF_IDLE;
static int32_t          s_xf_gain  = PA_Q21_ONE;
static int32_t          s_xf_warm_left;

void pa_audio_core_request_mute(void) {
    if (s_xf_state == PA_XF_IDLE || s_xf_state == PA_XF_FADE_IN || s_xf_state == PA_XF_WARM) s_xf_state = PA_XF_FADE_OUT;
}
int pa_audio_core_is_muted(void) { return s_xf_state == PA_XF_MUTED; }
void pa_audio_core_release_mute(void) { if (s_xf_state == PA_XF_MUTED) { s_xf_warm_left = (int32_t)PA_DC_HOLD_SAMPLES; s_xf_state = PA_XF_WARM; } }

/* The public names (declared in pa_fx.h) so that EVERY platform uses this one
 * state machine. Previously the device had a copy in main.c and the WASM shim
 * had another, so the fade that the preset-swap handshake triggered was not the
 * fade the audio path applied. */
void pa_audio_request_mute(void)  { pa_audio_core_request_mute(); }
int  pa_audio_is_muted(void)     { return pa_audio_core_is_muted(); }
void pa_audio_release_mute(void) { pa_audio_core_release_mute(); }

static void pa_audio_core_xf_apply(int32_t *L, int32_t *R, int n) {
    for (int i = 0; i < n; i++) {
        if (s_xf_state == PA_XF_FADE_OUT) {
            s_xf_gain -= PA_XF_STEP_Q21;
            if (s_xf_gain <= 0) { s_xf_gain = 0; s_xf_state = PA_XF_MUTED; }
        } else if (s_xf_state == PA_XF_WARM) {
            if (--s_xf_warm_left <= 0) s_xf_state = PA_XF_FADE_IN;              /* the gain is 0 until then */
        } else if (s_xf_state == PA_XF_FADE_IN) {
            s_xf_gain += PA_XF_STEP_Q21;
            if (s_xf_gain >= PA_Q21_ONE) { s_xf_gain = PA_Q21_ONE; s_xf_state = PA_XF_IDLE; }
        }
        L[i] = q21_mul(L[i], s_xf_gain);
        if (R && R != L) R[i] = q21_mul(R[i], s_xf_gain);
    }
}

/* The DAC master level, 0..100 %, on the audio taper: -40 dB at 0 % to 0 dB at
 * 100 %, as a Q15 gain (0x8000 = unity). Shared by the Settings OUTPUT LEVEL and
 * by the front-panel MASTER pot so the two never disagree. */
static uint8_t s_route;                 /* 0 = identity */
static float   s_gain_l = 1.0f, s_gain_r = 1.0f;

void pa_audio_core_set_routing(uint8_t route, int8_t balance) {
    if (route > 4u) route = 0u;
    if (balance < -50) balance = -50;
    if (balance > 50) balance = 50;
    s_gain_l = (balance > 0) ? (float)(50 - balance) * 0.02f : 1.0f;
    s_gain_r = (balance < 0) ? (float)(50 + balance) * 0.02f : 1.0f;
    s_route = route;                    /* last: the audio interrupt reads the gains once the mode is set */
}

void pa_audio_core_route_block(int32_t *L, int32_t *R, int n) {
    if (!L || !R || L == R) return;
    const uint8_t route = s_route;
    const float gl = s_gain_l, gr = s_gain_r;
    if (route == 0u && gl == 1.0f && gr == 1.0f) return;               /* the identity: not touched */
    for (int i = 0; i < n; i++) {
        int32_t l = L[i], r = R[i];
        switch (route) {
            case 1: l = r = (l >> 1) + (r >> 1); break;
            case 2: r = l; break;
            case 3: l = r; break;
            case 4: { const int32_t t = l; l = r; r = t; } break;
            default: break;
        }
        if (gl != 1.0f) l = (int32_t)((float)l * gl);
        if (gr != 1.0f) r = (int32_t)((float)r * gr);
        L[i] = l; R[i] = r;
    }
}

uint16_t pa_audio_core_master_q15(uint8_t pct) {
    if (pct > 100u) pct = 100u;
    if (pct == 0u) return 0u;
    double q = 32768.0 * pow(10.0, (-40.0 + 0.4 * (double)pct) / 20.0);
    if (q > 32768.0) q = 32768.0;
    return (uint16_t)q;
}

/* A 24-bit peak to the 0..100 % the level meters draw. 24-bit full scale
 * (8388607) is 0 dBFS; the meter spans -45 dB .. 0 dB. */
static uint8_t pa_peak_s24_to_pct(int32_t s24) {
    if (s24 <= 0) return 0;
    float db  = 20.0f * log10f((float)s24 / 8388608.0f);
    int   pct = (int)(((db + 45.0f) / 45.0f) * 100.0f);
    if (pct < 0)   return 0;
    if (pct > 100) return 100;
    return (uint8_t)pct;
}

/* ---- the settings mirror ---------------------------------------------------
 * The persisted settings live here so the device, the emulator and the WASM
 * build all persist the same values. */
static pa_settings_t g_settings;

/* The front-panel input layer. It lives in the firmware (not in the board
 * shim), so the device, the host emulator and the WebAssembly build all turn a
 * button press into the same single action. */
static pa_input_state_t g_input;

static void settings_to_ui(const pa_settings_t *st, pa_ui_ctx_t *ui) {
    ui->home_view_mode   = st->home_view_mode;
    ui->play_mode        = st->play_mode;
    ui->fizz_filter_enabled = st->fizz_level & 3u;
    ui->display_brightness  = st->display_brightness;
    ui->display_auto_dim    = st->display_auto_dim;
    ui->display_battery_pct = st->display_battery_pct;
    ui->theme           = st->theme;
    ui->input_trim_db   = st->input_gain_db;
    ui->gate_thresh_db  = st->gate_db;
    ui->out_level_pct   = st->out_level_pct;
    ui->master_volume   = st->out_level_pct;      /* the MASTER shown is the output level, never the preset volume */
    ui->io_output_type  = st->io_output_type;
    ui->io_cab_on_line  = st->io_cab_on_line;
    ui->usb_sample_rate = st->usb_sample_rate;
    ui->output_limit_mode = st->output_limit_mode;
    ui->usb_loopback      = st->usb_loopback;
    ui->usb_mode          = st->usb_mode > 2u ? 0u : st->usb_mode;
    /* Stored as width + 1 so that a settings record written before the stereo existed (the byte is 0) comes up with the default width, not with mono. */
    ui->stereo_width      = (st->stereo_width == 0u || st->stereo_width > 101u) ? PA_STEREO_WIDTH_DEFAULT : (uint8_t)(st->stereo_width - 1u);
    ui->out_route         = st->out_route > 4u ? 0u : st->out_route;
    ui->out_balance       = (st->out_balance < -50 || st->out_balance > 50) ? 0 : st->out_balance;
    ui->bt_mode         = st->bt_enabled ? 1u : 0u;     /* 0 OFF, 1 BLE, 2 BLE + A2DP (the old record's 0 / 1 keep their meaning) */
    ui->bt_state        = ui->bt_mode ? 1 : 0;                      /* the host builds: broadcasting; the device overwrites it from the radio every tick */
    ui->tuner.mute_output = st->tuner_mute;
    ui->tuner.ref_a4_hz = (st->tuner_ref_hz >= 430 && st->tuner_ref_hz <= 450) ? st->tuner_ref_hz : 440;
    ui->drum_bpm        = st->drum_bpm;
    ui->drum_pattern    = st->drum_pattern;
    ui->drum_volume     = st->drum_volume;
    ui->drum_drums_on   = st->drum_drums_on;
    ui->eq_enabled      = st->eq_enabled;
    for (int b = 0; b < 5; b++) ui->eq_bands_db[b] = st->eq_bands_db[b];
}

static volatile uint8_t s_user_store_changed;     /* set by the sysex handler when a capture / cabinet was stored or deleted, applied by the tick */

static void settings_from_ui(pa_settings_t *st, const pa_ui_ctx_t *ui) {
    st->last_preset     = ui->preset_index;
    st->home_view_mode  = ui->home_view_mode;
    st->play_mode       = ui->play_mode;
    st->fizz_level      = ui->fizz_filter_enabled;
    st->display_brightness  = ui->display_brightness;
    st->display_auto_dim    = ui->display_auto_dim;
    st->display_battery_pct = ui->display_battery_pct;
    st->theme           = ui->theme;
    st->input_gain_db   = ui->input_trim_db;
    st->gate_db         = ui->gate_thresh_db;
    st->out_level_pct   = ui->out_level_pct;
    st->io_output_type  = ui->io_output_type;
    st->io_cab_on_line  = ui->io_cab_on_line;
    st->usb_sample_rate = ui->usb_sample_rate;
    st->output_limit_mode = ui->output_limit_mode;
    st->usb_loopback      = ui->usb_loopback;
    st->usb_mode          = ui->usb_mode;
    st->stereo_width      = (uint8_t)(ui->stereo_width + 1u);
    st->out_route         = ui->out_route;
    st->out_balance       = ui->out_balance;
    st->bt_enabled      = ui->bt_mode > 2u ? 1u : ui->bt_mode;
    st->tuner_mute      = ui->tuner.mute_output;
    st->tuner_ref_hz    = ui->tuner.ref_a4_hz;
    st->drum_bpm        = ui->drum_bpm;
    st->drum_pattern    = ui->drum_pattern;
    st->drum_volume     = ui->drum_volume;
    st->drum_drums_on   = ui->drum_drums_on;
    st->eq_enabled      = ui->eq_enabled;
    for (int b = 0; b < 5; b++) st->eq_bands_db[b] = ui->eq_bands_db[b];
}

/* ---- boot ---------------------------------------------------------------- */
void pa_audio_core_boot(pa_fx_chain_t *chain, pa_ui_ctx_t *ui, pa_usb_ble_ctx_t *usb,
                        pa_drums_t *drums, pa_cpu_meter_t *meter,
                        const pa_board_services_t *bsp) {
    (void)usb; (void)drums; (void)meter; (void)bsp;
    pa_preset_init();
    pa_user_nam_init();
    pa_settings_load(&g_settings);                 /* defaults if blank / corrupt */
    settings_to_ui(&g_settings, ui);
    pa_ui_set_theme(ui->theme);
    if (bsp && bsp->set_backlight) bsp->set_backlight(ui->display_brightness);
    pa_ui_apply_io_settings(ui, chain);
    g_pa_audio.master_vol_q15 = pa_audio_core_master_q15(ui->out_level_pct);

    /* Apply the last-used preset for real - the old firmware showed "P08"
     * at boot while running an un-configured chain. */
    uint8_t idx = (g_settings.last_preset < PA_PRESET_COUNT) ? g_settings.last_preset : 0;
    pa_preset_t pr;
    if (pa_preset_load(idx, &pr) == 0) {
        pa_preset_apply_to_chain(&pr, chain);
        pa_ui_sync_from_preset(ui, &pr);
    }
    ui->preset_index = idx;
    ui->bank_group   = idx / 4u;
    ui->last_input_ms = bsp && bsp->now_ms ? bsp->now_ms() : 0;
    /* The real pool usage, so the first frame does not claim a made-up
     * figure. */
    if (bsp && bsp->pool_used) ui->sram_used_bytes = bsp->pool_used();
    ui->dirty = 1;
}

/* ---- the 60 Hz tick ------------------------------------------------------- */
void pa_audio_core_tick(pa_fx_chain_t *chain, pa_ui_ctx_t *ui, pa_drums_t *drums,
                        pa_usb_ble_ctx_t *usb, pa_cpu_meter_t *meter,
                        const pa_board_services_t *bsp,
                        uint32_t buttons, int32_t enc_delta, const uint16_t *pots) {
    if (bsp && bsp->feed_wdt) bsp->feed_wdt();
    const uint32_t now = (bsp && bsp->now_ms) ? bsp->now_ms() : 0;

    PA_CP(0x61);
    /* 1. Battery: the real SARADC reading, the LiPo curve, two warning levels. */
    if (bsp && bsp->battery_mv) {
        ui->battery_mv = (uint16_t)bsp->battery_mv();
        uint8_t pct;
        if (bsp->battery_adc) {                                                     /* the stock method: the percentage is computed from the averaged ADC count */
            ui->battery_adc = (uint16_t)bsp->battery_adc();
            pct = pa_battery_pct_from_adc(ui->battery_adc);
        } else pct = pa_battery_pct_from_mv(ui->battery_mv);
        if (bsp->charging) {                                                        /* a flip of the charging state redraws at once (the bolt appears and goes) */
            const uint8_t ch = (uint8_t)(bsp->charging() ? 1u : 0u);
            if (ch != ui->charging) { ui->charging = ch; ui->dirty = 1; }
        }
        if (pct != ui->batt_pct) { ui->batt_pct = pct; ui->dirty = 1; }
        uint8_t warn = ui->batt_warn;
        if (pct <= 5u)        warn = 2u;
        else if (pct <= 15u)  warn = (warn == 2u && pct < 8u) ? 2u : 1u;
        else if (pct >= 20u)  warn = 0u;
        if (warn != ui->batt_warn) {
            ui->batt_warn = warn; ui->dirty = 1;
            if (warn > ui->batt_warn_shown) {
                ui->batt_warn_shown = warn;
                ui->dimmed = 0;
                if (bsp->set_backlight) bsp->set_backlight(ui->display_brightness);
                ui->last_input_ms = now;
            }
            if (warn == 0) ui->batt_warn_shown = 0;
        }
        static uint8_t s_blink_phase;
        uint8_t phase = (uint8_t)((now / 400u) & 1u);
        if (ui->batt_warn >= 2 && phase != s_blink_phase) ui->dirty = 1;
        s_blink_phase = phase;
    }

    if (s_user_store_changed) {                                                     /* a capture / cabinet was deleted by the editor: a slot that played it goes back to a stock model */
        s_user_store_changed = 0u;
        pa_ui_drop_user_model(ui, chain, 0u, 0u);
        pa_ui_drop_user_model(ui, chain, 1u, 0u);
    }

    PA_CP(0x62);
    /* 2. MASTER pot -> the DAC master level.
     * The front panel has a master volume pot (SARADC) and the firmware used to
     * read it and then throw the value away, so the knob did nothing. */
#if defined(PA_HAS_MASTER_POT) && PA_HAS_MASTER_POT
    if (g_pa_master_hold && !(pots && pots[0] != 0xFFFFu) && now > 5000u) {      /* the knob never answered: fall back to the saved level */
        g_pa_master_hold = 0;
        pa_evlog(PA_EV_MASTER, now, 0x100u | ui->out_level_pct);
    }
    if (pots && pots[0] != 0xFFFFu) {                       /* 0xFFFF: the knob has not been read yet */
        uint32_t pct = ((uint32_t)pots[0] * 100u) / 4095u;
        if (pct > 100u) pct = 100u;
        /* The LAST CHANGE wins: compare with the reading the knob last set, not with the level, so a level set from the menu or over MIDI stays until the knob is turned (2 % of noise margin). */
        static uint8_t s_pot_last = 255u;
        const int dpct = (s_pot_last == 255u) ? 100 : (int)pct - (int)s_pot_last;
        if (g_pa_master_hold) {
            s_pot_last = (uint8_t)pct;                              /* the first reading: take it whatever the saved level was, and let the output up */
            ui->master_volume = (uint8_t)pct;
            ui->out_level_pct = (uint8_t)pct;
            g_pa_audio.master_vol_q15 = pa_audio_core_master_q15((uint8_t)pct);
            g_pa_master_hold = 0;
            pa_evlog(PA_EV_MASTER, now, pct);
            ui->dirty = 1;
        } else if (dpct >= 2 || dpct <= -2 || (pct == 0u && s_pot_last != 0u) || (pct == 100u && s_pot_last != 100u)) {
            s_pot_last = (uint8_t)pct;
            /* The pot and the Settings OUTPUT LEVEL are the same control on the
             * real device, so the pot updates both and the page follows. */
            ui->master_volume = (uint8_t)pct;
            ui->out_level_pct = (uint8_t)pct;
            g_pa_audio.master_vol_q15 = pa_audio_core_master_q15((uint8_t)pct);
            /* NOT marked as a setting to save (1.17): the pot is read again at every boot (the output stays silent until it has been), so the stored level is never used, and every pause in a turn of
             * the knob used to cost a flash write (the output muted, the interrupts off, the second core parked) that the overload rule then counted as an overload. */
            {
                static uint32_t s_master_dirty_ms;
                if (s_master_dirty_ms == 0u || (uint32_t)(now - s_master_dirty_ms) >= 50u
                    || pct == 0u || pct == 100u) {
                    s_master_dirty_ms = now;
                    ui->dirty = 1;
                }
            }
        }
    }
#endif

    PA_CP(0x63);
    /* 3. The toast / volume-popup timers count down. */
    static uint32_t s_last_tick_ms = 0;
    uint32_t dt = s_last_tick_ms ? (uint32_t)(now - s_last_tick_ms) : 0u;
    s_last_tick_ms = now;
    if (ui->view == PA_UI_VIEW_SAVED_TOAST) {
        if (ui->saved_toast_timer_ms > dt) ui->saved_toast_timer_ms = (uint16_t)(ui->saved_toast_timer_ms - dt);
        else { ui->saved_toast_timer_ms = 0;
               ui->view = (ui->prev_view == PA_UI_VIEW_PARAM_EDIT) ? PA_UI_VIEW_PARAM_EDIT : PA_UI_VIEW_MAIN_GRID;
               ui->dirty = 1; }
    }
    if (ui->show_vol_popup) {
        if (ui->vol_popup_timer_ms > dt) ui->vol_popup_timer_ms = (uint16_t)(ui->vol_popup_timer_ms - dt);
        else { ui->vol_popup_timer_ms = 0; ui->show_vol_popup = 0; ui->dirty = 1; }
    }

    PA_CP(0x64);
    /* 4. USB/BLE, then the input. */
    if (usb) pa_usb_ble_poll(usb);
    PA_CP(0x65);

    /* The USB audio sample rate (Settings > USB Audio: 44.1k / 48k).
     *
     * The row was drawn and the value was persisted, but pa_usb_audio_set_rate()
     * was NEVER called, so the setting did nothing at all: the USB capture and
     * playback always ran at whatever the hardware had negotiated. */
    if (usb && ui) {
        /* Two writers, one value: the Settings row and the host's UAC1 SET_CUR. Each is passed on only when ITS side changed since the last tick, so neither
         * overwrites the other every tick. UAC1 cannot push a rate to the host: the row is the rate the unit offers (GET_CUR) until a host asks for another. */
        static uint8_t  s_ui_prev   = 0xFFu;
        static uint32_t s_host_prev = 0u;
        if (s_ui_prev == 0xFFu) {                          /* first tick: the saved setting is the rate on offer */
            pa_usb_audio_set_rate(usb, ui->usb_sample_rate == 1u ? 44100u : 48000u);
            s_ui_prev = ui->usb_sample_rate; s_host_prev = usb->sample_rate_hz;
        } else if (usb->sample_rate_hz != s_host_prev) {   /* the host changed it */
            s_host_prev = usb->sample_rate_hz;
            ui->usb_sample_rate = (s_host_prev == 48000u) ? 0u : 1u;
            s_ui_prev = ui->usb_sample_rate;
            ui->dirty = 1;
        } else if (ui->usb_sample_rate != s_ui_prev) {     /* the user changed it */
            s_ui_prev = ui->usb_sample_rate;
            pa_usb_audio_set_rate(usb, s_ui_prev == 1u ? 44100u : 48000u);
            s_host_prev = usb->sample_rate_hz;
        }
    }

    /* The output limiter's mode and the USB audio loopback.
     *
     * The limiter mode was hard-wired to the brickwall, so it could not be
     * A/B-ed at all - and the limiter is the LAST stage of the chain, so
     * whether the cab's peaks are limited, softly compressed or left alone
     * changes the whole sound. The USB loopback was fixed ON, with no way to
     * turn it off. */
    pa_fx_chain_set_output_limit_mode(chain, (pa_lmt_mode_t)(ui->output_limit_mode % 3u));
    if (usb) pa_usb_ble_set_loopback(usb, ui->usb_loopback ? 1u : 0u);

    /* The button MASK the UI sees comes from the portable input layer, which
     * turns the raw level into one-tick action pulses, debounces the contacts
     * and synthesises the combo and HOLD bits. The board layer only has to
     * report which buttons are down. */
    pa_input_set_raw(&g_input, buttons);
    g_input.keys_hold_mode = (uint8_t)(ui->play_mode == 1u && ui->view == PA_UI_VIEW_MAIN_GRID);   /* STOMP mode: the four keys act on release, a long press assigns */
    uint32_t mask = pa_input_update(&g_input, now);

    /* The USB/BLE MIDI footswitch events are folded into the same input path
     * the physical buttons use. They were decoded in usb_ble.c and then read by
     * nothing, so Program Change / CC20-27 / CC64-67 did nothing at all. */
    uint32_t btn = mask;
    int32_t  enc = enc_delta;
    if (usb) {
        btn |= usb->midi_pedal_btn_events;
        enc += usb->midi_pedal_enc_delta;
        usb->midi_pedal_btn_events = 0;
        usb->midi_pedal_enc_delta  = 0;
        if (usb->midi_pedal_preset_req >= 0) {
            int p = usb->midi_pedal_preset_req;
            usb->midi_pedal_preset_req = -1;
            if (p >= 0 && p < (int)PA_PRESET_COUNT) pa_ui_load_preset(ui, chain, (uint8_t)p);
            ui->dirty = 1;
        }
        if (usb->midi_pedal_toggle_slot >= 0) {
            int s = usb->midi_pedal_toggle_slot;
            usb->midi_pedal_toggle_slot = -1;
            if (s < PA_SLOT_COUNT && chain->slots[s]) {
                fx_node_t *nd = chain->slots[s];
                pa_fx_chain_set_slot_enabled(chain, (pa_fx_slot_id_t)s, !(nd->active && nd->fade_dir >= 0));
            }
            ui->dirty = 1;
        }
        {   /* the MIDI control events (CC / NRPN from USB or BLE: MIDI-MAP-CUSTOM.md), applied one by one through the code the front panel uses */
            pa_midi_ev_t ev;
            uint32_t guard = 0u;
            while (guard++ < PA_MIDI_EV_N && pa_usb_midi_ev_pop(usb, &ev)) pa_ui_midi_control(ui, chain, &ev);
        }
    }
    PA_CP(0x66);
    pa_ui_handle_inputs(ui, chain, btn, enc, pots);
    PA_CP(0x67);
    pa_fx_chain_collect_garbage(chain);          /* frees blocks the ISR faded out */
    PA_CP(0x6A);

    /* 4b. The IN / OUT level meters.
     *
     * g_pa_audio.input_peak_s24 / output_peak_s24 are filled by the AUDIO path
     * on every platform: the device in the ALNK0 ISR, the emulator and the
     * browser in the jack. Reading them here makes the meters the firmware's
     * own - and it makes them WORK on the hardware, where they were dead:
     * nothing on the device ever wrote ui->in_peak_pct / ui->out_peak_pct, so
     * the meters the Onyx screen draws were always empty. */
    {
        uint8_t in_pct  = pa_peak_s24_to_pct(g_pa_audio.input_peak_s24);
        uint8_t out_pct = pa_peak_s24_to_pct(g_pa_audio.output_peak_s24);
        /* The meters are drawn on the home screens and the tuner only. On the other screens (the preset list, the settings, the editor) a
         * change in the level used to start a redraw 25 times a second for nothing. */
        const int meters_shown = (ui->view == PA_UI_VIEW_MAIN_GRID || ui->view == PA_UI_VIEW_MOVING ||
                                  ui->view == PA_UI_VIEW_SAVE_DIALOG || ui->view == PA_UI_VIEW_TUNER);
        /* ...and even there a redraw is worth it only when the bar moved visibly (4 % or more, or to/from zero) and not more often than
         * about 15 times a second: the meters changed by a percent nearly every tick and kept a frame running all the time. */
        static uint32_t s_meter_ms;
        const int d_in = (int)in_pct - (int)ui->in_peak_pct, d_out = (int)out_pct - (int)ui->out_peak_pct;
        const int moved = (d_in >= 4 || d_in <= -4 || (in_pct == 0u) != (ui->in_peak_pct == 0u) ||
                           d_out >= 4 || d_out <= -4 || (out_pct == 0u) != (ui->out_peak_pct == 0u));
        if (meters_shown && moved && (uint32_t)(now - s_meter_ms) >= 66u) {
            ui->in_peak_pct = in_pct; ui->out_peak_pct = out_pct; ui->dirty = 1; s_meter_ms = now;
        } else if (!meters_shown) {
            ui->in_peak_pct = in_pct; ui->out_peak_pct = out_pct;       /* kept current for when a screen with meters opens */
        }
    }

    {   /* OVERLOAD: require ~250 ms sustained so a fast master/param spike does not flash the banner. */
        extern volatile uint8_t g_pa_stereo_allow;
        static uint8_t s_ol_shown;
        static uint32_t s_ol_until, s_ol_last, s_ol_since, s_xr_prev, s_xr_t0, s_xr_burst_t, s_sw_prev;
        if ((uint32_t)(now - s_xr_t0) >= 1000u) {
            const uint32_t xr = g_pa_audio.xrun_count;
            const uint32_t sw = s_swap_done_seq + s_flash_ops;
            if (pa_xrun_burst(xr - s_xr_prev, sw - s_sw_prev)) s_xr_burst_t = now;
            s_xr_prev = xr; s_sw_prev = sw; s_xr_t0 = now;
        }
        const uint8_t ol = (uint8_t)(g_pa_audio.bypass || !g_pa_stereo_allow || g_pa_gov_level >= 2u
                                     || g_pa_overload_force
                                     || (s_xr_burst_t != 0u && (uint32_t)(now - s_xr_burst_t) < 1500u));
        if (ol) {
            if (s_ol_since == 0u) s_ol_since = now ? now : 1u;
            const int sustained = (g_pa_overload_force || (uint32_t)(now - s_ol_since) >= 250u);
            if (sustained && !s_ol_shown && (s_ol_last == 0u || (uint32_t)(now - s_ol_last) >= 8000u)) {
                ui->overload_ms = 3000u; s_ol_until = now + 3000u; s_ol_last = now;
                s_ol_shown = 1u; ui->dirty = 1;
            }
        } else {
            s_ol_since = 0u;
            s_ol_shown = 0u;
        }
        if (ui->overload_ms && (int32_t)(now - s_ol_until) >= 0) {
            ui->overload_ms = 0u; ui->dirty = 1;
        }
    }

    {   /* The status icons (USB diamond, Bluetooth) are part of the frame: a change of either, or the flash of the Bluetooth icon turning over (0.8 s), draws a new frame. */
        static uint8_t s_usb_prev, s_bt_prev, s_ph_prev;
        const uint8_t ph = (ui->bt_state == 1u) ? (uint8_t)(1u + ((now / 800u) & 1u)) : 0u;
        if ((uint8_t)ui->usb_connected != s_usb_prev || ui->bt_state != s_bt_prev || ph != s_ph_prev) {
            s_usb_prev = (uint8_t)ui->usb_connected; s_bt_prev = ui->bt_state; s_ph_prev = ph; ui->dirty = 1;
        }
    }

    {   /* The tuner screen: the needle follows the pitch, not the level meter. Redraw when what is shown changes (the note, the octave, the smoothed cents, locked or not), at most
         * about 30 times a second. */
        static uint8_t s_t_note, s_t_oct, s_t_lock, s_t_held; static int8_t s_t_cents; static uint32_t s_t_ms;
        if (ui->view == PA_UI_VIEW_TUNER && ui->tuner.active) {
            pa_tuner_ui_tick(&ui->tuner);
            const pa_tuner_state_t *tt = &ui->tuner;
            if ((tt->signal_locked != s_t_lock || tt->held != s_t_held || tt->note_index != s_t_note || (uint8_t)tt->octave != s_t_oct || tt->cents_disp != s_t_cents) && (uint32_t)(now - s_t_ms) >= 33u) {
                s_t_lock = tt->signal_locked; s_t_held = tt->held; s_t_note = tt->note_index; s_t_oct = (uint8_t)tt->octave; s_t_cents = tt->cents_disp; s_t_ms = now;
                ui->dirty = 1;
            }
        }
    }

    {   /* The drum machine pauses while the tuner is open (the drums would be heard over the string, and in LIVE they would reach the tuner's ear) and starts again when the tuner closes, if it was playing */
        static uint8_t s_t_was_active, s_drums_resume;
        const uint8_t t_act = (uint8_t)(ui->tuner.active != 0);
        if (t_act && !s_t_was_active) { s_drums_resume = ui->drum_playing ? 1u : 0u; ui->drum_playing = 0; ui->dirty = 1; }
        else if (!t_act && s_t_was_active) { if (s_drums_resume) ui->drum_playing = 1; s_drums_resume = 0; ui->dirty = 1; }
        s_t_was_active = t_act;
    }

    PA_CP(0x6B);
    /* 5. Groove Station: every UI setting reaches the engine.
     *
     * The redesign has no click mode: the groove style is always the drum
     * pattern the user picked, and the metronome patterns are not in the UI
     * any more (the 9 METRO styles are unreachable from the screen, and the
     * engine's own METRO handling is keyed off the style number, so the old
     * pa_drums_set_click() switch has gone). */
    pa_drums_set_bpm(drums, ui->drum_bpm);
    pa_drums_set_style(drums, (uint8_t)(ui->drum_pattern % PA_DRUM_GROOVE_STYLE_COUNT));
    pa_drums_set_level(drums, ui->drum_volume);
    pa_drums_set_playing(drums, ui->drum_playing);
    /* the playhead shows the step that is sounding (not the next one), so the tall
     * downbeat blocks light exactly on 1, 2, 3 and 4 */
    uint16_t sounding = pa_drums_sounding_step(drums);
    if (ui->view == PA_UI_VIEW_DRUMS && ui->drum_step != sounding) {
        ui->drum_step = sounding; ui->dirty = 1;
    }

    /* 6. Auto-dim (OFF / 30 s / 1 min / 5 min) to 15 % backlight. */
    static const uint32_t k_dim_ms[4] = { 0u, 30000u, 60000u, 300000u };
    uint32_t dim_after = k_dim_ms[ui->display_auto_dim & 3u];
    int32_t idle_ms = (int32_t)(now - ui->last_input_ms);
    if (dim_after && !ui->dimmed && ui->view != PA_UI_VIEW_TUNER && idle_ms > (int32_t)dim_after) {
        ui->dimmed = 1;
        if (bsp && bsp->set_backlight) bsp->set_backlight(15u);
    }

    PA_CP(0x6D);
    /* 7. Persist the settings once the person has stopped changing things: 5 s after the last change, at most once in 8 s (each save quiesces the audio: mute, interrupts off, the second core parked,
     * and every 32nd one rewrites the whole 4 KB sector), and not at all when nothing differs from what was stored last. */
    {
        static uint32_t s_saved_ms; static pa_settings_t s_saved; static uint8_t s_saved_ok;
        /* SIGNED: `now` was read at the START of this tick, and the UI stamps settings_dirty_ms with a fresh clock reading later in the same tick (a tick takes tens to hundreds of ms), so the
         * stamp can be a little AFTER `now`. now - stamp is then a small negative number, which an unsigned subtraction turns into ~4.29 billion: the debounce passed on the spot and every change
         * wrote the flash at once (the overload popup's real cause, together with the writes the master pot caused). */
        if (ui->settings_dirty && (int32_t)(now - ui->settings_dirty_ms) > (int32_t)PA_SETTINGS_QUIET_MS &&
            (s_saved_ms == 0u || (uint32_t)(now - s_saved_ms) >= PA_SETTINGS_MIN_GAP_MS) &&
            !(bsp && bsp->battery_brownout && bsp->battery_brownout())) {
            settings_from_ui(&g_settings, ui);
            if (s_saved_ok && memcmp(&g_settings, &s_saved, sizeof g_settings) == 0) ui->settings_dirty = 0;      /* the same as what the flash holds: nothing to write */
            else if (pa_settings_save(&g_settings) == 0) { ui->settings_dirty = 0; s_saved = g_settings; s_saved_ok = 1u; s_saved_ms = now ? now : 1u; }
            else ui->settings_dirty_ms = now;                    /* retry later */
        }
    }

    /* 7b. A preset / model swap just changed the chain the screen is drawn from (each slot's on/off, fade and tail state are read from its nodes). The redraw the change
     * itself asked for ran BEFORE the audio thread applied the swap (~20 ms later) and drew the old chain: 4 of 20 bands; nothing asked for another one, so the rest of the
     * screen waited 0.6 - 1.1 s for some unrelated trigger (pack 159 bench, tools/preset_latency_pc.py). */
    {   static uint32_t s_swap_seen;
        if (s_swap_done_seq != s_swap_seen) { s_swap_seen = s_swap_done_seq; ui->dirty = 1; }
    }

    /* 8. Publish the telemetry and render. */
    if (bsp && bsp->pool_used) ui->sram_used_bytes = bsp->pool_used();
    if (meter) ui->cpu_total_permille = pa_perf_permille(cpu_meter_total_permille(meter));
    PA_CP(0x68);
    pa_ui_render(ui, chain, meter);
    PA_CP(0x69);
}

void pa_audio_core_flush_settings(pa_ui_ctx_t *ui) {
    if (ui && ui->settings_dirty) {
        settings_from_ui(&g_settings, ui);
        if (pa_settings_save(&g_settings) == 0) ui->settings_dirty = 0;
    }
}

/* ---- deferred swaps ------------------------------------------------------- */
/* What is waiting for the mute. Several requests can pile up while the knob is turned: each amp / cab / effect type that was asked for is kept in its own entry (the old single entry let a
 * second request, an amp pick and then a cab pick a moment later, silently replace the first: the screen said one amp and the output played another), a preset is applied before the model
 * picks that came after it, and a live user cabinet replaces a stock cab pick and the other way round. PA_SWAP_SETTLE_BLOCKS: the output stays faded out until the knob has been quiet for this
 * many blocks, so scrolling through models is ONE silent gap and one swap, not a fade-out, a swap and a half fade-in for every click (the bursts and the flutter). A preset change does not wait. */
#define PA_SWAP_SETTLE_BLOCKS 24u                              /* 24 x 1.45 ms = 35 ms */
static uint8_t s_pend_preset, s_pend_ucab, s_settle;
static uint8_t s_applied_type[PA_SLOT_COUNT];              /* the model each slot was last SWAPPED to (tests and the readout; the UI's own slot_type is what was asked for) */
static uint8_t s_pend_have[PA_SLOT_COUNT], s_pend_type[PA_SLOT_COUNT];
static int swap_pending(void) {
    if (s_pend_preset || s_pend_ucab) return 1;
    for (int i = 0; i < PA_SLOT_COUNT; i++) if (s_pend_have[i]) return 1;
    return 0;
}
volatile uint8_t g_pa_user_cab_active;                         /* 1 while the cabinet is a live one from an upload (the UI shows USER CAB) */
static int32_t  *s_ucab_taps;                                  /* the upload staging buffer, valid until the next upload begins (sysex.c refuses one while a swap is pending) */
static uint32_t  s_ucab_n;
void pa_audio_core_user_store_changed(void) { s_user_store_changed = 1u; }
int pa_audio_core_user_cab_pending(void) { return s_pend_ucab; }
int      pa_audio_core_swap_pending(void) { return swap_pending(); }              /* the screen holds its redraw while a swap waits for the mute (ui_app.c pa_ui_render) */
uint8_t  pa_audio_core_applied_type(uint8_t slot) { return slot < PA_SLOT_COUNT ? s_applied_type[slot] : 0xFFu; }
uint32_t pa_audio_core_swap_count(void) { return s_swap_done_seq; }

/* A live user cabinet (an upload to PA_UPLOAD_CAB): raw Q18 taps, int32 little endian, 16..1024 of them. Loaded in the audio thread under the same fade as a model change. */
int pa_audio_core_load_user_cab(uint8_t *buf, uint32_t len) {
    if (!buf || (len & 3u) != 0u || len < 16u * 4u || len > PA_STOCK_CAB_TAPS * 4u) return -2;
    if (((uintptr_t)buf & 3u) != 0u) return -3;
    if (swap_pending()) return -5;                               /* another swap is still pending: send it again */
    s_ucab_taps = (int32_t *)buf;
    s_ucab_n    = len / 4u;
    s_pend_ucab = 1u;
    s_settle    = 0u;
    pa_evlog(PA_EV_SWAP_REQ, PA_SLOT_CAB, 0xFFu);
    pa_audio_core_request_mute();
    return 0;
}
static pa_preset_t    s_swap_preset;

/* The DSP half of a model change (the UI half is the caller's). This lives in
 * the core because it touches the AMP/CAB state and must run in the audio
 * thread. */
static void pa_audio_core_apply_model(pa_fx_chain_t *chain, pa_ui_ctx_t *ui,
                                      uint8_t slot, uint8_t type) {
    extern pa_nam_model_t     g_nam_model;
    extern pa_wavenet_model_t g_wn_model;
    extern int                g_active_amp_engine;

    if (slot == PA_SLOT_AMP) {
        uint8_t effective_type = type;
        if (type < PA_STOCK_AMP_COUNT) {
            if (chain->slots[PA_SLOT_AMP] && chain->slots[PA_SLOT_AMP]->state)
                pa_stock_load_amp(type, (pa_nam_model_t *)chain->slots[PA_SLOT_AMP]->state, NULL);
            g_active_amp_engine = 0;
            g_wn_model.active = 0;
            if (g_wn_model.is_amp_cab) {
                pa_fx_chain_set_slot_enabled_now(chain, PA_SLOT_CAB, 1);
            }
            g_wn_model.is_amp_cab = 0;
        } else {
            uint8_t u_idx = (uint8_t)(type - PA_STOCK_AMP_COUNT);
            const int ld_rc = pa_user_nam_load_into_dsp(u_idx, &g_nam_model, &g_wn_model);
            if (ld_rc != 0) {
                pa_evlog(PA_EV_LOAD_FAIL, (uint32_t)(-ld_rc), u_idx);          /* otherwise the fallback below is silent */
                /* Fallback to stock amp model 0 if user capture is missing or corrupted */
                pa_stock_load_amp(0, &g_nam_model, NULL);
                g_active_amp_engine = 0;
                g_wn_model.active = 0;
                g_wn_model.is_amp_cab = 0;
                effective_type = 0;
                if (ui) ui->slot_type[PA_SLOT_AMP] = 0;
            } else {
                if (ui) pa_user_nam_get_amp_defaults(u_idx, ui->slot_params[PA_SLOT_AMP]);      /* ui is the module's s_ui: null until the UI context is registered */
                if (g_wn_model.is_amp_cab) {
                    /* Baked-in cabinet: bypass the CAB slot so audio is not filtered twice */
                    pa_fx_chain_set_slot_enabled_now(chain, PA_SLOT_CAB, 0);
                }
            }
        }
        pa_amp_set_model_profile(effective_type);
        for (int k = 0; ui && k < 6; k++)
            pa_fx_chain_set_param(chain, PA_SLOT_AMP, k, ui->slot_params[PA_SLOT_AMP][k]);
        if (g_active_amp_engine == 0 && chain->slots[PA_SLOT_AMP] && chain->slots[PA_SLOT_AMP]->state) {
            pa_nam_prewarm((const pa_nam_model_t *)chain->slots[PA_SLOT_AMP]->state);
        }
    } else if (slot == PA_SLOT_CAB) {
        static int32_t c_taps[PA_STOCK_CAB_TAPS];
        uint32_t n_taps = PA_STOCK_CAB_TAPS;
        if (pa_cab_load(type, c_taps, &n_taps) == 0 && chain->slots[PA_SLOT_CAB]) {
            fx_cab_ir_load_q18(chain->slots[PA_SLOT_CAB], c_taps, n_taps);
            fx_cab_ir_set_stock_trim(chain->slots[PA_SLOT_CAB], type);
        }
    } else if (slot < PA_SLOT_COUNT) {
        /* DRIVE, MOD, DLY, REV, EQ, DYN: a type change of a slot that is sounding. Done live it reset the effect's state in the middle of the signal: the emulator click probe measured
         * MOD 5 of 7, DLY 3 of 9 and EQ 2 of 3 type changes at 7-68x the steady step. Here the output is muted, like an amp change. */
        pa_fx_chain_set_slot_type(chain, (pa_fx_slot_id_t)slot, type);
    }
}

/* Select a user NAM capture the same way the effect menu does.
 *
 * The capture has to go through the model's apply path, because that is what
 * resets the per-model trim: a user capture carries its own makeup gain, and
 * the stock amp's trim (+9 dB on some models) must NOT also be applied on
 * top of it. Loading a capture straight into the DSP skipped that, so a
 * capture loaded from the page was much louder than the same capture picked
 * from the effect menu. */
void pa_audio_core_select_user_nam(pa_ui_ctx_t *ui, pa_fx_chain_t *chain,
                                   uint8_t user_idx) {
    if (!ui || !chain) return;
    if (user_idx >= pa_user_nam_get_count()) return;
    pa_audio_core_request_model(ui, chain, PA_SLOT_AMP,
                                (uint8_t)(PA_STOCK_AMP_COUNT + user_idx));
}

void pa_audio_core_request_preset(const pa_preset_t *p) {
    if (!p) return;
    s_swap_preset = *p;
    s_pend_preset = 1u;
    s_pend_ucab   = 0u;                                          /* a preset brings its own cabinet and models: earlier picks are void */
    for (int i = 0; i < PA_SLOT_COUNT; i++) s_pend_have[i] = 0u;
    s_settle      = 0u;
    pa_evlog(PA_EV_SWAP_REQ, 0xFFu, 0u);
    pa_audio_core_request_mute();
}

void pa_audio_core_request_model(pa_ui_ctx_t *ui, pa_fx_chain_t *chain,
                                 uint8_t slot, uint8_t type) {
    (void)ui; (void)chain;
    if (slot >= PA_SLOT_COUNT) return;
    s_pend_have[slot] = 1u;
    s_pend_type[slot] = type;
    if (slot == PA_SLOT_CAB) s_pend_ucab = 0u;                   /* a stock cab pick replaces a live cabinet that was still waiting */
    s_settle = (uint8_t)PA_SWAP_SETTLE_BLOCKS;                   /* every request restarts the quiet time */
    pa_evlog(PA_EV_SWAP_REQ, slot, type);
    pa_audio_core_request_mute();
}

/* Runs in the AUDIO thread, at the start of a block, once the gain has
 * reached zero. */
/* The second core (src/sys/smp_cpu1.c) overrides this: wait until core 1 is idle and drop its in-flight block. */
__attribute__((weak)) void pa_smp_wait_idle(void) { }
__attribute__((weak)) void pa_sysex_set_editor_providers(pa_sysex_state_fn st, pa_sysex_names_fn nm) { (void)st; (void)nm; }      /* a host test without sysex.c */

/* ---- the flash quiesce (pa_spi_nor_begin / end, device only) -------------------------------------------------------------------------------------------------------------------------------
 * The flash controller leaves XIP while the driver runs, and every instruction of the amp, the audio interrupt, the radio and core 1 is fetched from the flash. So: the output is faded to silence
 * (the master hold: a one-block slew), the fade is waited out (3 audio blocks, bounded to 20 ms), the interrupts are masked, core 1 finishes the block it has and is parked (with the interrupts
 * masked nothing can give it another), the transmit DMA buffer is zeroed (the DMA keeps replaying it while the CPU is away), and only then does the driver run. Before the main loop starts (the boot
 * init) the interrupts are already off and the audio is not running: nothing to do. */
volatile uint8_t g_pa_irq_on;                                   /* main.c sets it when the main loop starts (the interrupts are on from there) */
#if !defined(PA_HOST_BUILD) && !defined(PA_HOST_SIM) && !defined(PA_EMU_DLL) && !defined(__wasm__) && !defined(__EMSCRIPTEN__)
extern void pa_alnk_silence_tx(void);
static uint8_t s_fl_prev_hold;
void pa_audio_core_flash_begin(void) {
    if (!g_pa_irq_on) return;
    s_fl_prev_hold = g_pa_master_hold;
    g_pa_master_hold = 1u;
    {   const uint32_t c0 = g_pa_audio.irq_count, t0 = plat_ms();
        while ((uint32_t)(g_pa_audio.irq_count - c0) < 3u && (uint32_t)(plat_ms() - t0) < 20u) { } }
    __asm__ volatile("cli" ::: "memory");
    pa_smp_wait_idle();
    pa_alnk_silence_tx();
}
void pa_audio_core_flash_end(void) {
    if (!g_pa_irq_on) return;
    __asm__ volatile("csync\n\tsti" ::: "memory");
    g_pa_master_hold = s_fl_prev_hold;                           /* the output comes back on a one-block slew */
    s_flash_ops++;
    { extern volatile uint8_t g_pa_gov_swap; g_pa_gov_swap = 1u; }      /* audio_alnk.c: the load governor must not count the block the flash work delayed */
}
#else
void pa_audio_core_flash_begin(void) { }
void pa_audio_core_flash_end(void) { }
#endif

#if !defined(PA_HOST_BUILD) && !defined(PA_HOST_SIM) && !defined(PA_EMU_DLL) && !defined(__wasm__)
/* The CPU load shown on the device is MEASURED: the audio interrupt's average time (core 0) or core 1's amp job, whichever is higher, as
 * a share of the 1451 us block. The cycle meter's calibrated table (used by the emulator) says nothing about the real device. */
extern volatile uint32_t g_pa_isr_avg_us;
extern uint32_t pa_smp_cpu1_load_permille(void);
static uint32_t pa_perf_permille(uint32_t modelled) {
    if (g_pa_isr_avg_us == 0u) return modelled;
    uint32_t c0 = (g_pa_isr_avg_us * 1000u) / PA_BLOCK_US;
    uint32_t c1 = pa_smp_cpu1_load_permille();
    uint32_t m = c0 > c1 ? c0 : c1;
    return m > 999u ? 999u : m;
}
#else
static uint32_t pa_perf_permille(uint32_t modelled) { return modelled; }
#endif

#if !defined(PA_HOST_BUILD) && !defined(PA_HOST_SIM) && !defined(PA_EMU_DLL) && !defined(__wasm__)
extern void pa_alnk_silence_tx(void);                    /* audio_alnk.c */
#else
static void pa_alnk_silence_tx(void) { }
#endif

__attribute__((weak)) void pa_smp_swap_note(uint32_t kind, uint32_t a, uint32_t b, uint32_t wait_ticks, uint32_t apply_ticks) {
    (void)kind; (void)a; (void)b; (void)wait_ticks; (void)apply_ticks;
}

static void pa_audio_core_run_pending(pa_fx_chain_t *chain, pa_ui_ctx_t *ui) {
    if (!swap_pending()) return;
    if (s_xf_state != PA_XF_MUTED) return;      /* the gain has not reached 0 yet */
    if (s_settle) { s_settle--; return; }       /* still being turned: stay silent until the knob has been quiet for PA_SWAP_SETTLE_BLOCKS */
    pa_alnk_silence_tx();                       /* the swap below outlasts this block: whatever the DMA replays must be silence */
#if !defined(PA_HOST_BUILD) && !defined(PA_HOST_SIM)
    const uint32_t ev_t0 = plat_ticks24();
    const uint32_t t_w0 = plat_ticks24();
#endif
    pa_smp_wait_idle();                         /* a swap rewrites the slots: core 1 must not be running them */
#if !defined(PA_HOST_BUILD) && !defined(PA_HOST_SIM)
    const uint32_t t_w = plat_ticks24() - t_w0;
    const uint32_t t_a0 = plat_ticks24();
#endif
    uint32_t ev_slot = 0xFFu, kind = 2u, a = 0u, b = 0u;
    const int had_preset = s_pend_preset, had_ucab = s_pend_ucab;
    s_pend_preset = 0u; s_pend_ucab = 0u;                     /* taken: a request that comes in while this swap runs (it takes tens of ms) stays pending and gets its own turn below */
    if (had_preset) {
        a = (uint32_t)s_swap_preset.amp_model_idx; b = (uint32_t)s_swap_preset.cab_model_idx; kind = 1u;
        pa_preset_apply_to_chain(&s_swap_preset, chain);
        g_pa_user_cab_active = 0u;                            /* the preset's own cabinet is loaded */
    }
    if (had_preset) { s_applied_type[PA_SLOT_AMP] = s_swap_preset.amp_model_idx; s_applied_type[PA_SLOT_CAB] = s_swap_preset.cab_model_idx; }
    for (int sl = 0; sl < PA_SLOT_COUNT; sl++) {              /* the picks made since (or without) a preset, in slot order */
        if (!s_pend_have[sl]) continue;
        const uint8_t ty = s_pend_type[sl];
        s_pend_have[sl] = 0u;
        s_applied_type[sl] = ty;
        if (!had_preset) { ev_slot = (uint32_t)sl; a = (uint32_t)sl; b = (uint32_t)ty; }
        pa_audio_core_apply_model(chain, ui, (uint8_t)sl, ty);
        if (sl == PA_SLOT_CAB) g_pa_user_cab_active = 0u;
    }
    if (had_ucab) {
        extern void fx_cab_ir_normalise_q18(int32_t *taps_q18, uint32_t num_taps);
        fx_cab_ir_normalise_q18(s_ucab_taps, s_ucab_n);       /* a third-party IR has no entry in the stock trim table: the same loudness as the reference cabinet */
        fx_cab_ir_load_q18(chain->slots[PA_SLOT_CAB], s_ucab_taps, s_ucab_n);
        fx_cab_ir_set_trim_db_x10(chain->slots[PA_SLOT_CAB], 0);
        g_pa_user_cab_active = 1u;
        if (!had_preset) { ev_slot = PA_SLOT_CAB; a = PA_SLOT_CAB; b = 0xFFu; kind = 2u; }
    }
#if !defined(PA_HOST_BUILD) && !defined(PA_HOST_SIM)
    const uint32_t t_a = plat_ticks24() - t_a0;
    pa_smp_swap_note(kind, a, b, t_w, t_a);
    pa_evlog(PA_EV_SWAP_DONE, (plat_ticks24() - ev_t0) / 24u, ev_slot);
    { extern volatile uint8_t g_pa_gov_swap; g_pa_gov_swap = 1u; }      /* audio_alnk.c: the load governor must not count this block */
#else
    (void)ev_slot; (void)kind; (void)a; (void)b;
#endif
    s_swap_done_seq = s_swap_done_seq + 1u;
    pa_audio_core_release_mute();
    if (swap_pending()) pa_audio_core_request_mute();         /* something was asked for during the swap: fade out again and apply it (the mute state would otherwise never see it) */
}

/* ---- the "jack": the device's audio I/O path, made portable ----------------
 * 24-bit samples in, 24-bit samples out, with the input gain, the master
 * volume slew and the 1-LSB TPDF dither. On the device this is fed by the
 * ALNK0 DMA; in the WebAssembly build it is fed by the browser's audio (on
 * Windows that IS Windows Audio / WASAPI, and it can be an ASIO interface
 * through a bridge driver); in the host emulator it is fed by PortAudio.
 *
 * The firmware therefore sees exactly the same 24-bit samples, with the same
 * input gain, the same volume law and the same dither, in every build - so
 * what the emulator plays is what the device plays. */
extern pa_audio_state_t g_pa_audio;

/* The runtime the jack path runs on. Bound once at boot by whichever platform
 * owns the chain, so the jack itself needs no platform arguments and the
 * device, the emulator and the WebAssembly build all run the SAME code. */
static pa_fx_chain_t   *s_chain = 0;
static pa_ui_ctx_t     *s_ui    = 0;
static pa_drums_t      *s_drums = 0;
static pa_usb_ble_ctx_t *s_usb   = 0;

/* The device allows a custom block callback to replace the amp chain (that is
 * how the ALNK driver is driven on the hardware). The hook lives here so all
 * three platforms honour it identically. */
static pa_audio_block_cb_t s_block_cb = 0;

static uint32_t editor_state_cb(uint8_t *out, uint32_t cap) { return pa_ui_state_snapshot(s_ui, s_chain, out, cap); }
static uint32_t editor_names_cb(uint8_t kind, uint8_t start, uint8_t *out, uint32_t cap) { return pa_ui_name_list(kind, start, out, cap); }

void pa_audio_core_bind(pa_fx_chain_t *chain, pa_ui_ctx_t *ui,
                        pa_drums_t *drums, pa_usb_ble_ctx_t *usb) {
    s_chain = chain;
    s_ui    = ui;
    s_drums = drums;
    s_usb   = usb;
    pa_sysex_set_editor_providers(editor_state_cb, editor_names_cb);          /* the editor page reads the state and the name lists through the sysex dispatcher */
}

void pa_audio_core_set_block_cb(pa_audio_block_cb_t cb) { s_block_cb = cb; }

/* The jack's power-on state: unity input gain and the master level the UI
 * asks for. The device's ALNK driver resets the whole audio state when it
 * starts, the emulator and the WebAssembly build start from a zeroed struct,
 * so without this the first block would be silent (a zero input gain) and the
 * volume slew would ramp up from nothing. */
/* ---- the power-on mute (the pop at power-on) ------------------------------------------------------------------------------------------------------------------------------------------------
 * Two things made a noise when the link started. (1) The first block went out at the saved master level: the ALNK driver put the level back after this function had set it to 0, so block 1 was
 * whatever the converter's input delivers while it starts, through the amp, fading from full level to 0 in 64 samples. (2) When the knob had been read the level went from 0 to its value in ONE
 * block (1.45 ms), on top of an input stage that is still settling: a step, not a fade.
 * Now: after the link starts the output is digital silence for s_boot_quiet blocks whatever the hold says (the board keeps its output stage off for the first part of that, audio_alnk.c), and
 * the first rise from that silence is limited to PA_BOOT_RAMP_STEP_Q15 a block. Armed on the device only (pa_audio_core_init_jack); the emulator and the browser start at their level as before. */
#define PA_BOOT_QUIET_BLOCKS  207u                 /* 207 x 1.451 ms = 300 ms of silence after the link starts */
#define PA_BOOT_RAMP_STEP_Q15 512                  /* a block: 0 to full level in 64 blocks = 93 ms */
static uint16_t s_boot_quiet;                      /* blocks of forced silence still to go */
static uint8_t  s_boot_ramp;                       /* 1 = the first rise from silence has not finished: rate-limited */

void pa_audio_core_boot_mute_arm(uint32_t quiet_blocks) {
    s_boot_quiet = (uint16_t)(quiet_blocks > 0xFFFFu ? 0xFFFFu : quiet_blocks);
    s_boot_ramp = 1u;
    g_pa_audio.master_vol_cur_q15 = 0;
}
int pa_audio_core_boot_muted(void) { return s_boot_quiet != 0u; }

void pa_audio_core_init_jack(pa_ui_ctx_t *ui) {
    g_pa_audio.input_gain_q15 = 0x8000u;               /* unity (1.0) */
    if (ui) g_pa_audio.master_vol_q15 = pa_audio_core_master_q15(ui->out_level_pct);
    g_pa_audio.master_vol_cur_q15 = g_pa_audio.master_vol_q15; /* no ramp from silence */
#if !defined(PA_HOST_BUILD) && !defined(PA_HOST_SIM) && !defined(PA_EMU_DLL) && !defined(__wasm__)
    pa_audio_core_boot_mute_arm(PA_BOOT_QUIET_BLOCKS); /* the device: silence while the converter and the output stage start, then a fade-in */
#if defined(PA_HAS_MASTER_POT) && PA_HAS_MASTER_POT
    g_pa_master_hold = 1;                              /* the device: silent until the knob is read */
#endif
#endif
    g_pa_audio.bypass = 0;
}

static q28_t   s_in_mono_q28[PA_AUDIO_BLOCK_FRAMES];
static q28_t   s_out_l_q28[PA_AUDIO_BLOCK_FRAMES];
static q28_t   s_out_r_q28[PA_AUDIO_BLOCK_FRAMES];
static int32_t s_l_q21[PA_AUDIO_BLOCK_FRAMES];

/* The output DC blocker's state (see step 5 of pa_audio_core_process_block). */
static int32_t s_out_dc_x1, s_out_dc_y1;
static int32_t s_out_dc_x1r, s_out_dc_y1r;
static int32_t s_r_q21[PA_AUDIO_BLOCK_FRAMES];

static pa_spread_t s_spread;            /* the stereo spread (src/dsp/pa_stereo_spread.h) */

/* Sign-extend a 24-bit I2S slot [23:0] to a 32-bit signed int. */
static inline int32_t s24_sign_extend(int32_t raw) { return (int32_t)((uint32_t)raw << 8) >> 8; }   /* the shift up is unsigned: a signed shift into the sign bit is undefined */
/* Saturate to the 24-bit signed range [-8388608, +8388607]. */
static inline int32_t s24_saturate(int32_t x) {
    if (x >  0x007FFFFF) return  0x007FFFFF;
    if (x < -0x00800000) return -0x00800000;
    return x;
}

PA_HOT void pa_audio_core_process_half(const int32_t *rx_half, int32_t *tx_half) {
    PA_ISR_CP(0x11);
    int32_t in_peak  = 0;
    int32_t out_peak = 0;
    uint32_t in_gain = g_pa_audio.input_gain_q15;
    uint32_t out_vol = g_pa_master_hold ? 0u : g_pa_audio.master_vol_q15;     /* silent until the MASTER knob has been read (see g_pa_master_hold) */
    if (s_boot_quiet) {                                                       /* the power-on mute: silence while the link and the output stage start (the chain runs, so it is warm) */
        s_boot_quiet--;
        out_vol = 0u;
    } else if (s_boot_ramp) {                                                 /* ... then the first rise is a fade, not a one-block step */
        const uint32_t lim = (uint32_t)g_pa_audio.master_vol_cur_q15 + (uint32_t)PA_BOOT_RAMP_STEP_Q15;
        if (out_vol > lim) out_vol = lim;
        else if (!g_pa_master_hold) s_boot_ramp = 0u;                         /* the level is reached: from here the knob's own one-block slew */
    }

    /* 1. Unpack 64 stereo frames from the RX half-buffer. The guitar input is
     *    on the Left slot (rx_half[2*i]). */
    for (int i = 0; i < (int)PA_AUDIO_BLOCK_FRAMES; i++) {
        int32_t s24 = s24_sign_extend(rx_half[2 * i]);
        if (in_gain != 0x8000u) {
            s24 = (int32_t)(((int64_t)s24 * (int32_t)in_gain) >> 15);
            s24 = s24_saturate(s24);
        }
        int32_t abs_s = (s24 < 0) ? -s24 : s24;
        if (abs_s > in_peak) in_peak = abs_s;
        s_in_mono_q28[i] = (q28_t)(s24 << 5);      /* 24-bit Q23 -> Q28 */
    }
    g_pa_audio.input_peak_s24 = in_peak;
    {   /* INSTRUMENTATION (boot input noise): the raw input peak and mean of every 32 blocks (46 ms) for the first 192 buckets (8.8 s after the audio starts), for tools/boot_input_trace_pc.py. The peak is
         * the 24-bit value >> 8 (0..32767); the mean is the same scale, signed: a steady offset (the input stage settling) shows in the mean, noise and clicks in the peak. */
        static uint32_t s_tr_blk, s_tr_n; static int32_t s_tr_peak; static int64_t s_tr_sum;
        if (s_tr_n < PA_IN_TRACE_N) {
            int64_t sm = 0;
            for (int i = 0; i < (int)PA_AUDIO_BLOCK_FRAMES; i++) sm += s24_sign_extend(rx_half[2 * i]);
            s_tr_sum += sm;
            if (in_peak > s_tr_peak) s_tr_peak = in_peak;
            if (++s_tr_blk >= 32u) {
                g_pa_in_trace[s_tr_n] = (uint16_t)(s_tr_peak >> 8);
                g_pa_in_trace_mean[s_tr_n] = (int16_t)((s_tr_sum / (int64_t)(32u * PA_AUDIO_BLOCK_FRAMES)) >> 8);
                s_tr_n++; s_tr_blk = 0u; s_tr_peak = 0; s_tr_sum = 0;
            }
        }
    }
    PA_ISR_CP(0x12);

    /* 2. Run the DSP chain (or bypass). */
    if (g_pa_audio.bypass) {
        memcpy(s_out_l_q28, s_in_mono_q28, sizeof(s_in_mono_q28));
        memcpy(s_out_r_q28, s_in_mono_q28, sizeof(s_in_mono_q28));
    } else if (s_block_cb) {
        /* A custom block callback replaces the amp chain (the device allows
         * this; the emulator and the browser do not use it). */
        s_block_cb(s_in_mono_q28, s_out_l_q28, s_out_r_q28, (int)PA_AUDIO_BLOCK_FRAMES);
    } else {
        for (int i = 0; i < (int)PA_AUDIO_BLOCK_FRAMES; i++) {
            int32_t s = s_in_mono_q28[i] >> 7;    /* Q28 -> Q21 */
            s_l_q21[i] = s;
            s_r_q21[i] = s;
        }
        PA_ISR_CP(0x13);
#if !defined(PA_HOST_BUILD) && !defined(PA_HOST_SIM)
        const uint32_t pb_t0 = plat_ticks24();
#endif
        pa_audio_core_process_block(s_chain, s_ui, s_drums, s_usb,
                                    s_l_q21, s_r_q21, (int)PA_AUDIO_BLOCK_FRAMES);
#if !defined(PA_HOST_BUILD) && !defined(PA_HOST_SIM)
        pa_timing(1, plat_ticks24() - pb_t0);
#endif
        PA_ISR_CP(0x14);
        for (int i = 0; i < (int)PA_AUDIO_BLOCK_FRAMES; i++) {
            s_out_l_q28[i] = (q28_t)(s_l_q21[i] << 7);
            s_out_r_q28[i] = (q28_t)(s_r_q21[i] << 7);
        }
    }

    /* 3. Q28 -> 24-bit, with the master volume on a linear slew and a 1-LSB
     *    high-pass TPDF dither at the physical output stage. */
    int32_t v_start = (int32_t)g_pa_audio.master_vol_cur_q15;
    int32_t v_target = (int32_t)out_vol;
    int32_t v_step = (v_target - v_start) / (int)PA_AUDIO_BLOCK_FRAMES;
    int32_t v_cur = v_start;

    for (int i = 0; i < (int)PA_AUDIO_BLOCK_FRAMES; i++) {
        v_cur += v_step;
        int64_t acc_l = (int64_t)s_out_l_q28[i] * v_cur;
        int64_t acc_r = (int64_t)s_out_r_q28[i] * v_cur;
        int32_t dither_l = pa_prng_tpdf() << 11;
        int32_t dither_r = pa_prng_tpdf() << 11;
        int32_t l24 = (int32_t)((acc_l + (1LL << 19) + dither_l) >> 20);
        int32_t r24 = (int32_t)((acc_r + (1LL << 19) + dither_r) >> 20);
        l24 = s24_saturate(l24);
        r24 = s24_saturate(r24);
        int32_t abs_l = (l24 < 0) ? -l24 : l24;
        (void)abs_l;
        {   /* The OUT meter reads the signal BEFORE the master volume (q28 >> 5 is the 24-bit value at master 1.0), the way the IN meter reads the input before anything: the knob sets how loud the
             * headphones are, not how hot the signal is, and a bar that followed the knob never matched the input bar. The louder of the two channels (USB playback can be on either). */
            const int32_t pl = (int32_t)(s_out_l_q28[i] >> 5), pr = (int32_t)(s_out_r_q28[i] >> 5);
            const int32_t al = pl < 0 ? -pl : pl, ar = pr < 0 ? -pr : pr;
            if (al > out_peak) out_peak = al;
            if (ar > out_peak) out_peak = ar;
        }
        tx_half[2 * i + 0] = l24 & 0x00FFFFFF;
        tx_half[2 * i + 1] = r24 & 0x00FFFFFF;
    }
    g_pa_audio.master_vol_cur_q15 = (uint16_t)v_target;
    g_pa_audio.output_peak_s24 = out_peak;
    g_pa_audio.irq_count++;
    PA_ISR_CP(0x15);
}

/* ---- shared block processing ---------------------------------------------- */

#if !defined(PA_HOST_BUILD) && !defined(PA_HOST_SIM)
#define PT_VAR(v) uint32_t v = plat_ticks24()
#define PT_SET(v) ((v) = plat_ticks24())
#else
#define PT_VAR(v) uint32_t v = 0u
#define PT_SET(v) ((void)(v))
#endif
static pa_mixlim_t s_mixlim;     /* the guitar + drums sum limiter (zeroed: inits itself) */

int32_t pa_soft_ceiling_q21(int32_t x) {
    /* Above -1 dBFS compress 4:1 up to a hard ceiling of 0 dBFS (only active
     * when drums/USB push the sum over). */
    const int32_t knee = 1869214, ceil = PA_Q21_ONE - 1;
    if (x >  knee) { x = knee + ((x - knee) >> 2); if (x >  ceil) x =  ceil; }
    if (x < -knee) { x = -knee - ((-knee - x) >> 2); if (x < -ceil) x = -ceil; }
    return x;
}

PA_HOT void pa_audio_core_process_block(pa_fx_chain_t *chain, pa_ui_ctx_t *ui,
                                 pa_drums_t *drums, pa_usb_ble_ctx_t *usb,
                                 int32_t *buf_L, int32_t *buf_R, int n) {
    /* 0. Apply a pending swap (preset or model) - this runs in the AUDIO thread,
     *    at a block boundary, once the output gain has reached zero, so the
     *    chain is never mutated while the audio is running it. */
    PA_ISR_CP(0x21);
    pa_audio_core_run_pending(chain, ui);
    PA_ISR_CP(0x22);

    /* 1. Chromatic Tuner (pre-FX clean guitar signal on the left channel) */
    if (ui && ui->tuner.active) {
        pa_tuner_process_block(&ui->tuner, buf_L, n);
        if (ui->tuner.mute_output) {
            for (int i = 0; i < n; i++) {
                buf_L[i] = 0;
                if (buf_R) buf_R[i] = 0;
            }
            if (drums) pa_drums_mix_block(drums, buf_L, n);
            if (buf_R) {
                for (int i = 0; i < n; i++) buf_R[i] = buf_L[i];
            }
            if (chain && chain->meter) {
                cpu_meter_block_done(chain->meter);
                ui->cpu_total_permille = pa_perf_permille(cpu_meter_total_permille(chain->meter));
            }
            return;
        }
    }

    /* 2. Run the full 8-slot chain in stereo (respects the custom slot order,
     *    true stereo MOD/DLY/REV) */
    PA_ISR_CP(0x23);
    PT_VAR(pt0); uint32_t pt1 = pt0, pt2 = pt0, pt3 = pt0, pt4 = pt0, pt5 = pt0;      /* one read of the timer, not six (each one is set again where its stage ends) */
    /* The tuner is open and the output is not muted: the stock plays the dry guitar at the tuner's bypass volume, no effects. The whole chain (up to 1.2 ms of the
     * 1.45 ms block with the heavy presets) would run for nothing and leave the screen almost no CPU - the tuner needle was choppy exactly then. */
    /* The USB audio mode: in INTERFACE modes the recording is the dry guitar as it came in, taken before the chain touches the block. */
    static int32_t s_dry[64];
    const uint8_t usb_mode = (ui && usb && n <= 64) ? ui->usb_mode : 0u;
    if (usb_mode) for (int i = 0; i < n; i++) s_dry[i] = buf_L[i];
    const int tuner_dry = (ui && ui->tuner.active);
    static uint8_t s_was_tuner_dry;
    if (s_was_tuner_dry && !tuner_dry) pa_smp_wait_idle();       /* back to the effects: the block core 1 still holds is from before the tuner */
    s_was_tuner_dry = (uint8_t)tuner_dry;
    static uint8_t s_was_usb_dry;
    if (s_was_usb_dry && !usb_mode) {
        pa_smp_wait_idle();          /* back to the effects from the USB bypass: core 1 may still hold a block from before it */
        if (chain) pa_fx_chain_reset_states(chain);
        s_xf_state = PA_XF_WARM;
        s_xf_gain = 0;
        s_xf_warm_left = (int32_t)PA_DC_HOLD_SAMPLES;
    }
    s_was_usb_dry = (uint8_t)(usb_mode != 0u);
    if (pa_audio_core_is_muted()) {
        for (int i = 0; i < n; i++) { buf_L[i] = 0; if (buf_R) buf_R[i] = 0; }
    } else if (usb_mode) {
        /* EXPERIMENTAL: In USB Interface mode (1: Interface, 2: Direct Monitor), completely bypass the 8-effect DSP engine!
         * This reclaims 65%+ CPU and eliminates pipeline latency so the pedal functions as a low-latency USB audio interface. */
        for (int i = 0; i < n; i++) { buf_L[i] = s_dry[i]; if (buf_R) buf_R[i] = s_dry[i]; }
        /* The chain is not what is heard here, so the fade has nothing to do: let the state machine finish at once. Left alone it stayed in FADE_OUT for ever (xf_apply is not called in this
         * branch), the swap that waits for the mute (a preset or model change) was never applied, and the screen's redraw hold waited for it: the presets looked stuck / copied until a restart. */
        if (s_xf_state == PA_XF_FADE_OUT) s_xf_state = PA_XF_MUTED;
        else if (s_xf_state != PA_XF_IDLE && s_xf_state != PA_XF_MUTED) { s_xf_state = PA_XF_IDLE; s_xf_gain = PA_Q21_ONE; }
    } else if (tuner_dry) {
        if (buf_R) for (int i = 0; i < n; i++) buf_R[i] = buf_L[i];       /* the tuner already scaled the left channel by the bypass volume */
        if (s_xf_state != PA_XF_IDLE) pa_audio_core_xf_apply(buf_L, buf_R, n);
    } else {
        if (chain) pa_fx_chain_run_stereo(chain, buf_L, buf_R, n);
        /* The headphone jack is always stereo: when the cabinet did not make the stereo this block (no cabinet, an amp with its own, the governor turned the
         * stereo cabinet off) the spread does, from the same WIDTH setting. */
        if (ui && ui->stereo_width && buf_R && buf_R != buf_L && !g_pa_cab_stereo) pa_spread_process(&s_spread, buf_L, buf_R, n, ui->stereo_width);
        PA_ISR_CP(0x24);
        PT_SET(pt1);
        if (s_xf_state != PA_XF_IDLE) pa_audio_core_xf_apply(buf_L, buf_R, n);
    }
    PT_SET(pt2);
    PA_ISR_CP(0x25);

    /* 3. Mix the Groove Station drums through the sum limiter (pa_mix_limiter.h): it
     *    turns the guitar + drums sum down a little when a hit would not fit, instead of
     *    letting the soft ceiling below clip it. Silent drums: the guitar is untouched. */
    if (drums && drums->playing) {
        int32_t drum_tmp[PA_AUDIO_BLOCK_FRAMES] = {0};
        PA_DRUMDBG_T(dt0);
        pa_drums_mix_block(drums, drum_tmp, n);
        PA_DRUMDBG_T(dt1);
        PA_DRUMDBG_TIME(2, dt1 - dt0);
        /* A block in which no drum sounded adds nothing: hand the limiter no input then, so its idle check can return at once (it still
         * drains the look-ahead while a hit is in it). The result is the same as feeding it a block of zeros. */
        pa_mixlim_process(&s_mixlim, buf_L, buf_R, drums->bus_idle ? 0 : drum_tmp, n);
        PA_DRUMDBG_TIME(3, plat_ticks24() - dt1);
    } else {
        pa_mixlim_process(&s_mixlim, buf_L, buf_R, 0, n);   /* drains the look-ahead */
    }
    PT_SET(pt3);
    PA_ISR_CP(0x26);
    /* + USB backing track */
    if (usb && usb_mode) {
        /* INTERFACE: nothing of the unit's own sound reaches the output (the amp, the drums); the host's audio is the output, and the dry guitar too when the direct monitor is on. The recording is the
         * dry guitar, so the host's own playback can never come back into it: no loopback to switch off. */
        for (int i = 0; i < n; i++) {
            const int32_t m = (usb_mode == 2u) ? s_dry[i] : 0;
            buf_L[i] = m;
            if (buf_R && buf_R != buf_L) buf_R[i] = m;
        }
        pa_usb_audio_push_capture_stereo(usb, s_dry, s_dry, n);
        pa_usb_audio_mix_playback_stereo(usb, buf_L, buf_R, n);
    } else if (usb) {
        if (usb->loopback_enabled) {                  /* LOOPBACK ON (the default): the host hears its own playback mixed into the recording */
            pa_usb_audio_mix_playback_stereo(usb, buf_L, buf_R, n);
            pa_usb_audio_push_capture_stereo(usb, buf_L, buf_R, n);
        } else {                                      /* OFF: the recording is the amp alone; the host's playback is still heard on the output */
            pa_usb_audio_push_capture_stereo(usb, buf_L, buf_R, n);
            pa_usb_audio_mix_playback_stereo(usb, buf_L, buf_R, n);
        }
    }
    PT_SET(pt4);
    PA_ISR_CP(0x27);

    /* 4. The soft-knee guard: a safety net now (the sum limiter keeps the drums under its
     *    knee); USB playback still relies on it. */
    /*    (It runs in the same loop as the DC blocker below, sample by sample in the same order: one pass over the block instead of two, the same numbers.) */

    /* 5. The output DC blocker - the LAST thing before the DAC.
     *
     * The chain's own DC blocker sits at its INPUT, before the model, so it
     * cannot remove the DC the model CREATES. And a non-linear stage
     * rectifies: the AMP's asymmetric clipping and the REVERB's shimmer both
     * turn a symmetric signal into one with a DC component.
     *
     * It has to be HERE, after the soft ceiling, and not inside the chain:
     * the soft ceiling hard-clips whatever the chain's limiter let through,
     * and a hard clip of an asymmetric waveform is a rectifier, which turns
     * the asymmetry into DC. Measured on the factory models, "10ModMetal"
     * (index 9) drove the chain to 3.45 FS - the chain limiter's slow attack
     * let it through - and the ceiling's hard clip then left +0.0659 of DC
     * on the output, 6 % of the peak. On a line output that is a click when
     * the cable is plugged in, and it wastes the headroom.
     *
     * R = 2086663 in Q21 is the same single-pole high-pass the DRIVE uses
     * after its clipping stage: a corner at about 20 Hz, below the lowest
     * note a guitar or bass can play, so it costs nothing audible. */
    {
        const int32_t dc_r = 2086663;
        const int stereo = (buf_R && buf_R != buf_L);
        int32_t x1 = s_out_dc_x1, y1 = s_out_dc_y1, x1r = s_out_dc_x1r, y1r = s_out_dc_y1r;      /* the filter state in registers for the block (it was four memory accesses a sample) */
        for (int i = 0; i < n; i++) {
            const int32_t x = pa_soft_ceiling_q21(buf_L[i]);              /* step 4, the soft-knee guard */
            const int32_t y = x - x1 + q21_mul(dc_r, y1);
            x1 = x;
            y1 = y;
            buf_L[i] = y;
            if (stereo) {
                const int32_t xr = pa_soft_ceiling_q21(buf_R[i]);
                const int32_t yr = xr - x1r + q21_mul(dc_r, y1r);
                x1r = xr;
                y1r = yr;
                buf_R[i] = yr;
            }
        }
        s_out_dc_x1 = x1; s_out_dc_y1 = y1; s_out_dc_x1r = x1r; s_out_dc_y1r = y1r;
    }

    pa_audio_core_route_block(buf_L, buf_R, n);       /* routing and balance: the very last thing before the DAC */
    PT_SET(pt5);
    pa_timing(5, pt1 - pt0);        /* the chain (slots + tail) */
    pa_timing(6, pt2 - pt1);        /* the crossfade of a model / preset swap */
    pa_timing(7, pt3 - pt2);        /* drums + the sum limiter */
    pa_timing(8, pt4 - pt3);        /* the USB audio copy */
    pa_timing(9, pt5 - pt4);        /* the soft ceiling + the output DC blocker */
    /* 6. Fold the per-stage cycle counts into the EMA exactly once per block,
     *    so the device, the host emulator and the WASM build all report the
     *    same DSP load. */
    if (chain && chain->meter) {
        cpu_meter_block_done(chain->meter);
        if (ui) ui->cpu_total_permille = pa_perf_permille(cpu_meter_total_permille(chain->meter));
    }
}
