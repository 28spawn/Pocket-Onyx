/* SPDX-License-Identifier: GPL-3.0-only */
/* Copyright (C) 2026 The Onyx Authors (@28spawn) */
/* test/test_delay_clicks.c - turning the delay's knobs while a note rings must not click.
 *
 * A steady 1 kHz note goes through the delay with feedback, so the output is a sum of sines of the same frequency: a smooth waveform whose second difference is small
 * ((2 pi f / fs)^2 times the amplitude, 0.02 of it). Any discontinuity - a read pointer landing on a different place in the line, a gain that steps - shows up as a second
 * difference many times larger. The metric is the largest second difference over the largest sample, in units of the smooth value: about 1 for a clean signal, tens for a click.
 * TIME is moved the way the encoder moves it (a step every few blocks, and a big jump), FEEDBACK and MIX are jumped, and the type is changed. */
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "pa_config.h"
#include "pa_model.h"
#include "pa_fx.h"
#include "../src/fx/fx_delay.c"

void *pa_alloc(uint32_t n) { return calloc(1, n); }
void  pa_free(void *p)     { free(p); }

static fx_node_t *nd;
static long total_blocks;
static double prev1, prev2;           /* the two previous outputs (continuity across blocks) */
static double worst, peak;            /* the largest second difference and the largest sample since the last reset of the metric */

static void run_blocks(int blocks)
{
    int32_t L[64];
    for (int b = 0; b < blocks; b++) {
        for (int i = 0; i < 64; i++) L[i] = (int32_t)(0.5 * 2097152.0 * sin(2.0 * M_PI * 1000.0 / 44100.0 * (double)(total_blocks * 64 + i)));
        nd->run_stereo(nd, L, 0, 64);
        total_blocks++;
        for (int i = 0; i < 64; i++) {
            const double y = (double)L[i] / 2097152.0;
            const double d2 = fabs(y - 2.0 * prev1 + prev2);
            if (total_blocks > 20 && d2 > worst) worst = d2;
            if (fabs(y) > peak) peak = fabs(y);
            prev2 = prev1; prev1 = y;
        }
    }
}

static double metric(void) { return peak > 0 ? worst / (0.0203 * peak) : 0; }
static void reset_metric(void) { worst = 0; peak = 0; }

int main(void)
{
    int fails = 0;
    nd = fx_delay_new();
    nd->create(nd);
    nd->set_param(nd, 10, DELAY_TYPE_DIGITAL);              /* the clean type: no modulation, no saturation */
    nd->set_param(nd, 0, 1048576);                          /* TIME 50 % */
    nd->set_param(nd, 1, (int32_t)(0.65 * PA_Q21_ONE));     /* feedback about 0.6 */
    nd->set_param(nd, 2, (int32_t)(0.5 * PA_Q21_ONE));      /* mix 50 % */
    nd->set_param(nd, 3, PA_Q21_ONE);                       /* tone open */
    run_blocks(700);                                        /* the echoes build up and ring */
    reset_metric();
    run_blocks(40);
    const double base = metric();
    printf("steady note, nothing touched: %.2f\n", base);

    /* TIME: an encoder's steps, every 6 blocks */
    reset_metric();
    for (int k = 0; k < 20; k++) { nd->set_param(nd, 0, 1048576 + (k + 1) * 20000); run_blocks(6); }
    const double m_time = metric();
    printf("TIME knob stepped 20 times while ringing: %.2f\n", m_time);

    /* TIME: a big jump back */
    reset_metric();
    nd->set_param(nd, 0, 300000);
    run_blocks(300);
    const double m_jump = metric();
    printf("TIME knob jumped across half the range: %.2f\n", m_jump);

    /* the echo really arrives at the new time within a fraction of a second after a big change (it used to take seconds) */
    {
        nd->set_param(nd, 0, 100000);                           /* a short delay... */
        nd->set_param(nd, 1, 0);                                /* no feedback: one clean echo */
        nd->set_param(nd, 2, (int32_t)(0.5 * PA_Q21_ONE));
        int32_t L[64];
        for (int b = 0; b < 900; b++) { memset(L, 0, sizeof L); nd->run_stereo(nd, L, 0, 64); }      /* the line empties (it holds 740 ms) */
        nd->set_param(nd, 0, 2000000);                          /* ...to a long one: the biggest kind of jump */
        for (int b = 0; b < 100; b++) { memset(L, 0, sizeof L); nd->run_stereo(nd, L, 0, 64); }      /* 145 ms later the new time must be in force (it took seconds when it only glided) */
        long arrive = -1;
        const long t0 = total_blocks;
        for (long b = 0; b < 1500; b++) {
            for (int i = 0; i < 64; i++) L[i] = (b == 0 && i == 0) ? (int32_t)(0.5 * 2097152.0) : 0;
            nd->run_stereo(nd, L, 0, 64);
            for (int i = 0; i < 64; i++) if (b * 64 + i > 800 && abs(L[i]) > 200000 && arrive < 0) arrive = b * 64 + i;       /* past the dry click */
        }
        (void)t0;
        nd->set_param(nd, 1, (int32_t)(0.65 * PA_Q21_ONE));     /* back to the ringing setup for the checks below */
        run_blocks(700);
        /* TIME 2000000/2097152 of the range: a delay of about 1024 + 0.954 * (hi - 1024) samples; the echo must be near that, whatever the glide */
        const long want = (long)(1024.0 + (2000000.0 / 2097152.0) * ((double)(DLY_BUF_LEN - 128u) - 1024.0));
        printf("echo after a big TIME jump arrives at %ld samples (the knob says %ld)\n", arrive, want);
        if (arrive < 0 || labs(arrive - want) > 300) { printf("FAIL: the echo is not at the knob's time\n"); fails++; }
    }

    /* FEEDBACK and MIX jumped */
    reset_metric();
    nd->set_param(nd, 1, (int32_t)(0.95 * PA_Q21_ONE)); run_blocks(10);
    nd->set_param(nd, 1, (int32_t)(0.20 * PA_Q21_ONE)); run_blocks(10);
    nd->set_param(nd, 2, (int32_t)(0.95 * PA_Q21_ONE)); run_blocks(10);
    nd->set_param(nd, 2, (int32_t)(0.10 * PA_Q21_ONE)); run_blocks(10);
    const double m_gain = metric();
    printf("FEEDBACK and MIX jumped: %.2f\n", m_gain);

    /* the modulated types: the chorus / wow sweep reads between samples now */
    nd->set_param(nd, 10, DELAY_TYPE_DTAPE);
    nd->set_param(nd, 2, (int32_t)(0.5 * PA_Q21_ONE));
    nd->set_param(nd, 1, (int32_t)(0.65 * PA_Q21_ONE));
    nd->set_param(nd, 6, PA_Q21_ONE);                       /* depth: full wow */
    run_blocks(400);
    reset_metric();
    run_blocks(80);
    const double m_mod = metric();
    printf("tape delay, full wow, steady: %.2f (a pitch-modulated note is not a pure sine, so this one is only compared with the old code)\n", m_mod);

    /* every type: switch between them while ringing, no NaN, no runaway */
    long bad = 0;
    for (int t = 0; t < DELAY_TYPE_COUNT; t++) {
        nd->set_param(nd, 10, t);
        for (int b = 0; b < 30; b++) {
            int32_t L[64], R[64];
            for (int i = 0; i < 64; i++) L[i] = R[i] = (int32_t)(0.5 * 2097152.0 * sin(2.0 * M_PI * 440.0 / 44100.0 * (double)(total_blocks * 64 + i)));
            nd->run_stereo(nd, L, R, 64); total_blocks++;
            for (int i = 0; i < 64; i++) if (abs(L[i]) > 0x20000000 || abs(R[i]) > 0x20000000) bad++;
        }
    }
    if (bad) { printf("FAIL: %ld extreme samples while switching types\n", bad); fails++; }

    if (base > 3.0)   { printf("FAIL: the steady note itself is not clean (%.2f)\n", base); fails++; }
    if (m_time > 3.0) { printf("FAIL: TIME steps click (%.2f, must stay under 3)\n", m_time); fails++; }
    if (m_jump > 3.0) { printf("FAIL: the TIME jump clicks (%.2f, must stay under 3)\n", m_jump); fails++; }
    if (m_gain > 3.0) { printf("FAIL: FEEDBACK / MIX steps click (%.2f, must stay under 3)\n", m_gain); fails++; }
    printf(fails ? "RESULT: FAILED (%d)\n" : "RESULT: ALL PASSED\n", fails);
    nd->destroy(nd);
    return fails ? 1 : 0;
}
