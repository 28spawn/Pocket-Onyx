/* fixedpoint.h — reusable fixed-point DSP primitives (I/O-free, host-testable).
 * Fixed-point Multiply-Accumulate (MAC) DSP kernel:
 *   out[i] += (in[i] * coeff) >> 7  with saturating Q arithmetic. */
#ifndef PA_FIXEDPOINT_H
#define PA_FIXEDPOINT_H
#include <stdint.h>
#include "pa_config.h"
#include "pa_model.h"   /* PA_Q21_ONE, PA_NAM_QSHIFT */

/* Q21 helpers (NAM) */
static inline int32_t q21_from_float(float w) { return (int32_t)(w * (float)PA_Q21_ONE); }
static inline __attribute__((always_inline)) int32_t q21_mul(int32_t a, int32_t b) {           /* (a*b + round) >> 21 */
    int64_t prod = (int64_t)a * (int64_t)b;
    return (int32_t)((prod + (1LL << (PA_NAM_QSHIFT - 1))) >> PA_NAM_QSHIFT);
}
static inline __attribute__((always_inline)) int32_t sat32(int64_t v) {
    if (v >  2147483647LL) return  2147483647;
    if (v < -2147483648LL) return -2147483648;
    return (int32_t)v;
}

/* High-Pass Triangular Probability Density Function (TPDF) Dither Generator
 * Eliminates harmonic distortion and gritty quantization noise on decaying notes. */
static inline int32_t pa_prng_tpdf(void) {
    static uint32_t s_prng_state = 0x87654321u;
    static int32_t  s_prev_r = 0;
    /* 32-bit xorshift PRNG */
    s_prng_state ^= (s_prng_state << 13);
    s_prng_state ^= (s_prng_state >> 17);
    s_prng_state ^= (s_prng_state << 5);
    int32_t r1 = (int32_t)(s_prng_state & 0x1FFu) - 256;
    int32_t tpdf = r1 - s_prev_r; /* High-pass shaped dither */
    s_prev_r = r1;
    return tpdf;
}

static inline int32_t q21_quantize_dither(int64_t acc64, int shift) {
    int32_t dither = (shift >= 9) ? (pa_prng_tpdf() << (shift - 9)) : 0;
    return sat32((acc64 + (1LL << (shift - 1)) + dither) >> shift);
}

/* 2-coefficient MAC block, the stock primitive (alternating coeff[0]/coeff[1], >>7 normalize).
 * out[i] += (in[i] * coeff[i&1]) >> 7 . Pure buffer math — safe to reuse/clone. */
static inline void mac_block(int32_t *out, const int32_t *in, int n,
                             int16_t cA, int16_t cB) {
    for (int i = 0; i < n; i++) {
        int16_t c = (i & 1) ? cB : cA;
        out[i] = sat32((int64_t)out[i] + (((int64_t)in[i] * c) >> 7));
    }
}

#endif /* PA_FIXEDPOINT_H */
