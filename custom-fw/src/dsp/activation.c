/* SPDX-License-Identifier: GPL-3.0-only */
/* Copyright (C) 2026 The Onyx Authors (@28spawn) */
/*
 * src/dsp/activation.c — High-Precision 512-Knot Sigmoid & Tanh Evaluator
 *
 * Implements exact 512-interval piecewise linear interpolation matching the AC791N hardware ROM:
 *   - Clamping to [-0x2000000, +0x1FFFFFF] ([-16.0, +16.0) in Q21)
 *   - 9-bit index extraction (>> 17) and 17-bit fractional interpolation
 *   - Zero runtime divisions
 *   - Strict zero-crossing alignment: tanh(0) == 0 strictly, sigmoid(0) == 0.5 (1048576) strictly
 */
#include "pa_config.h"
#include "pa_activation.h"
#include "fixedpoint.h"
#include <math.h>

static int s_act_inited = 0;
static int32_t s_sig_lut[513];
static int32_t s_tanh_lut[513];

void pa_activation_init(void) {
    if (s_act_inited) return;
    for (int i = 0; i <= 512; i++) {
        double x = ((double)i / 16.0) - 16.0;

        /* Sigmoid: s(x) = 1 / (1 + e^-x) in Q21 */
        double sig = 1.0 / (1.0 + exp(-x));
        s_sig_lut[i] = (int32_t)(sig * (double)PA_Q21_ONE + 0.5);

        /* Tanh: tanh(x) in Q21 */
        double th = tanh(x);
        s_tanh_lut[i] = (int32_t)(th * (double)PA_Q21_ONE + (th >= 0.0 ? 0.5 : -0.5));
    }

    /* Calibrate exact zero crossing entries: */
    s_tanh_lut[256] = 0;         /* tanh(0.0) == 0 strictly */
    s_sig_lut[256]  = 1048576;   /* sigmoid(0.0) == 0.5 in Q21 strictly */
    s_act_inited = 1;
}

/* The raw 513-knot tables (Q21), for the float GRU's own interpolation (src/nam/nam_engine.c). pair 0 = sigmoid, 1 = tanh. */
const int32_t *pa_activation_table(int pair) {
    if (!s_act_inited) pa_activation_init();
    return (pair == 0) ? s_sig_lut : s_tanh_lut;
}

int32_t pa_activation_lut(int32_t x, int pair) {
    if (!s_act_inited) pa_activation_init();

    /* Strict zero guard */
    if (pair == 1 && x == 0) return 0;

    const int32_t lo = -0x2000000; /* -16.0 in Q21 */
    const int32_t hi =  0x1FFFFFF; /* +16.0 - 1 LSB in Q21 */
    if (x < lo) x = lo;
    if (x > hi) x = hi;

    uint32_t u    = (uint32_t)(x + 0x2000000);
    uint32_t idx  = (u >> 17) & 0x1FFu;   /* 0..511 */
    uint32_t frac = u & 0x1FFFFu;         /* 17-bit fraction */

    const int32_t *lut = (pair == 0) ? s_sig_lut : s_tanh_lut;
    int32_t y0 = lut[idx];
    int32_t y1 = lut[idx + 1];
    int32_t slope = y1 - y0;

    int64_t interp = ((int64_t)slope * (int64_t)frac) >> 17;
    return sat32(y0 + interp);
}

int32_t pa_lut_sigmoid(int32_t x_q21) {
    return pa_activation_lut(x_q21, 0) << 7;
}

int32_t pa_lut_tanh(int32_t x_q21) {
    return pa_activation_lut(x_q21, 1) << 7;
}

int32_t pa_lut_fast_tanh(int32_t x_q21) {
    return pa_activation_lut(x_q21, 1);
}
