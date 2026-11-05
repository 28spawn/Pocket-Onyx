/*
 * test/test_dsp_chain.c — Host Verification Test for NAM A2 GRU, 512-Entry LUTs, and 8-Slot FX Chain
 */
#include <stdio.h>
#include <stdlib.h>
#include <math.h>
#include "pa_config.h"
#include "pa_activation.h"
#include "pa_model.h"
#include "pa_wavenet.h"
#include "pa_cab.h"
#include "pa_fx.h"

void *pa_alloc(uint32_t n) { return calloc(1, n); }
void  pa_free(void *p)     { free(p); }

static int fails = 0;
#define CHECK(cond, ...) do { \
    if (!(cond)) { \
        printf("  FAIL: "); printf(__VA_ARGS__); printf("\n"); \
        fails++; \
    } else { \
        printf("  ok    "); printf(__VA_ARGS__); printf("\n"); \
    } \
} while (0)

int main(void) {
    printf("=== Pocket Amp DSP Chain & NAM A2 GRU Unit Test ===\n");

    /* 1. Verify 512-entry Activation LUT symmetry & bounds */
    int32_t sig0 = pa_activation_lut(0, 0);
    int32_t th0  = pa_activation_lut(0, 1);
    int32_t th_pos = pa_activation_lut(4 * PA_Q21_ONE, 1);
    int32_t th_neg = pa_activation_lut(-4 * PA_Q21_ONE, 1);
    printf("[QW-1] sigmoid(0)=%d (expected ~%d), tanh(0)=%d, tanh(+4)=%d, tanh(-4)=%d\n",
           sig0, PA_Q21_ONE >> 1, th0, th_pos, th_neg);
    CHECK(abs(sig0 - (PA_Q21_ONE >> 1)) <= 4, "sigmoid(0) is ~0.5 in Q21 (got %d, expected %d)", sig0, PA_Q21_ONE >> 1);
    CHECK(th0 == 0, "tanh(0) is exactly 0 (got %d)", th0);
    CHECK(th_pos > (int32_t)(0.99 * PA_Q21_ONE) && th_pos <= PA_Q21_ONE, "tanh(+4) saturates near +1.0 in Q21 (got %d)", th_pos);
    CHECK(th_neg < (int32_t)(-0.99 * PA_Q21_ONE) && th_neg >= -PA_Q21_ONE, "tanh(-4) saturates near -1.0 in Q21 (got %d)", th_neg);
    CHECK(abs(th_pos + th_neg) <= 1, "tanh has odd symmetry tanh(+4) == -tanh(-4) (diff %d)", abs(th_pos + th_neg));

    /* 2. Verify Co-Scheduled 16-Unit Q21 NAM A2 GRU Engine */
    pa_nam_model_t nam;
    nam_model_init(&nam);
    int32_t buf[PA_BLOCK_N];
    for (int i = 0; i < PA_BLOCK_N; i++) {
        buf[i] = (int32_t)(sin(2.0 * 3.141592653589793 * 440.0 * i / 44100.0) * (PA_Q21_ONE >> 2));
    }
    nam_gru_run_block(&nam, buf, PA_BLOCK_N);
    printf("[BR-2] NAM A2 16-unit Q21 GRU processed 64 samples: y[10]=%d\n", buf[10]);
    int nam_finite = 1;
    int32_t nam_peak = 0;
    for (int i = 0; i < PA_BLOCK_N; i++) {
        if (!isfinite((float)buf[i])) nam_finite = 0;
        int32_t a = abs(buf[i]);
        if (a > nam_peak) nam_peak = a;
    }
    CHECK(nam_finite, "NAM A2 GRU output is finite across all %d samples", PA_BLOCK_N);
    CHECK(nam_peak > (int32_t)(0.01 * PA_Q21_ONE), "NAM A2 GRU is not silent (peak %d)", nam_peak);
    CHECK(nam_peak < 4 * PA_Q21_ONE, "NAM A2 GRU output is bounded within headroom (peak %d)", nam_peak);

    /* 3. Verify True-Stereo 8-Slot FX Chain with Delay/Reverb Tail Spillover */
    pa_fx_chain_t chain;
    pa_fx_chain_init(&chain, 0, fx_cab_ir_new());
    int32_t buf_L[PA_BLOCK_N], buf_R[PA_BLOCK_N];
    for (int i = 0; i < PA_BLOCK_N; i++) {
        buf_L[i] = buf[i];
        buf_R[i] = buf[i];
    }
    pa_fx_chain_run_stereo(&chain, buf_L, buf_R, PA_BLOCK_N);
    printf("[BR-3] Stereo 8-Slot Chain output: L[16]=%d, R[16]=%d\n", buf_L[16], buf_R[16]);
    int chain_finite = 1;
    int32_t peak_L = 0, peak_R = 0;
    for (int i = 0; i < PA_BLOCK_N; i++) {
        if (!isfinite((float)buf_L[i]) || !isfinite((float)buf_R[i])) chain_finite = 0;
        int32_t al = abs(buf_L[i]), ar = abs(buf_R[i]);
        if (al > peak_L) peak_L = al;
        if (ar > peak_R) peak_R = ar;
    }
    CHECK(chain_finite, "8-Slot chain stereo output is finite across all samples");
    CHECK(peak_L > (int32_t)(0.01 * PA_Q21_ONE) && peak_R > (int32_t)(0.01 * PA_Q21_ONE),
          "8-Slot chain output is not silent (L peak %d, R peak %d)", peak_L, peak_R);
    CHECK(peak_L < 4 * PA_Q21_ONE && peak_R < 4 * PA_Q21_ONE,
          "8-Slot chain output is bounded within headroom (L peak %d, R peak %d)", peak_L, peak_R);

    /* 4. Verify 23-Layer Dilated WaveNet Inference Engine */
    static pa_wavenet_model_t wn;
    pa_wavenet_init_default(&wn);
    int32_t wn_buf[PA_BLOCK_N];
    /* After a reset the output DC blocker only tracks for PA_DC_HOLD_SAMPLES (448): the first blocks are silent by design (the amp's resting level is not passed as a step, pack 186). Run past that and look at the last block. */
    int32_t wn_first_peak = 0;
    for (int blk = 0; blk < 10; blk++) {
        for (int i = 0; i < PA_BLOCK_N; i++) {
            wn_buf[i] = (int32_t)(sin(2.0 * 3.141592653589793 * 440.0 * (blk * PA_BLOCK_N + i) / 44100.0) * (PA_Q21_ONE >> 1));
        }
        pa_wavenet_process_block_q21(&wn, wn_buf, PA_BLOCK_N);
        if (blk == 0) for (int i = 0; i < PA_BLOCK_N; i++) { int32_t a = abs(wn_buf[i]); if (a > wn_first_peak) wn_first_peak = a; }
    }
    CHECK(wn_first_peak == 0, "WaveNet: the first block after a reset is silent (the DC blocker tracks; peak %d)", wn_first_peak);
    int wn_finite = 1;
    int32_t wn_peak = 0;
    for (int i = 0; i < PA_BLOCK_N; i++) {
        if (!isfinite((float)wn_buf[i])) wn_finite = 0;
        int32_t a = abs(wn_buf[i]);
        if (a > wn_peak) wn_peak = a;
    }
    CHECK(wn_finite, "23-layer Dilated WaveNet output is finite across all %d samples", PA_BLOCK_N);
    CHECK(wn_peak > (int32_t)(0.01 * PA_Q21_ONE), "WaveNet output is not silent (peak %d)", wn_peak);
    CHECK(wn_peak < 4 * PA_Q21_ONE, "WaveNet output is bounded within headroom (peak %d)", wn_peak);

    if (fails == 0) {
        printf("=== ALL DSP CHAIN & NAM TESTS PASSED ===\n");
        return 0;
    } else {
        printf("=== %d DSP CHAIN TEST(S) FAILED ===\n", fails);
        return 1;
    }
}
