/* SPDX-License-Identifier: GPL-3.0-only */
/* Copyright (C) 2026 The Onyx Authors (@28spawn) */
/*
 * custom-fw/bench/bench_dsp.c - fixed point vs float on the AC791N, measured.
 *
 *   biquads  the amp's 4-band tone stack exactly as pa_audio_core.c runs it (Q21, the
 *            firmware's q21_mul / sat32 from fixedpoint.h), against the same four filters
 *            in float; plus a float run with denormal state, to see whether the FPU slows
 *            down on denormals (and whether it flushes them to zero).
 *   cab      a 1,024-tap IR: the firmware's partitioned direct convolution (fx_cab_ir.c's
 *            loop, through pa_hw_accel_fir_part64 in its SOFTWARE form - this build compiles
 *            hw_accel.c with PA_HOST_SIM, so the unverified accelerator is never touched),
 *            against the float FFT convolution prototype (fft_conv.c). The outputs are
 *            compared, so the prototype is checked on the device too.
 *   gru      the stock-amp engine, nam_gru_run_block(), with the matvec in software (what
 *            the device does once the accelerator has timed out).
 */
#include "bench_dsp.h"
#include <string.h>
#include <math.h>
#include "fixedpoint.h"
#include "pa_model.h"
#include "pa_hw_accel.h"
#include "fft_conv.h"

#if defined(PA_HOST_SIM)
#include <time.h>                      /* host check: a clock in place of Timer4 */
static uint32_t host_ticks(void) { return (uint32_t)((double)clock() * 24000000.0 / CLOCKS_PER_SEC); }
#define T4_CNT host_ticks()
#else
#define T4_CNT (*(volatile uint32_t *)0x00010804u)
#endif
#define NMAX   2048
#define BQ_N   4

void nam_gru_run_block(pa_nam_model_t *m, int32_t *buf_q21, int nframes);
void pa_nam_init_default(pa_nam_model_t *m);
void pa_nam_reset(pa_nam_model_t *m);

typedef struct { int32_t b0, b1, b2, a1, a2; } bq_q21_t;     /* = amp_biquad_t */
typedef struct { float   b0, b1, b2, a1, a2; } bq_f_t;

static bq_q21_t s_bq_q[BQ_N];
static bq_f_t   s_bq_f[BQ_N];
static int32_t  s_qin[NMAX], s_qwork[NMAX];
static float    s_fin[NMAX], s_fwork[NMAX];

/* cab */
#define CAB_TAPS 1024
#define CAB_P    (CAB_TAPS / FC_B)
#define CAB_L    ((CAB_P + 1) * FC_B)
static int32_t    s_ir_q18[CAB_TAPS];
static float      s_ir_f[CAB_TAPS];
static int32_t    s_hist[CAB_L];
static fc_state_t s_fc;

static pa_nam_model_t s_gru;

static float f_abs(float x) { return x < 0.0f ? -x : x; }
static int32_t rnd(float x) { return (int32_t)(x >= 0.0f ? x + 0.5f : x - 0.5f); }
static uint32_t f_bits(float f) { uint32_t u; memcpy(&u, &f, 4); return u; }

/* RBJ peaking EQ, normalised (a0 = 1), in the firmware's sign convention:
 * y = b0 x + s1;  s1 = b1 x - a1 y + s2;  s2 = b2 x - a2 y */
static void peaking(bq_f_t *q, float f0, float db, float Q) {
    const float A = powf(10.0f, db / 40.0f), w = 6.28318530718f * f0 / 44100.0f;
    const float c = cosf(w), al = sinf(w) / (2.0f * Q);
    const float a0 = 1.0f + al / A;
    q->b0 = (1.0f + al * A) / a0; q->b1 = (-2.0f * c) / a0; q->b2 = (1.0f - al * A) / a0;
    q->a1 = (-2.0f * c) / a0;     q->a2 = (1.0f - al / A) / a0;
}

/* The amp's tone-stack loop, verbatim in shape (pa_audio_core.c, amp_slot_run step 3). */
static void run_bq_q21(int32_t *buf, int n) {
    for (int b = 0; b < BQ_N; b++) {
        const bq_q21_t *q = &s_bq_q[b];
        int32_t s1 = 0, s2 = 0;
        for (int i = 0; i < n; i++) {
            int32_t x = buf[i];
            int32_t y = sat32((int64_t)q21_mul(q->b0, x) + s1);
            s1 = sat32((int64_t)q21_mul(q->b1, x) - q21_mul(q->a1, y) + s2);
            s2 = sat32((int64_t)q21_mul(q->b2, x) - q21_mul(q->a2, y));
            buf[i] = y;
        }
    }
}

static void run_bq_f(float *buf, int n, float s_init) {
    for (int b = 0; b < BQ_N; b++) {
        const float b0 = s_bq_f[b].b0, b1 = s_bq_f[b].b1, b2 = s_bq_f[b].b2;
        const float a1 = s_bq_f[b].a1, a2 = s_bq_f[b].a2;
        float s1 = s_init, s2 = s_init;
        for (int i = 0; i < n; i++) {
            const float x = buf[i];
            const float y = b0 * x + s1;
            s1 = b1 * x - a1 * y + s2;
            s2 = b2 * x - a2 * y;
            buf[i] = y;
        }
    }
}

/* fx_cab_ir.c cab_ir_process_block(): age the history, append the block (Q21 -> Q18),
 * 16 partition FIRs. Output in Q18 in `acc`. */
static void cab_direct_block(const int32_t *blk_q21, int32_t *out_q18) {
    int32_t acc[FC_B];
    memmove(&s_hist[0], &s_hist[FC_B], (CAB_L - FC_B) * sizeof(int32_t));
    for (int k = 0; k < FC_B; k++) s_hist[CAB_L - FC_B + k] = (blk_q21[k] + 4) >> 3;
    for (int p = 0; p < CAB_P; p++)
        pa_hw_accel_fir_part64(&s_ir_q18[p * FC_B], &s_hist[CAB_L - 2 * FC_B - p * FC_B + 1], acc, FC_B, p > 0);
    memcpy(out_q18, acc, sizeof acc);
}

int bench_dsp_run(bench_dsp_res_t *r, const float *in, uint32_t n, uint32_t reps) {
    if (n == 0 || n > NMAX || (n % FC_B) != 0) return 0xE101;
    memset(r, 0, sizeof *r);

    for (uint32_t i = 0; i < n; i++) {
        s_fin[i] = 0.5f * in[i];
        s_qin[i] = (int32_t)rnd(s_fin[i] * 2097152.0f);
    }

    /* ---- 1. biquads ------------------------------------------------------------- */
    peaking(&s_bq_f[0], 100.0f,  4.0f, 0.7f);
    peaking(&s_bq_f[1], 500.0f, -3.0f, 0.7f);
    peaking(&s_bq_f[2], 2000.0f, 5.0f, 0.9f);
    peaking(&s_bq_f[3], 5000.0f, 3.0f, 0.7f);
    for (int b = 0; b < BQ_N; b++) {
        s_bq_q[b].b0 = (int32_t)rnd(s_bq_f[b].b0 * 2097152.0f);
        s_bq_q[b].b1 = (int32_t)rnd(s_bq_f[b].b1 * 2097152.0f);
        s_bq_q[b].b2 = (int32_t)rnd(s_bq_f[b].b2 * 2097152.0f);
        s_bq_q[b].a1 = (int32_t)rnd(s_bq_f[b].a1 * 2097152.0f);
        s_bq_q[b].a2 = (int32_t)rnd(s_bq_f[b].a2 * 2097152.0f);
    }
    uint32_t best_q = 0xFFFFFFFFu, best_f = 0xFFFFFFFFu, best_d = 0xFFFFFFFFu;
    for (uint32_t rep = 0; rep < reps; rep++) {
        memcpy(s_qwork, s_qin, n * sizeof(int32_t));
        uint32_t t0 = T4_CNT; run_bq_q21(s_qwork, (int)n); uint32_t dt = T4_CNT - t0;
        if (dt < best_q) best_q = dt;

        memcpy(s_fwork, s_fin, n * sizeof(float));
        t0 = T4_CNT; run_bq_f(s_fwork, (int)n, 0.0f); dt = T4_CNT - t0;
        if (dt < best_f) best_f = dt;
    }
    float maxd = 0.0f;
    for (uint32_t i = 0; i < n; i++) {
        float d = f_abs((float)s_qwork[i] * (1.0f / 2097152.0f) - s_fwork[i]);
        if (d > maxd) maxd = d;
    }
    r->bq_maxdiff_bits = f_bits(maxd);
    /* denormals: silent input, state seeded with a denormal value */
    for (uint32_t rep = 0; rep < reps; rep++) {
        memset(s_fwork, 0, n * sizeof(float));
        uint32_t t0 = T4_CNT; run_bq_f(s_fwork, (int)n, 1.0e-39f); uint32_t dt = T4_CNT - t0;
        if (dt < best_d) best_d = dt;
    }
    {
        volatile float tiny = 1.0e-39f, one = 1.0f;
        volatile float prod = tiny * one;
        r->ftz = (prod == 0.0f) ? 1u : 0u;
    }
    r->bq_q21_ticks = best_q; r->bq_f_ticks = best_f; r->bq_f_denorm_ticks = best_d;

    /* ---- 2. cab: direct Q18 (firmware) vs float FFT (prototype) ------------------- */
    {
        uint32_t rng = 777u;
        for (int i = 0; i < CAB_TAPS; i++) {
            rng = rng * 1664525u + 1013904223u;
            float v = ((float)((rng >> 8) & 0xFFFFFFu) * (2.0f / 16777216.0f) - 1.0f) *
                      0.6f * expf(-(float)i / 250.0f);
            s_ir_q18[i] = (int32_t)rnd(v * 262144.0f);
            s_ir_f[i] = (float)s_ir_q18[i] * (1.0f / 262144.0f);       /* the same IR, exactly */
        }
        r->cab_taps = CAB_TAPS;
    }
    fc_tables_init();
    uint32_t best_cd = 0xFFFFFFFFu, best_cf = 0xFFFFFFFFu;
    for (uint32_t rep = 0; rep < reps; rep++) {
        memset(s_hist, 0, sizeof s_hist);
        uint32_t t0 = T4_CNT;
        for (uint32_t b = 0; b < n; b += FC_B) cab_direct_block(&s_qin[b], &s_qwork[b]);
        uint32_t dt = T4_CNT - t0;
        if (dt < best_cd) best_cd = dt;

        fc_init(&s_fc, s_ir_f, CAB_TAPS);
        /* the FFT path gets exactly the direct path's Q18 input samples */
        for (uint32_t i = 0; i < n; i++) s_fin[i] = (float)((s_qin[i] + 4) >> 3) * (1.0f / 262144.0f);
        t0 = T4_CNT;
        for (uint32_t b = 0; b < n; b += FC_B) fc_block(&s_fc, &s_fin[b], &s_fwork[b]);
        dt = T4_CNT - t0;
        if (dt < best_cf) best_cf = dt;
    }
    maxd = 0.0f;
    for (uint32_t i = 0; i < n; i++) {
        float d = f_abs((float)s_qwork[i] * (1.0f / 262144.0f) - s_fwork[i]);
        if (d > maxd) maxd = d;
    }
    r->cab_direct_ticks = best_cd; r->cab_fft_ticks = best_cf; r->cab_maxdiff_bits = f_bits(maxd);

    /* ---- 3. the stock GRU engine ------------------------------------------------------ */
    pa_nam_init_default(&s_gru);
    uint32_t best_g = 0xFFFFFFFFu;
    for (uint32_t rep = 0; rep < reps; rep++) {
        pa_nam_reset(&s_gru);
        memcpy(s_qwork, s_qin, n * sizeof(int32_t));
        uint32_t t0 = T4_CNT;
        for (uint32_t b = 0; b < n; b += FC_B) nam_gru_run_block(&s_gru, &s_qwork[b], FC_B);
        uint32_t dt = T4_CNT - t0;
        if (dt < best_g) best_g = dt;
    }
    r->gru_ticks = best_g;
    r->gru_out_bits_last = (uint32_t)s_qwork[n - 1];
    return 0;
}
