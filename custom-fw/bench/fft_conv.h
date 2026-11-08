/* SPDX-License-Identifier: GPL-3.0-only */
/* Copyright (C) 2026 The Onyx Authors (@28spawn) */
/*
 * custom-fw/bench/fft_conv.h - float FFT convolution prototype for the cab IR.
 *
 * Uniformly partitioned overlap-save: 64-sample blocks (the firmware's block), a 128-point
 * real FFT (done as a 64-point complex FFT plus a split step), and one 64-tap partition of
 * the IR per frequency-domain delay-line slot. A 1,024-tap IR is 16 partitions.
 *
 * Per 64-sample block: one forward FFT, 16 x 65 complex multiply-accumulates, one inverse
 * FFT - about 14,000 float operations, against 65,536 multiply-accumulates for the direct
 * convolution the firmware does today.
 *
 * This is a PROTOTYPE for the benchmark (custom-fw/bench/). It is not in the firmware.
 */
#ifndef PA_FFT_CONV_H
#define PA_FFT_CONV_H

#include <stdint.h>

#define FC_B      64              /* block length = partition length */
#define FC_N      128             /* FFT size */
#define FC_BINS   (FC_N / 2 + 1)  /* 65 bins of a real 128-point FFT */
#ifndef FC_MAXP
#define FC_MAXP   16              /* partitions: 16 x 64 = 1,024 taps (the stock cabs) */
#endif

typedef struct { float re, im; } fc_cpx;

typedef struct {
    int    parts;
    int    head;
    fc_cpx H[FC_MAXP][FC_BINS];   /* IR partition spectra */
    fc_cpx X[FC_MAXP][FC_BINS];   /* frequency-domain delay line of input spectra */
    float  frame[FC_N];           /* [previous block | current block] */
} fc_state_t;

/* Build the tables (twiddles, bit reversal). Call once before anything else. */
void fc_tables_init(void);

/* Load an IR of `taps` floats (taps <= FC_MAXP * FC_B). Returns 0, or -1 if too long. */
int  fc_init(fc_state_t *s, const float *ir, int taps);

/* Convolve one block: 64 samples in, 64 samples out (in and out may be the same buffer). */
void fc_block(fc_state_t *s, const float *in, float *out);

#endif /* PA_FFT_CONV_H */
