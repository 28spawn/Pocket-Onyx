/* SPDX-License-Identifier: GPL-3.0-only */
/* Copyright (C) 2026 The Onyx Authors (@28spawn) */
#include "../../include/pa_config.h"
/*
 * nam_engine.c — Pocket Amp NAM A2 16-Unit Q21 GRU Engine (Big Rock 2)
 *
 * Implementation of NAM A2 16-Unit Q21 GRU pipeline:
 *   - 9 rows of each gate matrix run on the 0x01EE0400 hardware accelerator
 *   - 7 rows of each gate matrix + LUT activations of the previous gate run
 *     simultaneously on the pi32v2 CPU!
 */
#include "../../include/pa_evlog.h"
#include <string.h>
#include "../../include/pa_model.h"
#include "../../include/pa_hw_accel.h"
#include "../../include/pa_wavenet.h"
#include "../../include/pa_activation.h"
#include "../dsp/fixedpoint.h"

/* BENCH profile of one GRU step, summed over the samples run (TIMER4 ticks, 24 MHz), SRAM 0x01C7C660:
 * 0 accelerator submits, 1 CPU matvec rows, 2 accelerator waits, 3 gate bias + sigmoid, 4 combine / tanh /
 * state / head, 5 post filters, 6 whole step, 7 samples. Read with tools/jl_ram_read.py slots. Device only. */
#if !defined(PA_HOST_BUILD) && !defined(PA_HOST_SIM)
extern uint32_t plat_ticks24(void);
#define PG ((volatile uint32_t *)(uintptr_t)0x01C7C660u)
/* The step profile reads TIMER4 four times a sample, so it is switched on for one block in 16 only (pg_on, set per block). */
static uint8_t pg_on;
#define PG_T() uint32_t pg_t = pg_on ? plat_ticks24() : 0u
#define PG_COUNT() do { if (pg_on) PG[7]++; } while (0)
#define PG_ADD(i) do { if (pg_on) { uint32_t pg_n = plat_ticks24(); PG[(i)] += pg_n - pg_t; pg_t = pg_n; } } while (0)
#else
#define PG_T() (void)0
#define PG_ADD(i) (void)0
#define PG_COUNT() (void)0
#endif

/* Helper: CPU dot product for the 7 CPU-assigned rows */
static void cpu_matvec_q21(const int32_t *vec,
                           const int32_t *mat,
                           int32_t *dst,
                           int rows,
                           int cols) {
    for (int r = 0; r < rows; r++) {
        int64_t acc = 0;
        const int32_t *row_ptr = &mat[r * cols];
        for (int c = 0; c < cols; c++) {
            acc += (int64_t)row_ptr[c] * (int64_t)vec[c];
        }
        dst[r] = (int32_t)((acc + (1LL << (PA_NAM_Q_SHIFT - 1))) >> PA_NAM_Q_SHIFT);
    }
}

/* Helper: Add bias and apply 512-entry Sigmoid LUT to 16 gate elements */
static void gate_bias_and_sigmoid_q21(int32_t *gate, const int32_t *bias) {
    for (int i = 0; i < PA_NAM_HIDDEN; i++) {
        int32_t x_q21 = gate[i] + bias[i];
        gate[i] = pa_lut_sigmoid(x_q21) >> 7;
    }
}

/* ---- float GRU (PA_NAM_FLOAT) ------------------------------------------------------------------
 * The Q21 step above spends ~19 cycles per multiply-accumulate (64-bit integer MACs); this core's FPU does
 * a float add or multiply in ~2 cycles (bench/, 2026-10-31). The weights are the SAME numbers: they are
 * converted from the model's Q21 arrays once, and again whenever they change (a hash of the weight and
 * bias bytes is checked once per block). The activations still go through the 512-entry LUTs, so the
 * non-linearity is identical; only the matrix-vector sums are float. The accelerator is not used.
 * Set PA_NAM_FLOAT 0 to get the original Q21 + accelerator path back. */
#ifndef PA_NAM_FLOAT
#define PA_NAM_FLOAT 1
#endif
#if PA_NAM_FLOAT
#include <stddef.h>
#define NF_H PA_NAM_HIDDEN
#define NF_X PA_NAM_INPUT_EXT
static float    s_wr[NF_H * NF_X], s_wz[NF_H * NF_X], s_wn[NF_H * NF_H];
static float    s_br[NF_H], s_bz[NF_H], s_bni[NF_H], s_bnh[NF_H], s_win[NF_H], s_wout[NF_H], s_hf[NF_H];
static float    s_bout, s_scale = 1.0f;
static uint32_t s_sig;
static uint8_t  s_valid;

static const float k_inv_q21 = 1.0f / 2097152.0f;
static inline float q21_to_f(int32_t v) { return (float)v * k_inv_q21; }
static inline int32_t f_to_q21(float v) {
    if (v >  1000.0f) v =  1000.0f;
    if (v < -1000.0f) v = -1000.0f;
    return (int32_t)(v * 2097152.0f);
}
/* The activations: the SAME 513-knot tables as the Q21 path (piecewise linear over [-16, 16), knots 1/16 apart),
 * copied to float once, interpolated in float. The integer version cost ~170 cycles a call (a 64-bit multiply, a
 * clamp, an init check) and the GRU makes 48 calls per sample, which was over half of the amp. This is one
 * float-to-int conversion, two loads and three float operations. */
static float s_act_sig[514], s_act_tanh[514];
static inline float act_lut(const float *t, float x) {
    float u = (x + 16.0f) * 16.0f;
    if (u < 0.0f) u = 0.0f;
    if (u > 511.99f) u = 511.99f;
    const int idx = (int)u;
    const float y0 = t[idx];
    return y0 + (t[idx + 1] - y0) * (u - (float)idx);
}
static inline float act_sigmoid(float x) { return act_lut(s_act_sig, x); }
static inline float act_tanh(float x)    { return act_lut(s_act_tanh, x); }

static uint32_t nam_weights_sig(const pa_nam_model_t *m) {
    uint32_t h = 2166136261u;
    const uint32_t *p = (const uint32_t *)(const void *)m;
    uint32_t n = (uint32_t)(offsetof(pa_nam_model_t, r_gate) / 4u);
    for (uint32_t i = 0; i < n; i++) h = (h ^ p[i]) * 16777619u;
    p = (const uint32_t *)(const void *)&m->b_r;
    n = (uint32_t)((offsetof(pa_nam_model_t, b_out) + 4u - offsetof(pa_nam_model_t, b_r)) / 4u);
    for (uint32_t i = 0; i < n; i++) h = (h ^ p[i]) * 16777619u;
    return h ^ (uint32_t)m->scale_q21;
}

static void nam_float_sync(const pa_nam_model_t *m) {
    {   const int32_t *ts = pa_activation_table(0), *tt = pa_activation_table(1);
        for (int i = 0; i <= 512; i++) { s_act_sig[i] = q21_to_f(ts[i]); s_act_tanh[i] = q21_to_f(tt[i]); }
        s_act_sig[513] = s_act_sig[512]; s_act_tanh[513] = s_act_tanh[512]; }
    for (int r = 0; r < NF_H; r++) {
        const int32_t *wr = (r < PA_NAM_HW_ROWS) ? m->w_r_hw[r] : m->w_r_cpu[r - PA_NAM_HW_ROWS];
        const int32_t *wz = (r < PA_NAM_HW_ROWS) ? m->w_z_hw[r] : m->w_z_cpu[r - PA_NAM_HW_ROWS];
        const int32_t *wn = (r < PA_NAM_HW_ROWS) ? m->w_n_hw[r] : m->w_n_cpu[r - PA_NAM_HW_ROWS];
        for (int c = 0; c < NF_X; c++) { s_wr[r * NF_X + c] = q21_to_f(wr[c]); s_wz[r * NF_X + c] = q21_to_f(wz[c]); }
        for (int c = 0; c < NF_H; c++) s_wn[r * NF_H + c] = q21_to_f(wn[c]);
        s_br[r] = q21_to_f(m->b_r[r]);   s_bz[r]  = q21_to_f(m->b_z[r]);
        s_bni[r] = q21_to_f(m->b_ni[r]); s_bnh[r] = q21_to_f(m->b_nh[r]);
        s_win[r] = q21_to_f(m->w_in[r]); s_wout[r] = q21_to_f(m->w_out[r]);
    }
    s_bout  = q21_to_f(m->b_out);
    s_scale = (m->scale_q21 != 0 && m->scale_q21 != (1 << PA_NAM_Q_SHIFT)) ? q21_to_f(m->scale_q21) : 1.0f;
}

static int32_t nam_step_float(pa_nam_model_t *m, int32_t x_q21) {
    PG_T();
    const float x = q21_to_f(x_q21);
    float v[NF_X], r[NF_H], z[NF_H], nr[NF_H];
    for (int i = 0; i < NF_H; i++) v[i] = s_hf[i];
    v[NF_H] = x;

    /* Plain two-accumulator loops. Register-blocking four rows was tried (pack 3) and made the amp SLOWER
     * (2895 -> 3147 us a block): four accumulators plus four row pointers spill on this core, and every spill
     * or reload is another ~8-cycle memory access. */
    for (int i = 0; i < NF_H; i++) {                       /* reset and update gates: 16 rows x 17 columns */
        const float *wr = &s_wr[i * NF_X], *wz = &s_wz[i * NF_X];
        float r0 = 0.0f, r1 = 0.0f, z0 = 0.0f, z1 = 0.0f;
        for (int c = 0; c < NF_H; c += 2) {
            r0 += wr[c] * v[c];     r1 += wr[c + 1] * v[c + 1];
            z0 += wz[c] * v[c];     z1 += wz[c + 1] * v[c + 1];
        }
        r0 += wr[NF_H] * v[NF_H];  z0 += wz[NF_H] * v[NF_H];
        r[i] = act_sigmoid(r0 + r1 + s_br[i]);
        z[i] = act_sigmoid(z0 + z1 + s_bz[i]);
    }
    PG_ADD(0);                                             /* gates: 16 x 17 matvec x2 + two sigmoids per row */
    for (int i = 0; i < NF_H; i++) {                       /* candidate recurrent part: 16 x 16, plus its bias */
        const float *wn = &s_wn[i * NF_H];
        float a0 = 0.0f, a1 = 0.0f;
        for (int c = 0; c < NF_H; c += 2) { a0 += wn[c] * v[c]; a1 += wn[c + 1] * v[c + 1]; }
        nr[i] = a0 + a1 + s_bnh[i];
    }
    PG_ADD(1);                                             /* candidate: 16 x 16 matvec */
    float out = 0.0f;
    for (int i = 0; i < NF_H; i++) {                       /* combine, tanh, state update, head */
        const float pre = s_win[i] * x + r[i] * nr[i] + s_bni[i];
        const float n   = act_tanh(pre);
        const float hn  = n + z[i] * (s_hf[i] - n);
        s_hf[i] = (hn < 1.0e-18f && hn > -1.0e-18f) ? 0.0f : hn;       /* a decaying state must not reach the denormals (slow on an FPU without them) */
        out += hn * s_wout[i];
    }
    PG_ADD(2);                                             /* combine, tanh, state, head */
    float y = (out + s_bout) * s_scale;
    int32_t y_q21 = f_to_q21(y);

    /* the same post filters as the Q21 step: de-fizz low-pass pair, then the 5 Hz DC blocker */
    /* the post filters in float multiplies: the four 64-bit q21_mul calls cost about 47 cycles each on this core, on core 1's critical path
     * every sample. The constants are the same (1345532 and 2095697 over 2^21); the results differ by under 2 Q21 steps, and the DC blocker
     * removes any offset that leaves. */
    const float k_alpha_defizz = 1345532.0f / 2097152.0f, k_dc_r = 2095697.0f / 2097152.0f;
    m->defizz_lp1 += (int32_t)((float)(y_q21 - m->defizz_lp1) * k_alpha_defizz);
    m->defizz_lp2 += (int32_t)((float)(m->defizz_lp1 - m->defizz_lp2) * k_alpha_defizz);
    int32_t raw_y = m->defizz_lp2;
    int32_t dc_y = m->dc_hold ? (m->dc_hold--, 0) : sat32((int64_t)raw_y - m->dc_x1 + (int32_t)((float)m->dc_y1 * k_dc_r));     /* tracking right after a reset: no step */
    m->dc_x1 = raw_y;
    m->dc_y1 = dc_y;
    PG_ADD(3);                                             /* post filters */
    PG_COUNT();
    return dc_y;
}

/* ---- accelerator matvec mode (PA_NAM_HW_MATVEC) --------------------------------------------------------
 * All 16 rows of each gate go to the vector accelerator as two 9-row jobs (rows 0..8 and rows 7..15; the weight
 * matrices are contiguous: w_*_hw[9] is followed by w_*_cpu[7]). The CPU only does the activations (float
 * interpolation), the combine and the head. The accelerator's matvec results have never been checked on the
 * device, so the first block runs a self-test with random weights against the exact integer reference
 * (acc >> 21); any mismatch, timeout or "broken" flag leaves the engine on the CPU float path above. */
#ifndef PA_NAM_HW_MATVEC
#define PA_NAM_HW_MATVEC 1
#endif
/* PA_NAM_HW_USE: 0 = the self-test and the output decode run, but the GRU stays on the CPU float path. The 2026-10-31
 * self-test failed (452 of 864 compares) and the GRU model's 'active' byte read back as garbage afterwards, so the
 * accelerator most likely writes its results in a layout we do not know and past the 9 words we gave it. */
#ifndef PA_NAM_HW_USE_ROWS
#define PA_NAM_HW_USE_ROWS 12    /* rows per gate on the accelerator once the self-test has proved jobs of that size exact (the stock uses 9 and 10) */
#endif
#ifndef PA_NAM_HW_USE
#define PA_NAM_HW_USE 1      /* the accelerator matvec is verified bit-exact against the rounding reference (pack 6) */
#endif
#if PA_NAM_HW_MATVEC
static uint8_t s_mode_hw;
static uint16_t s_hw_mask;                       /* bit n set: jobs of n rows were exact in the self-test (n = 5..15) */
static uint8_t s_hw_rows = PA_NAM_HW_ROWS;       /* rows per gate the accelerator computes: 9, or more once the self-test has verified bigger jobs */

_Static_assert(offsetof(pa_nam_model_t, w_r_cpu) == offsetof(pa_nam_model_t, w_r_hw) + PA_NAM_HW_ROWS * PA_NAM_INPUT_EXT * 4, "w_r rows must be contiguous");
_Static_assert(offsetof(pa_nam_model_t, w_z_cpu) == offsetof(pa_nam_model_t, w_z_hw) + PA_NAM_HW_ROWS * PA_NAM_INPUT_EXT * 4, "w_z rows must be contiguous");
_Static_assert(offsetof(pa_nam_model_t, w_n_cpu) == offsetof(pa_nam_model_t, w_n_hw) + PA_NAM_HW_ROWS * PA_NAM_HIDDEN * 4, "w_n rows must be contiguous");

static int32_t s_hwout[6][16] __attribute__((aligned(8)));       /* slack words: the engine may write past 9 */

static inline void hw_mv(uint32_t ctrl, uint32_t stride, const int32_t *vec, const int32_t *mat, int32_t *dst) {
    pa_hw_vec_cmd_t c;
    c.config = PA_HW_CFG_MATVEC; c.src_a = vec; c.src_b = mat; c.dst = dst; c.ctrl = ctrl; c.stride = stride;
    pa_hw_accel_submit(&c);
    pa_hw_accel_wait();
}

#if !defined(PA_HOST_BUILD) && !defined(PA_HOST_SIM)
#define HWST ((volatile uint32_t *)(uintptr_t)0x01C7C800u)      /* 0 magic, 1 jobs, 2 total ticks, 3 mismatches, 4 ok, 5 mode */
#endif

#if !defined(PA_HOST_BUILD) && !defined(PA_HOST_SIM)
/* BENCH (cabinet FIR decode): the accelerator's FIR job writes 129 words for a 64-sample block and nobody has decoded them. Three jobs
 * with known data, run once next to the matvec self-test (same thread, so the accelerator is not shared with a running GRU), each into a
 * sentinel-filled buffer: 0x01C7DC00 + 144 words * experiment, header { "FIRx", words changed, first changed, last changed + 1 }, then
 * the 140 first output words as the engine wrote them. tools/jl_ram_read.py fir prints them.
 *   A: coefficients = an impulse at 0 (1.0 in Q18), history = a ramp (i + 1) * 4096
 *   B: coefficients = a ramp (k + 1) * 4096, history = an impulse (1.0) at index 10
 *   C: coefficients = an impulse at 63, history = the ramp */
static void cab_fir_experiment(int32_t *co, int32_t *hi, int32_t *ou) {      /* three buffers of the self-test (the .bss region is full): co, hi >= 192 words, ou >= 512 */
    volatile uint32_t *R = (volatile uint32_t *)(uintptr_t)0x01C7DC00u;
    for (int ex = 0; ex < 3; ex++) {
        for (int i = 0; i < 192; i++) { co[i] = 0; hi[i] = 0; }
        if (ex == 0) { co[0] = 1 << 18; for (int i = 0; i < 192; i++) hi[i] = (i + 1) * 4096; }
        if (ex == 1) { for (int k = 0; k < 64; k++) co[k] = (k + 1) * 4096; hi[10] = 1 << 18; }
        if (ex == 2) { co[63] = 1 << 18; for (int i = 0; i < 192; i++) hi[i] = (i + 1) * 4096; }
        for (int i = 0; i < 512; i++) ou[i] = 0x7FEEDBAC;
        pa_hw_accel_fir_part64(co, hi, ou, 64, 0);
        uint32_t n = 0, first = 0xFFFFu, last = 0;
        for (int i = 0; i < 512; i++) if (ou[i] != 0x7FEEDBAC) { if (first == 0xFFFFu) first = (uint32_t)i; last = (uint32_t)i + 1u; n++; }
        volatile uint32_t *E = R + 144u * (uint32_t)ex;
        E[1] = n; E[2] = first; E[3] = last;
        for (int i = 0; i < 140; i++) E[4 + i] = (uint32_t)ou[i];
        E[0] = 0x46495230u + (uint32_t)ex;                          /* "FIR0", "FIR1", "FIR2" */
    }
}

/* BENCH (does the accelerator beat the CPU for the cabinet's spectral multiply-accumulate?): run once, same thread as the self-test, so
 * the accelerator is idle and nobody else touches its command block. Results at SRAM 0x01C7E400:
 *   [0] "FIRB"  [1] ticks (24 MHz) for 256 overwrite jobs  [2] ticks for 256 accumulate jobs  [3] ticks for 256 float complex
 *   multiply-accumulates of the same 65 bins on the CPU  [4] longest single overwrite job (ticks)  [5] words that differ from
 *   floor((a*b) >> 18)  [6] words that differ from round-to-nearest  [7] largest absolute difference from the rounded value
 *   [8] words compared (128)  [9] longest single accumulate job (ticks)
 * Accuracy uses random 24-bit spectra; the reference is the complex product (re = ar*br - ai*bi, im = ar*bi + ai*br) >> 18. */
#ifndef PA_CAB_BENCH
#define PA_CAB_BENCH 0
#endif
__attribute__((unused)) static void cab_fir_bench(int32_t *co, int32_t *hi, int32_t *ou) {
    volatile uint32_t *B = (volatile uint32_t *)(uintptr_t)0x01C7E400u;
    uint32_t seed = 0xC0FFEE11u;
    for (int i = 0; i < 130; i++) {
        seed = seed * 1664525u + 1013904223u; co[i] = (int32_t)(seed >> 8) - (1 << 23);
        seed = seed * 1664525u + 1013904223u; hi[i] = (int32_t)(seed >> 8) - (1 << 23);
    }
    /* accuracy: one overwrite job against the reference */
    for (int i = 0; i < 200; i++) ou[i] = 0x7FEEDBAC;
    pa_hw_accel_fir_part64(co, hi, ou, 64, 0);
    uint32_t mfloor = 0, mround = 0, maxd = 0;
    for (int j = 0; j < 64; j++) {
        const int64_t ar = co[2 * j], ai = co[2 * j + 1], br = hi[2 * j], bi = hi[2 * j + 1];
        const int64_t rr = ar * br - ai * bi, ri = ar * bi + ai * br;
        const int64_t fr = rr >> 18, fi = ri >> 18, nr = (rr + (1LL << 17)) >> 18, ni = (ri + (1LL << 17)) >> 18;
        if (ou[2 * j] != (int32_t)fr) mfloor++;
        if (ou[2 * j + 1] != (int32_t)fi) mfloor++;
        if (ou[2 * j] != (int32_t)nr) mround++;
        if (ou[2 * j + 1] != (int32_t)ni) mround++;
        int64_t d0 = (int64_t)ou[2 * j] - nr, d1 = (int64_t)ou[2 * j + 1] - ni;
        if (d0 < 0) d0 = -d0;
        if (d1 < 0) d1 = -d1;
        if ((uint32_t)d0 > maxd) maxd = (uint32_t)d0;
        if ((uint32_t)d1 > maxd) maxd = (uint32_t)d1;
    }
    /* timing */
    uint32_t t_ow = 0, t_ac = 0, mx_ow = 0, mx_ac = 0;
    for (int i = 0; i < 256; i++) { const uint32_t a = plat_ticks24(); pa_hw_accel_fir_part64(co, hi, ou, 64, 0); const uint32_t d = plat_ticks24() - a; t_ow += d; if (d > mx_ow) mx_ow = d; }
    for (int i = 0; i < 256; i++) { const uint32_t a = plat_ticks24(); pa_hw_accel_fir_part64(co, hi, ou, 64, 1); const uint32_t d = plat_ticks24() - a; t_ac += d; if (d > mx_ac) mx_ac = d; }
    float *X = (float *)(void *)(ou + 200), *H = (float *)(void *)(ou + 330), *Y = (float *)(void *)(ou + 460);
    for (int i = 0; i < 130; i++) { X[i] = (float)(co[i] >> 8) * (1.0f / 65536.0f); H[i] = (float)(hi[i] >> 8) * (1.0f / 65536.0f); Y[i] = 0.0f; }
    const uint32_t c0 = plat_ticks24();
    for (int it = 0; it < 256; it++) {                                 /* the cabinet's own inner loop: 65 bins, 4 multiplies and 4 adds each */
        for (int k = 0; k < 65; k++) {
            Y[2 * k]     += X[2 * k] * H[2 * k] - X[2 * k + 1] * H[2 * k + 1];
            Y[2 * k + 1] += X[2 * k] * H[2 * k + 1] + X[2 * k + 1] * H[2 * k];
        }
    }
    const uint32_t t_cpu = plat_ticks24() - c0;
    B[1] = t_ow; B[2] = t_ac; B[3] = t_cpu; B[4] = mx_ow; B[5] = mfloor; B[6] = mround; B[7] = maxd; B[8] = 128u; B[9] = mx_ac;
    B[0] = 0x42524946u;                                                /* "FIRB" */
}
#endif

/* BENCH (what is the stock's config 0x0A81?): three jobs with known data, results at SRAM 0x01C7E500 + 144 words * experiment, header
 * { "A81x", words changed, first, last + 1 } then the 140 first words of the destination. The job is built the way the stock builds it
 * Vector accelerator job: config 0x0A81, ctrl = (shift << 24) | 0x1000 | count, stride 0, src_a = one buffer, src_b = dst = the other (in place).
 *   V0: src_a = ramp (i + 1) * 4096,  dst before = 1.0 (262144) everywhere   -> multiply by a constant?
 *   V1: src_a = 1.0 everywhere,       dst before = ramp (i + 1) * 4096        -> which buffer is scaled?
 *   V2: src_a = ramp (i + 1) * 4096,  dst before = 0                          -> accumulate, copy or multiply?
 * shift = 18, count = 64. A guard of sentinel words follows the destination so a longer write shows up. */
static void cab_a81_probe(int32_t *a, int32_t *d, int32_t *ou) {
    (void)ou;
    volatile uint32_t *R = (volatile uint32_t *)(uintptr_t)0x01C7E500u;
    for (int ex = 0; ex < 3; ex++) {
        for (int i = 0; i < 192; i++) a[i] = 0;
        for (int i = 0; i < 192; i++) d[i] = 0x7FEEDBAC;
        for (int i = 0; i < 64; i++) {
            if (ex == 0) { a[i] = (i + 1) * 4096; d[i] = 1 << 18; }
            if (ex == 1) { a[i] = 1 << 18; d[i] = (i + 1) * 4096; }
            if (ex == 2) { a[i] = (i + 1) * 4096; d[i] = 0; }
        }
        pa_hw_vec_cmd_t c;
        c.config = 0x00000A81u; c.src_a = a; c.src_b = d; c.dst = d;
        c.ctrl = (18u << 24) | 0x1000u | 64u; c.stride = 0u;
        pa_hw_accel_submit(&c);
        pa_hw_accel_wait();
        uint32_t n = 0, first = 0xFFFFu, last = 0;
        for (int i = 64; i < 192; i++) if (d[i] != 0x7FEEDBAC) { /* words written PAST the 64 the job was given */ if (first == 0xFFFFu) first = (uint32_t)i; last = (uint32_t)i + 1u; n++; }
        volatile uint32_t *E = R + 144u * (uint32_t)ex;
        E[1] = n; E[2] = first; E[3] = last;
        for (int i = 0; i < 140; i++) E[4 + i] = (uint32_t)d[i];
        E[0] = 0x31384130u + (uint32_t)ex;                          /* "0A81"-ish tag: 0x3138 4130 + ex */
    }
}

/* WNOP probe: A2-Lite layer job configuration. It uses the matvec config 0xAA1 with
 * (a) a control word 0x18000000 | rows << 12 | cols, rows up to 35 (conv layers) or 72, cols = 3 * taps + 2, and (b) a stride word 0xC00000 | cols * 4
 * for the layer jobs and 0x40000C for the head job (48 columns = 16 taps x 3 channels, 12-byte time steps). The hypothesis: the "rows" are TIME
 * SAMPLES, each row a window of cols words starting 12 bytes (3 words) after the previous row's, the vector is the weights of one output channel.
 * Each experiment fills A and B with an impulse or a ramp, runs one job into a sentinel-filled buffer and records what came out. Shift 0 (top byte of
 * the control word) shows the raw integer sums; experiment 6 uses shift 24.
 * Results at SRAM 0x01C7F000: 8 blocks of 84 words: [0] "WN0"+ex, [1] words changed in the first 256, [2] first changed, [3] last + 1,
 * [4..83] the first 80 output words. tools/jl_ram_read.py wnop prints them.
 *   ex  ctrl (rows, cols)   stride      A (vector?)        B (matrix?)
 *   0   8, 48               0x40000C    ramp (j+1)*4096    impulse 4096 at 10
 *   1   8, 48               0x40000C    impulse 4096 at 5  ramp (i+1)*4096
 *   2   8, 48               0x00000C    ramp               impulse at 10
 *   3   8, 20               0xC00050    ramp               impulse at 10
 *   4   8, 20               0xC00050    impulse at 5       ramp
 *   5   35, 20              0xC00050    ramp               ramp
 *   6   8, 48, shift 24     0x40000C    (j+1) << 20        impulse 1 << 10 at 10
 *   7   72, 20              0xC00050    ramp               ramp */
__attribute__((unused)) static void wn_op_probe(int32_t *A, int32_t *B, int32_t *D) {
    static const struct { uint32_t shift, rows, cols, stride; uint8_t am, bm; } ex_[8] = {
        { 0, 8, 48, 0x0040000Cu, 0, 1 }, { 0, 8, 48, 0x0040000Cu, 1, 0 }, { 0, 8, 48, 0x0000000Cu, 0, 1 }, { 0, 8, 20, 0x00C00050u, 0, 1 },
        { 0, 8, 20, 0x00C00050u, 1, 0 }, { 0, 35, 20, 0x00C00050u, 0, 0 }, { 24, 8, 48, 0x0040000Cu, 2, 3 }, { 0, 72, 20, 0x00C00050u, 0, 0 } };
    volatile uint32_t *R = (volatile uint32_t *)(uintptr_t)0x01C7F000u;
    for (int ex = 0; ex < 8; ex++) {
        for (int i = 0; i < 48; i++) A[i] = 0;
        for (int i = 0; i < 270; i++) B[i] = 0;
        for (int i = 0; i < 1024; i++) D[i] = 0x7FEEDBAC;
        switch (ex_[ex].am) {
            case 0: for (int i = 0; i < 48; i++) A[i] = (i + 1) * 4096; break;
            case 1: A[5] = 4096; break;
            case 2: for (int i = 0; i < 48; i++) A[i] = (i + 1) << 20; break;
        }
        switch (ex_[ex].bm) {
            case 0: for (int i = 0; i < 270; i++) B[i] = (i + 1) * 4096; break;
            case 1: B[10] = 4096; break;
            case 3: B[10] = 1 << 10; break;
        }
        pa_hw_vec_cmd_t c;
        c.config = PA_HW_CFG_MATVEC; c.src_a = A; c.src_b = B; c.dst = D;
        c.ctrl = (ex_[ex].shift << 24) | (ex_[ex].rows << 12) | ex_[ex].cols; c.stride = ex_[ex].stride;
        pa_hw_accel_submit(&c);
        pa_hw_accel_wait();
        uint32_t n = 0, first = 0xFFFFu, last = 0;
        for (int i = 0; i < 256; i++) if (D[i] != 0x7FEEDBAC) { if (first == 0xFFFFu) first = (uint32_t)i; last = (uint32_t)i + 1u; n++; }
        volatile uint32_t *E = R + 84u * (uint32_t)ex;
        E[1] = n; E[2] = first; E[3] = last;
        for (int i = 0; i < 80; i++) E[4 + i] = (uint32_t)D[i];
        E[0] = 0x574E3000u + (uint32_t)ex;
    }
}


/* (the table is for row counts 5..15 now; word 23 is the largest n up to which every size from 5 was exact)
 * How many rows can one accelerator job compute, correctly, and how long does it take? The control word's row field (bits 15:12) was 9 for the
 * reset gate and 10 for the update and candidate gates in the stock code, and the job for the latter wrote a correct 10th row that the hybrid
 * step then recomputed on the CPU. Each row count 9..15 is run on random data against the exact reference (and a sentinel just past its output
 * checks that it writes no more), and 64 jobs are timed. Results at SRAM 0x01C7ED00: [0] "GRUR", then per row count n = 9..15 two words
 * (1 = every row exact and nothing written past n, 0 = not; ticks for 64 jobs), [15] the largest n up to which every count was exact. */
static uint32_t hw_rows_probe(int32_t *W, int32_t *Wn, int32_t *o) {
    static int32_t v[PA_NAM_INPUT_EXT];
    uint32_t seed = 0x0BADF00Du, best = 4u;
#if !defined(PA_HOST_BUILD) && !defined(PA_HOST_SIM)
    volatile uint32_t *R = (volatile uint32_t *)(uintptr_t)0x01C7ED00u;
#endif
    for (uint32_t i = 0; i < PA_NAM_HIDDEN * PA_NAM_INPUT_EXT; i++) { seed = seed * 1664525u + 1013904223u; W[i] = (int32_t)(seed >> 11) - (1 << 20); }
    for (uint32_t i = 0; i < PA_NAM_HIDDEN * PA_NAM_HIDDEN; i++)    { seed = seed * 1664525u + 1013904223u; Wn[i] = (int32_t)(seed >> 11) - (1 << 20); }
    int still_ok = 1;
    s_hw_mask = 0;
    for (uint32_t n = 5; n <= 15u; n++) {
        int ok = 1;
        for (int g = 0; g < 2; g++) {                                  /* the 17-column job (reset / update gates) and the 16-column job (candidate) */
            const int cols = g ? PA_NAM_HIDDEN : PA_NAM_INPUT_EXT;
            const int32_t *mat = g ? Wn : W;
            const uint32_t ctrl = 0x15000000u | (n << 12) | (uint32_t)cols, stride = g ? PA_HW_STRIDE_16 : PA_HW_STRIDE_17;
            for (int trial = 0; trial < 3; trial++) {
                for (int i = 0; i < PA_NAM_INPUT_EXT; i++) { seed = seed * 1664525u + 1013904223u; v[i] = (int32_t)(seed >> 11) - (1 << 20); }
                for (int i = 0; i < 64; i++) o[i] = 0x7FEEDBAC;
                hw_mv(ctrl, stride, v, mat, o);
                for (uint32_t r = 0; r < n; r++) {
                    int64_t acc = 0;
                    for (int c = 0; c < cols; c++) acc += (int64_t)mat[r * cols + c] * (int64_t)v[c];
                    if (o[r] != (int32_t)((acc + (1LL << 20)) >> 21)) ok = 0;
                }
                if (o[n] != 0x7FEEDBAC) ok = 0;                        /* it wrote past the rows it was asked for */
            }
        }
        uint32_t t_job = 0;
#if !defined(PA_HOST_BUILD) && !defined(PA_HOST_SIM)
        const uint32_t t0 = plat_ticks24();
#endif
        for (int i = 0; i < 64; i++) hw_mv(0x15000000u | (n << 12) | (uint32_t)PA_NAM_INPUT_EXT, PA_HW_STRIDE_17, v, W, o);
#if !defined(PA_HOST_BUILD) && !defined(PA_HOST_SIM)
        t_job = plat_ticks24() - t0;
        R[1 + 2 * (n - 5)] = (uint32_t)ok; R[2 + 2 * (n - 5)] = t_job;
#endif
        (void)t_job;
        if (ok) s_hw_mask |= (uint16_t)(1u << n);
        if (!ok) still_ok = 0;
        if (still_ok) best = n;
    }
#if !defined(PA_HOST_BUILD) && !defined(PA_HOST_SIM)
    R[23] = best; R[0] = 0x52555247u;                                  /* "GRUR" */
#endif
    return best;
}

static uint8_t nam_hw_selftest(void) {
    /* The control words of the z and n jobs carry a row count of 10 (PA_HW_CTRL_GRU_9x17_B, PA_HW_CTRL_GRU_9x16): a second pass of those starting at row 7 read rows 7..16, one row
     * past these 16-row matrices. Their second pass starts at row 6 instead, so the job ends on row 15. (Growing the arrays by a row cost 132 bytes the Bluetooth build does not have.) */
    enum { ST_ROWS = PA_NAM_HIDDEN };
    static int32_t W[ST_ROWS * PA_NAM_INPUT_EXT], Wn[ST_ROWS * PA_NAM_HIDDEN], v[PA_NAM_INPUT_EXT];
    static int32_t o[1024] __attribute__((aligned(8)));            /* big, so a stray write stays inside it */
    uint32_t seed = 0x1234567u, errs = 0, jobs = 0, ticks = 0;
    for (uint32_t i = 0; i < ST_ROWS * PA_NAM_INPUT_EXT; i++) { seed = seed * 1664525u + 1013904223u; W[i] = (int32_t)(seed >> 11) - (1 << 20); }
    for (uint32_t i = 0; i < ST_ROWS * PA_NAM_HIDDEN; i++)    { seed = seed * 1664525u + 1013904223u; Wn[i] = (int32_t)(seed >> 11) - (1 << 20); }
    for (int trial = 0; trial < 4; trial++) {
        for (int i = 0; i < PA_NAM_INPUT_EXT; i++) { seed = seed * 1664525u + 1013904223u; v[i] = (int32_t)(seed >> 11) - (1 << 20); }
        for (int g = 0; g < 3; g++) {
            const uint32_t ctrl   = (g == 0) ? PA_HW_CTRL_GRU_9x17_A : (g == 1) ? PA_HW_CTRL_GRU_9x17_B : PA_HW_CTRL_GRU_9x16;
            const uint32_t stride = (g == 2) ? PA_HW_STRIDE_16 : PA_HW_STRIDE_17;
            const int cols = (g == 2) ? PA_NAM_HIDDEN : PA_NAM_INPUT_EXT;
            const int32_t *mat = (g == 2) ? Wn : W;
            for (int part = 0; part < 2; part++) {
                const int job_rows = (int)((ctrl >> 12) & 0xFu);                              /* 9 or 10: how many rows the engine reads */
                const int off = part ? ST_ROWS - job_rows : 0;                                  /* 7 for a 9-row job, 6 for a 10-row one: always inside the matrix */
                for (int i = 0; i < 200; i++) o[i] = 0x7FEEDBAC;
#if !defined(PA_HOST_BUILD) && !defined(PA_HOST_SIM)
                const uint32_t t0 = plat_ticks24();
#endif
                hw_mv(ctrl, stride, v, mat + off * cols, o);
#if !defined(PA_HOST_BUILD) && !defined(PA_HOST_SIM)
                ticks += plat_ticks24() - t0;
#endif
                jobs++;
#if !defined(PA_HOST_BUILD) && !defined(PA_HOST_SIM)
                if (trial == 0) {                                  /* decode: which output words did this job write? */
                    volatile uint32_t *D = (volatile uint32_t *)(uintptr_t)(0x01C7C840u + 64u * (uint32_t)(g * 2 + part));
                    uint32_t n = 0;
                    for (int i = 0; i < 200; i++) {
                        if (o[i] != 0x7FEEDBAC) { if (n < 12u) { D[2u + n] = ((uint32_t)i << 24) | ((uint32_t)o[i] & 0xFFFFFFu); } n++; }
                    }
                    D[0] = 0x434F4544u; D[1] = n;                  /* "DECO", words changed; then (index << 24 | low 24 bits of the value) */
                }
#endif
                for (int r = 0; r < 9; r++) {
                    int64_t acc = 0;
                    for (int c = 0; c < cols; c++) acc += (int64_t)mat[(off + r) * cols + c] * (int64_t)v[c];
                    if (o[r] != (int32_t)((acc + (1LL << 20)) >> 21)) errs++;     /* the hardware rounds to nearest */
                }
            }
        }
    }
#if !defined(PA_HOST_BUILD) && !defined(PA_HOST_SIM)
    {   /* identity decode: W = identity in the first 9 columns, v[c] = (c + 1) * 4096, so out[r] should be (r + 1) * 4096 and every
         * word the engine writes tells its row and column. The raw output goes to SRAM 0x01C7CD00 (40 words), the expected value
         * and the job's duration after it. */
        for (int i = 0; i < ST_ROWS * PA_NAM_INPUT_EXT; i++) W[i] = 0;
        for (int r = 0; r < 16; r++) W[r * PA_NAM_INPUT_EXT + r] = (1 << 21);
        for (int i = 0; i < PA_NAM_INPUT_EXT; i++) v[i] = (i + 1) * 4096;
        for (int i = 0; i < 200; i++) o[i] = 0x7FEEDBAC;
        hw_mv(PA_HW_CTRL_GRU_9x17_A, PA_HW_STRIDE_17, v, W, o);
        volatile uint32_t *I = (volatile uint32_t *)(uintptr_t)0x01C7CD00u;
        for (int i = 0; i < 40; i++) I[i] = (uint32_t)o[i];
        I[40] = 0x44454D49u;                                         /* "IMED" */
    }
#endif
    const uint8_t ok = (errs == 0u && !pa_hw_accel_broken() && pa_hw_accel_errors() == 0u) ? 1u : 0u;
    if (ok) {                                                      /* how many rows can the jobs compute exactly? (the hybrid step uses PA_NAM_HW_USE_ROWS of them) */
        (void)hw_rows_probe(W, Wn, o);
        const uint32_t want = PA_NAM_HW_USE_ROWS;
        s_hw_rows = (uint8_t)((((s_hw_mask >> want) & 1u) && !pa_hw_accel_broken()) ? want : PA_NAM_HW_ROWS);
    }
#if !defined(PA_HOST_BUILD) && !defined(PA_HOST_SIM)
    if (ok) {
        cab_fir_experiment(W, Wn, o);
#if PA_CAB_BENCH
        cab_fir_bench(W, Wn, o);                                    /* about 3.5 ms on core 1 in the first block: it made the readouts show a 5.5 ms amp block and a 4.2 ms hand-over; the numbers are in the learnings */
#endif
        cab_a81_probe(W, Wn, o);
        /* wn_op_probe(Wn, W, o): decoded in pack 59 (see its comment); not run any more, the first block on core 1 was at 5.4 ms with it, 0.6 ms from the hang detector */
        /* wn_timing_probe(o): measured once (pack 60: 64x20 17.3 us, 64x47 36.5 us, 64x3 5.2 us, 64x48 37.2 us, 64x256 185 us, 64x1024 731 us = about 12 ns a
         * multiply-add). It takes about 17 ms, run on core 1 in the first block - past the 6 ms hang detector, which switched core 1 off for good at boot and left every amp
         * on core 0 with 50 % overruns. Never run probes this long in the audio thread. */
    }                                   /* only when the matvec jobs worked: the experiment is three more jobs */
    HWST[1] = jobs; HWST[2] = ticks; HWST[3] = errs; HWST[4] = ok; HWST[0] = 0x54535748u;
#endif
    return ok;
}

__attribute__((unused)) static int32_t nam_step_hw(pa_nam_model_t *m, int32_t x_q21) {
    PG_T();
    const float x = q21_to_f(x_q21);
    int32_t *hs = m->h_state;
    hs[PA_NAM_HIDDEN] = x_q21;
    const int32_t *wr = &m->w_r_hw[0][0], *wz = &m->w_z_hw[0][0], *wn = &m->w_n_hw[0][0];
    hw_mv(PA_HW_CTRL_GRU_9x17_A, PA_HW_STRIDE_17, hs, wr,                     s_hwout[0]);   /* r rows 0..8  */
    hw_mv(PA_HW_CTRL_GRU_9x17_A, PA_HW_STRIDE_17, hs, wr + 7 * PA_NAM_INPUT_EXT, s_hwout[1]); /* r rows 7..15 */
    hw_mv(PA_HW_CTRL_GRU_9x17_B, PA_HW_STRIDE_17, hs, wz,                     s_hwout[2]);   /* z rows 0..8  */
    hw_mv(PA_HW_CTRL_GRU_9x17_B, PA_HW_STRIDE_17, hs, wz + 7 * PA_NAM_INPUT_EXT, s_hwout[3]); /* z rows 7..15 */
    hw_mv(PA_HW_CTRL_GRU_9x16,   PA_HW_STRIDE_16, hs, wn,                     s_hwout[4]);   /* n rows 0..8  */
    hw_mv(PA_HW_CTRL_GRU_9x16,   PA_HW_STRIDE_16, hs, wn + 7 * PA_NAM_HIDDEN,   s_hwout[5]); /* n rows 7..15 */
    PG_ADD(0);                                                    /* the six accelerator jobs */

    float r[NF_H], z[NF_H], nr[NF_H];
    for (int i = 0; i < NF_H; i++) {
        const int j = (i < PA_NAM_HW_ROWS) ? 0 : 1, k = (i < PA_NAM_HW_ROWS) ? i : i - 7;
        r[i]  = act_sigmoid(q21_to_f(s_hwout[0 + j][k] + m->b_r[i]));
        z[i]  = act_sigmoid(q21_to_f(s_hwout[2 + j][k] + m->b_z[i]));
        nr[i] = q21_to_f(s_hwout[4 + j][k] + m->b_nh[i]);
    }
    PG_ADD(1);                                                    /* activations */
    float out = 0.0f;
    for (int i = 0; i < NF_H; i++) {
        const float pre = s_win[i] * x + r[i] * nr[i] + s_bni[i];
        const float n   = act_tanh(pre);
        const float hn  = n + z[i] * (q21_to_f(hs[i]) - n);
        hs[i] = f_to_q21(hn);
        out += hn * s_wout[i];
    }
    int32_t y_q21 = f_to_q21((out + s_bout) * s_scale);
    PG_ADD(2);                                                    /* combine, tanh, state, head */
    /* the post filters in float multiplies: the four 64-bit q21_mul calls cost about 47 cycles each on this core, on core 1's critical path
     * every sample. The constants are the same (1345532 and 2095697 over 2^21); the results differ by under 2 Q21 steps, and the DC blocker
     * removes any offset that leaves. */
    const float k_alpha_defizz = 1345532.0f / 2097152.0f, k_dc_r = 2095697.0f / 2097152.0f;
    m->defizz_lp1 += (int32_t)((float)(y_q21 - m->defizz_lp1) * k_alpha_defizz);
    m->defizz_lp2 += (int32_t)((float)(m->defizz_lp1 - m->defizz_lp2) * k_alpha_defizz);
    int32_t raw_y = m->defizz_lp2;
    int32_t dc_y = m->dc_hold ? (m->dc_hold--, 0) : sat32((int64_t)raw_y - m->dc_x1 + (int32_t)((float)m->dc_y1 * k_dc_r));     /* tracking right after a reset: no step */
    m->dc_x1 = raw_y;
    m->dc_y1 = dc_y;
    PG_ADD(3);
    PG_COUNT();
    return dc_y;
}

/* The hybrid step (what the stock firmware does): per gate, the accelerator takes rows 0..8 while the CPU computes rows 9..15
 * in float, and the previous gate's activations run while the next job is in flight. The GRU state is the float s_hf; the
 * accelerator reads the same state as Q21 integers in m->h_state, refreshed every sample. */
static inline void hw_submit(uint32_t ctrl, uint32_t stride, const int32_t *vec, const int32_t *mat, int32_t *dst) {
    pa_hw_vec_cmd_t c;
    c.config = PA_HW_CFG_MATVEC; c.src_a = vec; c.src_b = mat; c.dst = dst; c.ctrl = ctrl; c.stride = stride;
    pa_hw_accel_submit(&c);
}

static inline __attribute__((always_inline)) int32_t nam_step_hybrid_rows(pa_nam_model_t *m, int32_t x_q21, const int HR) {
    PG_T();
    const float x = q21_to_f(x_q21);
    int32_t *hs = m->h_state;
    hs[PA_NAM_HIDDEN] = x_q21;
    const int32_t *wr = &m->w_r_hw[0][0], *wz = &m->w_z_hw[0][0], *wn = &m->w_n_hw[0][0];
    float v[NF_X], rp[NF_H], zp[NF_H], np[NF_H], r[NF_H], z[NF_H];
    const uint32_t c17 = 0x15000000u | ((uint32_t)HR << 12) | 0x11u, c16 = 0x15000000u | ((uint32_t)HR << 12) | 0x10u;
    for (int i = 0; i < NF_H; i++) v[i] = s_hf[i];
    v[NF_H] = x;

    /* reset gate: accelerator rows 0..8, CPU rows 9..15 */
    hw_submit(c17, PA_HW_STRIDE_17, hs, wr, s_hwout[0]);
    for (int i = HR; i < NF_H; i++) {
        const float *w = &s_wr[i * NF_X]; float a0 = 0.0f, a1 = 0.0f;
        for (int c = 0; c < NF_H; c += 2) { a0 += w[c] * v[c]; a1 += w[c + 1] * v[c + 1]; }
        rp[i] = a0 + a1 + w[NF_H] * v[NF_H] + s_br[i];
    }
    pa_hw_accel_wait();
    for (int i = 0; i < HR; i++) rp[i] = q21_to_f(s_hwout[0][i]) + s_br[i];

    /* update gate; the reset gate's activations run while the accelerator works */
    hw_submit(c17, PA_HW_STRIDE_17, hs, wz, s_hwout[1]);
    for (int i = 0; i < NF_H; i++) r[i] = act_sigmoid(rp[i]);
    for (int i = HR; i < NF_H; i++) {
        const float *w = &s_wz[i * NF_X]; float a0 = 0.0f, a1 = 0.0f;
        for (int c = 0; c < NF_H; c += 2) { a0 += w[c] * v[c]; a1 += w[c + 1] * v[c + 1]; }
        zp[i] = a0 + a1 + w[NF_H] * v[NF_H] + s_bz[i];
    }
    pa_hw_accel_wait();
    for (int i = 0; i < HR; i++) zp[i] = q21_to_f(s_hwout[1][i]) + s_bz[i];

    /* candidate (h only, 16 columns); the update gate's activations run meanwhile */
    hw_submit(c16, PA_HW_STRIDE_16, hs, wn, s_hwout[2]);
    for (int i = 0; i < NF_H; i++) z[i] = act_sigmoid(zp[i]);
    for (int i = HR; i < NF_H; i++) {
        const float *w = &s_wn[i * NF_H]; float a0 = 0.0f, a1 = 0.0f;
        for (int c = 0; c < NF_H; c += 2) { a0 += w[c] * v[c]; a1 += w[c + 1] * v[c + 1]; }
        np[i] = a0 + a1 + s_bnh[i];
    }
    pa_hw_accel_wait();
    for (int i = 0; i < HR; i++) np[i] = q21_to_f(s_hwout[2][i]) + s_bnh[i];
    PG_ADD(0);

    float out = 0.0f;
    for (int i = 0; i < NF_H; i++) {
        const float pre = s_win[i] * x + r[i] * np[i] + s_bni[i];
        const float n   = act_tanh(pre);
        const float hn  = n + z[i] * (s_hf[i] - n);
        s_hf[i] = (hn < 1.0e-18f && hn > -1.0e-18f) ? 0.0f : hn;       /* a decaying state must not reach the denormals (slow on an FPU without them) */
        hs[i] = f_to_q21(hn);
        out += hn * s_wout[i];
    }
    int32_t y_q21 = f_to_q21((out + s_bout) * s_scale);
    PG_ADD(2);
    const float k_alpha_defizz = 1345532.0f / 2097152.0f, k_dc_r = 2095697.0f / 2097152.0f;       /* float multiplies: four 64-bit q21_mul calls were about 47 cycles each, on core 1's path every sample */
    m->defizz_lp1 += (int32_t)((float)(y_q21 - m->defizz_lp1) * k_alpha_defizz);
    m->defizz_lp2 += (int32_t)((float)(m->defizz_lp1 - m->defizz_lp2) * k_alpha_defizz);
    int32_t raw_y = m->defizz_lp2;
    int32_t dc_y = m->dc_hold ? (m->dc_hold--, 0) : sat32((int64_t)raw_y - m->dc_x1 + (int32_t)((float)m->dc_y1 * k_dc_r));     /* tracking right after a reset: no step */
    m->dc_x1 = raw_y;
    m->dc_y1 = dc_y;
    PG_ADD(3);
    PG_COUNT();
    return dc_y;
}

/* the two sizes, each compiled with its row count as a constant */
static int32_t nam_step_hybrid9(pa_nam_model_t *m, int32_t x_q21)  { return nam_step_hybrid_rows(m, x_q21, 9); }
static int32_t nam_step_hybrid10(pa_nam_model_t *m, int32_t x_q21) { return nam_step_hybrid_rows(m, x_q21, 10); }
static int32_t nam_step_hybrid11(pa_nam_model_t *m, int32_t x_q21) { return nam_step_hybrid_rows(m, x_q21, 11); }
static int32_t nam_step_hybrid12(pa_nam_model_t *m, int32_t x_q21) { return nam_step_hybrid_rows(m, x_q21, 12); }
static int32_t nam_step_hybrid13(pa_nam_model_t *m, int32_t x_q21) { return nam_step_hybrid_rows(m, x_q21, 13); }
static int32_t nam_step_hybrid14(pa_nam_model_t *m, int32_t x_q21) { return nam_step_hybrid_rows(m, x_q21, 14); }
static int32_t nam_step_hybrid15(pa_nam_model_t *m, int32_t x_q21) { return nam_step_hybrid_rows(m, x_q21, 15); }

/* The accelerator does the first h rows of a gate while the CPU does the other 16 - h and the previous gate's activations. A job costs about
 * 1.7 us plus 0.23 us a row (measured: 9 rows 2.84 us, 15 rows 4.22 us); a CPU row costs far less than an accelerator row, so the best h is
 * where the two sides take the same time, which only the unit can tell. A few seconds after the amp starts, each size in k_at_cand (the ones
 * the self-test proved exact) runs for 384 blocks, core 1's own time per block is averaged, and the fastest stays. Every size computes the
 * same numbers (only the split between the accelerator and the CPU differs), so the switch is inaudible. The result (see below) is at SRAM 0x01C7EE00:
 * "ATU2" at SRAM 0x01C7EE00 (not ED80 any more): the chosen row count, then every visit's mean ticks per block, 8 visits for each of 9..13 rows. */
#define AT_N 5
#define AT_R 8                                                  /* rounds: every size is visited this many times, the direction alternating */
#define AT_VISIT 128u                                           /* blocks per visit (the first AT_SKIP are not counted) */
#define AT_SKIP 8u
static const uint8_t k_at_cand[AT_N] = { 9, 10, 11, 12, 13 };   /* 14 and 15 rows are slower (pack 54: 1,117 and 1,291 us against 1,082 at 13) and overrun the block while they are tried */
static int16_t  s_at_visit = -1;                                /* -1 waiting, 0..AT_N*AT_R-1 measuring, AT_N*AT_R done */
static uint32_t s_at_blocks, s_at_n, s_at_ticks, s_at_res[AT_N][AT_R];
static uint8_t  s_at_cur;

static uint8_t at_cand_of(int v) { const int r = v / AT_N, k = v % AT_N; return (uint8_t)((r & 1) ? (AT_N - 1 - k) : k); }

static void at_next(void) {
    if (s_at_visit < AT_N * AT_R) { s_at_cur = at_cand_of(s_at_visit); s_hw_rows = k_at_cand[s_at_cur]; s_at_n = 0; s_at_ticks = 0; return; }
    uint32_t bestv = 0xFFFFFFFFu; uint8_t best = PA_NAM_HW_USE_ROWS;      /* done: the size with the lowest mean over all its visits stays */
    for (int i = 0; i < AT_N; i++) {
        if (!((s_hw_mask >> k_at_cand[i]) & 1u)) continue;
        uint32_t sum = 0; for (int r = 0; r < AT_R; r++) sum += s_at_res[i][r];
        sum /= AT_R;
        if (sum < bestv) { bestv = sum; best = k_at_cand[i]; }
    }
    s_hw_rows = best;
#if !defined(PA_HOST_BUILD) && !defined(PA_HOST_SIM)
    {   volatile uint32_t *T = (volatile uint32_t *)(uintptr_t)0x01C7EE00u;
        T[1] = best; T[2] = AT_N; T[3] = AT_R; T[4] = AT_VISIT - AT_SKIP; T[5] = s_at_blocks;
        for (int i = 0; i < AT_N; i++) { T[8 + AT_N + i] = k_at_cand[i]; for (int r = 0; r < AT_R; r++) T[8 + 2 * AT_N + i * AT_R + r] = s_at_res[i][r]; }
        T[0] = 0x32555441u; }                                           /* "ATU2" */
#endif
}
static void at_account(uint32_t dt) {
    if (s_at_visit >= AT_N * AT_R) return;
    s_at_blocks++;
    if (s_at_visit < 0) { if (s_at_blocks >= 6000u) { s_at_visit = 0; at_next(); } return; }
    if (++s_at_n > AT_SKIP) s_at_ticks += dt;
    if (s_at_n >= AT_VISIT) {
        s_at_res[s_at_cur][s_at_visit / AT_N] = s_at_ticks / (AT_VISIT - AT_SKIP);
        s_at_visit++; at_next();
    }
}

#endif /* PA_NAM_HW_MATVEC */
#endif /* PA_NAM_FLOAT */

void pa_nam_reset(pa_nam_model_t *m) {
    if (!m) return;
    memset(m->r_gate, 0, sizeof(m->r_gate));
    memset(m->z_gate, 0, sizeof(m->z_gate));
    memset(m->n_rec,  0, sizeof(m->n_rec));
    memset(m->h_state, 0, sizeof(m->h_state));
    m->defizz_lp1 = 0;
    m->defizz_lp2 = 0;
    m->dc_x1      = 0;
    m->dc_y1      = 0;
    m->dc_hold    = PA_DC_HOLD_SAMPLES;
#if PA_NAM_FLOAT
    for (int i = 0; i < NF_H; i++) s_hf[i] = 0.0f;
#endif
}

void pa_nam_init_default(pa_nam_model_t *m) {
    if (!m) return;
    memset(m, 0, sizeof(*m));
    pa_hw_accel_init();
    pa_activation_init();

    /* Default warm tube-like identity/overdrive weights in Q21 (1.0 = 1 << 21 = 2097152) */
    const int32_t q21_one = (1 << PA_NAM_Q_SHIFT);
    m->w_in[0]   = q21_one + (q21_one >> 2); /* 1.25 input drive into unit 0 */
    m->w_out[0]  = q21_one;                  /* 1.0 readout from unit 0 */
    m->b_z[0]    = -(q21_one * 2);           /* Fast update gate (z ~ 0.12) */
    m->b_r[0]    =  (q21_one * 2);           /* Open reset gate (r ~ 0.88) */
    m->scale_q21 = q21_one;
    m->active    = 1;
}

/*
 * Single-sample NAM A2 GRU step in native Q21 — co-scheduled HW + CPU pipeline
 */
int32_t nam_step_q21(pa_nam_model_t *m, int32_t x_q21) {
    if (!m || !m->active) return x_q21;

    PG_T();
    m->h_state[PA_NAM_HIDDEN] = x_q21;

    /* STAGE 1: Reset Gate r[0..15] = W_r * [h, x] */
    pa_hw_vec_cmd_t cmd_r = {
        .config = PA_HW_CFG_MATVEC,
        .src_a  = m->h_state,
        .src_b  = &m->w_r_hw[0][0],
        .dst    = &m->r_gate[0],
        .ctrl   = PA_HW_CTRL_GRU_9x17_A,
        .stride = PA_HW_STRIDE_17
    };
    pa_hw_accel_submit(&cmd_r);
    PG_ADD(0);
    cpu_matvec_q21(m->h_state, &m->w_r_cpu[0][0], &m->r_gate[PA_NAM_HW_ROWS],
                   PA_NAM_CPU_ROWS, PA_NAM_INPUT_EXT);
    PG_ADD(1);
    pa_hw_accel_wait();
    PG_ADD(2);

    /* STAGE 2: Update Gate z[0..15] = W_z * [h, x] + Sigmoid(r) */
    pa_hw_vec_cmd_t cmd_z = {
        .config = PA_HW_CFG_MATVEC,
        .src_a  = m->h_state,
        .src_b  = &m->w_z_hw[0][0],
        .dst    = &m->z_gate[0],
        .ctrl   = PA_HW_CTRL_GRU_9x17_B,
        .stride = PA_HW_STRIDE_17
    };
    pa_hw_accel_submit(&cmd_z);
    PG_ADD(0);
    gate_bias_and_sigmoid_q21(m->r_gate, m->b_r);
    PG_ADD(3);
    cpu_matvec_q21(m->h_state, &m->w_z_cpu[0][0], &m->z_gate[PA_NAM_HW_ROWS],
                   PA_NAM_CPU_ROWS, PA_NAM_INPUT_EXT);
    PG_ADD(1);
    pa_hw_accel_wait();
    PG_ADD(2);

    /* STAGE 3: Candidate Recurrent n_rec[0..15] = W_n * h + Sigmoid(z) */
    pa_hw_vec_cmd_t cmd_n = {
        .config = PA_HW_CFG_MATVEC,
        .src_a  = m->h_state,
        .src_b  = &m->w_n_hw[0][0],
        .dst    = &m->n_rec[0],
        .ctrl   = PA_HW_CTRL_GRU_9x16,
        .stride = PA_HW_STRIDE_16
    };
    pa_hw_accel_submit(&cmd_n);
    PG_ADD(0);
    gate_bias_and_sigmoid_q21(m->z_gate, m->b_z);
    PG_ADD(3);
    cpu_matvec_q21(m->h_state, &m->w_n_cpu[0][0], &m->n_rec[PA_NAM_HW_ROWS],
                   PA_NAM_CPU_ROWS, PA_NAM_HIDDEN);
    PG_ADD(1);
    pa_hw_accel_wait();
    PG_ADD(2);

    /* STAGE 4: Candidate Combine + Tanh + State Update + Head */
    int64_t out_acc64 = 0;
    for (int i = 0; i < PA_NAM_HIDDEN; i++) {
        int32_t n_rec_biased = m->n_rec[i] + m->b_nh[i];
        int64_t mac = (int64_t)m->w_in[i] * (int64_t)x_q21
                    + (int64_t)m->r_gate[i] * (int64_t)n_rec_biased;
        int32_t pre_tanh_q21 = (int32_t)((mac + (1LL << (PA_NAM_Q_SHIFT - 1))) >> PA_NAM_Q_SHIFT) + m->b_ni[i];

        int32_t n_i = pa_lut_tanh(pre_tanh_q21) >> 7;

        int32_t h_old = m->h_state[i];
        int32_t diff  = h_old - n_i;
        int32_t h_new = n_i + (int32_t)(((int64_t)m->z_gate[i] * (int64_t)diff + (1LL << (PA_NAM_Q_SHIFT - 1))) >> PA_NAM_Q_SHIFT);
        m->h_state[i] = h_new;

        out_acc64 += (int64_t)h_new * (int64_t)m->w_out[i];
    }

    PG_ADD(4);
    int32_t y_q21 = sat32(((out_acc64 + (1LL << (PA_NAM_Q_SHIFT - 1))) >> PA_NAM_Q_SHIFT) + m->b_out);
    if (m->scale_q21 != 0 && m->scale_q21 != (1 << PA_NAM_Q_SHIFT)) {
        y_q21 = sat32(((int64_t)y_q21 * (int64_t)m->scale_q21 + (1LL << (PA_NAM_Q_SHIFT - 1))) >> PA_NAM_Q_SHIFT);
    }

    /* Post-NAM Reactive Load / De-fizz filter (smooth 7.2 kHz analog roll-off) */
    const int32_t alpha_defizz = 1345532; /* ~7.2 kHz in Q21 */
    m->defizz_lp1 += q21_mul(alpha_defizz, y_q21 - m->defizz_lp1);
    m->defizz_lp2 += q21_mul(alpha_defizz, m->defizz_lp1 - m->defizz_lp2);

    /* 5 Hz DC blocker: removes persistent tube amp model cathode/grid bias */
    int32_t raw_y = m->defizz_lp2;
    int32_t dc_y = m->dc_hold ? (m->dc_hold--, 0) : sat32((int64_t)raw_y - m->dc_x1 + q21_mul(2095697, m->dc_y1)); /* R = 0.9993; tracking right after a reset: no step */
    m->dc_x1 = raw_y;
    m->dc_y1 = dc_y;
    PG_ADD(5);
    PG_COUNT();
    return dc_y;
}

q28_t pa_nam_step_sample(pa_nam_model_t *m, q28_t x_in_q28) {
    int32_t y_q21 = nam_step_q21(m, (int32_t)(x_in_q28 >> 7));
    if (y_q21 > 0x00FFFFFF)  y_q21 = 0x00FFFFFF;
    if (y_q21 < -0x01000000) y_q21 = -0x01000000;
    return (q28_t)(y_q21 << 7);
}

void pa_nam_process_block(pa_nam_model_t *m, q28_t *samples, int nframes) {
    if (!m || !m->active) return;
    for (int i = 0; i < nframes; i++) {
        samples[i] = pa_nam_step_sample(m, samples[i]);
    }
}

void nam_model_init(pa_nam_model_t *m) {
    pa_nam_init_default(m);
}

void pa_nam_prewarm(const pa_nam_model_t *m) {
    if (!m || !m->active) return;
#if PA_NAM_FLOAT
    const uint32_t sig = nam_weights_sig(m);
    if (!s_valid || sig != s_sig) {
#if PA_NAM_HW_MATVEC
        if (!s_valid) {
            s_mode_hw = (uint8_t)(nam_hw_selftest() && PA_NAM_HW_USE);
#if !defined(PA_HOST_BUILD) && !defined(PA_HOST_SIM)
            HWST[5] = s_mode_hw;
#endif
        }
#endif
        nam_float_sync(m);
        s_sig = sig;
        s_valid = 1u;
    }
#endif
}

PA_HOT void nam_gru_run_block(pa_nam_model_t *m, int32_t *buf_q21, int nframes) {
    if (!m || !m->active) return;
#if !defined(PA_HOST_BUILD) && !defined(PA_HOST_SIM)
    { static uint32_t s_blk; pg_on = ((s_blk++ & 15u) == 0u); }
#endif
#if PA_NAM_FLOAT
    {
        const uint32_t sig = nam_weights_sig(m);
        if (!s_valid || sig != s_sig) {
            pa_nam_prewarm(m);
        }
    }
#if PA_NAM_HW_MATVEC
    if (s_mode_hw) {
#if !defined(PA_HOST_BUILD) && !defined(PA_HOST_SIM)
        const uint32_t at_t0 = plat_ticks24();
#endif
        switch (s_hw_rows) {
            case 15: for (int i = 0; i < nframes; i++) buf_q21[i] = nam_step_hybrid15(m, buf_q21[i]); break;
            case 14: for (int i = 0; i < nframes; i++) buf_q21[i] = nam_step_hybrid14(m, buf_q21[i]); break;
            case 13: for (int i = 0; i < nframes; i++) buf_q21[i] = nam_step_hybrid13(m, buf_q21[i]); break;
            case 12: for (int i = 0; i < nframes; i++) buf_q21[i] = nam_step_hybrid12(m, buf_q21[i]); break;
            case 11: for (int i = 0; i < nframes; i++) buf_q21[i] = nam_step_hybrid11(m, buf_q21[i]); break;
            case 10: for (int i = 0; i < nframes; i++) buf_q21[i] = nam_step_hybrid10(m, buf_q21[i]); break;
            default: for (int i = 0; i < nframes; i++) buf_q21[i] = nam_step_hybrid9(m, buf_q21[i]);  break;
        }
#if !defined(PA_HOST_BUILD) && !defined(PA_HOST_SIM)
        if (nframes == 64) at_account(plat_ticks24() - at_t0);
#endif
    } else
#endif
    for (int i = 0; i < nframes; i++) {
        buf_q21[i] = nam_step_float(m, buf_q21[i]);
    }
#else
    for (int i = 0; i < nframes; i++) {
        buf_q21[i] = nam_step_q21(m, buf_q21[i]);
    }
#endif
#if PA_NAM_FLOAT
    {   /* The GRU state is a blend of tanh outputs, so it can never leave [-1, 1]. Anything else (NaN, infinity, a wild value)
         * means the float state is corrupt: it would stay that way for ever. Reset it and give this block silence. Also keep
         * the output inside +-16 full scale so a rare spike cannot drive the effects after the amp into saturation. */
        int bad = 0;
        for (int i = 0; i < NF_H; i++) { const float h = s_hf[i]; if (!(h >= -1.01f && h <= 1.01f)) bad = 1; }
        if (bad) {
            for (int i = 0; i < NF_H; i++) { s_hf[i] = 0.0f; m->h_state[i] = 0; }
            for (int i = 0; i < nframes; i++) buf_q21[i] = 0;
            pa_evlog(PA_EV_AMP_RESET, 1u, 0u);
        } else {
            const int32_t lim = 16 << 21;
            for (int i = 0; i < nframes; i++) { if (buf_q21[i] > lim) buf_q21[i] = lim; else if (buf_q21[i] < -lim) buf_q21[i] = -lim; }
        }
    }
#endif
}
