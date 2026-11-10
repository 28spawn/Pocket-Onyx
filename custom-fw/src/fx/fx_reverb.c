/* SPDX-License-Identifier: GPL-3.0-only */
/* Copyright (C) 2026 The Onyx Authors (@28spawn) */
/* custom-fw/src/fx/fx_reverb.c — Pocket Amp 6-Algorithm Stereo Reverb + "Reverb Tail"
 *
 * Matches the official Pocket Amp User Manual (Pages 12–13) & V105 Release Notes:
 *   0: REV_TYPE_ROOM    ("Room")    — Decay, Mix, HPass, LPass, Depth (cyclical pitch modulation)
 *   1: REV_TYPE_HALL    ("Hall")    — Decay, Mix, HPass, LPass, Depth
 *   2: REV_TYPE_SWELL   ("Swell")   — Decay, Mix, HPass, LPass, RiseTime (volume-pedal envelope swell)
 *   3: REV_TYPE_SPRING  ("Spring")  — Decay, Mix, HPass, LPass, Combs (number of active springs 1..4)
 *   4: REV_TYPE_SHIMMER ("Shimmer") — Decay, Mix, Tone, Pitch (0..+2 octaves), Amount (shimmer feedback)
 *   5: REV_TYPE_CLOUD   ("Cloud")   — Decay, Mix, HPass, LPass, Diffusion
 *   6: REV_TYPE_PLATE   ("Plate")   — a plate: input diffusers into a two-branch allpass
 *                                     "figure-eight" tank (the classic published plate
 *                                     topology; this project's own delay lengths, sized to
 *                                     fit the tank below), float. Decay 0.5 .. 6 s, Tone =
 *                                     damping, Depth = modulation of the tank allpasses
 *   7: REV_TYPE_AMBIENCE("Ambience")— early reflections only: 10 taps per side spread over
 *                                     SIZE (8 .. 80 ms), two diffusers, no long tail -
 *                                     space without wash, for rhythm parts
 *   Plate and Ambience run in the same 12,648-word tank as the comb types (only one type
 *   runs at a time); a switch between the two families clears the tank.
 *   + V105 "Reverb Tail" (`REV_PARAM_TAIL`): spillover decay on bypass before C7 SRAM reclaim.
 */

#include <string.h>
#include <math.h>
#include "pa_fx.h"
#include "pa_config.h"
#include "../dsp/fixedpoint.h"
#include "pa_prof.h"

#ifndef PA_REV_CLASSIC_FLOAT
#define PA_REV_CLASSIC_FLOAT 1   /* 1: Room / Hall / Swell / Spring / Cloud run in float (reverb_run_classic_f); 0: the original integer loop */
#endif
#define REV_NUM_COMBS    4
#define REV_NUM_ALLPASS  3

/* ---- the tank geometry ---------------------------------------------------
 *
 * The tank is ONE int32_t array of Q21 samples, sliced into non-overlapping regions:
 *
 *     0                      8,800      10,600          12,648
 *     |-- comb 0..3 (2200) --|-- ap 0..2 (600) --|-- shimmer --|
 *
 * It used to be int16_t (Q21 >> 6, i.e. +-1.0 full scale). At high decay a comb's gain
 * approaches 1/(1-g) = 25 while its input is a quarter of the signal, so the tank clipped
 * (Spring at max decay: 112,000 clipped samples per 8 s; Shimmer: tens of millions),
 * and the 15-bit requantisation noise recirculated in every comb and allpass. Q21 in
 * int32 has 1,024x headroom and no requantisation. 12,648 x 4 B = 50,592 B, 1,440 B
 * more than the old 24,576 x 2 B allocation (half of which was never used).
 *
 * Every region gets a fixed STRIDE, so a comb or allpass can be retuned inside
 * its slot by editing k_*_combs / k_ap_lens alone - no pointer moves.
 *
 * This used to be implicit: comb_buf[c] = p; p += 2200, ap_buf[a] = p; p += 600,
 * then shim_buf = tank + 10240. That last hard-coded 10,240 lands inside
 * ap_buf[2]'s 10,000..10,599 stride (and inside its live ring at 10,000..
 * 10,440), so the Shimmer path aliased the third allpass and both were
 * corrupted. The regions below are disjoint by construction and the asserts
 * below pin them at compile time.
 *
 * D - the comb period - is the only place the tank size and the decay law
 * meet: the feedback gain g is applied ONCE PER D SAMPLES, so
 *      RT60 = (3 / |log10 g|) * D / 44100  seconds.
 * The comb tables are therefore frozen: changing a length changes RT60 for the
 * same knob position, and the DECAY table in reverb_set_param() must be
 * re-derived (custom-fw/test/test_reverb.c fails if it is not).
 */
#define REV_COMB_STRIDE   2200u   /* >= longest comb  (Cloud 2111) */
#define REV_AP_STRIDE      600u   /* >= longest allpass (556) */
#define REV_SHIM_LEN      2048u   /* power of two: the pitch shifter masks it */
#define REV_SHIM_MASK     (REV_SHIM_LEN - 1u)
#define REV_SHIM_OFFSET   (REV_NUM_COMBS * REV_COMB_STRIDE + \
                           REV_NUM_ALLPASS * REV_AP_STRIDE)          /* 10,600 */
#define REV_TANK_SAMPLES  12648u  /* 12,648 x int32_t (Q21) = 50,592 B, all used */

/* Room / Hall / Cloud "Depth": the allpass delays move by up to this many samples,
 * smoothly (triangle LFO, fractional read with linear interpolation). */
#define REV_MOD_MAX       8u
/* Shimmer: the pitch-shifted feedback loop's gain is at most this (see the injection
 * in reverb_run_stereo), so it can never run away. */
#define REV_SHIM_LOOP_MAX 1782579 /* 0.85 in Q21 */
#define REV_DC_POLE       2086666 /* 0.995 in Q21: ~35 Hz DC blocker in the shimmer loop */

_Static_assert(REV_SHIM_OFFSET + REV_SHIM_LEN <= REV_TANK_SAMPLES,
               "reverb tank: shimmer region runs past the end of the tank");
_Static_assert(REV_SHIM_OFFSET + REV_SHIM_LEN == 12648u,
               "reverb tank: layout changed - update the diagram above");

typedef enum {
    REV_TYPE_ROOM    = 0,
    REV_TYPE_HALL    = 1,
    REV_TYPE_SWELL   = 2,
    REV_TYPE_SPRING  = 3,
    REV_TYPE_SHIMMER = 4,
    REV_TYPE_CLOUD   = 5,
    REV_TYPE_PLATE   = 6,
    REV_TYPE_AMBIENCE= 7,
    REV_TYPE_COUNT   = 8
} rev_type_t;

/* ---- the float engines (Plate, Ambience) in the tank ------------------------------- */
#define PL_NDIFF 4
static const uint16_t k_pl_diff[PL_NDIFF] = { 131, 97, 347, 263 };      /* input diffusers */
static const float    k_pl_diff_g[PL_NDIFF] = { 0.75f, 0.75f, 0.625f, 0.625f };
/* the tank: per branch a modulated allpass, a delay, an allpass, a delay */
static const uint16_t k_pl_ap1[2] = { 509, 673 };
static const uint16_t k_pl_d1[2]  = { 2311, 2179 };
static const uint16_t k_pl_ap2[2] = { 929, 1237 };
static const uint16_t k_pl_d2[2]  = { 1847, 1531 };
#define PL_MOD_MAX 12                       /* modulated allpass excursion, samples */
/* the decay gain is applied twice per branch, so once per ~2,800 samples */
#define PL_G_SPACING 2804.0f
/* the measured RT60 / design RT60 of the plate (custom-fw/test/test_reverb.c [7]) */
#define PL_RT_MEASURED 0.79f

#define AMB_LINE 4096u                      /* > 80 ms */
#define AMB_TAPS 10
static const float k_amb_r[2][AMB_TAPS] = {
    { 0.11f, 0.17f, 0.24f, 0.31f, 0.38f, 0.46f, 0.55f, 0.66f, 0.79f, 0.97f },
    { 0.13f, 0.19f, 0.27f, 0.33f, 0.42f, 0.49f, 0.59f, 0.71f, 0.83f, 1.00f },
};
static const uint16_t k_amb_ap[2] = { 149, 211 };

typedef struct {
    float   *buf;
    uint32_t len, idx;
} fline_t;

static inline float fl_read(const fline_t *l, uint32_t back) {   /* back = 1 .. len */
    uint32_t i = l->idx + l->len - back;
    if (i >= l->len) i -= l->len;
    return l->buf[i];
}
static inline void fl_write(fline_t *l, float v) {
    l->buf[l->idx] = v;
    if (++l->idx >= l->len) l->idx = 0;
}
/* allpass (Schroeder): v = x + g * z, y = z - g * v */
static inline float fl_allpass(fline_t *l, float x, float g) {
    float z = l->buf[l->idx];
    float v = x + g * z;
    l->buf[l->idx] = v;
    if (++l->idx >= l->len) l->idx = 0;
    return z - g * v;
}
/* modulated allpass: the read moves `m` samples (fractional) back from the loop length.
 * The fractional part is read through a first-order ALLPASS (Thiran) interpolator, whose
 * gain is exactly 1 at every frequency: a linear or Hermite read sits inside the tank's
 * feedback loop and dulls the highs on every lap, which shortened the plate's measured
 * tail to 40..55 % of its design at long decays. The fraction is kept in [0.5, 1.5) for
 * the interpolator's stability. `st` is the interpolator's memory. */
static inline float fl_allpass_mod(fline_t *l, float x, float g, float m, float *st) {
    float back = (float)(l->len - PL_MOD_MAX - 3u) + m;  /* <= len - 3: the reads stay inside */
    uint32_t bi = (uint32_t)(back - 0.5f);
    float fr = back - (float)bi;                         /* 0.5 .. 1.5 */
    float eta = (1.0f - fr) / (1.0f + fr);
    float z = eta * fl_read(l, bi) + fl_read(l, bi + 1u) - eta * *st;
    *st = z;
    float v = x + g * z;
    fl_write(l, v);
    return z - g * v;
}

enum {
    REV_PARAM_TYPE   = 0, /* 0..5: Room, Hall, Swell, Spring, Shimmer, Cloud */
    REV_PARAM_DECAY  = 1, /* Decay duration in Q21 */
    REV_PARAM_MIX    = 2, /* Wet/Dry ratio in Q21 */
    REV_PARAM_LPASS  = 3, /* Low-pass HF damping (or Tone on Shimmer) in Q21 */
    REV_PARAM_HPASS  = 4, /* High-pass LF cut in Q21 */
    REV_PARAM_AUX    = 5, /* Mode-specific: Depth (Room/Hall), RiseTime (Swell), Combs (Spring), Pitch (Shimmer), Diffusion (Cloud) */
    REV_PARAM_AMOUNT = 6, /* Shimmer Amount in Q21 */
    REV_PARAM_TAIL   = 7  /* V105 Reverb Tail spillover toggle */
};

typedef struct {
    int32_t   *tank_mem;
    int32_t   *comb_buf[REV_NUM_COMBS];
    uint32_t   comb_len[REV_NUM_COMBS];
    uint32_t   comb_idx[REV_NUM_COMBS];
    int32_t    comb_lp[REV_NUM_COMBS];
    int32_t   *ap_buf[REV_NUM_ALLPASS];
    uint32_t   ap_len[REV_NUM_ALLPASS];
    uint32_t   ap_idx[REV_NUM_ALLPASS];
    int32_t   *shim_buf;
    uint32_t   shim_wr;
    uint32_t   shim_phs_q8;     /* Fractional phase (24.8) supporting 0..+2 octaves pitch shift */
    uint32_t   shim_step_q8;    /* Pitch step: 0=unison, 256=+1 oct, 768=+2 octaves */
    int32_t    shim_dc_x1, shim_dc_y1;  /* DC blocker inside the shimmer feedback loop */

    rev_type_t type;
    volatile uint8_t type_req;  /* the type the UI asked for; the AUDIO thread makes it the real one at the start of a block (rev_apply_type): the layout of the tank
                                 * (pointers into it) is several steps, and the audio interrupt, which can fire between any two of them, must never see a half-switched state */
    uint8_t    sw_state;        /* 0 normal, 1 the wet is fading out for a type change, 2 fading back in (see reverb_run_stereo) */
    uint16_t   sw_pos;          /* samples into the fade */
    int32_t    decay;
    int32_t    mix;
    int32_t    damp;
    int32_t    hpass_coeff;
    int32_t    diff_gain;
    int32_t    mod_depth;       /* Room/Hall cyclical pitch depth */
    int32_t    decay_knob;      /* the DECAY knob (Q21) as set: the feedback gain depends on the type, so a type change recomputes it from this */
    int32_t    wet_gain;        /* MIX x the type's wet scale (Q21): what the wet signal is multiplied by */
    int32_t    swell_rise_rate; /* Swell envelope attack slew per sample */
    int32_t    swell_env;       /* Current Swell wet envelope (0..PA_Q21_ONE) */
    uint8_t    spring_combs;    /* 1..4 active springs in Spring mode */
    int32_t    shimmer_amount;  /* Shimmer feedback injection level in Q21 */

    int32_t    hp_x1, hp_y1;
    uint32_t   lfo_phase;
    uint8_t    tail_active;     /* 1 while bypass spillover tail is draining */
    uint16_t   tail_hold;       /* countdown blocks to prevent premature silence cutoff */

    /* the float engines */
    int        float_family;           /* 1 while the tank holds Plate / Ambience floats */
    fline_t    pl_diff[PL_NDIFF];
    fline_t    pl_ap1[2], pl_d1[2], pl_ap2[2], pl_d2[2];
    float      pl_bw, pl_damp_st[2], pl_fb[2], pl_lfo, pl_thiran[2];
    float      pl_decay_knob;          /* 0..1 */
    float      pl_g_knob, pl_g;        /* the plate's decay gain for the knob value it was computed from (two powf calls; once per knob change, not per sample) */
    uint8_t    pl_g_valid;
    float      amb_knob_c;             /* the ambience's size knob the tap positions below were computed from */
    uint8_t    amb_valid;
    uint32_t   amb_back[2][AMB_TAPS];  /* the ambience taps' read positions */
    fline_t    amb_line, amb_ap[2];
    float      amb_lp;
    float      amb_size_knob;          /* 0..1 */
} reverb_state_t;

extern void *pa_alloc(uint32_t);
extern void  pa_free(void *);

/* Frozen. A change here changes D, hence RT60 at the same decay knob position -
 * see the DECAY comment in reverb_set_param() and custom-fw/test/test_reverb.c. */
static const uint16_t k_hall_combs[REV_NUM_COMBS]   = { 1557, 1617, 1491, 1422 };
static const uint16_t k_room_combs[REV_NUM_COMBS]   = { 1167, 1212, 1118, 1067 };   /* 1.13: 1.5 x the old 778 / 808 / 745 / 711 (RT60 up to ~3.3 s instead of ~2.2 s) */
static const uint16_t k_spring_combs[REV_NUM_COMBS] = { 1874, 2196, 1245, 1475 };   /* 1.13: 1.85 x the old 1013 / 1187 / 673 / 797, the delay lines of a 22.05 kHz design (RT60 up to ~5.4 s instead of ~2.9 s); 2196 fits the 2200 stride */
static const uint16_t k_cloud_combs[REV_NUM_COMBS]  = { 1993, 2111, 1879, 1789 };
static const uint16_t k_ap_lens[REV_NUM_ALLPASS]    = {  225,  556,  441 };

static const uint16_t *rev_comb_table(int type) {
    switch (type) {
        case REV_TYPE_ROOM:   return k_room_combs;
        case REV_TYPE_SPRING: return k_spring_combs;
        case REV_TYPE_CLOUD:  return k_cloud_combs;
        default:              return k_hall_combs;  /* Hall + Swell + Shimmer */
    }
}

static void reverb_configure_lengths(reverb_state_t *s) {
    const uint16_t *tbl = rev_comb_table((int)s->type);
    for (int c = 0; c < REV_NUM_COMBS; c++) {
        s->comb_len[c] = tbl[c];
        if (s->comb_idx[c] >= s->comb_len[c]) s->comb_idx[c] = 0;
    }
}

/* Dual-grain pitch shifter, unison up to +2 octaves (manual page 13).
 *
 * A read pointer that moves FASTER than the write pointer raises the pitch: for a ratio
 * r the grain delay must SHRINK by (r - 1) samples per sample. shim_step_q8 is (r-1)*256
 * (0 = unison, 256 = +1 octave, 768 = +2 octaves), so the delay is LEN - phase.
 * It used to be +phase: the delay GREW by one sample per sample at "+1 octave", the read
 * pointer stood still, and the shifter output a frozen value - a DC level that the
 * feedback loop then pumped into the tank until it pinned at the rails ("Shimmer stops
 * working after a few seconds").
 *
 * Two grains half a buffer apart, crossfaded with triangular weights that are zero where
 * a grain's delay wraps; each read is linearly interpolated (the phase is fractional). */
static inline int32_t shim_tap(const reverb_state_t *s, uint32_t dly_q8) {
    const uint32_t di = (dly_q8 >> 8) & REV_SHIM_MASK, fr = dly_q8 & 255u;
    const int32_t a = s->shim_buf[(s->shim_wr - di) & REV_SHIM_MASK];
    const int32_t b = s->shim_buf[(s->shim_wr - di - 1u) & REV_SHIM_MASK];
    return a + (int32_t)(((int64_t)(b - a) * (int32_t)fr) >> 8);
}

static inline int32_t shimmer_pitch_shift(reverb_state_t *s, int32_t in_q21) {
    s->shim_buf[s->shim_wr] = in_q21;
    s->shim_phs_q8 = (s->shim_phs_q8 + s->shim_step_q8) & ((REV_SHIM_LEN << 8) - 1u);

    const uint32_t d0_q8 = ((REV_SHIM_LEN << 8) - s->shim_phs_q8) & ((REV_SHIM_LEN << 8) - 1u);
    const uint32_t d1_q8 = (d0_q8 + ((REV_SHIM_LEN / 2u) << 8)) & ((REV_SHIM_LEN << 8) - 1u);
    const uint32_t d0 = d0_q8 >> 8;
    const int32_t  w0 = (d0 < (REV_SHIM_LEN / 2u)) ? (int32_t)d0 : (int32_t)(REV_SHIM_LEN - d0);
    const int32_t  w1 = (int32_t)(REV_SHIM_LEN / 2u) - w0;

    const int32_t g0 = shim_tap(s, d0_q8);
    const int32_t g1 = shim_tap(s, d1_q8);
    s->shim_wr = (s->shim_wr + 1u) & REV_SHIM_MASK;
    return (int32_t)(((int64_t)g0 * w0 + (int64_t)g1 * w1) >> 10);
}

static float rev_knob01(int32_t v) {
    float k = (float)v * (1.0f / 2097152.0f);
    return k < 0.0f ? 0.0f : (k > 1.0f ? 1.0f : k);
}

/* Plate RT60 (s) for the DECAY knob: 0.5 .. 6 s, exponential */
static float plate_rt60(float knob) { return 0.5f * powf(12.0f, knob); }
/* Ambience SIZE (ms): 8 .. 80 ms */
static float amb_size_ms(float knob) { return 8.0f * powf(10.0f, knob); }

static void float_layout(reverb_state_t *s) {
    float *f = (float *)s->tank_mem;
    uint32_t off = 0;
    memset(s->tank_mem, 0, REV_TANK_SAMPLES * sizeof(int32_t));        /* 0 is 0.0f too */
#define TAKE(line, n) do { (line).buf = f + off; (line).len = (n); (line).idx = 0; off += (n); } while (0)
    if (s->type == REV_TYPE_PLATE) {
        for (int i = 0; i < PL_NDIFF; i++) TAKE(s->pl_diff[i], k_pl_diff[i]);
        for (int b = 0; b < 2; b++) {
            TAKE(s->pl_ap1[b], (uint32_t)k_pl_ap1[b] + PL_MOD_MAX + 3u);
            TAKE(s->pl_d1[b], k_pl_d1[b]);
            TAKE(s->pl_ap2[b], k_pl_ap2[b]);
            TAKE(s->pl_d2[b], k_pl_d2[b]);
        }
    } else {
        TAKE(s->amb_line, AMB_LINE);
        TAKE(s->amb_ap[0], k_amb_ap[0]);
        TAKE(s->amb_ap[1], k_amb_ap[1]);
    }
#undef TAKE
    s->pl_bw = 0; s->pl_damp_st[0] = s->pl_damp_st[1] = 0; s->pl_fb[0] = s->pl_fb[1] = 0;
    s->pl_thiran[0] = s->pl_thiran[1] = 0;
    s->amb_lp = 0;
    s->float_family = 1;
}

/* Make the requested type the real one. Runs in the thread that runs the effect (the audio interrupt) at the start of a block, and in reset (the node is not running then).
 * Before this, set_param did all of it itself: it set `type` first and laid the tank out afterwards (a 50 KB clear, then the line pointers), so an interrupt in between ran
 * the Plate with the Ambience's lines (or the other way round), whose buffers are still NULL in a state that was never in that mode: a write through a NULL pointer, which
 * is what hung the unit when the reverb type was scrolled from Plate to Ambience. */
static void rev_clear_comb_state(reverb_state_t *s) {
    memset(s->tank_mem, 0, REV_TANK_SAMPLES * sizeof(int32_t));
    for (int c = 0; c < REV_NUM_COMBS; c++) { s->comb_idx[c] = 0; s->comb_lp[c] = 0; }
    for (int a = 0; a < REV_NUM_ALLPASS; a++) s->ap_idx[a] = 0;
    s->shim_wr = 0; s->shim_phs_q8 = 0; s->shim_dc_x1 = s->shim_dc_y1 = 0;
    s->hp_x1 = 0; s->hp_y1 = 0;
    s->float_family = 0;
}

static int32_t rev_decay_from_knob(int type, int32_t val);
static void rev_update_wet(reverb_state_t *s);
static void rev_apply_type(reverb_state_t *s) {
    const rev_type_t want = (rev_type_t)s->type_req;
    if (want == s->type) return;
    s->type = want;
    s->decay = rev_decay_from_knob(want, s->decay_knob);   /* g and the wet level depend on the type: changed here, once the old tail has faded out */
    rev_update_wet(s);
    s->swell_env = 0;                   /* the Swell rises from silence in a new type, not from where the last one left it */
    s->hp_x1 = 0; s->hp_y1 = 0;
    if (want >= REV_TYPE_PLATE) float_layout(s);
    else rev_clear_comb_state(s);       /* a comb type starts from an empty tank: the old type's signal read with the new type's comb lengths is noise (it was the glitch between Room and Hall) */
    reverb_configure_lengths(s);
}

/* the float engines' wet output for one sample of (already high-passed) input */
static void float_wet(reverb_state_t *s, float x, float *wl, float *wr,
                      float pl_damp, float pl_bw, float pl_exc, float amb_diff, float amb_lp_k) {
    if (s->type == REV_TYPE_PLATE) {
        const float g = s->pl_g;
        const float damp = pl_damp;                                      /* TONE: damping (0 = none) */
        const float bw = pl_bw;                                          /* and input bandwidth */
        s->pl_bw += (x - s->pl_bw) * bw;
        float v = s->pl_bw;
        for (int i = 0; i < PL_NDIFF; i++) v = fl_allpass(&s->pl_diff[i], v, k_pl_diff_g[i]);
        s->pl_lfo += 0.000114f;                                          /* ~0.8 Hz */
        if (s->pl_lfo > 1.0f) s->pl_lfo -= 1.0f;
        const float tri = (s->pl_lfo < 0.5f) ? 4.0f * s->pl_lfo - 1.0f : 3.0f - 4.0f * s->pl_lfo;
        const float exc = pl_exc;
        float out[2];
        for (int b = 0; b < 2; b++) {
            float in = v + g * s->pl_fb[1 - b];                         /* the figure eight */
            float m = (float)PL_MOD_MAX * 0.5f + exc * (b ? -tri : tri);
            float a = fl_allpass_mod(&s->pl_ap1[b], in, -0.7f, m, &s->pl_thiran[b]);
            fl_write(&s->pl_d1[b], a);
            float d = fl_read(&s->pl_d1[b], s->pl_d1[b].len);
            s->pl_damp_st[b] += (d - s->pl_damp_st[b]) * (1.0f - damp);
            float e = fl_allpass(&s->pl_ap2[b], s->pl_damp_st[b] * g, 0.5f);
            fl_write(&s->pl_d2[b], e);
            out[b] = fl_read(&s->pl_d2[b], s->pl_d2[b].len);
            s->pl_fb[b] = out[b];
        }
        (void)out;
        /* output taps spread over both branches; left and right use mirrored sets */
        const fline_t *A1 = &s->pl_d1[0], *A2 = &s->pl_d2[0], *B1 = &s->pl_d1[1], *B2 = &s->pl_d2[1];
        float L = fl_read(B1, B1->len * 27 / 100) + fl_read(B1, B1->len * 74 / 100)
                - fl_read(B2, B2->len * 46 / 100) + fl_read(B2, B2->len * 91 / 100)
                - fl_read(A1, A1->len * 33 / 100) - fl_read(A2, A2->len * 19 / 100);
        float R = fl_read(A1, A1->len * 29 / 100) + fl_read(A1, A1->len * 78 / 100)
                - fl_read(A2, A2->len * 43 / 100) + fl_read(A2, A2->len * 88 / 100)
                - fl_read(B1, B1->len * 36 / 100) - fl_read(B2, B2->len * 22 / 100);
        *wl = 0.07f * L;                 /* level-matched to the Hall at the same MIX */
        *wr = 0.07f * R;
    } else {
        fl_write(&s->amb_line, x);
        float acc[2] = { 0.0f, 0.0f };
        for (int c = 0; c < 2; c++) {
            float gain = 0.9f;
            for (int k = 0; k < AMB_TAPS; k++) {
                float tap = fl_read(&s->amb_line, s->amb_back[c][k]);
                acc[c] += ((k & 1) ? -gain : gain) * tap;
                gain *= 0.82f;
            }
        }
        const float diff = amb_diff;                                     /* Depth = diffusion */
        float l = fl_allpass(&s->amb_ap[0], acc[0], diff);
        float r = fl_allpass(&s->amb_ap[1], acc[1], diff);
        const float lp = amb_lp_k;                                       /* TONE: darker up */
        s->amb_lp += ((l + r) * 0.5f - s->amb_lp) * lp;
        *wl = 0.55f * (l * 0.7f + s->amb_lp * 0.3f);
        *wr = 0.55f * (r * 0.7f + s->amb_lp * 0.3f);
    }
}

static void reverb_run_float(fx_node_t *nd, reverb_state_t *s, int32_t *buf_L, int32_t *buf_R, int n) {
    int32_t tail_peak = 0;
    const int32_t dry_gain = (s->mix <= PA_Q21_ONE / 2) ? PA_Q21_ONE
                           : (s->mix >= PA_Q21_ONE ? 0 : 2 * (PA_Q21_ONE - s->mix));
    if (s->type == REV_TYPE_PLATE) {
        if (!s->pl_g_valid || s->pl_g_knob != s->pl_decay_knob) {        /* two powf calls: 128 of them a block when they sat here per sample */
            s->pl_g_knob = s->pl_decay_knob;
            s->pl_g = powf(10.0f, -3.0f * PL_G_SPACING / (PA_SAMPLE_RATE_F * plate_rt60(s->pl_decay_knob)));
            s->pl_g_valid = 1u;
        }
    } else {
        if (!s->amb_valid || s->amb_knob_c != s->amb_size_knob) {        /* the tap positions follow the size knob: once per change */
            const float size = amb_size_ms(s->amb_size_knob) * (PA_SAMPLE_RATE_F * 0.001f);   /* samples */
            for (int c = 0; c < 2; c++)
                for (int k = 0; k < AMB_TAPS; k++) {
                    uint32_t back = (uint32_t)(size * k_amb_r[c][k]) + 1u;
                    if (back > AMB_LINE) back = AMB_LINE;
                    s->amb_back[c][k] = back;
                }
            s->amb_knob_c = s->amb_size_knob;
            s->amb_valid = 1u;
        }
    }
    const float damp01   = rev_knob01(s->damp);
    const float mod01    = rev_knob01(s->mod_depth);
    const float pl_damp  = 0.7f * damp01;
    const float pl_bw    = 0.9995f - 0.6f * damp01;
    const float pl_exc   = (float)PL_MOD_MAX * 0.5f * mod01;
    const float amb_diff = 0.25f + 0.45f * mod01;
    const float amb_lp_k = 0.9f - 0.75f * damp01;

    for (int i = 0; i < n; i++) {
        int32_t x_L = buf_L[i];
        int32_t x_R = buf_R ? buf_R[i] : x_L;
        int32_t raw_in = nd->tail_draining ? 0 : ((x_L >> 1) + (x_R >> 1));
        int32_t hp_out = sat32((int64_t)raw_in - s->hp_x1 + q21_mul(s->hpass_coeff, s->hp_y1));
        s->hp_x1 = raw_in;
        s->hp_y1 = hp_out;
        float wl, wr;
        float_wet(s, (float)hp_out * (1.0f / 2097152.0f), &wl, &wr,
                  pl_damp, pl_bw, pl_exc, amb_diff, amb_lp_k);
        if (!(wl == wl) || !(wr == wr)) {                     /* never let a NaN out */
            float_layout(s);
            wl = wr = 0.0f;
        }
        float a = wl * 2097152.0f, b = wr * 2097152.0f;
        if (a > 2.0e9f) a = 2.0e9f; else if (a < -2.0e9f) a = -2.0e9f;
        if (b > 2.0e9f) b = 2.0e9f; else if (b < -2.0e9f) b = -2.0e9f;
        int32_t wet_L = (int32_t)a, wet_R = (int32_t)b;
        int32_t abs_w = (wet_L < 0) ? -wet_L : wet_L;
        if (abs_w > tail_peak) tail_peak = abs_w;
        if (nd->tail_draining) {
            buf_L[i] = sat32((int64_t)x_L + q21_mul(wet_L, s->wet_gain));
            if (buf_R) buf_R[i] = sat32((int64_t)x_R + q21_mul(wet_R, s->wet_gain));
        } else {
            buf_L[i] = sat32((int64_t)q21_mul(x_L, dry_gain) + q21_mul(wet_L, s->wet_gain));
            if (buf_R) buf_R[i] = sat32((int64_t)q21_mul(x_R, dry_gain) + q21_mul(wet_R, s->wet_gain));
        }
    }
    if (nd->tail_draining) {
        if (!s->tail_active) {
            s->tail_active = 1;
            s->tail_hold = 64; /* ~93 ms hold allows audio to traverse tank diffusers/combs */
        }
        if (s->tail_hold > 0) {
            s->tail_hold--;
        } else if (tail_peak < 2048) {
            nd->tail_draining   = 0;
            nd->pending_destroy = 1;
            s->tail_active      = 0;
        }
    } else {
        s->tail_active = 0;
    }
}

/* The classic reverbs (Room, Hall, Swell, Spring, Cloud) with the arithmetic in float. The tank still lives in int32 Q21 (the memory
 * layout and the Shimmer path are untouched); a sample is converted when it is read and when it is written, and everything between is
 * float multiplies. The integer version did about 30 64-bit multiplies per sample (q21_mul, and two more in each modulated allpass for
 * the read position and the interpolation): about 47 cycles each on this core, 4.3 us a sample, 273 us a block. */
PA_HOT static void reverb_run_classic_f(fx_node_t *nd, reverb_state_t *s, int32_t *buf_L, int32_t *buf_R, int n) {
    const float inv = 1.0f / 2097152.0f;
    const int active_combs = (s->type == REV_TYPE_SPRING) ? (int)s->spring_combs : REV_NUM_COMBS;
    const float damp1 = (float)s->damp * inv, damp2 = (float)(PA_Q21_ONE - s->damp) * inv, decay = (float)s->decay * inv;
    const float hpc = (float)s->hpass_coeff * inv, mixf = (float)s->mix * inv, wetf = (float)s->wet_gain * inv;
    const float g_ap = (float)((s->type == REV_TYPE_SPRING) ? 1468006 : s->diff_gain) * inv;
    const int modulated = (s->type == REV_TYPE_CLOUD || s->type == REV_TYPE_ROOM || s->type == REV_TYPE_HALL);
    const float mod_k = (float)s->mod_depth * (float)REV_MOD_MAX * (1.0f / 536870912.0f);     /* (tri >> 15) * depth * MAX >> 29 */
    const float dry_gain = (s->mix <= PA_Q21_ONE / 2) ? 1.0f : (s->mix >= PA_Q21_ONE ? 0.0f : 2.0f * (1.0f - mixf));
    const float lim = 2147483000.0f;
    int32_t tail_peak = 0;

    for (int i = 0; i < n; i++) {
        const int32_t x_L = buf_L[i];
        const int32_t x_R = buf_R ? buf_R[i] : x_L;
        const int32_t raw_in = nd->tail_draining ? 0 : ((x_L >> 1) + (x_R >> 1));

        /* 1. Wet HPass */
        float hpf = ((float)raw_in - (float)s->hp_x1) + hpc * (float)s->hp_y1;
        if (hpf > lim) hpf = lim; else if (hpf < -lim) hpf = -lim;
        const int32_t hp_out = (int32_t)hpf;
        s->hp_x1 = raw_in;
        s->hp_y1 = hp_out;
        const float tank_in = (float)(hp_out >> 2);

        /* 2. Parallel comb bank */
        int32_t comb_L = 0, comb_R = 0;
        for (int c = 0; c < active_combs; c++) {
            uint32_t idx = s->comb_idx[c];
            const int32_t del = s->comb_buf[c][idx];
            const float lp = (float)del * damp2 + (float)s->comb_lp[c] * damp1;
            s->comb_lp[c] = (int32_t)lp;
            float fb = tank_in + lp * decay;
            if (fb > lim) fb = lim; else if (fb < -lim) fb = -lim;
            s->comb_buf[c][idx] = (int32_t)fb;
            if (++idx >= s->comb_len[c]) idx = 0;
            s->comb_idx[c] = idx;
            comb_L += del;
            comb_R += (c & 1) ? -del : del;
        }

        /* 3. Series all-pass diffusers (modulated for Room / Hall / Cloud) */
        float diff = (float)comb_L;
        s->lfo_phase += 36000u;
        for (int a = 0; a < REV_NUM_ALLPASS; a++) {
            uint32_t wr_idx = s->ap_idx[a];
            const uint32_t len = s->ap_len[a];
            float buf_out;
            if (modulated) {
                const uint32_t ph  = s->lfo_phase + (uint32_t)a * 0x55555555u;
                const uint32_t tri = (ph & 0x80000000u) ? ~ph : ph;
                const uint32_t m_q8 = (uint32_t)((float)(tri >> 15) * mod_k);
                uint32_t r0 = wr_idx + (m_q8 >> 8);
                if (r0 >= len) r0 %= len;
                uint32_t r1 = r0 + 1u;
                if (r1 >= len) r1 = 0u;
                const int32_t s0 = s->ap_buf[a][r0], s1 = s->ap_buf[a][r1];
                buf_out = (float)s0 + (float)(s1 - s0) * ((float)(m_q8 & 255u) * (1.0f / 256.0f));
            } else {
                buf_out = (float)s->ap_buf[a][wr_idx];
            }
            float vn = diff + buf_out * g_ap;
            if (vn > lim) vn = lim; else if (vn < -lim) vn = -lim;
            s->ap_buf[a][wr_idx] = (int32_t)vn;
            diff = buf_out - vn * g_ap;
            if (++wr_idx >= len) wr_idx = 0;
            s->ap_idx[a] = wr_idx;
        }

        if (diff > lim) diff = lim; else if (diff < -lim) diff = -lim;
        float wet_L = diff;
        float wet_R = 0.5f * (diff + (float)comb_R);

        /* 4. Swell envelope (the integer envelope state, as before) */
        if (s->type == REV_TYPE_SWELL) {
            const int32_t abs_in = (raw_in < 0) ? -raw_in : raw_in;
            if (abs_in > 32768) {
                s->swell_env += s->swell_rise_rate;
                if (s->swell_env > PA_Q21_ONE) s->swell_env = PA_Q21_ONE;
            } else {
                s->swell_env -= s->swell_env >> 15;
            }
            const float env = (float)s->swell_env * inv;
            wet_L *= env;
            wet_R *= env;
        }

        const int32_t wl = (int32_t)wet_L;
        const int32_t abs_w = (wl < 0) ? -wl : wl;
        if (abs_w > tail_peak) tail_peak = abs_w;

        float oL, oR = 0.0f;
        if (nd->tail_draining) {
            oL = (float)x_L + wet_L * wetf;
            if (buf_R) oR = (float)x_R + wet_R * wetf;
        } else {
            oL = (float)x_L * dry_gain + wet_L * wetf;
            if (buf_R) oR = (float)x_R * dry_gain + wet_R * wetf;
        }
        if (oL > lim) oL = lim; else if (oL < -lim) oL = -lim;
        buf_L[i] = (int32_t)oL;
        if (buf_R) { if (oR > lim) oR = lim; else if (oR < -lim) oR = -lim; buf_R[i] = (int32_t)oR; }
    }

    if (nd->tail_draining) {
        if (!s->tail_active) {
            s->tail_active = 1;
            s->tail_hold = 64;
        }
        if (s->tail_hold > 0) {
            s->tail_hold--;
        } else if (tail_peak < 2048) {
            nd->tail_draining   = 0;
            nd->pending_destroy = 1;
            s->tail_active      = 0;
        }
    } else {
        s->tail_active = 0;
    }
}

PA_HOT static void reverb_run_stereo_core(fx_node_t *nd, int32_t *buf_L, int32_t *buf_R, int n) {
    reverb_state_t *s = (reverb_state_t *)nd->state;
    if (!s || !s->tank_mem) return;
    if (s->type >= REV_TYPE_PLATE) {
        if (!s->float_family) float_layout(s);
        reverb_run_float(nd, s, buf_L, buf_R, n);
        return;
    }
    if (s->float_family) {                     /* back from Plate / Ambience: clear the tank */
        memset(s->tank_mem, 0, REV_TANK_SAMPLES * sizeof(int32_t));
        for (int c = 0; c < REV_NUM_COMBS; c++) { s->comb_idx[c] = 0; s->comb_lp[c] = 0; }
        for (int a = 0; a < REV_NUM_ALLPASS; a++) s->ap_idx[a] = 0;
        s->shim_wr = 0; s->shim_phs_q8 = 0; s->shim_dc_x1 = s->shim_dc_y1 = 0;
        s->float_family = 0;
    }

#if PA_REV_CLASSIC_FLOAT
    if (s->type != REV_TYPE_SHIMMER) {                        /* the pitch shifter's integer loop stays as it was */
        reverb_run_classic_f(nd, s, buf_L, buf_R, n);
        return;
    }
#endif
    int32_t tail_peak = 0;
    int active_combs = (s->type == REV_TYPE_SPRING) ? (int)s->spring_combs : REV_NUM_COMBS;

    for (int i = 0; i < n; i++) {
        int32_t x_L = buf_L[i];
        int32_t x_R = buf_R ? buf_R[i] : x_L;
        int32_t raw_in = nd->tail_draining ? 0 : ((x_L >> 1) + (x_R >> 1));

        /* 1. Wet HPass regulation (Manual Page 12) */
        int32_t hp_out = sat32((int64_t)raw_in - s->hp_x1 + q21_mul(s->hpass_coeff, s->hp_y1));
        s->hp_x1 = raw_in;
        s->hp_y1 = hp_out;
        int32_t tank_in = hp_out >> 2;

        /* Shimmer pitch-shifted feedback injection (Pitch + Amount, Manual Page 13).
         * The loop runs tank_in -> comb 0 -> comb_lp[0] -> shifter -> tank_in. A comb's
         * gain from its input to comb_lp is at most 1/(1-g) and the shifter's is at most
         * 1, so an injection gain of amount * 0.85 * (1-g) keeps the loop gain at or
         * below 0.85 for every Decay and Amount: it rings, it cannot run away. (It used
         * to be a fixed amount/2, i.e. a loop gain of up to 12.5 at high decay.) A DC
         * blocker inside the loop stops any offset from accumulating. */
        if (s->type == REV_TYPE_SHIMMER) {
            int32_t sh = shimmer_pitch_shift(s, s->comb_lp[0]);
            int32_t dc = sh - s->shim_dc_x1 + q21_mul(REV_DC_POLE, s->shim_dc_y1);
            s->shim_dc_x1 = sh;
            s->shim_dc_y1 = dc;
            int32_t k = q21_mul(q21_mul(s->shimmer_amount, PA_Q21_ONE - s->decay), REV_SHIM_LOOP_MAX);
            tank_in = sat32((int64_t)tank_in + q21_mul(dc, k));
        }

        /* 2. Parallel Comb Bank (1..4 active springs on Spring mode per `Combs` param) */
        int32_t comb_L = 0, comb_R = 0;
        int32_t damp1 = s->damp;
        int32_t damp2 = PA_Q21_ONE - damp1;

        for (int c = 0; c < active_combs; c++) {
            uint32_t idx = s->comb_idx[c];
            int32_t  del = s->comb_buf[c][idx];
            s->comb_lp[c] = q21_mul(del, damp2) + q21_mul(s->comb_lp[c], damp1);
            int32_t fb_val = sat32((int64_t)tank_in + q21_mul(s->comb_lp[c], s->decay));
            s->comb_buf[c][idx] = fb_val;
            idx++;
            if (idx >= s->comb_len[c]) idx = 0;
            s->comb_idx[c] = idx;

            comb_L += del;
            comb_R += (c & 1) ? -del : del;
        }

        /* 3. Series All-Pass Diffusers with Depth pitch modulation (Room/Hall/Cloud).
         *
         * The delay of allpass a is len - m(t): m follows a slow triangle LFO (~0.37 Hz,
         * the three allpasses a third of a cycle apart), up to REV_MOD_MAX samples at full
         * Depth, read with linear interpolation. The old code had two faults, each a
         * discontinuity that the feedback then recirculated (+17..18 dB of HF in the tail
         * at max decay, measured by test/test_reverb_modes.c):
         *   - m was a 0..7 sawtooth of WHOLE samples, jumping 7 -> 0 several times a second;
         *   - a read past the end of the ring returned index 0 instead of wrapping, so the
         *     last m samples of every lap (every 5..13 ms) read the wrong sample - a buzz. */
        int32_t diff_sig = comb_L;
        int32_t g_ap = (s->type == REV_TYPE_SPRING) ? 1468006 : s->diff_gain;
        s->lfo_phase += 36000u;
        const int modulated = (s->type == REV_TYPE_CLOUD || s->type == REV_TYPE_ROOM ||
                               s->type == REV_TYPE_HALL);

        for (int a = 0; a < REV_NUM_ALLPASS; a++) {
            uint32_t wr_idx = s->ap_idx[a];
            uint32_t len    = s->ap_len[a];
            int32_t  buf_out;
            if (modulated) {
                uint32_t ph  = s->lfo_phase + (uint32_t)a * 0x55555555u;
                uint32_t tri = (ph & 0x80000000u) ? ~ph : ph;            /* 0..2^31-1 */
                uint32_t m_q8 = (uint32_t)(((uint64_t)(tri >> 15) * (uint32_t)s->mod_depth *
                                            REV_MOD_MAX) >> 29);          /* 0..MAX samples, Q8 */
                uint32_t r0 = wr_idx + (m_q8 >> 8);
                if (r0 >= len) r0 %= len;
                uint32_t r1 = r0 + 1u;
                if (r1 >= len) r1 = 0u;
                int32_t s0 = s->ap_buf[a][r0], s1 = s->ap_buf[a][r1];
                buf_out = s0 + (int32_t)(((int64_t)(s1 - s0) * (int32_t)(m_q8 & 255u)) >> 8);
            } else {
                buf_out = s->ap_buf[a][wr_idx];
            }
            int32_t vn      = sat32((int64_t)diff_sig + q21_mul(buf_out, g_ap));
            s->ap_buf[a][wr_idx] = vn;
            diff_sig = sat32((int64_t)buf_out - q21_mul(vn, g_ap));
            wr_idx++;
            if (wr_idx >= len) wr_idx = 0;
            s->ap_idx[a] = wr_idx;
        }

        int32_t wet_L = diff_sig;
        int32_t wet_R = sat32((int64_t)(diff_sig >> 1) + (comb_R >> 1));

        /* 4. SWELL Reverb Envelope (`RiseTime` volume-pedal swell behind dry signal, Manual Page 12) */
        if (s->type == REV_TYPE_SWELL) {
            int32_t abs_in = (raw_in < 0) ? -raw_in : raw_in;
            if (abs_in > 32768) {
                /* Gradually swell wet reverb up at `swell_rise_rate` */
                s->swell_env += s->swell_rise_rate;
                if (s->swell_env > PA_Q21_ONE) s->swell_env = PA_Q21_ONE;
            } else {
                /* let go slowly (~0.75 s), so the tail rings out after the playing stops
                 * (>> 10 was 23 ms: the tail was cut as soon as the note dropped) */
                s->swell_env -= s->swell_env >> 15;
            }
            wet_L = q21_mul(wet_L, s->swell_env);
            wet_R = q21_mul(wet_R, s->swell_env);
        }

        int32_t abs_w = (wet_L < 0) ? -wet_L : wet_L;
        if (abs_w > tail_peak) tail_peak = abs_w;

        if (nd->tail_draining) {
            buf_L[i] = sat32((int64_t)x_L + q21_mul(wet_L, s->wet_gain));
            if (buf_R) buf_R[i] = sat32((int64_t)x_R + q21_mul(wet_R, s->wet_gain));
        } else {
            /* the dry stays at unity up to MIX 50 %, then fades out (the wet is MIX itself):
             * engaging the reverb no longer turns the guitar down */
            int32_t dry_gain = (s->mix <= PA_Q21_ONE / 2) ? PA_Q21_ONE
                             : (s->mix >= PA_Q21_ONE ? 0 : 2 * (PA_Q21_ONE - s->mix));
            buf_L[i] = sat32((int64_t)q21_mul(x_L, dry_gain) + q21_mul(wet_L, s->wet_gain));
            if (buf_R) buf_R[i] = sat32((int64_t)q21_mul(x_R, dry_gain) + q21_mul(wet_R, s->wet_gain));
        }
    }

    if (nd->tail_draining) {
        if (!s->tail_active) {
            s->tail_active = 1;
            s->tail_hold = 64; /* ~93 ms hold allows audio to traverse tank diffusers/combs */
        }
        if (s->tail_hold > 0) {
            s->tail_hold--;
        } else if (tail_peak < 2048) {
            nd->tail_draining   = 0;
            nd->pending_destroy = 1;   /* never pa_free() in the ISR - UI thread collects it */
            s->tail_active      = 0;
        }
    } else {
        s->tail_active = 0;
    }
}

/* CHANGING THE TYPE while the reverb rings. The types differ in comb lengths, allpass lengths and the engine itself, and the tank holds the old type's signal: switching in
 * the middle of a block puts a discontinuity in the output (a click, and noise when the old signal is read with the new lengths). So the switch is made out of earshot:
 * the wet signal fades to nothing over REV_SW_LEN samples, THEN the new type is made real (layout / cleared tank, in the audio thread, at a block boundary), THEN the wet
 * fades back in. What fades is the reverb's own contribution: out = out - (1-g) * (out - dry*dry_gain), with the dry the block came in with. The cost of a fade is one
 * copy of the input block and a short loop; a block with no switch pending goes straight to the engine. */
#ifndef REV_SW_LEN
#define REV_SW_LEN 256u            /* 5.8 ms each way: a multiple of the block, so the switch lands on a block boundary */
#endif

PA_HOT static void reverb_run_stereo(fx_node_t *nd, int32_t *buf_L, int32_t *buf_R, int n) {
    reverb_state_t *s = (reverb_state_t *)nd->state;
    if (!s || !s->tank_mem) return;
    if (s->sw_state == 0u && (rev_type_t)s->type_req == s->type) {                                                              /* the usual block */
        const uint32_t pf0 = PA_PROF_T();
        reverb_run_stereo_core(nd, buf_L, buf_R, n);
        if (n == (int)PA_BLOCK_N) {                                                                                            /* the unit's per-type timing (pa_prof.h) */
            const unsigned ty = (unsigned)s->type & 7u;
            pa_prof_begin(PA_PROF_REVERB, 0x50564552u, 16u);
            pa_prof_add(PA_PROF_REVERB, ty, PA_PROF_T() - pf0); pa_prof_add(PA_PROF_REVERB, 8u + ty, 1u);
        }
        return;
    }
    if (n > (int)PA_BLOCK_N) { rev_apply_type(s); reverb_run_stereo_core(nd, buf_L, buf_R, n); return; }                        /* (no such block exists; stay safe) */

    if (s->sw_state == 0u) { s->sw_state = 1u; s->sw_pos = 0u; }                                    /* start fading out */
    else if (s->sw_state == 2u && (rev_type_t)s->type_req != s->type) { s->sw_state = 1u; s->sw_pos = (uint16_t)(REV_SW_LEN - s->sw_pos); }   /* asked again while fading in: turn round from where the gain is */
    if (s->sw_state == 1u && s->sw_pos >= REV_SW_LEN) {                                             /* silent: now it is safe to change */
        rev_apply_type(s);
        s->sw_state = 2u; s->sw_pos = 0u;
    }

    int32_t dry_L[PA_BLOCK_N], dry_R[PA_BLOCK_N];
    for (int i = 0; i < n; i++) { dry_L[i] = buf_L[i]; dry_R[i] = buf_R ? buf_R[i] : buf_L[i]; }
    const int32_t dg = nd->tail_draining ? PA_Q21_ONE : ((s->mix <= PA_Q21_ONE / 2) ? PA_Q21_ONE : (s->mix >= PA_Q21_ONE ? 0 : 2 * (PA_Q21_ONE - s->mix)));

    reverb_run_stereo_core(nd, buf_L, buf_R, n);

    for (int i = 0; i < n; i++) {
        const uint32_t p = (uint32_t)s->sw_pos + (uint32_t)i;
        const uint32_t pp = p > REV_SW_LEN ? REV_SW_LEN : p;
        const int32_t g = (s->sw_state == 1u) ? (int32_t)(((REV_SW_LEN - pp) * (uint32_t)PA_Q21_ONE) / REV_SW_LEN)       /* 1 -> 0 */
                                              : (int32_t)((pp * (uint32_t)PA_Q21_ONE) / REV_SW_LEN);                      /* 0 -> 1 */
        const int32_t ng = PA_Q21_ONE - g;                                                          /* how much of the reverb is taken away */
        buf_L[i] = sat32((int64_t)buf_L[i] - q21_mul(ng, buf_L[i] - q21_mul(dry_L[i], dg)));
        if (buf_R) buf_R[i] = sat32((int64_t)buf_R[i] - q21_mul(ng, buf_R[i] - q21_mul(dry_R[i], dg)));
    }
    s->sw_pos = (uint16_t)(s->sw_pos + (uint16_t)n);
    if (s->sw_state == 2u && s->sw_pos >= REV_SW_LEN) { s->sw_state = 0u; s->sw_pos = 0u; }
}

static void reverb_run(fx_node_t *nd, int32_t *buf, int n) {
    reverb_run_stereo(nd, buf, 0, n);
}

static void reverb_create(fx_node_t *nd) {
    reverb_state_t *s = (reverb_state_t *)pa_alloc(sizeof(reverb_state_t));
    if (s) {
        s->tank_mem = (int32_t *)pa_alloc(REV_TANK_SAMPLES * sizeof(int32_t));
        if (!s->tank_mem) {            /* IMP-13: all-or-nothing, never a "silent" half-created node */
            pa_free(s);
            nd->state = 0;
            return;
        }
        if (s->tank_mem) {
            for (uint32_t i = 0; i < REV_TANK_SAMPLES; i++) s->tank_mem[i] = 0;
            /* Fixed, non-overlapping slices - see the tank geometry comment at
             * the top of the file. Every pointer is derived from a stride, never
             * from a hard-coded offset. */
            for (int c = 0; c < REV_NUM_COMBS; c++) {
                s->comb_buf[c] = s->tank_mem + (uint32_t)c * REV_COMB_STRIDE;
                s->comb_idx[c] = 0; s->comb_lp[c] = 0;
            }
            for (int a = 0; a < REV_NUM_ALLPASS; a++) {
                s->ap_buf[a] = s->tank_mem + REV_NUM_COMBS * REV_COMB_STRIDE
                                          + (uint32_t)a * REV_AP_STRIDE;
                s->ap_len[a] = k_ap_lens[a]; s->ap_idx[a] = 0;
            }
            s->shim_buf = s->tank_mem + REV_SHIM_OFFSET;
        }
        s->type            = REV_TYPE_HALL;
        s->type_req        = (uint8_t)REV_TYPE_HALL;
        s->decay           = 1761607;  /* g = 0.84 - the archived, known-good
                                         * default; the DECAY knob's 71 % point
                                         * produces the same gain. Do not retune
                                         * this: it is ~1.37 s RT60 on Hall. */
        s->mix             = 734003;
        s->decay_knob      = 1482310;   /* 71 %: the g = 0.84 default above */
        rev_update_wet(s);
        s->damp            = 629145;
        s->hpass_coeff     = 2055208;
        s->diff_gain       = 1048576;
        s->mod_depth       = 524288;
        s->swell_rise_rate = 128;     /* ~0.35 s swell rise default */
        s->swell_env       = 0;
        s->spring_combs    = 4;
        s->shimmer_amount  = 1048576;
        s->shim_wr         = 0;
        s->shim_phs_q8     = 0;
        s->shim_step_q8    = 256u;    /* +1 octave default */
        s->shim_dc_x1      = 0;
        s->shim_dc_y1      = 0;
        s->hp_x1 = 0; s->hp_y1 = 0; s->lfo_phase = 0;
        s->tail_active = 0; s->tail_hold = 0;
        s->pl_decay_knob = 0.707f;
        s->amb_size_knob = 0.5f;
        s->float_family  = 0;
        reverb_configure_lengths(s);
    }
    nd->state = s;
}

static void reverb_destroy(fx_node_t *nd) {
    reverb_state_t *s = (reverb_state_t *)nd->state;
    if (s) {
        if (s->tank_mem) pa_free(s->tank_mem);
        pa_free(s);
    }
    nd->state = 0;
}

/* DECAY knob -> comb feedback g. 1.14: calibrated on real DI guitar, in RT60 terms: 0.3 s at 0 %, 0.6 s at 20 %, 1.7 s at 50 %, 3 s at 80 %, 4.5 s at 100 % (Hall; the other types
 * scale it). g is derived from the type's mean comb period D, because RT60 = k * 3 / |log10 g| * D / 44100 (the DECAY comment in reverb_set_param). Before, g was a straight line
 * of the knob: 0.5 - 0.8 s at 50 % and the whole range crammed into the top fifth. */
static float rev_type_k(int type) {
    switch (type) {
        case REV_TYPE_ROOM:   return 0.76f;
        case REV_TYPE_SPRING: return 0.82f;
        case REV_TYPE_CLOUD:  return 0.75f;
        default:              return 0.745f;   /* Hall, Swell, Shimmer: the Hall tank */
    }
}
static float rev_type_rt_scale(int type) {
    switch (type) {
        case REV_TYPE_ROOM:    return 0.85f;
        case REV_TYPE_SHIMMER: return 1.1f;
        case REV_TYPE_CLOUD:   return 1.3f;
        default:               return 1.0f;
    }
}
static float rev_comb_mean(int type) {
    const uint16_t *tbl = rev_comb_table(type);
    return 0.25f * (float)(tbl[0] + tbl[1] + tbl[2] + tbl[3]);
}
/* the RT60 (s) the screen shows and the feedback is set for */
static float rev_rt_target(int type, float k) {
    static const float kn[5] = { 0.0f, 0.2f, 0.5f, 0.8f, 1.0f }, rt[5] = { 0.3f, 0.6f, 1.7f, 3.0f, 4.5f };
    int i = 0;
    while (i < 3 && k > kn[i + 1]) i++;
    const float f = (k - kn[i]) / (kn[i + 1] - kn[i]);
    return rev_type_rt_scale(type) * rt[i] * powf(rt[i + 1] / rt[i], f);
}
static float rev_g_for(int type, float k) {
    if (type == REV_TYPE_SHIMMER) {                  /* the Shimmer keeps the original straight-line curve: its injection is tied to (1 - g), and with the longer calibrated decays the octave got 13 dB weaker */
        const float g0 = 0.55f + 0.41f * k;
        return g0 > 0.96f ? 0.96f : g0;
    }
    const float lg = rev_type_k(type) * 3.0f * (rev_comb_mean(type) / PA_SAMPLE_RATE_F) / rev_rt_target(type, k);   /* |log10 g| */
    float g = powf(10.0f, -lg);
    if (g < 0.55f) g = 0.55f;
    if (g > 0.975f) g = 0.975f;
    return g;
}
static int32_t rev_decay_from_knob(int type, int32_t val) {
    return (int32_t)(rev_g_for(type, rev_knob01(val)) * 2097152.0f);
}

/* The wet level per type (calibrated on DI guitar so that MIX 50 % puts the wet about 3 - 4 dB under the dry: before it sat 11 - 14 dB under it; more than this made the cab-swap steps exceed the F-18 limit). */
static const float k_wet_scale[8] = { 1.6f, 1.6f, 1.6f, 1.6f, 2.2f, 1.5f, 7.0f, 1.0f };
static void rev_update_wet(reverb_state_t *s) {
    s->wet_gain = (int32_t)((float)s->mix * k_wet_scale[s->type & 7u]);
}

/* The decay the screen shows: RT60 = 3 / |log10 g| * D / 44100 over the type's mean comb
 * period D, times the ratio the tail really measures against that law (the comb damping
 * and the allpasses shorten it by ~25 %; test_reverb.c [7] checks this against measured
 * impulse responses). */
uint32_t pa_rev_decay_ms(int type, int32_t knob_q21) {
    if (knob_q21 < 0) knob_q21 = 0;
    if (knob_q21 > PA_Q21_ONE) knob_q21 = PA_Q21_ONE;
    if (type == REV_TYPE_PLATE)                   /* the RT60 the tank is set to, x the measured ratio */
        return (uint32_t)(plate_rt60(rev_knob01(knob_q21)) * PL_RT_MEASURED * 1000.0f + 0.5f);
    if (type == REV_TYPE_AMBIENCE)                /* SIZE: the span of the reflections */
        return (uint32_t)(amb_size_ms(rev_knob01(knob_q21)) + 0.5f);
    const float g = rev_g_for(type, rev_knob01(knob_q21));
    const float rt = rev_type_k(type) * 3.0f / __builtin_fabsf(log10f(g)) * (rev_comb_mean(type) / PA_SAMPLE_RATE_F);
    return (uint32_t)(rt * 1000.0f + 0.5f);
}

static void reverb_set_param(fx_node_t *nd, int id, int32_t val) {
    reverb_state_t *s = (reverb_state_t *)nd->state;
    if (!s) return;
    switch (id) {
        case 0: /* Knob 0: DECAY.
         * The comb feedback g is applied ONCE PER COMB PERIOD D, not per
         * sample: the value written at comb_buf[c][idx] is read back D samples
         * later (step 2 of reverb_run_stereo). So
         *      RT60 = (3 / |log10 g|) * D / 44100  seconds
         * and a usable decay knob is a usable GAIN range, 0.55 .. 0.96:
         *      g = 0.55 + 0.41 * knob
         * With the frozen comb tables above, that spans ~0.19 s (Room, 711) to
         * ~8.1 s (Cloud, 2111); the factory default g = 0.84 (knob 0.707) is
         * ~1.37 s on Hall. The four combs of one type decay at slightly
         * different rates (D differs) - that spread is intentional, it is what
         * makes the tail move.
         *
         * Do NOT "fix" this with g = 10^(-3 / (RT60 * 44100)): that law assumes
         * the gain is applied once per SAMPLE and produced multi-minute tails
         * plus a knob that did nothing. test_reverb.c measures the actual RT60
         * of every type and fails if this mapping and the law disagree. */
        s->decay_knob = val;
        s->decay = rev_decay_from_knob(s->type, val);
        s->pl_decay_knob = rev_knob01(val);
        s->amb_size_knob = rev_knob01(val);
        break;
        case 1: /* Knob 1: LOW CUT. IMP-13: the knob used to be written straight into the
                 * HPF pole, so 20 % (factory) = pole 0.2 = ~5 kHz high-pass -> thin, almost
                 * inaudible reverb. Map 0..1 -> pole 0.995..0.925 (~25 Hz .. ~530 Hz). */
            s->hpass_coeff = 2086666 - q21_mul(val, 146800);
            break;
        case 2: /* Knob 2: TONE / LPASS DAMP */
            s->damp = val;
            break;
        case 3: /* Knob 3: MIX */
            s->mix = val;
            rev_update_wet(s);
            break;
        case 5: /* Aux */
            if (val < 0) val = 0;
            if (val > PA_Q21_ONE) val = PA_Q21_ONE;
            s->mod_depth       = val;
            /* RiseTime: larger value = slower rise time (Manual Page 12) */
            s->swell_rise_rate = 32 + (int32_t)((PA_Q21_ONE - val) >> 11);
            /* Combs: 1..4 springs (Manual Page 13) */
            s->spring_combs    = (uint8_t)(1u + (((uint32_t)val * 3u) >> 21));
            if (s->spring_combs > 4u) s->spring_combs = 4u;
            /* Shimmer Pitch: 0 (unison) .. +2 octaves (step_q8 = 0..768) */
            s->shim_step_q8    = (uint32_t)(((uint64_t)(uint32_t)val * 768u) >> 21);
            if (val <= 1572864) s->diff_gain = val;
            break;
        case 6: /* Amount */
            s->shimmer_amount = val;
            break;
        case 7: /* Tail */
            nd->tail_enabled = (val != 0) ? 1u : 0u;
            break;
        case 10: /* Explicit Model Type selection from UI dropdown */
            /* Only the request: the audio thread applies it at the start of its next block (rev_apply_type). Plate and Ambience lay the tank out their own way
             * (and clear it); a switch back to a comb type is cleared by reverb_run_stereo(). */
            s->type_req = (uint8_t)((uint32_t)val % REV_TYPE_COUNT);
            if (s->type_req == REV_TYPE_AMBIENCE) s->type_req = REV_TYPE_ROOM;
            /* (an old preset that still holds the Ambience, removed in 1.13, gets the Room) */
            break;
        default: break;
    }
}

static void reverb_reset(fx_node_t *nd) {
    reverb_state_t *s = (reverb_state_t *)nd->state;
    if (s && s->tank_mem) {
        rev_apply_type(s);
        s->sw_state = 0u; s->sw_pos = 0u;               /* a reset is the end of any fade */
        memset(s->tank_mem, 0, REV_TANK_SAMPLES * sizeof(int32_t));
        for (int c = 0; c < REV_NUM_COMBS; c++) {
            s->comb_idx[c] = 0;
            s->comb_lp[c]  = 0;
        }
        for (int a = 0; a < REV_NUM_ALLPASS; a++) {
            s->ap_idx[a] = 0;
        }
        if (s->shim_buf) {
            memset(s->shim_buf, 0, REV_SHIM_LEN * sizeof(int32_t));
            s->shim_wr = 0;
            s->shim_phs_q8 = 0;
            s->shim_dc_x1 = 0;
            s->shim_dc_y1 = 0;
        }
        s->hp_x1 = 0;
        s->hp_y1 = 0;
        s->swell_env = 0;
        if (s->type >= REV_TYPE_PLATE) float_layout(s);
        else s->float_family = 0;
    }
}

fx_node_t *fx_reverb_new(void) {
    static fx_node_t n;
    n.run          = reverb_run;
    n.run_stereo   = reverb_run_stereo;
    n.create       = reverb_create;
    n.destroy      = reverb_destroy;
    n.set_param    = reverb_set_param;
    n.reset        = reverb_reset;
    n.active       = 1;
    n.frame        = PA_BLOCK_N;
    n.tag          = "reverb";
    n.tail_enabled = 1;
    n.tail_draining= 0;
    return &n;
}

/* ---- diagnostics ---------------------------------------------------------
 * The tank layout and the decay law are the two things a regression breaks
 * silently, and both are private to this file. These read-only hooks let
 * custom-fw/test/test_reverb.c check the REAL layout of a REAL node (not a
 * second copy of the arithmetic). They are not used by the firmware. */

uint32_t pa_rev_tank_samples(void) { return REV_TANK_SAMPLES; }

int32_t *pa_rev_tank_base(const fx_node_t *nd) {
    const reverb_state_t *s = nd ? (const reverb_state_t *)nd->state : 0;
    return s ? s->tank_mem : 0;
}

/* The static plan for `type`: offsets and occupied lengths of
 * 4 combs, 3 allpasses, then the shimmer. Returns the region count. */
uint32_t pa_rev_regions(int type, uint32_t *off, uint32_t *len, uint32_t max) {
    const uint16_t *tbl = rev_comb_table(type);
    uint32_t n = 0;
    for (int c = 0; c < REV_NUM_COMBS && n < max; c++, n++) {
        off[n] = (uint32_t)c * REV_COMB_STRIDE;
        len[n] = tbl[c];
    }
    for (int a = 0; a < REV_NUM_ALLPASS && n < max; a++, n++) {
        off[n] = REV_NUM_COMBS * REV_COMB_STRIDE + (uint32_t)a * REV_AP_STRIDE;
        len[n] = k_ap_lens[a];
    }
    if (n < max) {
        off[n] = REV_SHIM_OFFSET;
        len[n] = REV_SHIM_LEN;
        n++;
    }
    return n;
}

/* The layout as realized in `nd`'s tank, relative to tank_mem. Returns 0 if the
 * node has no state (create() failed). */
uint32_t pa_rev_state_regions(const fx_node_t *nd, uint32_t *off, uint32_t *len,
                              uint32_t max) {
    const reverb_state_t *s = nd ? (const reverb_state_t *)nd->state : 0;
    if (!s || !s->tank_mem) return 0;
    uint32_t n = 0;
    for (int c = 0; c < REV_NUM_COMBS && n < max; c++, n++) {
        off[n] = (uint32_t)(s->comb_buf[c] - s->tank_mem);
        len[n] = s->comb_len[c];
    }
    for (int a = 0; a < REV_NUM_ALLPASS && n < max; a++, n++) {
        off[n] = (uint32_t)(s->ap_buf[a] - s->tank_mem);
        len[n] = s->ap_len[a];
    }
    if (n < max) {
        off[n] = (uint32_t)(s->shim_buf - s->tank_mem);
        len[n] = REV_SHIM_LEN;
        n++;
    }
    return n;
}

int32_t pa_rev_decay_gain(const fx_node_t *nd) {
    const reverb_state_t *s = nd ? (const reverb_state_t *)nd->state : 0;
    return s ? s->decay : 0;
}
