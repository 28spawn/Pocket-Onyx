/* SPDX-License-Identifier: GPL-3.0-only */
/* Copyright (C) 2026 The Onyx Authors (@28spawn) */
/* custom-fw/src/dsp/pa_biquad.c - see include/pa_biquad.h */
#include "pa_config.h"
#include "pa_biquad.h"
#include "fixedpoint.h"

static inline int32_t bq_ssub(int32_t a, int32_t b) { return sat32((int64_t)a - (int64_t)b); }

void pa_biquad_q21_c(int32_t *buf, int n, const int32_t *k, int32_t *s1p, int32_t *s2p) {
    const int32_t b0 = k[0], b1 = k[1], b2 = k[2], a1 = k[3], a2 = k[4];
    int32_t s1 = *s1p, s2 = *s2p;
    for (int i = 0; i < n; i++) {
        const int32_t x = buf[i];
        const int32_t y = sat32((int64_t)q21_mul(b0, x) + (int64_t)s1);
        s1 = sat32((int64_t)bq_ssub(q21_mul(b1, x), q21_mul(a1, y)) + (int64_t)s2);
        s2 = bq_ssub(q21_mul(b2, x), q21_mul(a2, y));
        buf[i] = y;
    }
    *s1p = s1; *s2p = s2;
}

#if !defined(PA_HOST_BUILD) && !defined(PA_HOST_SIM) && !defined(PA_EMU_DLL) && !defined(__wasm__) && !defined(__EMSCRIPTEN__)
extern void pa_biquad_asm(int32_t *buf, int n, const int32_t *k, int32_t *state2);
static int8_t s_bq_ok = -1;

/* the assembly against the C on pseudo-random blocks: ordinary levels and levels that saturate, one block per coefficient set */
static int bq_selftest(void) {
    int32_t a[64], b[64], k[5], sa[2], sb[2];
    uint32_t x = 0x9E3779B9u;
#define BQ_RND() (x = x * 1664525u + 1013904223u, x)
    for (int t = 0; t < 6; t++) {
        for (int j = 0; j < 5; j++) k[j] = (int32_t)(BQ_RND() >> 9) - (1 << 22);          /* about +-2.0 in Q21 */
        const int hot = (t >= 4);
        for (int i = 0; i < 64; i++) {
            const int32_t v = (int32_t)BQ_RND() >> (hot ? 1 : 6);
            a[i] = b[i] = v;
        }
        sa[0] = sb[0] = (int32_t)(BQ_RND() >> 4) - (1 << 27);
        sa[1] = sb[1] = (int32_t)(BQ_RND() >> 4) - (1 << 27);
        pa_biquad_q21_c(a, 64, k, &sa[0], &sa[1]);
        pa_biquad_asm(b, 64, k, sb);
        for (int i = 0; i < 64; i++) if (a[i] != b[i]) return 0;
        if (sa[0] != sb[0] || sa[1] != sb[1]) return 0;
    }
#undef BQ_RND
    return 1;
}

int pa_biquad_asm_ok(void) {
    if (s_bq_ok < 0) {
        s_bq_ok = (int8_t)bq_selftest();
        *(volatile uint32_t *)(uintptr_t)0x01C7E7D8u = 0x42510000u | (uint32_t)s_bq_ok;       /* for jl_ram_read.py: "BQ" + 1 = the assembly passed */
    }
    return s_bq_ok;
}

void pa_biquad_q21(int32_t *buf, int n, const int32_t *k, int32_t *s1p, int32_t *s2p) {
    if (pa_biquad_asm_ok()) {
        int32_t st[2];
        st[0] = *s1p; st[1] = *s2p;
        pa_biquad_asm(buf, n, k, st);
        *s1p = st[0]; *s2p = st[1];
    } else {
        pa_biquad_q21_c(buf, n, k, s1p, s2p);
    }
}
#else
int pa_biquad_asm_ok(void) { return 0; }
void pa_biquad_q21(int32_t *buf, int n, const int32_t *k, int32_t *s1p, int32_t *s2p) { pa_biquad_q21_c(buf, n, k, s1p, s2p); }
#endif
