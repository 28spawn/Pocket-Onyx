/* SPDX-License-Identifier: GPL-3.0-only */
/* Copyright (C) 2026 The Onyx Authors (@28spawn) */
/* custom-fw/tools/dsp_artifact_probe.c
 *
 * Measurement harness for the Pocket Amp DSP. It drives the real fx_node_t
 * callbacks on the host and reports the numbers, so DSP behaviour can be
 * checked without flashing hardware.
 *
 * Build (from custom-fw/):
 *   clang -O2 -Wall -Iinclude -DPA_HOST_SIM=1 tools/dsp_artifact_probe.c \
 *         src/fx/fx_graph.c src/fx/fx_dynamics.c src/fx/fx_drive.c src/fx/fx_cab_ir.c \
 *         src/fx/fx_eq.c src/fx/fx_mod.c src/fx/fx_delay.c src/fx/fx_reverb.c \
 *         src/dsp/hw_accel.c -lm -o probe && ./probe
 */
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <math.h>
#include "pa_fx.h"
#include "pa_config.h"
#include "pa_model.h"

#define Q21 PA_Q21_ONE

/* ---- tracking allocator (the device uses the DSP pool in pa_pool.c) ------ */
static uint32_t s_alloc_total = 0;
void *pa_alloc(uint32_t n) {
    uint32_t a = (n + 7u) & ~7u;
    uint32_t *p = (uint32_t *)calloc(1, a + 8u);
    if (!p) return 0;
    p[0] = a; s_alloc_total += a; return (void *)(p + 2);
}
void pa_free(void *q) {
    if (!q) return;
    uint32_t *p = ((uint32_t *)q) - 2; s_alloc_total -= p[0]; free(p);
}

static double pk(const int32_t *b, int n) {
    double p = 0;
    for (int i = 0; i < n; i++) { double v = fabs(b[i] / (double)Q21); if (v > p) p = v; }
    return p;
}
static double db(double v) { return v > 0 ? 20.0 * log10(v) : -999.0; }

/* ===================================================================== */
/* 1. Reverb decay knob -> measured RT60                                   */
/*    The knob is param 0 (DECAY); the mapping lives in fx_reverb.c.        */
/* ===================================================================== */
static double measure_rt60(uint8_t knob_pct, double *floor_db) {
    fx_node_t *nd = fx_reverb_new();
    nd->create(nd);
    nd->set_param(nd, 0, (int32_t)(((uint64_t)knob_pct * Q21) / 100u));   /* knob 0 = DECAY */

    const int BLK = 64, NBLK = 1800;                 /* 1800 blocks ~ 2.6 s */
    int32_t *L = calloc(NBLK * BLK, sizeof(int32_t));
    int32_t *R = calloc(NBLK * BLK, sizeof(int32_t));
    L[0] = Q21; R[0] = Q21;

    double peak = 0; int peak_b = 0;
    double last_sound = 0;
    for (int b = 0; b < NBLK; b++) {
        nd->run_stereo(nd, L + b * BLK, R + b * BLK, BLK);
        double p = pk(L + b * BLK, BLK);
        if (p > peak) { peak = p; peak_b = b; }
        if (p > peak * 0.001) last_sound = p;          /* last block above -60 dB */
    }
    /* -60 dB point: the last block whose level was still above peak*0.001 */
    int end_b = peak_b;
    for (int b = peak_b; b < NBLK; b++) {
        if (pk(L + b * BLK, BLK) > peak * 0.001) end_b = b;
    }
    double rt60 = (double)(end_b - peak_b) * BLK / 44100.0;

    /* residual floor: measure the last 100 blocks */
    double f = 0;
    for (int b = NBLK - 100; b < NBLK; b++) { double p = pk(L + b * BLK, BLK); if (p > f) f = p; }
    if (floor_db) *floor_db = db(f);

    nd->destroy(nd);
    free(L); free(R);
    return rt60;
}

static void reverb_knob(void) {
    printf("\n===== REVERB: DECAY knob -> measured RT60 (from the real fx_reverb.c) =====\n");
    printf("  a usable reverb needs RT60 0.2 s .. 6 s\n");
    printf("\n   knob    RT60        floor      verdict\n");
    const uint8_t knobs[] = { 0, 25, 50, 75, 100 };
    for (unsigned i = 0; i < 5; i++) {
        double floor_db = -999;
        double rt = measure_rt60(knobs[i], &floor_db);
        printf("   %3u%%   %8.4f s  %7.1f dBFS   %s\n", knobs[i], rt, floor_db,
               (rt < 0.05) ? "<-- a CLICK, not a reverb" : "ok");
    }
    double floor_db = -999;
    double rt = measure_rt60(50, &floor_db);
    printf("\n   (default is whatever create() sets - measure with the knob at 50%% above)\n");
}

/* ===================================================================== */
/* 2. Reverb tank truncation limit cycle                                   */
/* ===================================================================== */
static void tank_limit_cycle(void) {
    printf("\n===== REVERB/DELAY tank conversion: is there a limit cycle? =====\n");
    /* The two exact conversions from the firmware. */
    for (int which = 0; which < 2; which++) {
        const char *nm = which ? "DELAY  (x+32)>>6" : "REVERB round-to-nearest";
        int32_t x = -1;
        printf("  %s : ", nm);
        for (int i = 0; i < 3; i++) {
            int32_t v;
            if (which) v = (x + 32) >> 6;
            else       v = (x >= 0) ? ((x + 32) >> 6) : -(((-x) + 32) >> 6);
            if (v > 32767) v = 32767;
            if (v < -32768) v = -32768;
            int16_t s = (int16_t)v;
            x = ((int32_t)s) << 6;
            printf("%d -> %d -> %d   ", s, s, x);
        }
        printf("\n");
    }
    printf("  A converged x (the last value repeating) means a permanent DC pedestal.\n");
}

/* ===================================================================== */
/* 3. CAB: block-size handling and IR survival across a bypass             */
/* ===================================================================== */
static void cab_block_size(void) {
    printf("\n===== CAB: block size and bypass behaviour =====\n");
    fx_node_t *nd = fx_cab_ir_new();
    nd->create(nd);

    int32_t a[128], b[128];
    for (int i = 0; i < 128; i++) a[i] = (int32_t)(0.5 * Q21 * sin(2 * 3.14159265358979 * 220.0 * i / 44100.0));
    memcpy(b, a, sizeof(a));
    nd->run(nd, b, 64);
    for (int i = 0; i < 128; i++) a[i] = (int32_t)(0.5 * Q21 * sin(2 * 3.14159265358979 * 220.0 * i / 44100.0));
    nd->run(nd, a, 128);
    int raw = 0;
    for (int i = 64; i < 128; i++) {
        double ref = 0.5 * Q21 * sin(2 * 3.14159265358979 * 220.0 * i / 44100.0);
        if (a[i] == (int32_t)ref) raw++;
    }
    printf("  128-sample call: second-half samples left UNCABBED = %d / 64 %s\n",
           raw, raw ? "<-- BUG" : "(ok)");

    /* bypass -> re-enable: is the IR preserved? */
    static int32_t taps[1024];
    memset(taps, 0, sizeof(taps));
    for (int i = 0; i < 1024; i++) taps[i] = (int32_t)(20000.0 * sin(0.01 * i));
    fx_cab_ir_load_q18(nd, taps, 1024);
    /* emulate the ISR finishing a bypass fade, then the UI thread collecting it */
    nd->active = 0; nd->pending_destroy = 1;
    if (nd->pending_destroy && nd->destroy) nd->destroy(nd);
    printf("  after bypass + GC: state = %p\n", (void *)nd->state);
    if (!nd->state && nd->create) { nd->create(nd); if (nd->reset) nd->reset(nd); }
    printf("  after re-enable : state = %p  -> the remembered IR is restored by create()\n",
           (void *)nd->state);
    nd->destroy(nd);
}

/* ===================================================================== */
/* 4. Output limiter: per-sample vs per-block                              */
/* ===================================================================== */
static void chain_limiter(void) {
    printf("\n===== CHAIN: output limiter behaviour =====\n");
    pa_fx_chain_t ch; memset(&ch, 0, sizeof(ch));
    fx_node_t *amp = fx_drive_new(), *cab = fx_cab_ir_new();
    pa_fx_chain_init(&ch, amp, cab);
    ch.output_limit_mode = PA_LMT_MODE_BRICKWALL;
    const int BLK = 64, NBLK = 200;
    int32_t *L = calloc(NBLK * BLK, sizeof(int32_t));
    int32_t *R = calloc(NBLK * BLK, sizeof(int32_t));
    for (int i = 0; i < NBLK * BLK; i++)
        L[i] = (int32_t)(0.8 * Q21 * sin(2 * 3.14159265358979 * 220.0 * i / 44100.0));
    for (int b = 0; b < NBLK; b++) pa_fx_chain_run_stereo(&ch, L + b * BLK, R + b * BLK, BLK);
    double mx = 0;
    for (int i = 0; i < NBLK * BLK; i++) { double v = fabs(L[i] / (double)Q21); if (v > mx) mx = v; }
    printf("  0.8 FS input -> chain output peak = %.4f (%.2f dBFS)\n", mx, db(mx));
    /* a per-block gain step shows up as a discontinuity at a block boundary */
    double worst = 0; int at = 0;
    for (int b = 1; b < NBLK; b++) {
        int i = b * BLK - 1;
        double d = fabs((double)L[i + 1] - (double)L[i]);
        if (d > worst) { worst = d; at = b; }
    }
    printf("  largest block-boundary discontinuity = %.4e at block %d\n", worst, at);
    free(L); free(R);
}

int main(void) {
    printf("=====================================================\n");
    printf(" Pocket Amp DSP measurement probe\n");
    printf("=====================================================\n");
    tank_limit_cycle();
    reverb_knob();
    cab_block_size();
    chain_limiter();
    printf("\n  pool bytes still allocated: %u\n", s_alloc_total);
    return 0;
}
