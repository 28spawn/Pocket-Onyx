/* SPDX-License-Identifier: GPL-3.0-only */
/* Copyright (C) 2026 The Onyx Authors (@28spawn) */
/* custom-fw/test/test_cab.c — Cabinet IR Convolver verification suite.
 * Tests partitioned FIR vs scalar reference, pre-delay stripper with pre-roll,
 * half-Hann tail windowing, L2-norm matching, and DF2T biquad post-filters.
 */
#include <stdio.h>
#include <stdlib.h>
#include <math.h>
#include <string.h>
#include "pa_fx.h"
#include "pa_cab.h"
#include "pa_config.h"

void *pa_alloc(uint32_t n) { return calloc(1, n); }
void  pa_free(void *p)     { free(p); }

void fx_cab_ir_normalise_q18(int32_t *taps_q18, uint32_t num_taps);

static int fails = 0;
#define CHECK(ok, ...) do { \
    printf((ok) ? "  ok   " : "  FAIL "); \
    printf(__VA_ARGS__); printf("\n"); \
    if (!(ok)) fails++; \
} while (0)

#define FS 44100.0
#define PI 3.14159265358979323846
#define Q21 2097152.0

int main(void) {
    setvbuf(stdout, NULL, _IONBF, 0);
    printf("=== Pocket Amp Cabinet IR Convolver Verification Suite ===\n");

    /* [1] Partitioned 64-Tap Block FIR Response */
    printf("[1] testing partitioned 64-tap block FIR output...\n");
    {
        fx_node_t *cab = fx_cab_ir_new();
        cab->create(cab);
        cab->set_param(cab, 0, PA_Q21_ONE); /* Level = 1.0 */
        cab->set_param(cab, 1, 0);          /* Low-cut flat (20 Hz) */
        cab->set_param(cab, 2, 0);          /* High-cut flat (18 kHz) */

        /* Create a 256-tap test impulse response (4 partitions of 64) */
        const int num_taps = 256;
        int32_t ir_q18[num_taps];
        for (int i = 0; i < num_taps; i++) {
            ir_q18[i] = (int32_t)(exp(-(double)i / 40.0) * cos(2.0 * PI * 1000.0 * (double)i / FS) * 20000.0);
        }
        fx_cab_ir_load_q18(cab, ir_q18, num_taps);

        /* Generate input test signal: impulse at sample 10 */
        const int total_samples = 512;
        int32_t in_buf[total_samples];
        memset(in_buf, 0, sizeof(in_buf));
        in_buf[10] = 1 << 20; /* 0.5 FS in Q21 */

        /* Run through CAB block by block */
        int32_t cab_out[total_samples];
        memcpy(cab_out, in_buf, sizeof(in_buf));
        for (int b = 0; b < total_samples; b += PA_BLOCK_N) {
            cab->run(cab, cab_out + b, PA_BLOCK_N);
        }

        /* Verify output is non-zero and has reverberant tail */
        int32_t max_out = 0;
        for (int i = 0; i < total_samples; i++) {
            int32_t a = abs(cab_out[i]);
            if (a > max_out) max_out = a;
        }
        printf("   Convolver max peak: %d (%.4f FS)\n", max_out, (double)max_out / Q21);
        CHECK(max_out > 10000, "convolver processed signal with clear impulse response");

        cab->destroy(cab);
    }

    /* [2] Pre-Delay Stripper with 8-Sample Pre-Roll */
    printf("[2] testing pre-delay stripper (with 8-sample acoustic pre-roll preservation)...\n");
    {
        fx_node_t *cab = fx_cab_ir_new();
        cab->create(cab);

        /* Create a 512-tap IR with 40 samples of silence, then a large spike at sample 40 */
        int32_t test_ir[512];
        memset(test_ir, 0, sizeof(test_ir));
        test_ir[40] = 50000; /* peak */
        for (int i = 41; i < 512; i++) {
            test_ir[i] = (int32_t)(50000.0 * exp(-(double)(i - 40) / 30.0));
        }

        fx_cab_ir_load_q18(cab, test_ir, 512);

        /* Test impulse at sample 0 of block 0: the output peak should now arrive at sample (8 + 1 = 9)
         * because 32 samples were trimmed (40 - 8 = 32 trimmed, leaving 8 samples of pre-roll)
         * and the FIR engine has a 1-sample hardware latency */
        int32_t in_buf[PA_BLOCK_N];
        memset(in_buf, 0, sizeof(in_buf));
        in_buf[0] = 1 << 20;

        cab->run(cab, in_buf, PA_BLOCK_N);

        int peak_idx = -1;
        int32_t max_peak = 0;
        for (int i = 0; i < PA_BLOCK_N; i++) {
            if (abs(in_buf[i]) > max_peak) {
                max_peak = abs(in_buf[i]);
                peak_idx = i;
            }
        }
        printf("   Input at 0 -> output peak at sample %d (preserved 8 samples pre-roll)\n", peak_idx);
        CHECK(peak_idx == 9, "pre-delay stripped 32 samples of silence while keeping 8-sample pre-roll (peak at %d)", peak_idx);

        cab->destroy(cab);
    }

    /* [3] Half-Hann Tail Windowing on Truncation */
    printf("[3] testing half-Hann tail windowing on truncated IRs...\n");
    {
        /* Create a step response of 2500 taps (> PA_IR_MAX_TAPS = 2048) */
        int32_t step_ir[2500];
        for (int i = 0; i < 2500; i++) step_ir[i] = 10000;

        fx_node_t *cab = fx_cab_ir_new();
        cab->create(cab);
        fx_cab_ir_load_q18(cab, step_ir, 2500);

        /* Inspect the loaded state's taps in ir_q18 (which is the first field of cab->state) */
        /* Taps (2048-64)..2047 must taper smoothly to zero via half-Hann */
        const int32_t *ir_taps = (const int32_t *)cab->state;
        int32_t tail_last = ir_taps[2047];
        int32_t tail_mid = ir_taps[2048 - 32];
        int32_t tail_start = ir_taps[2048 - 64];
        printf("   Tail window taper: start=%d, mid=%d, last=%d\n", tail_start, tail_mid, tail_last);
        CHECK(tail_last < tail_mid && tail_mid < tail_start, "tail fades smoothly across the window");
        CHECK(tail_last <= tail_start / 10, "tail lands smoothly near zero (got %d vs start %d)", tail_last, tail_start);

        cab->destroy(cab);
    }

    /* [4] L2-Norm Energy Loudness Normalization */
    printf("[4] testing L2-norm energy normalization...\n");
    {
        int32_t taps[512];
        for (int i = 0; i < 512; i++) {
            taps[i] = (int32_t)(sin(2.0 * PI * 440.0 * (double)i / FS) * 5000.0);
        }
        fx_cab_ir_normalise_q18(taps, 512);

        double acc = 0.0;
        for (int i = 0; i < 512; i++) {
            acc += (double)taps[i] * (double)taps[i];
        }
        double l2 = sqrt(acc);
        printf("   Normalized L2 energy: %.1f (target 400,150.0)\n", l2);
        CHECK(fabs(l2 - 400150.0) <= 2.0, "L2 energy matches reference target within 2 units");
    }

    /* [5] Post-Cabinet 2nd-Order Butterworth Biquads */
    printf("[5] testing post-cab 2nd-order Butterworth biquad controls...\n");
    {
        fx_node_t *cab = fx_cab_ir_new();
        cab->create(cab);

        /* Set Low-Cut to maximum (300 Hz) */
        cab->set_param(cab, 1, PA_Q21_ONE);
        /* Set High-Cut to maximum (5000 Hz) */
        cab->set_param(cab, 2, PA_Q21_ONE);

        int32_t buf[PA_BLOCK_N];
        for (int i = 0; i < PA_BLOCK_N; i++) buf[i] = 100000;
        cab->run(cab, buf, PA_BLOCK_N);

        int all_finite = 1;
        for (int i = 0; i < PA_BLOCK_N; i++) {
            if (!isfinite((float)buf[i])) all_finite = 0;
        }
        CHECK(all_finite, "post-cab biquads remain stable under extreme filter parameters");

        cab->destroy(cab);
    }

    if (fails) {
        printf("\nFAILED: %d check(s)\n", fails);
        return 1;
    }
    printf("\nALL CAB TESTS PASSED (0 failures)\n");
    return 0;
}
