/* SPDX-License-Identifier: GPL-3.0-only */
/* Copyright (C) 2026 The Onyx Authors (@28spawn) */
/* custom-fw/src/fx/fx_delay.c — Pocket Amp V105 Stereo 5-Mode Delay + Wet Modulation + "Delay Tail"
 *
 * Matches the official Pocket Amp User Manual (Pages 10–11) & V105 Release Notes:
 *   1. Analog:  Time, Feedback, Mix, Phaser (wet all-pass sweep), Pitch (wet detune)
 *   2. Ducking: Time, Feedback, Mix, Unpack (release smoothness), Speed, Depth (wet chorus)
 *   3. dTape:   Time, Feedback, Mix, Grit (tape bias overdrive), Speed (crinkle), Depth (wow/flutter)
 *   4. Dual:    Time, Feedback, Mix, Time-Mode (Echo 2 lag ratio), Speed, Depth (wet chorus)
 *   5. Lofi:    Time, Feedback, Mix, Grit (overdrive), Speed, Depth (wet chorus)
 *   6. Digital:   Time, Feedback, Mix, Tone - clean, uncoloured repeats
 *   7. Ping Pong: Time, Feedback, Mix, Tone - left at TIME, right at 2 x TIME, alternating
 *                 (TIME up to 371 ms: the right tap needs twice the line)
 *   8. Reverse:   Time, Feedback, Mix, Tone - each TIME-long chunk is played backwards, two
 *                 readers half a chunk apart cross-faded (triangular windows that sum to 1)
 *                 so the chunk joins never click (TIME up to 371 ms: a backward read spans
 *                 two chunks of line)
 *   9. Slapback:  Time 40..200 ms, Feedback (a third of the usual range: one repeat), Mix,
 *                 Tone - a dark, slightly saturated single echo
 *
 * Stereo: the single-tap modes put the SAME processed echo on both sides (the right side
 * used to read a second, unprocessed tap at 3/4 of the time - an off-beat echo, doubled in
 * mono). DUAL is the two-tap mode: TIME on the left, TIME x RATIO (the 4th knob, 0.5..1.0)
 * on the right, both in the feedback and both in a mono output.
 * MIX keeps the dry signal at unity up to 50 % and fades it out above (the wet level is
 * MIX itself): engaging the delay no longer turns the guitar down.
 */

#include <string.h>
#include "pa_fx.h"
#include "pa_config.h"
#include "../dsp/fixedpoint.h"

/* 32,768 int16 = 64 KB. Power of two, so DLY_BUF_MASK stays valid; the longest
 * delay is 32,767 samples = 743 ms (it was 16,384 = 371 ms). This is the whole
 * reason the delay line is a power of two: a 44,100-sample (1 s) line would need
 * a mask rewrite and 131 KB of RAM. */
#define DLY_BUF_LEN  32768u
#define DLY_BUF_MASK (DLY_BUF_LEN - 1u)

typedef enum {
    DELAY_TYPE_ANALOG = 0,
    DELAY_TYPE_DUCK   = 1,
    DELAY_TYPE_DTAPE  = 2,
    DELAY_TYPE_DUAL   = 3,
    DELAY_TYPE_LOFI   = 4,
    DELAY_TYPE_DIGITAL  = 5,
    DELAY_TYPE_PINGPONG = 6,
    DELAY_TYPE_REVERSE  = 7,
    DELAY_TYPE_SLAPBACK = 8,
    DELAY_TYPE_COUNT    = 9
} delay_type_t;

enum {
    DLY_PARAM_TYPE  = 0,
    DLY_PARAM_FB    = 1,
    DLY_PARAM_MIX   = 2,
    DLY_PARAM_TIME  = 3,
    DLY_PARAM_AUX   = 4, /* Phaser (Analog) | Unpack (Duck) | Grit (dTape/Lofi) | Time-Mode (Dual) */
    DLY_PARAM_SPEED = 5, /* Wet modulation LFO speed / Tape Crinkle rate */
    DLY_PARAM_DEPTH = 6, /* Wet modulation depth / Pitch detune / Tape Wow-Flutter */
    DLY_PARAM_TAIL  = 7  /* V105 Delay Tail spillover toggle */
};

typedef struct {
    int16_t     *buffer;
    uint32_t     write_index;
    uint32_t     delay_length;
    uint32_t     cur_q8;     /* the delay the read pointer is at now, in 1/256 sample: it glides to delay_length * 256 (see DLY_GLIDE_Q8) */
    uint32_t     b_q8;       /* the second read head of a crossfade (a big TIME jump), at its delay in 1/256 sample */
    uint16_t     xf_pos;     /* samples into the crossfade from cur_q8 to b_q8 */
    uint8_t      xf_on;      /* 1 while a crossfade runs */
    uint8_t      snap;       /* 1 until the first block runs: a length written to a fresh line (just created or reset) is taken at once, there is nothing in it to click */
    delay_type_t type;
    int32_t      feedback;
    int32_t      mix;
    int32_t      dry_gain;   /* Q21: 1.0 up to MIX 50 %, then down to 0 at 100 % */
    int32_t      fb_s, mix_s, dry_s;   /* feedback, mix and dry gain as the audio uses them: they follow the knobs over a few milliseconds (no zipper) */
    int32_t      param_aux;  /* Q21 auxiliary parameter */
    uint32_t     lfo_phase;
    uint32_t     lfo_inc;    /* Speed */
    int32_t      mod_depth;  /* Depth / Pitch */
    int32_t      envelope;
    int32_t      lp_state;
    int32_t      ap_x1, ap_y1; /* Wet Phaser stage for Analog Delay */
    int32_t      dc_x1, dc_y1; /* Feedback DC blocker */
    int32_t      time_knob;    /* TIME, kept so a type change re-derives the length */
    int32_t      tone_lp;      /* TONE low-pass (Digital / Ping Pong / Reverse / Slapback) */
    uint32_t     rev_phase;    /* Reverse: position in the chunk, 0 .. delay_length-1 */
    uint32_t     tail_hold;    /* Minimum blocks to drain before checking silence threshold */
    uint8_t      tail_active;  /* Flag: tail draining has been initiated */
} delay_state_t;

extern void *pa_alloc(uint32_t);
extern void  pa_free(void *);

static inline __attribute__((always_inline)) int16_t dly_q21_to_s16(int32_t x) {
    int32_t v = (x + 32) >> 6;
    if (v >  32767) v =  32767;
    if (v < -32768) v = -32768;
    return (int16_t)v;
}
static inline __attribute__((always_inline)) int32_t dly_s16_to_q21(int16_t s) { return ((int32_t)s) * 64; }

/* WHEN THE TIME KNOB MOVES while a note rings, the echo must not jump: a read pointer that lands on a different place in the line is a waveform discontinuity, a click,
 * every knob step. The pointer instead GLIDES to the new delay at no more than 32/256 of a sample per sample (12.5 %: the swoop of a tape delay, about two semitones at
 * the most) and reads between two samples (linear interpolation), so the echo bends instead of cutting. The same interpolated read gives the chorus / wow offset a smooth
 * sweep (it used to move in whole samples: grain). */
#define DLY_GLIDE_Q8 32
/* A BIG jump of TIME (more than DLY_JUMP_SAMPLES) would take seconds to glide, so it crossfades instead: a second read head sits at the new delay and takes over from the
 * first over DLY_XF_LEN samples (46 ms), no pitch swoop, no click. Smaller moves glide, which is what a turning knob needs. */
#define DLY_JUMP_SAMPLES 2048
#define DLY_XF_LEN       2048

/* the echo at `len_q8` (1/256 sample) behind the write point, interpolated */
static inline __attribute__((always_inline)) int32_t dly_read_q8(const delay_state_t *s, int32_t len_q8) {
    const uint32_t ip = (uint32_t)len_q8 >> 8, fr = (uint32_t)len_q8 & 255u;
    const int32_t a = dly_s16_to_q21(s->buffer[(s->write_index - ip) & DLY_BUF_MASK]);         /* ip samples ago */
    const int32_t b = dly_s16_to_q21(s->buffer[(s->write_index - ip - 1u) & DLY_BUF_MASK]);    /* ip + 1 samples ago */
    return a + (int32_t)(((int64_t)(b - a) * (int32_t)fr) >> 8);
}

static inline __attribute__((always_inline)) int32_t dly_clamp_q8(int32_t v) {
    if (v < (16 << 8)) return 16 << 8;
    if (v > (int32_t)((DLY_BUF_LEN - 16u) << 8)) return (int32_t)((DLY_BUF_LEN - 16u) << 8);
    return v;
}

/* the echo taps of one read-head position: the left tap, and the right one where the type has two (PING PONG at 2 x TIME, DUAL at TIME x RATIO) */
static inline __attribute__((always_inline)) void dly_taps(const delay_state_t *s, int32_t head_q8, int32_t mod_q8, int32_t *tap_l, int32_t *tap_r) {
    *tap_l = dly_read_q8(s, dly_clamp_q8(head_q8 - mod_q8));
    *tap_r = *tap_l;
    if (s->type == DELAY_TYPE_PINGPONG) {
        *tap_r = dly_read_q8(s, dly_clamp_q8((int32_t)(2u * (uint32_t)head_q8)));         /* the right side is the next bounce: 2 x TIME */
    } else if (s->type == DELAY_TYPE_DUAL) {
        /* the second tap: TIME x RATIO, RATIO = 0.5 + aux / 2. In 64 bits: the old 32-bit product overflowed above ~3,000 samples and put the "echo" at ~33 ms. */
        uint32_t t2 = (uint32_t)(((uint64_t)(uint32_t)head_q8 * (1048576u + ((uint32_t)s->param_aux >> 1))) >> 21);
        if (t2 < (64u << 8)) t2 = 64u << 8;
        *tap_r = dly_read_q8(s, dly_clamp_q8((int32_t)t2 + mod_q8));
    }
}

/* a gain follows its knob: about 3 ms (1/128 of the distance a sample) and exact once it is close */
static inline __attribute__((always_inline)) int32_t dly_slew(int32_t cur, int32_t tgt) {
    const int32_t d = tgt - cur;
    return (d > -128 && d < 128) ? tgt : cur + (d >> 7);
}

static inline __attribute__((always_inline)) int32_t dly_lfo_tri_q21(uint32_t phase) {
    return (phase < 0x80000000u)
         ? ((int32_t)(phase >> 9) - PA_Q21_ONE)
         : (PA_Q21_ONE - (int32_t)((phase - 0x80000000u) >> 9));
}

/* PA_RAMFN: this loop runs from SRAM (pa_config.h); it makes no call (the helpers above are forced inline) */
PA_RAMFN static void delay_run_stereo(fx_node_t *nd, int32_t *buf_L, int32_t *buf_R, int n) {
    delay_state_t *s = (delay_state_t *)nd->state;
    if (!s || !s->buffer) return;

    int32_t tail_peak = 0;

    for (int i = 0; i < n; i++) {
        int32_t x_L = buf_L[i];
        int32_t x_R = buf_R ? buf_R[i] : x_L;
        int32_t tank_in = nd->tail_draining ? 0 : x_L;

        /* Advance wet Chorus / Tape Wow-Flutter LFO */
        s->lfo_phase += s->lfo_inc;
        int32_t lfo = dly_lfo_tri_q21(s->lfo_phase);
        int32_t mod_q8 = (q21_mul(lfo, s->mod_depth) >> 7);   /* ±64 samples max chorus/wow sweep, in 1/256 sample */

        /* the clean modern types are not modulated */
        if (s->type >= DELAY_TYPE_DIGITAL) mod_q8 = 0;

        /* the read head follows the TIME knob: a small move GLIDES (a turning knob), a big jump CROSSFADES to a second head (REVERSE keeps its own integer chunk logic
         * and takes the length at once) */
        {
            const int32_t tgt = (int32_t)(s->delay_length << 8);
            const int32_t jump = DLY_JUMP_SAMPLES << 8;
            int32_t cur = (int32_t)s->cur_q8, b = (int32_t)s->b_q8;
            if (s->type == DELAY_TYPE_REVERSE) { cur = tgt; s->xf_on = 0u; }
            else if (!s->xf_on) {
                const int32_t d = tgt - cur;
                if (d > jump || d < -jump) { s->xf_on = 1u; s->xf_pos = 0u; b = tgt; }                                   /* a big jump: crossfade to the new delay */
                else if (d) cur += (d > DLY_GLIDE_Q8) ? DLY_GLIDE_Q8 : (d < -DLY_GLIDE_Q8 ? -DLY_GLIDE_Q8 : d);
            } else {
                const int32_t d = tgt - b;
                if (d > jump || d < -jump) {                                                                              /* another big jump mid-fade: turn round from the head that has the larger weight */
                    if (2u * s->xf_pos >= (uint32_t)DLY_XF_LEN) cur = b;
                    b = tgt; s->xf_pos = 0u;
                } else if (d) b += (d > DLY_GLIDE_Q8) ? DLY_GLIDE_Q8 : (d < -DLY_GLIDE_Q8 ? -DLY_GLIDE_Q8 : d);          /* the new head follows small moves too */
                if (++s->xf_pos >= (uint16_t)DLY_XF_LEN) { cur = b; s->xf_on = 0u; s->xf_pos = 0u; }                     /* the fade is done: the new head is the head */
            }
            s->cur_q8 = (uint32_t)cur; s->b_q8 = (uint32_t)b;
        }
        s->fb_s  = dly_slew(s->fb_s,  s->feedback);
        s->mix_s = dly_slew(s->mix_s, s->mix);
        s->dry_s = dly_slew(s->dry_s, s->dry_gain);

        int32_t delayed_L, delayed_R;
        dly_taps(s, (int32_t)s->cur_q8, mod_q8, &delayed_L, &delayed_R);
        if (s->xf_on) {
            int32_t bl, br;
            dly_taps(s, (int32_t)s->b_q8, mod_q8, &bl, &br);
            const int32_t w = (int32_t)(((uint32_t)s->xf_pos * (uint32_t)PA_Q21_ONE) / (uint32_t)DLY_XF_LEN);          /* 0 -> 1 */
            delayed_L += q21_mul(bl - delayed_L, w);
            delayed_R += q21_mul(br - delayed_R, w);
        }
        if (s->type == DELAY_TYPE_REVERSE) {
            /* two backward readers half a chunk apart: index = w - 2p - 1 moves BACKWARDS
             * one sample per sample through the last two chunks */
            uint32_t N = s->delay_length, p = s->rev_phase, q = p + (N >> 1);
            if (q >= N) q -= N;
            int32_t a = dly_s16_to_q21(s->buffer[(s->write_index - 2u * p - 1u) & DLY_BUF_MASK]);
            int32_t b = dly_s16_to_q21(s->buffer[(s->write_index - 2u * q - 1u) & DLY_BUF_MASK]);
            /* triangular windows, 0 at a reader's jump, and wa + wb = 1 */
            uint32_t pa_ = (p < N - p) ? p : N - p, qb = (q < N - q) ? q : N - q;
            int32_t wa = (int32_t)(((uint64_t)pa_ << 22) / N), wb = (int32_t)(((uint64_t)qb << 22) / N);
            delayed_L = sat32((int64_t)q21_mul(a, wa) + q21_mul(b, wb));
            delayed_R = delayed_L;
            if (++s->rev_phase >= N) s->rev_phase = 0;
        }
        int32_t eff_fb    = s->fb_s;

        switch (s->type) {
            case DELAY_TYPE_ANALOG: {
                /* Tube warmth LPF + wet Phaser modulation (Manual Page 10) */
                s->lp_state += (delayed_L - s->lp_state) >> 2;
                int32_t warm = s->lp_state;
                int32_t a = 524288 + (q21_mul((lfo + PA_Q21_ONE) >> 1, s->param_aux) >> 1);
                int32_t ap_out = sat32((int64_t)(-q21_mul(a, warm)) + s->ap_x1 + q21_mul(a, s->ap_y1));
                s->ap_x1 = warm;
                s->ap_y1 = ap_out;
                delayed_L = q21_mul(warm, PA_Q21_ONE - s->param_aux) + q21_mul(ap_out, s->param_aux);
                break;
            }
            case DELAY_TYPE_DUCK: {
                /* Ducking with `Unpack` release smoothness control (Manual Page 10) */
                int32_t abs_x = (x_L < 0) ? -x_L : x_L;
                int32_t rel_shift = 4 + ((s->param_aux * 5) >> 21); /* Unpack slows sudden volume rise */
                if (abs_x > s->envelope) s->envelope += (abs_x - s->envelope) >> 3;
                else                     s->envelope -= s->envelope >> rel_shift;
                int32_t duck = (PA_Q21_ONE > s->envelope) ? (PA_Q21_ONE - s->envelope) : 0;
                eff_fb = q21_mul(s->fb_s, duck);
                break;
            }
            case DELAY_TYPE_DTAPE: {
                /* Tape Bias (`Grit` = param_aux) + Tape Crinkle (`Speed`) + Wow/Flutter (`Depth`) */
                int32_t grit_gain = PA_Q21_ONE + s->param_aux;
                int32_t biased = q21_mul(delayed_L, grit_gain);
                /* Monotonic tape saturation: clamp at turnover peak sqrt(4/3) = 1.155 FS to prevent foldback */
                if (biased >  2421582) biased =  2421582;
                if (biased < -2421582) biased = -2421582;
                int32_t d_sq = q21_mul(biased, biased);
                int32_t d_cu = q21_mul(d_sq, biased);
                delayed_L = sat32((int64_t)biased - (d_cu >> 2));
                break;
            }
            case DELAY_TYPE_DUAL:
                break;                       /* the two taps stay apart (L / R) */
            case DELAY_TYPE_LOFI: {
                /* Overdrive Grit + vinyl bit-crush/filter */
                int32_t driven = q21_mul(delayed_L, PA_Q21_ONE + s->param_aux);
                delayed_L = sat32(driven) & ~0x0FFFu;
                break;
            }
            case DELAY_TYPE_DIGITAL:
            case DELAY_TYPE_REVERSE: {
                /* TONE: a one-pole low-pass, ~1.5 kHz (0 %) .. open (100 %) */
                int32_t a = 419430 + q21_mul(s->param_aux, 1677722);
                s->tone_lp += q21_mul(delayed_L - s->tone_lp, a);
                delayed_L = s->tone_lp;
                delayed_R = delayed_L;
                break;
            }
            case DELAY_TYPE_PINGPONG: {
                /* TONE on the bounce that feeds back (the first left repeat stays clear) */
                int32_t a = 419430 + q21_mul(s->param_aux, 1677722);
                s->tone_lp += q21_mul(delayed_R - s->tone_lp, a);
                delayed_R = s->tone_lp;
                break;
            }
            case DELAY_TYPE_SLAPBACK: {
                /* darker TONE range (~700 Hz .. ~8 kHz) and a gentle saturation */
                int32_t a = 209715 + q21_mul(s->param_aux, 838861);
                s->tone_lp += q21_mul(delayed_L - s->tone_lp, a);
                int32_t v = s->tone_lp;
                /* Monotonic warm saturation: clamp at turnover peak sqrt(8/3) = 1.633 FS to prevent foldback */
                if (v >  3424632) v =  3424632;
                if (v < -3424632) v = -3424632;
                int32_t v3 = q21_mul(q21_mul(v, v), v);
                delayed_L = sat32((int64_t)v - (v3 >> 3));
                delayed_R = delayed_L;
                eff_fb = q21_mul(s->fb_s, 699051);              /* x 1/3: a single repeat */
                break;
            }
            default: break;
        }
        /* the feedback and a mono output carry both taps in DUAL, the one echo otherwise;
         * PING PONG feeds back the right bounce (left T, right 2T, left 3T ...) */
        int32_t tap1 = delayed_L;
        if (s->type == DELAY_TYPE_DUAL) delayed_L = (delayed_L >> 1) + (delayed_R >> 1);
        int32_t pp_fb = delayed_R;

        /* 1st-order DC Blocker in delay feedback path (R = 0.995 => ~15 Hz HPF)
         * Kills circulating sub-audio DC drift and non-linear tape grit bias! */
        int32_t dc_in = delayed_L;
        int32_t dc_out = sat32((int64_t)dc_in - s->dc_x1 + q21_mul(2086666, s->dc_y1));
        s->dc_x1 = dc_in;
        s->dc_y1 = dc_out;
        delayed_L = dc_out;
        /* what the two outputs play: the same echo, or DUAL's / PING PONG's two taps */
        int32_t out_L = delayed_L, out_R = delayed_L;
        if (s->type == DELAY_TYPE_DUAL) {
            if (buf_R) { out_L = tap1; out_R = delayed_R; }
        } else if (s->type == DELAY_TYPE_PINGPONG) {
            if (buf_R) { out_L = tap1; out_R = delayed_R; }
            else       { out_L = (tap1 >> 1) + (delayed_R >> 1); }
        }

        int32_t fb_src = (s->type == DELAY_TYPE_PINGPONG) ? pp_fb : delayed_L;
        int32_t to_write = sat32((int64_t)tank_in + q21_mul(fb_src, eff_fb));
        s->buffer[s->write_index] = dly_q21_to_s16(to_write);
        s->write_index = (s->write_index + 1u) & DLY_BUF_MASK;

        int32_t abs_del = (delayed_L < 0) ? -delayed_L : delayed_L;
        if (abs_del > tail_peak) tail_peak = abs_del;

        if (nd->tail_draining) {
            buf_L[i] = sat32((int64_t)x_L + q21_mul(out_L, s->mix_s));
            if (buf_R) buf_R[i] = sat32((int64_t)x_R + q21_mul(out_R, s->mix_s));
        } else {
            buf_L[i] = sat32((int64_t)q21_mul(x_L, s->dry_s) + q21_mul(out_L, s->mix_s));
            if (buf_R) buf_R[i] = sat32((int64_t)q21_mul(x_R, s->dry_s) + q21_mul(out_R, s->mix_s));
        }
    }
    s->snap = 0;                                   /* the line has run: from now on a new length glides */

    if (nd->tail_draining) {
        if (!s->tail_active) {
            s->tail_active = 1;
            s->tail_hold = (s->delay_length / PA_BLOCK_N) + 8u;
        } else if (s->tail_hold > 0) {
            s->tail_hold--;
        }
        if (s->tail_active && s->tail_hold == 0 && tail_peak < 2048) {
            nd->tail_draining   = 0;
            s->tail_active      = 0;
            nd->pending_destroy = 1;   /* never pa_free() in the ISR - UI thread collects it */
        }
    } else {
        s->tail_active = 0;
        s->tail_hold   = 0;
    }
}

static void delay_run(fx_node_t *nd, int32_t *buf, int n) {
    /* delay_run_stereo lives in SRAM (PA_RAMFN), more than the 4 MB a call instruction reaches from the flash: call it through a pointer (the empty asm stops the
     * optimizer turning the pointer back into a direct call) */
    void (*fn)(fx_node_t *, int32_t *, int32_t *, int) = delay_run_stereo;
    __asm__ volatile("" : "+r"(fn));
    fn(nd, buf, 0, n);
}

static void delay_create(fx_node_t *nd) {
    delay_state_t *s = (delay_state_t *)pa_alloc(sizeof(delay_state_t));
    if (s) {
        s->delay_length = 14700u;
        s->cur_q8       = 14700u << 8;
        s->snap         = 1u;
        s->time_knob    = 1048576;     /* (a knob value is always written before use) */
        s->buffer = (int16_t *)pa_alloc(DLY_BUF_LEN * sizeof(int16_t));
        if (!s->buffer) { pa_free(s); nd->state = 0; return; }   /* IMP-13: all-or-nothing */
        if (s->buffer) {
            for (uint32_t i = 0; i < DLY_BUF_LEN; i++) s->buffer[i] = 0;
        }
        s->type        = DELAY_TYPE_DTAPE;
        s->feedback    = 838861;
        s->mix         = 838861;
        s->dry_gain    = PA_Q21_ONE;
        s->fb_s = s->feedback; s->mix_s = s->mix; s->dry_s = s->dry_gain;
        s->param_aux   = 524288;
        s->lfo_phase   = 0;
        s->lfo_inc     = 97390u; /* ~1.0 Hz wet modulation */
        s->mod_depth   = 262144;
        s->write_index = 0;
        s->envelope    = 0;
        s->lp_state    = 0;
        s->ap_x1       = 0;
        s->ap_y1       = 0;
        s->dc_x1       = 0;
        s->dc_y1       = 0;
    }
    nd->state = s;
}

static void delay_destroy(fx_node_t *nd) {
    delay_state_t *s = (delay_state_t *)nd->state;
    if (s) {
        if (s->buffer) pa_free(s->buffer);
        pa_free(s);
    }
    nd->state = 0;
}

/* TIME knob -> delay length in samples, per type. Shared with the display.
 *   most types   23 ms .. 743 ms (the whole line)
 *   PING PONG    23 ms .. 371 ms (its right tap is at 2 x TIME)
 *   REVERSE      23 ms .. 371 ms (a backward read spans two chunks)
 *   SLAPBACK     40 ms .. 200 ms */
static uint32_t dly_len_from_knob(int type, int32_t val) {
    if (val < 0) val = 0;
    if (val > PA_Q21_ONE) val = PA_Q21_ONE;
    uint32_t lo = 1024u, hi = DLY_BUF_LEN - 128u;
    if (type == DELAY_TYPE_PINGPONG || type == DELAY_TYPE_REVERSE) hi = DLY_BUF_LEN / 2u - 64u;
    if (type == DELAY_TYPE_SLAPBACK) { lo = (uint32_t)PA_SAMPLE_RATE_HZ / 25u; hi = (uint32_t)PA_SAMPLE_RATE_HZ / 5u; }   /* 40 .. 200 ms */
    uint32_t len = lo + (uint32_t)(((uint64_t)(uint32_t)val * (hi - lo)) >> 21);
    if (len >= DLY_BUF_LEN) len = DLY_BUF_LEN - 1u;
    return len;
}

uint32_t pa_dly_time_ms(int type, int32_t knob_q21) {
    return (dly_len_from_knob(type, knob_q21) * 1000u + (uint32_t)PA_SAMPLE_RATE_HZ / 2u) / (uint32_t)PA_SAMPLE_RATE_HZ;
}

static void delay_set_param(fx_node_t *nd, int id, int32_t val) {
    delay_state_t *s = (delay_state_t *)nd->state;
    if (!s) return;
    switch (id) {
        case 0: /* Knob 0: TIME */
            s->time_knob = val;
            s->delay_length = dly_len_from_knob((int)s->type, val);
            if (s->rev_phase >= s->delay_length) s->rev_phase = 0;
            if (s->snap) { s->cur_q8 = s->delay_length << 8; s->xf_on = 0u; }       /* a fresh line has nothing in it to click */
            break;
        case 1: /* Knob 1: FDBK. Cap at 0.92 so 100 % can't run away into a full-scale squeal */
            s->feedback = q21_mul(val, 1929380);
            break;
        case 2: /* Knob 2: MIX. The dry stays at unity up to 50 %, then fades out. */
            s->mix = val;
            s->dry_gain = (val <= PA_Q21_ONE / 2) ? PA_Q21_ONE
                        : (val >= PA_Q21_ONE ? 0 : 2 * (PA_Q21_ONE - val));
            break;
        case 3: /* Knob 3: TONE / AUX */
            s->param_aux = val;
            break;
        case 5: /* Speed */
            s->lfo_inc = 9739u + (uint32_t)(((uint64_t)(uint32_t)val * 389567u) >> 21);
            break;
        case 6: /* Depth */
            s->mod_depth = val;
            break;
        case 7: /* Tail */
            nd->tail_enabled = (val != 0) ? 1u : 0u;
            break;
        case 10: /* Explicit Model Type selection from UI dropdown */
            s->type = (delay_type_t)((uint32_t)val % DELAY_TYPE_COUNT);
            s->delay_length = dly_len_from_knob((int)s->type, s->time_knob);   /* per-type range: the read pointer glides there */
            if (s->snap) { s->cur_q8 = s->delay_length << 8; s->xf_on = 0u; }
            s->rev_phase = 0;
            /* tone_lp is NOT zeroed: it is a one-pole low-pass of the echo, it settles in a few samples on its own, and zeroing it under a ringing note is a step in the output */
            break;
        default: break;
    }
}

static void delay_reset(fx_node_t *nd) {
    delay_state_t *s = (delay_state_t *)nd->state;
    if (s && s->buffer) {
        memset(s->buffer, 0, DLY_BUF_LEN * sizeof(int16_t));
        s->write_index = 0;
        s->lp_state    = 0;
        s->ap_x1       = 0;
        s->ap_y1       = 0;
        s->dc_x1       = 0;
        s->dc_y1       = 0;
        s->envelope    = 0;
        s->tone_lp     = 0;
        s->rev_phase   = 0;
        s->tail_hold   = 0;
        s->cur_q8      = s->delay_length << 8;       /* the line was just cleared: the pointer is where the knob is */
        s->xf_on       = 0u; s->xf_pos = 0u; s->b_q8 = s->cur_q8;
        s->fb_s = s->feedback; s->mix_s = s->mix; s->dry_s = s->dry_gain;
        s->snap        = 1u;
    }
}

fx_node_t *fx_delay_new(void) {
    static fx_node_t n;
    n.run          = delay_run;
    n.run_stereo   = delay_run_stereo;
    n.create       = delay_create;
    n.destroy      = delay_destroy;
    n.set_param    = delay_set_param;
    n.reset        = delay_reset;
    n.active       = 1;
    n.frame        = PA_BLOCK_N;
    n.tag          = "delay";
    n.tail_enabled = 1;
    n.tail_draining= 0;
    return &n;
}
