/* SPDX-License-Identifier: GPL-3.0-only */
/* Copyright (C) 2026 The Onyx Authors (@28spawn) */
/* test/test_reverb_switch_race.c - changing the reverb type while the audio interrupt runs.
 *
 * On the unit the UI thread (the main loop) calls set_param(10, type) and the audio interrupt runs the reverb between any two of its instructions. The reverb used to make
 * the type change itself: it set `type` and then laid the tank out (a 50 KB clear, then the line pointers). An interrupt in between ran the Plate with the Ambience's lines,
 * or the other way round - a state that has never been in that mode has NULL line buffers, so the interrupt wrote through a NULL pointer (the unit hung when the type was
 * scrolled from Plate to Ambience).
 *
 * This test includes the reverb source and replaces memset with a function that, while the UI thread is inside set_param, runs one audio block: the interrupt arriving in the
 * middle of the type change. The change must now be a request only (the audio thread applies it at the start of its next block), so no block ever sees a half-switched tank.
 * A block that does hit a NULL line buffer is caught by the pa_alloc'd guard page of the state (a segfault here, a hang on the unit), or by the output check below. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>

static void *(*real_memset)(void *, int, size_t) = memset;
static int g_in_ui;                         /* 1 while the "UI thread" is inside set_param */
static void hooked_run(void);
static void *hooked_memset(void *p, int c, size_t n)
{
    if (g_in_ui && n >= 40000u) { g_in_ui = 0; hooked_run(); g_in_ui = 1; }      /* the interrupt: right in the middle of the tank clear */
    return real_memset(p, c, n);
}
#define memset hooked_memset
#include "../src/fx/fx_reverb.c"
#undef memset

void *pa_alloc(uint32_t n) { return calloc(1, n); }
void  pa_free(void *p)     { free(p); }

static fx_node_t *nd;
static long blocks, nonfinite;

static void hooked_run(void)
{
    int32_t L[64], R[64];
    for (int i = 0; i < 64; i++) L[i] = R[i] = (int32_t)(0.5 * 2097152.0 * sin(0.05 * (blocks * 64 + i)));
    nd->run_stereo(nd, L, R, 64);
    blocks++;
    for (int i = 0; i < 64; i++) if (abs(L[i]) > 0x70000000 || abs(R[i]) > 0x70000000) nonfinite++;
}

int main(void)
{
    nd = fx_reverb_new();
    nd->create(nd);
    nd->set_param(nd, 3, PA_Q21_ONE);                    /* wet only: the tank's output is heard */
    int bad = 0;
    /* every type to every other type, the audio block firing inside each change; and a normal block between them */
    for (int round = 0; round < 3; round++)
        for (int a = 0; a < REV_TYPE_COUNT; a++)
            for (int b = 0; b < REV_TYPE_COUNT; b++) {
                g_in_ui = 0; nd->set_param(nd, 10, a); hooked_run(); hooked_run();
                g_in_ui = 1; nd->set_param(nd, 10, b); g_in_ui = 0;
                hooked_run(); hooked_run();
            }
    /* and from a state that was never in the target mode, built fresh each time (NULL line buffers) */
    for (int a = 0; a < REV_TYPE_COUNT; a++)
        for (int b = 0; b < REV_TYPE_COUNT; b++) {
            nd->destroy(nd); nd->create(nd);
            nd->set_param(nd, 3, PA_Q21_ONE);
            g_in_ui = 0; nd->set_param(nd, 10, a); hooked_run();
            g_in_ui = 1; nd->set_param(nd, 10, b); g_in_ui = 0;
            hooked_run();
        }
    if (nonfinite) { printf("FAIL: %ld extreme output samples\n", nonfinite); bad++; }
    printf("%s: %ld audio blocks run inside type changes, %ld extreme samples\n", bad ? "FAILED" : "RESULT: ALL PASSED", blocks, nonfinite);
    nd->destroy(nd);
    return bad ? 1 : 0;
}
