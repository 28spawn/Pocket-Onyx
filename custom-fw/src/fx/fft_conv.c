#include "../../include/pa_config.h"
/*
 * custom-fw/src/fx/fft_conv.c - float FFT convolution for the cabinet (see include/fft_conv.h).
 * Promoted from the bench prototype (bench/fft_conv.c), verified there against the direct
 * convolution (max difference 3.8e-6) and measured at 1,153 cycles/sample for 1,024 taps.
 *
 * The 128-point real FFT is a 64-point complex FFT of z[n] = x[2n] + i*x[2n+1], followed
 * by the standard split:   X[k] = E[k] + W^k O[k],  W = exp(-2*pi*i/128),
 *   E[k] = (Z[k] + conj Z[64-k]) / 2,   O[k] = (Z[k] - conj Z[64-k]) / (2i).
 * The inverse undoes it:   E[k] = (X[k] + conj X[64-k]) / 2,
 *                          O[k] = (X[k] - conj X[64-k]) * conj(W^k) / 2,
 *                          z    = IFFT64(E + iO),  x[2n] = Re z[n], x[2n+1] = Im z[n].
 */
#include "fft_conv.h"
#if !defined(PA_HOST_BUILD) && !defined(PA_HOST_SIM) && !defined(PA_EMU_DLL) && !defined(__wasm__) && !defined(__EMSCRIPTEN__)
extern uint32_t plat_ticks24(void);
#define FC_PROF 1
#endif
#include <string.h>
#include <math.h>

static fc_cpx  s_tw64[48];        /* exp(-2*pi*i*k/64), k < 48 (the radix-4 stages use up to 3 * 15) */
static fc_cpx  s_w128[65];        /* exp(-2*pi*i*k/128) */
static uint8_t s_rev4[64];        /* base-4 digit reversal of 0..63 */

void fc_tables_init(void)
{
    for (int k = 0; k < 48; k++) {
        s_tw64[k].re = cosf(-6.28318530718f * (float)k / 64.0f);
        s_tw64[k].im = sinf(-6.28318530718f * (float)k / 64.0f);
    }
    for (int k = 0; k <= 64; k++) {
        s_w128[k].re = cosf(-6.28318530718f * (float)k / 128.0f);
        s_w128[k].im = sinf(-6.28318530718f * (float)k / 128.0f);
    }
    for (int i = 0; i < 64; i++) {
        const int d0 = i & 3, d1 = (i >> 2) & 3, d2 = (i >> 4) & 3;
        s_rev4[i] = (uint8_t)(d2 | (d1 << 2) | (d0 << 4));
    }
}

/* In-place forward complex FFT of 64 points, radix 4 (three stages), decimation in time. The input must already be in base-4
 * digit-reversed order (the callers store it that way, so there is no permutation pass). The inverse is the same routine with the real and
 * imaginary parts swapped on the way in and out (swap(FFT(swap(x))) is the unscaled inverse FFT).
 * A radix-4 butterfly takes four complex values and three twiddles and does the work of four radix-2 butterflies in about a third of the
 * memory accesses, which is what this core pays for (an SRAM load is 7-10 cycles; the multiplies are cheap).
 * With the sub-transform length L = 1, 4, 16 and the group size 4L, element k of the group combines a[k], a[k+L], a[k+2L], a[k+3L]
 * multiplied by W^0, W^k, W^2k, W^3k, W = exp(-2 pi i / 4L), and writes back to the same four places:
 *   y0 = b0 + b1 + b2 + b3     y1 = b0 - j b1 - b2 + j b3     y2 = b0 - b1 + b2 - b3     y3 = b0 + j b1 - b2 - j b3 */
PA_HOT static void fft64(fc_cpx *a)
{
    /* stage 1: L = 1, no twiddles */
    for (int g = 0; g < 64; g += 4) {
        fc_cpx *p = &a[g];
        const float t0r = p[0].re + p[2].re, t0i = p[0].im + p[2].im;
        const float t1r = p[0].re - p[2].re, t1i = p[0].im - p[2].im;
        const float t2r = p[1].re + p[3].re, t2i = p[1].im + p[3].im;
        const float t3r = p[1].re - p[3].re, t3i = p[1].im - p[3].im;
        p[0].re = t0r + t2r;  p[0].im = t0i + t2i;
        p[2].re = t0r - t2r;  p[2].im = t0i - t2i;
        p[1].re = t1r + t3i;  p[1].im = t1i - t3r;       /* b0 - j b1... : -j * t3 = (t3i, -t3r) */
        p[3].re = t1r - t3i;  p[3].im = t1i + t3r;
    }
    /* stage 2: L = 4, groups of 16; stage 3: L = 16, one group of 64. ts = twiddle step into s_tw64 (16 / L * k) */
    for (int stage = 0; stage < 2; stage++) {
        const int L = stage == 0 ? 4 : 16, ts = stage == 0 ? 4 : 1;
        for (int base = 0; base < 64; base += 4 * L) {
            for (int k = 0; k < L; k++) {
                const fc_cpx w1 = s_tw64[ts * k], w2 = s_tw64[2 * ts * k], w3 = s_tw64[3 * ts * k];
                fc_cpx *p0 = &a[base + k], *p1 = p0 + L, *p2 = p1 + L, *p3 = p2 + L;
                const float b1r = p1->re * w1.re - p1->im * w1.im, b1i = p1->re * w1.im + p1->im * w1.re;
                const float b2r = p2->re * w2.re - p2->im * w2.im, b2i = p2->re * w2.im + p2->im * w2.re;
                const float b3r = p3->re * w3.re - p3->im * w3.im, b3i = p3->re * w3.im + p3->im * w3.re;
                const float b0r = p0->re, b0i = p0->im;
                const float t0r = b0r + b2r, t0i = b0i + b2i;
                const float t1r = b0r - b2r, t1i = b0i - b2i;
                const float t2r = b1r + b3r, t2i = b1i + b3i;
                const float t3r = b1r - b3r, t3i = b1i - b3i;
                p0->re = t0r + t2r;  p0->im = t0i + t2i;
                p2->re = t0r - t2r;  p2->im = t0i - t2i;
                p1->re = t1r + t3i;  p1->im = t1i - t3r;
                p3->re = t1r - t3i;  p3->im = t1i + t3r;
            }
        }
    }
}

/* Real 128-point FFT: x[128] -> X[0..64]. */
PA_HOT static void rfft128(const float *x, fc_cpx *X)
{
    fc_cpx z[64];
    for (int n = 0; n < 64; n++) { fc_cpx *d = &z[s_rev4[n]]; d->re = x[2 * n]; d->im = x[2 * n + 1]; }   /* stored digit-reversed */
    fft64(z);
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
PA_HOT static void irfft128(const fc_cpx *X, float *x)
{
    fc_cpx z[64];
    for (int k = 0; k < 64; k++) {
        const fc_cpx a = X[k], b = X[64 - k];
        const float er = 0.5f * (a.re + b.re), ei = 0.5f * (a.im - b.im);     /* (X + conj Xr)/2 */
        const float dr = 0.5f * (a.re - b.re), di = 0.5f * (a.im + b.im);     /* (X - conj Xr)/2 */
        const fc_cpx w = s_w128[k];                                           /* times conj(W^k) */
        const float orr = dr * w.re + di * w.im, oi = di * w.re - dr * w.im;
        fc_cpx *d = &z[s_rev4[k]];                                             /* stored digit-reversed and swapped: re <-> im gives the inverse */
        d->im = er - oi;                                                       /* E + iO: re = er - oi, im = ei + orr, swapped */
        d->re = ei + orr;
    }
    fft64(z);
    const float sc = 1.0f / 64.0f;
    for (int n = 0; n < 64; n++) { x[2 * n] = z[n].im * sc; x[2 * n + 1] = z[n].re * sc; }               /* swap back */
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

int fc_ir_spectra(fc_cpx (*H)[FC_BINS], const float *ir, int taps)
{
    if (taps < 1 || taps > FC_MAXP * FC_B) return -1;
    const int parts = (taps + FC_B - 1) / FC_B;
    float buf[FC_N];
    for (int p = 0; p < parts; p++) {
        memset(buf, 0, sizeof buf);
        for (int k = 0; k < FC_B && p * FC_B + k < taps; k++) buf[k] = ir[p * FC_B + k];
        rfft128(buf, H[p]);
    }
    return parts;
}

void fc_set_limit(fc_state_t *s, int n) { s->limit = n; }

/* Move every partition's weight one step toward its target (1 inside the limit, 0 beyond it). Returns how many partitions still count (the highest with a weight, plus one). The first call after
 * fc_init snaps the weights to the targets: a cabinet that starts with a limit starts there. */
static int fc_step_weights(fc_state_t *s, int np)
{
    const float step = 1.0f / (float)FC_RAMP_BLOCKS;
    int top = 0;
    for (int p = 0; p < s->parts; p++) {
        const float t = (p < np) ? 1.0f : 0.0f;
        if (!s->pw_init) s->pw[p] = t;
        else if (s->pw[p] < t) { s->pw[p] += step; if (s->pw[p] > t) s->pw[p] = t; }
        else if (s->pw[p] > t) { s->pw[p] -= step; if (s->pw[p] < t) s->pw[p] = t; }
        if (s->pw[p] > 0.0f) top = p + 1;
    }
    s->pw_init = 1;
    return top;
}

/* The multiply-accumulate of the partitions, with the sums kept in registers. The old loop (partition outside, bin inside) loaded and stored the output spectrum Y on every partition and every bin:
 * 19 instructions and 38 cycles per bin and partition on the unit (111 us for the 16 partitions of a cabinet, measured with FC_PROF). Here the bin is outside: the sum of a bin over all partitions
 * stays in four registers (two independent accumulators for the real and imaginary parts, so the adds do not wait for each other) and Y is written once per bin. The frequency-domain delay line is a
 * ring: partition p reads X[(head + p) mod parts], which is two contiguous runs, so each run is a plain pointer walk. Partition p always goes to accumulator p mod 2, and each accumulator adds its
 * partitions in increasing p, so the result does not depend on where the ring head is: a one-block-shifted run (the second core's pipeline, which the SMP test compares bit for bit with the
 * single-core chain) gets exactly the same sums. Against the old loop the order differs: float rounding only. */
static inline void fc_mac_run(const fc_cpx *x, const fc_cpx *h, int n, int par, float *a0r, float *a0i, float *a1r, float *a1i)
{
    float r0 = *a0r, i0 = *a0i, r1 = *a1r, i1 = *a1i;
    int p = 0;
    if (par && n > 0) {                                          /* this run starts on an odd partition: it belongs to the second accumulator, whatever the ring position */
        r1 += x[0].re * h[0].re - x[0].im * h[0].im;
        i1 += x[0].re * h[0].im + x[0].im * h[0].re;
        x += FC_BINS; h += FC_BINS; p = 1;
    }
    for (; p + 1 < n; p += 2) {
        const fc_cpx x0 = x[0], h0 = h[0], x1 = x[FC_BINS], h1 = h[FC_BINS];
        r0 += x0.re * h0.re - x0.im * h0.im;
        i0 += x0.re * h0.im + x0.im * h0.re;
        r1 += x1.re * h1.re - x1.im * h1.im;
        i1 += x1.re * h1.im + x1.im * h1.re;
        x += 2 * FC_BINS; h += 2 * FC_BINS;
    }
    if (p < n) {
        r0 += x[0].re * h[0].re - x[0].im * h[0].im;
        i0 += x[0].re * h[0].im + x[0].im * h[0].re;
    }
    *a0r = r0; *a0i = i0; *a1r = r1; *a1i = i1;
}

/* Y[k] = sum over p < np of X[(head + p) mod parts][k] * H[p][k], for every bin. H is parts x FC_BINS contiguous (the cabinet's own, or a second channel's). */
static void fc_mac(const fc_state_t *s, const fc_cpx *H, int np, fc_cpx *Y)
{
    const int run1 = (s->parts - s->head < np) ? s->parts - s->head : np;     /* partitions that read X[head .. parts - 1] */
    const int run2 = np - run1;                                              /* the wrap: X[0 ..] */
    const fc_cpx *X1 = s->X[s->head], *X2 = s->X[0];
    for (int k = 0; k < FC_BINS; k++) {
        float a0r = 0.0f, a0i = 0.0f, a1r = 0.0f, a1i = 0.0f;
        fc_mac_run(X1 + k, H + k, run1, 0, &a0r, &a0i, &a1r, &a1i);
        if (run2 > 0) fc_mac_run(X2 + k, H + (size_t)run1 * FC_BINS + k, run2, run1 & 1, &a0r, &a0i, &a1r, &a1i);
        Y[k].re = a0r + a1r;
        Y[k].im = a0i + a1i;
    }
}

PA_HOT void fc_block(fc_state_t *s, const float *in, float *out)
{
#if FC_PROF
    const uint32_t p0 = plat_ticks24();
#endif
    /* slide the frame: [old current | new block] */
    memmove(s->frame, s->frame + FC_B, FC_B * sizeof(float));
    memcpy(s->frame + FC_B, in, FC_B * sizeof(float));

    s->head = (s->head == 0) ? s->parts - 1 : s->head - 1;
#if FC_PROF
    const uint32_t p1 = plat_ticks24();
#endif
    rfft128(s->frame, s->X[s->head]);
#if FC_PROF
    const uint32_t p2 = plat_ticks24();
#endif

    /* Y = sum_p X[t-p] * H[p] */
    fc_cpx Y[FC_BINS];
    memset(Y, 0, sizeof Y);
    int slot = s->head;
    const int np = fc_step_weights(s, (s->limit > 0 && s->limit < s->parts) ? s->limit : s->parts);
    int steady = 1;
    for (int p = 0; p < np; p++) if (s->pw[p] < 1.0f) { steady = 0; break; }
    if (steady) {
        fc_mac(s, &s->H[0][0], np, Y);                            /* the usual case: every weight is 1 */
    } else {
    for (int p = 0; p < np; p++) {
        const fc_cpx *x = s->X[slot], *h = s->H[p];
        const float w = s->pw[p];
        for (int k = 0; k < FC_BINS; k++) {
            Y[k].re += w * (x[k].re * h[k].re - x[k].im * h[k].im);
            Y[k].im += w * (x[k].re * h[k].im + x[k].im * h[k].re);
        }
        if (++slot == s->parts) slot = 0;
    }
    }

#if FC_PROF
    const uint32_t p3 = plat_ticks24();
#endif
    float y[FC_N];
    irfft128(Y, y);
    memcpy(out, y + FC_B, FC_B * sizeof(float));     /* overlap-save: the valid half */
#if FC_PROF
    {   /* where the cabinet's time goes, on the device, in the real audio path (SRAM 0x01C7E440; jl_ram_read.py fir): sums of 24 MHz ticks
         * over the blocks since the sums were last cleared (every 2048 blocks) */
        static uint32_t s_n, s_move, s_fft, s_mac, s_ifft;
        const uint32_t p4 = plat_ticks24();
        if (s_n >= 2048u) { s_n = 0u; s_move = s_fft = s_mac = s_ifft = 0u; }
        s_n++; s_move += p1 - p0; s_fft += p2 - p1; s_mac += p3 - p2; s_ifft += p4 - p3;
        volatile uint32_t *P = (volatile uint32_t *)(uintptr_t)0x01C7E440u;
        P[1] = s_n; P[2] = s_move; P[3] = s_fft; P[4] = s_mac; P[5] = s_ifft; P[6] = (uint32_t)np; P[7] = (uint32_t)s->parts;
        P[0] = 0x52504346u;                                           /* "FCPR" */
    }
#endif
}

PA_HOT void fc_block2(fc_state_t *s, const fc_cpx (*H2)[FC_BINS], int parts2, const float *in, float *outL, float *outR)
{
    memmove(s->frame, s->frame + FC_B, FC_B * sizeof(float));
    memcpy(s->frame + FC_B, in, FC_B * sizeof(float));
    s->head = (s->head == 0) ? s->parts - 1 : s->head - 1;
    rfft128(s->frame, s->X[s->head]);                            /* ONE forward transform for both channels */

    fc_cpx YL[FC_BINS], YR[FC_BINS];
    memset(YL, 0, sizeof YL);
    memset(YR, 0, sizeof YR);
    int slot = s->head;
    const int np = fc_step_weights(s, (s->limit > 0 && s->limit < s->parts) ? s->limit : s->parts);
    const int np2 = (np < parts2) ? np : parts2;
    int steady = 1;
    for (int p = 0; p < np; p++) if (s->pw[p] < 1.0f) { steady = 0; break; }
    if (steady) {                                                /* the usual case: every weight is 1 */
        fc_mac(s, &s->H[0][0], np, YL);
        fc_mac(s, &H2[0][0], np2, YR);
        slot = 0;
    }
    for (int p = 0; !steady && p < np; p++) {
        const fc_cpx *x = s->X[slot], *h = s->H[p];
        const float w = s->pw[p];
        if (w >= 1.0f) {                                         /* no weight to apply */
            for (int k = 0; k < FC_BINS; k++) {
                YL[k].re += x[k].re * h[k].re - x[k].im * h[k].im;
                YL[k].im += x[k].re * h[k].im + x[k].im * h[k].re;
            }
            if (p < np2) {
                const fc_cpx *h2 = H2[p];
                for (int k = 0; k < FC_BINS; k++) {
                    YR[k].re += x[k].re * h2[k].re - x[k].im * h2[k].im;
                    YR[k].im += x[k].re * h2[k].im + x[k].im * h2[k].re;
                }
            }
        } else {
            for (int k = 0; k < FC_BINS; k++) {
                YL[k].re += w * (x[k].re * h[k].re - x[k].im * h[k].im);
                YL[k].im += w * (x[k].re * h[k].im + x[k].im * h[k].re);
            }
            if (p < np2) {
                const fc_cpx *h2 = H2[p];
                for (int k = 0; k < FC_BINS; k++) {
                    YR[k].re += w * (x[k].re * h2[k].re - x[k].im * h2[k].im);
                    YR[k].im += w * (x[k].re * h2[k].im + x[k].im * h2[k].re);
                }
            }
        }
        if (++slot == s->parts) slot = 0;
    }
    float y[FC_N];
    irfft128(YL, y);
    memcpy(outL, y + FC_B, FC_B * sizeof(float));
    irfft128(YR, y);
    memcpy(outR, y + FC_B, FC_B * sizeof(float));
}
