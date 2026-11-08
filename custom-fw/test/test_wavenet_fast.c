/* SPDX-License-Identifier: GPL-3.0-only */
/* Copyright (C) 2026 The Onyx Authors (@28spawn) */
/* test_wavenet_fast.c - the optimised WaveNet steps against the baseline.
 *
 *   pa_wavenet_step_sample_fast()  must be BIT-EXACT: every output sample and the whole
 *                                  model state (ring buffers, positions) identical.
 *   pa_wavenet_step_sample_fast6() reorders additions: every output within 1e-5 of the
 *                                  baseline (outputs are clipped to +-2.0).
 *
 * Two models: the built-in default weights and a random A2-Lite capture, driven with a
 * sine, noise, silence and full-scale bursts.
 *
 *   clang -O2 -Icustom-fw/include -DPA_HOST_SIM=1 custom-fw/test/test_wavenet_fast.c \
 *         custom-fw/src/nam/pa_wavenet.c custom-fw/src/nam/pa_wavenet_fast.c -lm
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include "pa_wavenet.h"
#include "pa_wavenet_fast.h"

static float packed[PA_WN_FAST_PACKED_FLOATS];

static int fails = 0;
#define CHECK(ok, ...) do { printf((ok) ? "  ok   " : "  FAIL "); printf(__VA_ARGS__); printf("\n"); if (!(ok)) fails++; } while (0)

static uint32_t rng = 12345u;
static float frand(void) { rng = rng * 1664525u + 1013904223u; return (float)((rng >> 8) & 0xFFFFFF) / 16777216.0f * 2.0f - 1.0f; }

static float input_at(int i) {
    if (i % 4000 < 300) return 0.0f;                               /* silence */
    if (i % 5000 < 40)  return (i & 1) ? 1.0f : -1.0f;             /* full-scale burst */
    return 0.6f * sinf((float)i * 0.0571f) + 0.2f * frand();       /* sine + noise */
}

static void run(const char *name, const pa_wavenet_model_t *proto, int n) {
    pa_wavenet_model_t *a = malloc(sizeof *a), *b = malloc(sizeof *b), *c = malloc(sizeof *c);
    memcpy(a, proto, sizeof *a); memcpy(b, proto, sizeof *b); memcpy(c, proto, sizeof *c);
    CHECK(pa_wavenet_fast_pack(proto, packed) == 0, "the weights pack");
    int mism = 0, first = -1;
    double max6 = 0.0, peak = 0.0;
    rng = 777u;
    for (int i = 0; i < n; i++) {
        float x = input_at(i);
        float ya = pa_wavenet_step_sample(a, x);
        float yb = pa_wavenet_step_sample_fast(b, packed, x);
        float yc = pa_wavenet_step_sample_fast6(c, packed, x);
        if (memcmp(&ya, &yb, sizeof ya) != 0) { if (first < 0) first = i; mism++; }
        double d = fabs((double)ya - (double)yc);
        if (d > max6) max6 = d;
        if (fabs(ya) > peak) peak = fabs(ya);
    }
    printf("[%s] %d samples, output peak %.3f\n", name, n, peak);
    CHECK(mism == 0, "fast is bit-exact on every output (%d mismatches, first at %d)", mism, first);
    CHECK(memcmp(a->ring_buf, b->ring_buf, sizeof a->ring_buf) == 0 &&
          memcmp(a->ring_pos, b->ring_pos, sizeof a->ring_pos) == 0 &&
          memcmp(a->head_ring, b->head_ring, sizeof a->head_ring) == 0 && a->head_pos == b->head_pos,
          "fast leaves the model state bit-identical (ring buffers, positions, head ring)");
    CHECK(max6 < 1e-5, "fast6 stays within 1e-5 of the baseline (max diff %.3g)", max6);
    CHECK(peak > 0.01, "the model is not silent (the comparison means something)");
    free(a); free(b); free(c);
}

int main(void) {
    setvbuf(stdout, NULL, _IONBF, 0);
    pa_wavenet_model_t *m = malloc(sizeof *m);
    pa_wavenet_init_default(m);
    run("default weights", m, 30000);

    float *w = malloc(sizeof(float) * PA_WN_LITE_WEIGHTS);
    rng = 4242u;
    for (int i = 0; i < PA_WN_LITE_WEIGHTS; i++) w[i] = 0.35f * frand();
    w[PA_WN_LITE_WEIGHTS - 1] = 0.8f;                      /* head scale */
    pa_wavenet_init_default(m);
    if (pa_wavenet_load_weights(m, w, PA_WN_LITE_WEIGHTS, 0.8f) != 0) { printf("FAIL load\n"); return 1; }
    run("random capture", m, 30000);

    /* A model that is not 3-channel is passed through, never read with the wrong stride. */
    pa_wavenet_init_default(m);
    m->channels = 8;
    float y = pa_wavenet_step_sample_fast(m, packed, 0.3f), y6 = pa_wavenet_step_sample_fast6(m, packed, 0.3f);
    CHECK(y == 0.3f && y6 == 0.3f, "a non-3-channel model is passed through untouched");
    CHECK(pa_wavenet_fast_pack(m, packed) == -1, "packing a non-3-channel model is refused");

    printf("\n%s\n", fails ? "FAILED" : "WAVENET FAST OK");
    free(w); free(m);
    return fails ? 1 : 0;
}
