/* SPDX-License-Identifier: GPL-3.0-only */
/* Copyright (C) 2026 The Onyx Authors (@28spawn) */
/* test/test_biquad.c - the shared biquad block against the inline code it replaced (the fizz filter and the global EQ): the same output word for word on ordinary levels. */
#include <stdio.h>
#include <string.h>
#include "pa_config.h"
#include "pa_biquad.h"
#include "../src/dsp/fixedpoint.h"

static void old_block(int32_t *buf, int n, const int32_t *k, int32_t *s1p, int32_t *s2p) {
    int32_t s1 = *s1p, s2 = *s2p;
    for (int i = 0; i < n; i++) {
        int32_t x = buf[i];
        int32_t y = sat32((int64_t)q21_mul(k[0], x) + s1);
        s1 = sat32((int64_t)q21_mul(k[1], x) - q21_mul(k[3], y) + s2);
        s2 = sat32((int64_t)q21_mul(k[2], x) - q21_mul(k[4], y));
        buf[i] = y;
    }
    *s1p = s1; *s2p = s2;
}

int main(void) {
    uint32_t x = 12345u;
    int bad = 0, blocks = 0;
    for (int t = 0; t < 400; t++) {
        int32_t k[5], a[64], b[64], s[2] = {0, 0}, u[2] = {0, 0};
        for (int j = 0; j < 5; j++) { x = x * 1664525u + 1013904223u; k[j] = (int32_t)(x >> 10) - (1 << 21); }   /* about +-1.0 in Q21 */
        k[3] = k[3] / 2; k[4] = k[4] / 4;                                                                       /* a stable-ish filter */
        for (int blk = 0; blk < 4; blk++) {
            for (int i = 0; i < 64; i++) { x = x * 1664525u + 1013904223u; a[i] = b[i] = (int32_t)(x >> 8) - (1 << 23); }   /* +-1.0 of full scale */
            old_block(a, 64, k, &s[0], &s[1]);
            pa_biquad_q21(b, 64, k, &u[0], &u[1]);
            blocks++;
            if (memcmp(a, b, sizeof a) || s[0] != u[0] || s[1] != u[1]) bad++;
        }
    }
    printf("biquad: %d blocks, %d differ from the old inline code\n", blocks, bad);
    printf(bad ? "RESULT: FAILED\n" : "RESULT: ALL PASSED\n");
    return bad ? 1 : 0;
}
