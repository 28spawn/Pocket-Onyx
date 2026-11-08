/* SPDX-License-Identifier: GPL-3.0-only */
/* Copyright (C) 2026 The Onyx Authors (@28spawn) */
/* include/pa_wild.h - "wild value" checks, for the places where one bad sample would poison what follows (a limiter's envelope, a filter's state).
 *
 * Wild means a number no signal can be: a NaN, an infinity, or a value many orders of magnitude past full scale. It is NOT a loud signal: a hot passage that goes 2 or 10 full scales over (a boosted
 * capture, a cabinet overshoot) is the limiter's job and must reach it unharmed. (Q21: 1.0 FS = 2,097,152.)
 *
 *   pa_f_wild(x)    a float that is not a usable sample: NaN, +-infinity, or past the int32 range the Q21 buffers can hold.
 *   pa_q21_wild(x)  a Q21 sample more than 64 full scales (+36 dBFS) out: the footprint of a saturated float-to-integer conversion, never of music.
 */
#ifndef PA_WILD_H
#define PA_WILD_H
#include <stdint.h>

#define PA_WILD_Q21 134217728             /* 64 x 2,097,152 */

static inline int pa_f_wild(float x) { return !(x > -2147483000.0f && x < 2147483000.0f); }     /* false for NaN too: every comparison with a NaN is false */
static inline int pa_f_nonfinite(float x) { return (x - x) != 0.0f; }                            /* NaN or +-infinity (a huge finite value is not) */
static inline int pa_q21_wild(int32_t x) { return x > PA_WILD_Q21 || x < -PA_WILD_Q21; }

#endif
