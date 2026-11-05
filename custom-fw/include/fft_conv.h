/*
 * custom-fw/include/fft_conv.h - float FFT convolution for the cab IR (used by src/fx/fx_cab_ir.c).
 *
 * Uniformly partitioned overlap-save: 64-sample blocks (the firmware's block), a 128-point
 * real FFT (done as a 64-point complex FFT plus a split step), and one 64-tap partition of
 * the IR per frequency-domain delay-line slot. A 1,024-tap IR is 16 partitions.
 *
 * Per 64-sample block: one forward FFT, 16 x 65 complex multiply-accumulates, one inverse
 * FFT - about 14,000 float operations, against 65,536 multiply-accumulates for the direct
 * convolution the firmware does today.
 *
 * Promoted from the benchmark prototype once it was measured on the device.
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
    int    limit;                 /* partitions actually summed (<= parts): the load governor shortens the cabinet with it */
    int    head;
    fc_cpx H[FC_MAXP][FC_BINS];   /* IR partition spectra */
    fc_cpx X[FC_MAXP][FC_BINS];   /* frequency-domain delay line of input spectra */
    float  frame[FC_N];           /* [previous block | current block] */
    float  pw[FC_MAXP];           /* the weight each partition has in the sum now: it moves to 1 (inside the limit) or 0 (beyond it) over FC_RAMP_BLOCKS blocks, so a change of the
                                   * limit fades the cabinet's tail instead of cutting it (a hard cut clicked: 7-26x the steady step in the emulator probe) */
    uint8_t pw_init;
} fc_state_t;

#define FC_RAMP_BLOCKS 24         /* 24 x 64 samples = 35 ms */

/* Build the tables (twiddles, bit reversal). Call once before anything else. */
void fc_tables_init(void);

/* Load an IR of `taps` floats (taps <= FC_MAXP * FC_B). Returns 0, or -1 if too long. */
int  fc_init(fc_state_t *s, const float *ir, int taps);

/* Use only the first n partitions of the IR from now on (n <= parts); 0 or more than parts = all of them. */
void fc_set_limit(fc_state_t *s, int n);

/* Convolve one block: 64 samples in, 64 samples out (in and out may be the same buffer). */
void fc_block(fc_state_t *s, const float *in, float *out);

/* A SECOND impulse response on the same input (the right channel of a stereo cabinet). Its partition spectra live in storage the caller owns
 * (FC_MAXP x FC_BINS fc_cpx); the input spectra (the frequency-domain delay line) are shared, so the second channel costs one more
 * multiply-accumulate pass and one more inverse FFT, not a second forward FFT. Returns the number of partitions, or -1 if the IR is too long. */
int  fc_ir_spectra(fc_cpx (*H)[FC_BINS], const float *ir, int taps);

/* Like fc_block, and the same result for outL; outR is the block convolved with the IR whose spectra are H2 (at most as many partitions as s has). */
void fc_block2(fc_state_t *s, const fc_cpx (*H2)[FC_BINS], int parts2, const float *in, float *outL, float *outR);

#endif /* PA_FFT_CONV_H */
