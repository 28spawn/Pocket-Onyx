/*
 * pa_activation.h — Pocket Amp 512-Entry Piecewise-Linear Sigmoid & Tanh Activation LUTs
 *
 * 512-knot piecewise linear activation lookup tables:
 *   - Input domain: [-16.0, +16.0) in Q21 -> [-0x2000000, +0x1FFFFFF]
 *   - Index:        idx = ((x + 0x2000000) >> 17) & 0x1FF  (0..511)
 *   - Fraction:     frac = (x + 0x2000000) & 0x1FFFF       (17-bit linear interpolation)
 */
#ifndef PA_ACTIVATION_H
#define PA_ACTIVATION_H

#include <stdint.h>

#define PA_ACT_LUT_SIZE  512

/* Evaluate activation in Q21: pair 0 = sigmoid [0 .. 2^21], pair 1 = tanh [-2^21 .. +2^21] */
int32_t pa_activation_lut(int32_t x_q21, int pair);

/* Q28 wrappers used by nam_engine.c (shifted << 7 from Q21) */
int32_t pa_lut_sigmoid(int32_t x_q21);
int32_t pa_lut_tanh(int32_t x_q21);
int32_t pa_lut_fast_tanh(int32_t x_q21);
void pa_activation_init(void);
const int32_t *pa_activation_table(int pair);   /* the 513-knot Q21 tables: 0 sigmoid, 1 tanh */

#endif /* PA_ACTIVATION_H */
