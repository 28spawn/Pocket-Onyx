/* SPDX-License-Identifier: GPL-3.0-only */
/* Copyright (C) 2026 The Onyx Authors (@28spawn) */
/*
 * custom-fw/bench/fft_conv.c - float FFT convolution prototype (see fft_conv.h).
 *
 * The 128-point real FFT is a 64-point complex FFT of z[n] = x[2n] + i*x[2n+1], followed
 * by the standard split:   X[k] = E[k] + W^k O[k],  W = exp(-2*pi*i/128),
 *   E[k] = (Z[k] + conj Z[64-k]) / 2,   O[k] = (Z[k] - conj Z[64-k]) / (2i).
 * The inverse undoes it:   E[k] = (X[k] + conj X[64-k]) / 2,
 *                          O[k] = (X[k] - conj X[64-k]) * conj(W^k) / 2,
 *                          z    = IFFT64(E + iO),  x[2n] = Re z[n], x[2n+1] = Im z[n].
 */
#include "fft_conv.h"
#include <string.h>
#include <math.h>

static fc_cpx  s_tw64[32];        /* exp(-2*pi*i*k/64) */
static fc_cpx  s_w128[65];        /* exp(-2*pi*i*k/128) */
static uint8_t s_brev[64];

void fc_tables_init(void)
{
    for (int k = 0; k < 32; k++) {
        s_tw64[k].re = cosf(-6.28318530718f * (float)k / 64.0f);
        s_tw64[k].im = sinf(-6.28318530718f * (float)k / 64.0f);
    }
    for (int k = 0; k <= 64; k++) {
        s_w128[k].re = cosf(-6.28318530718f * (float)k / 128.0f);
        s_w128[k].im = sinf(-6.28318530718f * (float)k / 128.0f);
    }
    for (int i = 0; i < 64; i++) {
        int r = 0;
        for (int b = 0; b < 6; b++) if (i & (1 << b)) r |= 1 << (5 - b);
        s_brev[i] = (uint8_t)r;
    }
}

/* In-place radix-2 complex FFT of 64 points. inverse: conjugate twiddles, no scaling. */
static void fft64(fc_cpx *a, int inverse)
{
    for (int i = 0; i < 64; i++) {
        int j = s_brev[i];
        if (i < j) { fc_cpx t = a[i]; a[i] = a[j]; a[j] = t; }
    }
    for (int len = 2; len <= 64; len <<= 1) {
        const int half = len >> 1, step = 64 / len;
        for (int i = 0; i < 64; i += len) {
            for (int j = 0; j < half; j++) {
                const fc_cpx w = s_tw64[j * step];
                const float wim = inverse ? -w.im : w.im;
                fc_cpx *p = &a[i + j], *q = &a[i + j + half];
                const float vr = q->re * w.re - q->im * wim;
                const float vi = q->re * wim  + q->im * w.re;
                q->re = p->re - vr; q->im = p->im - vi;
                p->re += vr;        p->im += vi;
            }
        }
    }
}

/* Real 128-point FFT: x[128] -> X[0..64]. */
static void rfft128(const float *x, fc_cpx *X)
{
    fc_cpx z[64];
    for (int n = 0; n < 64; n++) { z[n].re = x[2 * n]; z[n].im = x[2 * n + 1]; }
    fft64(z, 0);
    for (int k = 0; k <= 64; k++) {
        const fc_cpx a = z[k & 63], b = z[(64 - k) & 63];      /* Z[k], Z[64-k] */
        const float er = 0.5f * (a.re + b.re), ei = 0.5f * (a.im - b.im);   /* (Z + conj Zr)/2 */
        const float orr = 0.5f * (a.im + b.im), oi = -0.5f * (a.re - b.re); /* (Z - conj Zr)/(2i) */
        const fc_cpx w = s_w128[k];
        X[k].re = er + (orr * w.re - oi * w.im);
        X[k].im = ei + (orr * w.im + oi * w.re);
    }
}

/* Inverse real 128-point FFT: X[0..64] -> x[128], scaled so irfft(rfft(x)) == x. */
static void irfft128(const fc_cpx *X, float *x)
{
    fc_cpx z[64];
    for (int k = 0; k < 64; k++) {
        const fc_cpx a = X[k], b = X[64 - k];
        const float er = 0.5f * (a.re + b.re), ei = 0.5f * (a.im - b.im);     /* (X + conj Xr)/2 */
        const float dr = 0.5f * (a.re - b.re), di = 0.5f * (a.im + b.im);     /* (X - conj Xr)/2 */
        const fc_cpx w = s_w128[k];                                           /* times conj(W^k) */
        const float orr = dr * w.re + di * w.im, oi = di * w.re - dr * w.im;
        z[k].re = er - oi;                                                     /* E + iO */
        z[k].im = ei + orr;
    }
    fft64(z, 1);
    const float sc = 1.0f / 64.0f;
    for (int n = 0; n < 64; n++) { x[2 * n] = z[n].re * sc; x[2 * n + 1] = z[n].im * sc; }
}

int fc_init(fc_state_t *s, const float *ir, int taps)
{
    if (taps < 1 || taps > FC_MAXP * FC_B) return -1;
    memset(s, 0, sizeof *s);
    s->parts = (taps + FC_B - 1) / FC_B;
    float buf[FC_N];
    for (int p = 0; p < s->parts; p++) {
        memset(buf, 0, sizeof buf);
        for (int k = 0; k < FC_B && p * FC_B + k < taps; k++) buf[k] = ir[p * FC_B + k];
        rfft128(buf, s->H[p]);
    }
    return 0;
}

void fc_block(fc_state_t *s, const float *in, float *out)
{
    /* slide the frame: [old current | new block] */
    memmove(s->frame, s->frame + FC_B, FC_B * sizeof(float));
    memcpy(s->frame + FC_B, in, FC_B * sizeof(float));

    s->head = (s->head == 0) ? s->parts - 1 : s->head - 1;
    rfft128(s->frame, s->X[s->head]);

    /* Y = sum_p X[t-p] * H[p] */
    fc_cpx Y[FC_BINS];
    memset(Y, 0, sizeof Y);
    int slot = s->head;
    for (int p = 0; p < s->parts; p++) {
        const fc_cpx *x = s->X[slot], *h = s->H[p];
        for (int k = 0; k < FC_BINS; k++) {
            Y[k].re += x[k].re * h[k].re - x[k].im * h[k].im;
            Y[k].im += x[k].re * h[k].im + x[k].im * h[k].re;
        }
        if (++slot == s->parts) slot = 0;
    }

    float y[FC_N];
    irfft128(Y, y);
    memcpy(out, y + FC_B, FC_B * sizeof(float));     /* overlap-save: the valid half */
}
