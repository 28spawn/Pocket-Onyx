/* SPDX-License-Identifier: GPL-3.0-only */
/* Copyright (C) 2026 The Onyx Authors (@28spawn) */
/* custom-fw/src/dsp/drum_codec.h - how the Groove Station's voices are read back
 *
 * The kit (drum_kit.inc, made by tools/gen_drum_kit.py) is not stored as 16-bit PCM any more.  Each voice is one of
 *
 *   PA_DK_PCM16   16-bit samples (the metronome click)
 *   PA_DK_ADPCM4  IMA-ADPCM, 4 bits a sample, low nibble first, predictor and step index start at 0
 *   PA_DK_ULAW8   G.711 mu-law, one byte a sample
 *   PA_DK_KICK    synthesised: a sine whose pitch falls, two exponential decays, a burst of low-passed noise, a saturator
 *   PA_DK_TOM     synthesised: a pitch-gliding sine, a decaying overtone at 1.58x, band-passed noise, a saturator
 *
 * All of them are read strictly forward, one source sample at a time (pa_dsrc_next), which is how the engine plays a voice.
 * The synthesised voices are the same recipe the generator measures: exponential envelopes as one multiply a sample, a
 * 256-entry sine table with linear interpolation, a 32-bit LCG for the noise, one-pole filters, a rational tanh.
 */
#ifndef PA_DRUM_CODEC_H
#define PA_DRUM_CODEC_H

#include <stdint.h>
#include "pa_drums.h"      /* pa_dsrc_t, a voice's read position */

enum { PA_DK_PCM16 = 0, PA_DK_ADPCM4, PA_DK_ULAW8, PA_DK_KICK, PA_DK_TOM };

/* the constants of one synthesised voice (written by the generator) */
typedef struct {
    float gain;                    /* brings the raw voice to the kit's peak */
    float pinc0, g0, kg;           /* pitch: base (turns a sample), glide start, glide decay multiplier */
    float a1, k1, a2, k2;          /* body and overtone/second-stage envelopes: start, multiplier */
    float a3, k3;                  /* click / attack-noise envelope */
    float a_hp, a_lp;              /* noise filter coefficients (a_hp is 0 for the kick) */
    float att_inv;                 /* 1 / attack length in samples */
    float drive;                   /* saturator input gain */
    uint32_t seed;
    const float *sin_tab;          /* 257 entries: one turn in 256 steps and the wrap point */
} pa_dsynth_t;

static const int16_t k_ima_step[89] = {
    7, 8, 9, 10, 11, 12, 13, 14, 16, 17, 19, 21, 23, 25, 28, 31, 34, 37, 41, 45, 50, 55, 60, 66, 73, 80, 88, 97, 107, 118, 130,
    143, 157, 173, 190, 209, 230, 253, 279, 307, 337, 371, 408, 449, 494, 544, 598, 658, 724, 796, 876, 963, 1060, 1166, 1282,
    1411, 1552, 1707, 1878, 2066, 2272, 2499, 2749, 3024, 3327, 3660, 4026, 4428, 4871, 5358, 5894, 6484, 7132, 7845, 8630,
    9493, 10442, 11487, 12635, 13899, 15289, 16818, 18500, 20350, 22385, 24623, 27086, 29794, 32767,
};
static const int8_t k_ima_idx[8] = { -1, -1, -1, -1, 2, 4, 6, 8 };

static inline void pa_dsrc_start(pa_dsrc_t *s, uint8_t kind, const void *data) {
    s->n = 0; s->rng = 0; s->pred = 0; s->sidx = 0;
    s->ph = 0.0f; s->g = 0.0f; s->e1 = s->e2 = s->e3 = 0.0f; s->l1 = s->l2 = s->l3 = 0.0f;
    if (kind == PA_DK_KICK || kind == PA_DK_TOM) {
        const pa_dsynth_t *p = (const pa_dsynth_t *)data;
        s->rng = p->seed * 2654435761u + 12345u;
        s->g = p->g0; s->e1 = p->a1; s->e2 = p->a2; s->e3 = p->a3;
    }
}

static inline float pa_dsyn_tanh(float u) {
    const float u2 = u * u;
    if (u2 < 0.25f) return u * (1.0f - u2 * (0.333333343f - u2 * 0.133333340f));      /* |u| < 0.5, no divide: tanh to 0.03 % */
    if (u > 3.0f) return 1.0f;
    if (u < -3.0f) return -1.0f;
    return u * (27.0f + u2) / (27.0f + 9.0f * u2);
}

static inline float pa_dsyn_sin(const float *tab, float turns) {      /* turns in [0, 1) */
    const float x = turns * 256.0f;
    const int32_t i = (int32_t)x;
    return tab[i] + (tab[i + 1] - tab[i]) * (x - (float)i);
}

static inline float pa_dsyn_noise(uint32_t *rng) {                    /* uniform on +-sqrt(3) */
    *rng = *rng * 1664525u + 1013904223u;
    return (float)(*rng >> 8) * (1.7320508f / 8388608.0f) - 1.7320508f;
}

/* The next source sample of the voice, in 16-bit units; `frames` is the voice's length (the synthesised voices fade out over their
 * last 10 ms).  Calling it more than `frames` times is the caller's error. */
static inline float pa_dsrc_next(pa_dsrc_t *s, uint8_t kind, const void *data, uint32_t frames) {
    const uint32_t i = s->n++;
    switch (kind) {
    case PA_DK_PCM16:
        return (float)((const int16_t *)data)[i];
    case PA_DK_ULAW8: {
        const uint32_t u = (~((const uint8_t *)data)[i]) & 0xFFu;
        const int32_t t = ((int32_t)(((u & 15u) << 3) + 0x84u) << ((u >> 4) & 7u)) - 0x84;
        return (float)((u & 0x80u) ? -t : t);
    }
    case PA_DK_ADPCM4: {
        const uint32_t code = (((const uint8_t *)data)[i >> 1] >> ((i & 1u) * 4u)) & 15u;
        const int32_t step = k_ima_step[s->sidx];
        int32_t vp = step >> 3;
        if (code & 4u) vp += step;
        if (code & 2u) vp += step >> 1;
        if (code & 1u) vp += step >> 2;
        s->pred += (code & 8u) ? -vp : vp;
        if (s->pred > 32767) s->pred = 32767; else if (s->pred < -32768) s->pred = -32768;
        s->sidx += k_ima_idx[code & 7u];
        if (s->sidx < 0) s->sidx = 0; else if (s->sidx > 88) s->sidx = 88;
        return (float)s->pred;
    }
    default: {
        const pa_dsynth_t *p = (const pa_dsynth_t *)data;
        float ph = s->ph + p->pinc0 + s->g;
        if (ph >= 1.0f) ph -= 1.0f;
        s->ph = ph;
        float ramp = (float)i * p->att_inv;
        if (ramp > 1.0f) ramp = 1.0f;
        float x;
        if (kind == PA_DK_KICK) {
            if (s->e3 > 1.0e-3f) s->l1 += p->a_lp * (pa_dsyn_noise(&s->rng) - s->l1);       /* the beater click is gone after ~25 ms: stop making it */
            x = pa_dsyn_sin(p->sin_tab, ph) * (s->e1 + s->e2) * ramp + s->l1 * s->e3;
        } else {
            x = pa_dsyn_sin(p->sin_tab, ph) * s->e1 * ramp;
            if (s->e2 > 4.0e-3f) {                                  /* the overtone, until it is 60 dB down */
                float ph2 = ph * 1.58f;
                if (ph2 >= 1.0f) ph2 -= 1.0f;
                x += 0.25f * pa_dsyn_sin(p->sin_tab, ph2) * s->e2 * ramp;
            }
            if (s->e3 > 1.0e-3f) {                                  /* the attack noise, until it is 60 dB down */
                const float v = pa_dsyn_noise(&s->rng);
                s->l1 += p->a_hp * (v - s->l1); const float h1 = v - s->l1;
                s->l2 += p->a_hp * (h1 - s->l2); const float h2 = h1 - s->l2;
                s->l3 += p->a_lp * (h2 - s->l3);
                x += s->l3 * s->e3;
            }
        }
        float y = pa_dsyn_tanh(p->drive * x) * p->gain;
        if (i + 220u >= frames) y *= (float)(frames - 1u - i) * (1.0f / 219.0f);      /* the last 10 ms fade out */
        /* the envelopes are multiplied down tick by tick; once one is below what is audible it is set to zero, so no value ever reaches the
         * denormal range (arithmetic on those is very slow on most FPUs) */
        s->g *= p->kg; s->e1 *= p->k1;
        s->e2 = (s->e2 > 1.0e-4f) ? s->e2 * p->k2 : 0.0f;
        s->e3 = (s->e3 > 1.0e-3f) ? s->e3 * p->k3 : 0.0f;
        return y;
    }
    }
}

#endif /* PA_DRUM_CODEC_H */
