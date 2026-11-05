/* src/dsp/pa_stereo_spread.h - THE STEREO SPREAD: what makes the headphone jack stereo when nothing else in the chain does (no cabinet in the preset, an amp that holds
 * its own cabinet, the cabinet switched off, or the governor has turned the stereo cabinet off).
 *
 * The mid signal M = (L+R)/2 goes through TWO chains of four first-order allpass filters whose outputs are 90 degrees apart at every frequency from 70 Hz to 12 kHz
 * (v = chain B, u = chain A, u leads v by 90 degrees, within 1.7 degrees over that band: the coefficients come from a numeric least-squares fit of the phase difference).
 * The two channels are the sum and the difference of the two quadrature signals:
 *       L' = (v + k u) / sqrt(1 + k^2)         R' = (v - k u) / sqrt(1 + k^2)         k = width / 100
 * Because u and v are in quadrature, v and u carry no energy in common at any frequency, so |L'| = |R'| at every frequency (the left and right channels are equally loud for
 * ANY input spectrum - an earlier design built from a pair of unrelated allpass chains was balanced only for white noise and made the right ear 4-7 dB louder on a guitar),
 * and each channel keeps the level of the input (the 1/sqrt(1+k^2)). At k = 0 the output is a phase-shifted copy of M in both channels (the caller does not call then);
 * at k = 1 the channels are uncorrelated. Folded to mono the sum is 2v/sqrt(1+k^2): the same magnitude at every frequency, only phase-shifted, and quieter by the
 * decorrelation (-1.3 dB at width 60, -3 dB at 100).
 * A real stereo input keeps its own side signal S = (L-R)/2: L' = S + (v + k u)/sqrt(1+k^2), R' = -S + (v - k u)/sqrt(1+k^2).
 * A first-order allpass is y = c (x - y1) + x1; its state is the previous input and output. */
#ifndef PA_STEREO_SPREAD_H
#define PA_STEREO_SPREAD_H

#include <math.h>
#include <stdint.h>

#define PA_SPREAD_SECTIONS 4

typedef struct { float x1[2][PA_SPREAD_SECTIONS], y1[2][PA_SPREAD_SECTIONS]; } pa_spread_t;

static inline void pa_spread_process(pa_spread_t *s, int32_t *L, int32_t *R, int n, uint8_t width)
{
    static const float k_c[2][PA_SPREAD_SECTIONS] = {
        {  0.411333f, -0.588874f, -0.982929f, -0.906896f },           /* chain A: u, leads by 90 degrees */
        { -0.798937f, -0.958603f, -0.220616f, -0.995398f } };         /* chain B: v */
    const float k = (float)width * 0.01f;
    const float g = 1.0f / sqrtf(1.0f + k * k);
    for (int i = 0; i < n; i++) {
        const float l = (float)L[i], r = (float)R[i];
        const float m = 0.5f * (l + r), side = 0.5f * (l - r);
        float v[2] = { m, m };
        for (int ch = 0; ch < 2; ch++) {
            for (int j = 0; j < PA_SPREAD_SECTIONS; j++) {
                const float y = k_c[ch][j] * (v[ch] - s->y1[ch][j]) + s->x1[ch][j];
                s->x1[ch][j] = v[ch];
                s->y1[ch][j] = (y > -4.0e9f && y < 4.0e9f) ? y : 0.0f;                 /* NaN / wild: never let it circle */
                v[ch] = s->y1[ch][j];
            }
        }
        const float u = v[0], w = v[1];
        L[i] = (int32_t)(side + g * (w + k * u));
        R[i] = (int32_t)(-side + g * (w - k * u));
    }
}

#endif
