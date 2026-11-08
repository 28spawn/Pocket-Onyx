/* SPDX-License-Identifier: GPL-3.0-only */
/* Copyright (C) 2026 The Onyx Authors (@28spawn) */
/*
 * custom-fw/bench/bench_dsp.h - the fixed-point vs float measurements of the benchmark:
 * tone-stack biquads, the cab convolution (direct Q18 vs float FFT) and the GRU step.
 */
#ifndef PA_BENCH_DSP_H
#define PA_BENCH_DSP_H

#include <stdint.h>

typedef struct {
    uint32_t bq_q21_ticks;        /* 4-biquad tone stack, Q21 (the amp's code), N samples */
    uint32_t bq_f_ticks;          /* the same 4 biquads in float */
    uint32_t bq_f_denorm_ticks;   /* float biquads with denormal state and silent input */
    uint32_t ftz;                 /* 1 if the FPU flushes denormals to zero */
    uint32_t bq_maxdiff_bits;     /* float: max |Q21 - float| output difference */
    uint32_t cab_direct_ticks;    /* 1,024-tap cab, firmware's partitioned Q18 path (software) */
    uint32_t cab_fft_ticks;       /* 1,024-tap cab, float FFT convolution prototype */
    uint32_t cab_maxdiff_bits;    /* float: max |direct - FFT| output difference */
    uint32_t cab_taps;
    uint32_t gru_ticks;           /* stock GRU engine (nam_gru_run_block, software matvec) */
    uint32_t gru_out_bits_last;   /* last GRU output (sanity) */
} bench_dsp_res_t;

#define BENCH_DSP_RES_WORDS 11

/* Run every measurement over `n` samples (a multiple of 64, <= 2048), `reps` times each,
 * keeping the fastest. `in` is the test signal in [-1, 1]. Returns 0, or an error code. */
int bench_dsp_run(bench_dsp_res_t *r, const float *in, uint32_t n, uint32_t reps);

#endif /* PA_BENCH_DSP_H */
