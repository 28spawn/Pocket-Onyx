/* SPDX-License-Identifier: GPL-3.0-only */
/* Copyright (C) 2026 The Onyx Authors (@28spawn) */
/* custom-fw/src/sys/pa_mix_limiter.h - the limiter on the guitar + drums sum
 *
 * Header-only: pa_audio_core.c uses it, and test/test_mix_limiter.c measures the same code.
 *
 * The problem (2026-10-23, test/test_drum_clip.c): the chain's own limiter holds the guitar
 * to -0.2 dBFS, and the Groove Station adds its drums AFTER it. With a loud preset the sum
 * went over, and the output's static soft ceiling (4:1 above -1 dBFS, hard at 0 dBFS)
 * waveshaped it: 38 of the 49 patterns hard-clipped the DAC under a -1 dBFS guitar.
 *
 * The limiter turns the WHOLE sum down a little when a drum hit would not fit, instead:
 *   - the drums pass through a 72-sample (1.6 ms) look-ahead delay here; the guitar is not
 *     delayed at all (no added latency on the instrument)
 *   - per 8-sample chunk it predicts the sum's peak when that drum chunk comes out of the
 *     delay: the guitar's recent peak (held 11.6..23 ms, instant rise) plus the drum chunk's
 *     peak - a sum of peaks, so it can only over-estimate
 *   - the gain that keeps that prediction under -1 dBFS (the soft ceiling's knee) is held
 *     for 9 chunks, released over ~80 ms, averaged over 8 chunks and interpolated per
 *     sample (the drum bus limiter's scheme): it is down before the hit arrives, and it
 *     moves without steps
 *   - the guitar can jump up while drums are already in the delay (a strum on the beat):
 *     each chunk re-checks the drums in flight against the guitar as it is now, and goes
 *     down at once (over one 8-sample chunk) when they no longer fit
 *   - a chunk with no drum in it asks for nothing: with the drums silent the guitar is not
 *     touched (bit-exact), and the output stage is what it was
 * The soft ceiling stays after it as a safety net (a guitar that jumps up in the middle of
 * a drum hit, or USB playback, which is not part of this sum).
 */
#ifndef PA_MIX_LIMITER_H
#define PA_MIX_LIMITER_H

#include <stdint.h>

#define PA_MIXLIM_CHUNK      8u
#define PA_MIXLIM_CHUNKS     8u
#define PA_MIXLIM_LOOKAHEAD  (PA_MIXLIM_CHUNK * (PA_MIXLIM_CHUNKS + 1u))   /* 72 */
#define PA_MIXLIM_HOLD_WIN   512u                                          /* 11.6 ms */
#define PA_MIXLIM_CEIL       (0.89125f * 2097152.0f)   /* -1 dBFS: the soft ceiling's knee */
#define PA_MIXLIM_REL        0.99102f                   /* per chunk: ~80 ms release */

typedef struct {
    uint8_t  inited;
    int32_t  dly[PA_MIXLIM_LOOKAHEAD];     /* the drums, delayed */
    uint16_t pos;
    uint16_t quiet;                        /* samples since the last non-zero drum input */
    float    d_peak;                       /* drum peak of the chunk being filled */
    float    g_win, g_prev;                /* the guitar's held peak: this window, the last */
    uint16_t g_n;
    float    ring[PA_MIXLIM_CHUNKS + 1u];  /* the gain each chunk needs */
    float    dring[PA_MIXLIM_CHUNKS + 1u]; /* the drum peak of each chunk in the delay */
    float    box[PA_MIXLIM_CHUNKS];
    uint8_t  ri, bi;
    float    rel;                          /* after the hold and the release */
    float    gain, gain_from;              /* end of this chunk / of the last one */
    float    gmin;                         /* lowest gain since the caller last cleared it */
} pa_mixlim_t;

static inline void pa_mixlim_reset(pa_mixlim_t *m) {
    for (uint32_t k = 0; k < PA_MIXLIM_LOOKAHEAD; k++) m->dly[k] = 0;
    for (uint32_t k = 0; k <= PA_MIXLIM_CHUNKS; k++) { m->ring[k] = 1.0f; m->dring[k] = 0.0f; }
    for (uint32_t k = 0; k < PA_MIXLIM_CHUNKS; k++) m->box[k] = 1.0f;
    m->pos = 0; m->quiet = PA_MIXLIM_LOOKAHEAD; m->d_peak = 0.0f;
    m->g_win = m->g_prev = 0.0f; m->g_n = 0;
    m->ri = m->bi = 0;
    m->rel = m->gain = m->gain_from = 1.0f;
    m->gmin = 1.0f;
    m->inited = 1;
}

/* idle: no drum in the delay line and the gain fully back at 1 */
static inline int pa_mixlim_idle(const pa_mixlim_t *m) {
    return m->quiet >= PA_MIXLIM_LOOKAHEAD && m->gain >= 1.0f && m->gain_from >= 1.0f && m->rel >= 1.0f;
}

/* The peaks are tracked as integers (a peak is the absolute value of a sample): the compare is the same as on the floats they were (a conversion to float
 * never reverses an order) and each sample saves three int -> float conversions. The struct keeps floats. */
static inline int32_t pa_mixlim_abs(int32_t v) { const int32_t a = v < 0 ? -v : v; return a < 0 ? 2147483647 : a; }
static inline int32_t pa_mixlim_f2i(float f) { return f >= 2147483648.0f ? 2147483647 : (int32_t)f; }

/* Adds the drums (drums may be 0 = silence) into L / R and limits the sum. Any n works
 * (the chunks follow the delay position, not the block). R may be 0 or the same as L. */
static inline void pa_mixlim_process(pa_mixlim_t *m, int32_t *L, int32_t *R,
                                     const int32_t *drums, int n) {
    if (!m->inited) pa_mixlim_reset(m);
    if (!drums && pa_mixlim_idle(m)) return;          /* nothing to add, nothing to undo */
    const int stereo = (R && R != L);

    /* The state lives in locals for the whole block (written back at the end): each m->field in the per-sample loop was a memory access of
     * 7-10 cycles on this core, and the compiler could not keep them in registers because the stores to dly[], L[] and R[] might alias the
     * struct. The arithmetic is unchanged. */
    int32_t  d_peak = pa_mixlim_f2i(m->d_peak), g_win = pa_mixlim_f2i(m->g_win), g_prev = pa_mixlim_f2i(m->g_prev);
    float    rel = m->rel, gain = m->gain, gain_from = m->gain_from, gmin = m->gmin;
    int      unity = (gain_from >= 1.0f && gain >= 1.0f);       /* the gain is 1 all through this chunk: no scaling, and no gain to work out per sample */
    uint32_t quiet = m->quiet, g_n = m->g_n, pos = m->pos, ri = m->ri, bi = m->bi;

    for (int i = 0; i < n; i++) {
        const int32_t din = drums ? drums[i] : 0;
        const int32_t a = pa_mixlim_abs(din);
        if (a > d_peak) d_peak = a;
        if (din) quiet = 0; else if (quiet < 0xFFFFu) quiet++;

        /* the guitar's held peak: instant rise, held for 1..2 windows */
        int32_t gl = pa_mixlim_abs(L[i]);
        if (stereo) { const int32_t gr = pa_mixlim_abs(R[i]); if (gr > gl) gl = gr; }
        if (gl > g_win) g_win = gl;
        if (++g_n >= PA_MIXLIM_HOLD_WIN) { g_prev = g_win; g_win = 0.0f; g_n = 0; }

        const uint32_t p0 = pos;
        const int32_t dd = m->dly[p0];
        m->dly[p0] = din;
        pos = (p0 + 1u >= PA_MIXLIM_LOOKAHEAD) ? 0u : p0 + 1u;                      /* a compare, not a modulo by 72 */

        const uint32_t k = p0 % PA_MIXLIM_CHUNK;
        if (k == 0u) {
            gain_from = gain;
            float need = 1.0f;
            if (d_peak > 0) {
                const float g = (float)(g_win > g_prev ? g_win : g_prev);
                const float sum = g + (float)d_peak;
                if (sum > PA_MIXLIM_CEIL) need = PA_MIXLIM_CEIL / sum;
            }
            ri = (ri + 1u) % (PA_MIXLIM_CHUNKS + 1u);
            m->ring[ri] = need;
            m->dring[ri] = (float)d_peak;
            d_peak = 0;
            float hold = 1.0f;
            for (uint32_t r = 0; r <= PA_MIXLIM_CHUNKS; r++) if (m->ring[r] < hold) hold = m->ring[r];
            rel = (hold < rel) ? hold : hold + (rel - hold) * PA_MIXLIM_REL;
            if (rel > 0.99999f) rel = 1.0f;
            bi = (bi + 1u) % PA_MIXLIM_CHUNKS;
            m->box[bi] = rel;
            float sm = 0.0f;
            for (uint32_t r = 0; r < PA_MIXLIM_CHUNKS; r++) sm += m->box[r];
            gain = sm * (1.0f / (float)PA_MIXLIM_CHUNKS);
            /* The guitar can jump up while drums are already in the delay (a strum on the
             * beat). The prediction above used the guitar as it was, so check the drums in
             * flight against the guitar as it is NOW and, if they no longer fit, go down at
             * once (over this one chunk, 0.18 ms) and release from there as usual. */
            {
                float dmax = 0.0f;
                for (uint32_t r = 0; r <= PA_MIXLIM_CHUNKS; r++) if (m->dring[r] > dmax) dmax = m->dring[r];
                if (dmax > 0.0f) {
                    const float gnow = (float)(g_win > g_prev ? g_win : g_prev);
                    const float sum = gnow + dmax;
                    if (sum > PA_MIXLIM_CEIL) {
                        const float urgent = PA_MIXLIM_CEIL / sum;
                        if (urgent < gain) {
                            gain = urgent;
                            if (urgent < rel) rel = urgent;
                            for (uint32_t r = 0; r < PA_MIXLIM_CHUNKS; r++) if (m->box[r] > urgent) m->box[r] = urgent;
                        }
                    }
                }
            }
            if (gain > 0.99999f) gain = 1.0f;
            if (gain < gmin) gmin = gain;
            unity = (gain_from >= 1.0f && gain >= 1.0f);
        }
        const float g = unity ? 1.0f : gain_from + (gain - gain_from) * ((float)(k + 1u) * (1.0f / (float)PA_MIXLIM_CHUNK));

        /* The sum is clamped to 32 bits first and scaled as a 32-bit value: a 64-bit <-> float conversion is a call into the compiler's
         * run-time library on this core (__floatdisf / __fixsfdi), and the gain is below 1 for ~80 ms after every limiting event. The
         * buffers are Q21, so a sum anywhere near the clamp is already far beyond full scale. */
        int64_t sl = (int64_t)L[i] + dd;
        if (sl > 2147483647) sl = 2147483647;
        if (sl < -2147483647 - 1) sl = -2147483647 - 1;
        int32_t sl32 = (int32_t)sl;
        if (g < 1.0f) sl32 = (int32_t)((float)sl32 * g);
        L[i] = sl32;
        if (stereo) {
            int64_t sr = (int64_t)R[i] + dd;
            if (sr > 2147483647) sr = 2147483647;
            if (sr < -2147483647 - 1) sr = -2147483647 - 1;
            int32_t sr32 = (int32_t)sr;
            if (g < 1.0f) sr32 = (int32_t)((float)sr32 * g);
            R[i] = sr32;
        }
    }
    m->d_peak = (float)d_peak; m->g_win = (float)g_win; m->g_prev = (float)g_prev;
    m->rel = rel; m->gain = gain; m->gain_from = gain_from; m->gmin = gmin;
    m->quiet = (uint16_t)quiet; m->g_n = (uint16_t)g_n; m->pos = (uint16_t)pos; m->ri = (uint8_t)ri; m->bi = (uint8_t)bi;
}

#endif /* PA_MIX_LIMITER_H */
