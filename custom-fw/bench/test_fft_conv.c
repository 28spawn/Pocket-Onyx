/* SPDX-License-Identifier: GPL-3.0-only */
/* Copyright (C) 2026 The Onyx Authors (@28spawn) */
/*
 * custom-fw/bench/test_fft_conv.c - host checks for the FFT convolution prototype.
 *
 *   1. an impulse in gives the IR out (all 1,024 taps, across 16 partitions)
 *   2. random input matches a double-precision direct convolution
 *   3. it matches the FIRMWARE's partitioned Q18 cab convolution (pa_hw_accel_fir_part64,
 *      the same code the cab runs, here in its software form) on the same signal - which
 *      also checks the cab's own partition indexing
 *
 *   clang -O2 -Icustom-fw/include -Icustom-fw/bench -DPA_HOST_SIM=1 custom-fw/bench/test_fft_conv.c \
 *         custom-fw/bench/fft_conv.c custom-fw/src/dsp/hw_accel.c -lm
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include "fft_conv.h"
#include "pa_hw_accel.h"

#define TAPS 1024
#define NBLK 64                               /* 4,096 samples */

static int fails = 0;
#define CHECK(ok, ...) do { printf((ok) ? "  ok   " : "  FAIL "); printf(__VA_ARGS__); printf("\n"); if (!(ok)) fails++; } while (0)

static uint32_t rng = 99u;
static float frand(void) { rng = rng * 1664525u + 1013904223u; return (float)((rng >> 8) & 0xFFFFFF) / 16777216.0f * 2.0f - 1.0f; }

static fc_state_t fc;
static float  ir[TAPS], x[NBLK * FC_B], y[NBLK * FC_B];
static double ref[NBLK * FC_B];

int main(void) {
    setvbuf(stdout, NULL, _IONBF, 0);
    fc_tables_init();
    for (int i = 0; i < TAPS; i++) ir[i] = 0.6f * frand() * expf(-(float)i / 250.0f);   /* a decaying, cab-like IR */

    /* 1. impulse */
    fc_init(&fc, ir, TAPS);
    float imp[FC_B], out[FC_B];
    double maxe = 0.0;
    for (int b = 0; b < TAPS / FC_B + 1; b++) {
        memset(imp, 0, sizeof imp);
        if (b == 0) imp[0] = 1.0f;
        fc_block(&fc, imp, out);
        for (int i = 0; i < FC_B; i++) {
            int t = b * FC_B + i;
            double want = (t < TAPS) ? ir[t] : 0.0;
            double e = fabs(out[i] - want);
            if (e > maxe) maxe = e;
        }
    }
    CHECK(maxe < 1e-5, "an impulse reproduces the 1,024-tap IR (max error %.2g)", maxe);

    /* 2. random input vs double-precision direct convolution */
    fc_init(&fc, ir, TAPS);
    for (int i = 0; i < NBLK * FC_B; i++) x[i] = 0.5f * frand();
    for (int b = 0; b < NBLK; b++) fc_block(&fc, x + b * FC_B, y + b * FC_B);
    double rms = 0.0; maxe = 0.0;
    for (int t = 0; t < NBLK * FC_B; t++) {
        double s = 0.0;
        for (int m = 0; m < TAPS && m <= t; m++) s += (double)ir[m] * x[t - m];
        ref[t] = s; rms += s * s;
        double e = fabs(y[t] - s);
        if (e > maxe) maxe = e;
    }
    rms = sqrt(rms / (NBLK * FC_B));
    CHECK(maxe < 1e-5 * (rms > 1 ? rms : 1) * 10, "random input matches direct convolution (max error %.2g, signal rms %.3f)", maxe, rms);

    /* 3. against the firmware's partitioned Q18 direct path (fx_cab_ir.c's loop) */
    {
        enum { P = TAPS / FC_B, L = (P + 1) * FC_B };
        static int32_t ir_q18[TAPS], hist[L], acc[FC_B];
        for (int i = 0; i < TAPS; i++) ir_q18[i] = (int32_t)lrintf(ir[i] * 262144.0f);
        memset(hist, 0, sizeof hist);
        double maxr = 0.0, peak = 0.0;
        for (int b = 0; b < NBLK; b++) {
            memmove(&hist[0], &hist[FC_B], (L - FC_B) * sizeof(int32_t));
            for (int k = 0; k < FC_B; k++) hist[L - FC_B + k] = (int32_t)lrintf(x[b * FC_B + k] * 262144.0f);
            for (int p = 0; p < P; p++)
                pa_hw_accel_fir_part64(&ir_q18[p * FC_B], &hist[L - 2 * FC_B - p * FC_B + 1], acc, FC_B, p > 0);
            for (int i = 0; i < FC_B; i++) {
                double fw = acc[i] / 262144.0, e = fabs(fw - y[b * FC_B + i]);
                if (e > maxr) maxr = e;
                if (fabs(fw) > peak) peak = fabs(fw);
            }
        }
        CHECK(maxr < 1e-3 * (peak > 1 ? peak : 1), "it matches the firmware's Q18 cab convolution (max diff %.2g, peak %.3f; Q18 step is 3.8e-6)", maxr, peak);
    }

    printf("\n%s\n", fails ? "FAILED" : "FFT CONVOLUTION OK");
    return fails ? 1 : 0;
}
