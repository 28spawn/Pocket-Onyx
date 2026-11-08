/* SPDX-License-Identifier: GPL-3.0-only */
/* Copyright (C) 2026 The Onyx Authors (@28spawn) */
/*
 * custom-fw/bench/bench_fx.h - the DRV and MOD blocks, timed per model / type.
 */
#ifndef PA_BENCH_FX_H
#define PA_BENCH_FX_H

#include <stdint.h>

#define BENCH_FX_DRV 6               /* TS-808, KLON, OCD, RAT, BIG MUFF, FUZZ FACE */
#define BENCH_FX_MOD 7               /* CHORUS, PHASER, FLANGER, TREMOLO, VIBRATO, UNIVIBE, AUTO FILTER */

typedef struct {
    uint32_t drv_ticks[BENCH_FX_DRV];  /* fx_drive.c run(), mono, n samples, min of reps */
    uint32_t mod_ticks[BENCH_FX_MOD];  /* fx_mod.c run_stereo() - the path the device uses */
    uint32_t drv_out_bits_last;        /* last output sample of the last model (sanity) */
    uint32_t mod_out_bits_last;
    uint32_t nan_count;                /* non-finite outputs seen (must be 0) */
} bench_fx_res_t;

#define BENCH_FX_RES_WORDS (BENCH_FX_DRV + BENCH_FX_MOD + 3)

/* The number of samples bench_fx_run() really times for a request of n: at most 1,024
 * (the image's RAM is 256 KB). The host divides the ticks by this. */
uint32_t bench_fx_samples(uint32_t n);

/* Time every drive model and mod type over `n` samples (a multiple of 64; capped at 1,024),
 * `reps` times each, keeping the fastest. `in` is the test signal in [-1, 1].
 * Returns 0, or an error code (0xE2xx). */
int bench_fx_run(bench_fx_res_t *r, const float *in, uint32_t n, uint32_t reps);

#endif /* PA_BENCH_FX_H */
