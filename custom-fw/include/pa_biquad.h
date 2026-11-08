/* SPDX-License-Identifier: GPL-3.0-only */
/* Copyright (C) 2026 The Onyx Authors (@28spawn) */
/* custom-fw/include/pa_biquad.h - one biquad section over a block, Q21, bit-exact between the C reference and the assembly kernel.
 *   y  = sat32(q21(b0 x) + s1)
 *   s1 = sat32(sat32(q21(b1 x) - q21(a1 y)) + s2)
 *   s2 = sat32(q21(b2 x) - q21(a2 y))                    q21(v) = (v + 2^20) >> 21 of the 64-bit product
 * k = {b0, b1, b2, a1, a2} (Q21). The state is two words. On the device the assembly kernel (pa_biquad_asm.c) does it in about 30 cycles a sample, where the compiled
 * C took about 210: a global EQ of 5 stereo bands went from ~370 us to ~60 us a block. A load-time self-test compares the two and falls back to C on any difference. */
#ifndef PA_BIQUAD_H
#define PA_BIQUAD_H
#include <stdint.h>
void pa_biquad_q21(int32_t *buf, int n, const int32_t *k, int32_t *s1, int32_t *s2);
void pa_biquad_q21_c(int32_t *buf, int n, const int32_t *k, int32_t *s1, int32_t *s2);   /* the reference */
int  pa_biquad_asm_ok(void);                                                              /* 1: the device runs the assembly */
#endif
