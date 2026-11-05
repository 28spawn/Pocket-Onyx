/*
 * custom-fw/bench/bench_fx.c - the DRV and MOD blocks, timed on the device.
 *
 * It runs the firmware's own src/fx/fx_drive.c and src/fx/fx_mod.c, compiled unchanged,
 * in 64-sample blocks exactly as the audio core calls them (the MOD block through
 * run_stereo(), the path pa_fx_chain_run_stereo() takes on the device), for every drive
 * model and every mod type. The settings are mid-knob (GRIT / TONE / LEVEL 50 %; RATE 30,
 * DEPTH 50, MIX 50, FDBK 30 %) - none of the code paths depend on the knob positions, only
 * on the model / type.
 *
 * pa_alloc() here is a bump allocator over a static arena, reset before each node: the
 * firmware's pool is not part of the benchmark image.
 */
#include "bench_fx.h"
#include <string.h>
#include "pa_config.h"
#include "pa_fx.h"

#if defined(PA_HOST_SIM)
#include <time.h>                      /* host check: a clock in place of Timer4 */
static uint32_t host_ticks(void) { return (uint32_t)((double)clock() * 24000000.0 / CLOCKS_PER_SEC); }
#define T4_CNT host_ticks()
#else
#define T4_CNT (*(volatile uint32_t *)0x00010804u)
#endif

#define NMAX       1024              /* the image has 256 KB of RAM in all */
#define Q21        2097152.0f
#define ARENA_SIZE 6144u               /* one node at a time: mod state + its 4 KB line */

fx_node_t *fx_drive_new(void);
fx_node_t *fx_mod_new(void);

static uint8_t  s_arena[ARENA_SIZE] __attribute__((aligned(8)));
static uint32_t s_arena_used;

void *pa_alloc(uint32_t n) {
    n = (n + 7u) & ~7u;
    if (s_arena_used + n > ARENA_SIZE) return 0;
    void *p = &s_arena[s_arena_used];
    s_arena_used += n;
    memset(p, 0, n);                   /* the firmware's pool hands out zeroed memory */
    return p;
}
void pa_free(void *p) { (void)p; }    /* released all at once: s_arena_used = 0 */

static int32_t s_L[NMAX], s_R[NMAX];

static int32_t knob(float v) { return (int32_t)(v * Q21); }

static uint32_t count_nonfinite(const int32_t *b, uint32_t n) {
    /* the blocks clamp to int32, so a NaN inside shows up as a stuck rail value */
    uint32_t c = 0;
    for (uint32_t i = 0; i < n; i++)
        if (b[i] == 2147483000 || b[i] == -2147483000 || b[i] == (int32_t)0x80000000) c++;
    return c;
}

uint32_t bench_fx_samples(uint32_t n) { return n > NMAX ? NMAX : n; }

int bench_fx_run(bench_fx_res_t *r, const float *in, uint32_t n, uint32_t reps) {
    if (n > NMAX) n = NMAX;                  /* see bench_fx_samples() */
    if (n == 0 || (n & 63u)) return 0xE201;
    memset(r, 0, sizeof *r);

    /* DRV: every model */
    for (int t = 0; t < BENCH_FX_DRV; t++) {
        s_arena_used = 0;
        fx_node_t *nd = fx_drive_new();
        nd->create(nd);
        if (!nd->state) return 0xE202;
        nd->set_param(nd, 3, t);                  /* model */
        nd->set_param(nd, 0, knob(0.5f));         /* GRIT */
        nd->set_param(nd, 1, knob(0.5f));         /* TONE */
        nd->set_param(nd, 2, knob(0.5f));         /* LEVEL */
        uint32_t best = 0xFFFFFFFFu;
        for (uint32_t rep = 0; rep < reps; rep++) {
            for (uint32_t i = 0; i < n; i++) s_L[i] = (int32_t)(in[i] * 0.5f * Q21);   /* -6 dBFS */
            uint32_t t0 = T4_CNT;
            for (uint32_t i = 0; i < n; i += 64u) nd->run(nd, &s_L[i], 64);
            uint32_t dt = T4_CNT - t0;
            if (dt < best) best = dt;
        }
        r->drv_ticks[t] = best;
        r->nan_count += count_nonfinite(s_L, n);
        memcpy(&r->drv_out_bits_last, &s_L[n - 1], 4);
        nd->destroy(nd);
    }

    /* MOD: every type, stereo */
    for (int t = 0; t < BENCH_FX_MOD; t++) {
        s_arena_used = 0;
        fx_node_t *nd = fx_mod_new();
        nd->create(nd);
        if (!nd->state) return 0xE203;
        nd->set_param(nd, 10, t);                 /* type */
        nd->set_param(nd, 0, knob(0.3f));         /* RATE */
        nd->set_param(nd, 1, knob(0.5f));         /* DEPTH */
        nd->set_param(nd, 2, knob(0.5f));         /* MIX */
        nd->set_param(nd, 3, knob(0.3f));         /* FDBK */
        uint32_t best = 0xFFFFFFFFu;
        for (uint32_t rep = 0; rep < reps; rep++) {
            for (uint32_t i = 0; i < n; i++) s_L[i] = s_R[i] = (int32_t)(in[i] * 0.5f * Q21);
            uint32_t t0 = T4_CNT;
            for (uint32_t i = 0; i < n; i += 64u) nd->run_stereo(nd, &s_L[i], &s_R[i], 64);
            uint32_t dt = T4_CNT - t0;
            if (dt < best) best = dt;
        }
        r->mod_ticks[t] = best;
        r->nan_count += count_nonfinite(s_L, n) + count_nonfinite(s_R, n);
        memcpy(&r->mod_out_bits_last, &s_L[n - 1], 4);
        nd->destroy(nd);
    }
    return 0;
}

#if defined(PA_HOST_SIM) && defined(BENCH_FX_HOST_MAIN)
/* host check: clang -O2 -DPA_HOST_SIM=1 -DBENCH_FX_HOST_MAIN -Iinclude -Ibench
 *             bench/bench_fx.c src/fx/fx_drive.c src/fx/fx_mod.c -lm */
#include <stdio.h>
#include <math.h>
int main(void) {
    static float in[NMAX];
    for (int i = 0; i < NMAX; i++) in[i] = 0.9f * sinf(6.2831853f * 196.0f * i / 44100.0f);
    bench_fx_res_t r;
    int rc = bench_fx_run(&r, in, NMAX, 5);
    printf("rc %d, non-finite %u\n", rc, r.nan_count);
    for (int t = 0; t < BENCH_FX_DRV; t++) printf("drv %d: %u ticks\n", t, r.drv_ticks[t]);
    for (int t = 0; t < BENCH_FX_MOD; t++) printf("mod %d: %u ticks\n", t, r.mod_ticks[t]);
    return rc;
}
#endif
