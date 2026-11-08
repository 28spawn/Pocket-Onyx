/* SPDX-License-Identifier: GPL-3.0-only */
/* Copyright (C) 2026 The Onyx Authors (@28spawn) */
/* test/test_gate.c - the noise gates of the DYN block, measured on the real node.
 *
 * Gate-only mode (type GATE, LEVEL ignored), so output = input x gate gain, and the gain
 * can be read back as output / input. Checks:
 *   [1] detection is flat across the guitar range: the amplitude that opens the gate at
 *       82 Hz (low E), 196 Hz, 660 Hz, 2 kHz and 5 kHz (hiss region) differs by < 6 dB
 *   [2] the gate never amplifies: output peak <= input peak on every signal
 *   [3] the release (RATIO knob on a gate) gets LONGER as the knob goes up, 20 ms .. 1 s
 *   [4] a decaying note crosses the threshold once: no chatter
 *   [5] a held note above threshold passes untouched: output == input, sample for sample
 *   [6] ATTACK: on GATE it is the gate's attack (0 % opens in < 1 ms, 100 % in 3..15 ms);
 *       on COMP+GATE it is the compressor's and must not slow the gate (< 2 ms at 100 %)
 *   [7] silence below threshold is really gated (>= 60 dB down)
 *   [8] LEVEL is the output volume on GATE and AI GATE MS: 100 % unity, 50 % -6 dB, 0 % silent
 *
 * clang -O2 -Icustom-fw/include -DPA_HOST_SIM=1 custom-fw/test/test_gate.c \
 *       custom-fw/src/fx/fx_dynamics.c -lm   (add --report to only print)
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include "pa_config.h"
#include "pa_fx.h"

void *pa_alloc(uint32_t n) { return calloc(1, n); }
void  pa_free(void *p)     { free(p); }
fx_node_t *fx_dynamics_new(void);

#define FS  44100
#define B   PA_BLOCK_N
#define Q21 2097152.0
#define PI  3.14159265358979

static int fails = 0, strict = 1;
#define CHECK(ok, ...) do { if (strict) { printf((ok) ? "  ok   " : "  FAIL "); printf(__VA_ARGS__); printf("\n"); if (!(ok)) fails++; } else { printf("  info "); printf(__VA_ARGS__); printf("\n"); } } while (0)

static int g_type = 0;                                 /* 0 GATE, 2 COMP+GATE */
static double g_level = 1.0;                           /* LEVEL knob, 0..1 */
static fx_node_t *mk(double thresh, double ratio, double attack) {
    fx_node_t *nd = fx_dynamics_new();
    nd->create(nd);
    nd->set_param(nd, 10, g_type);
    nd->set_param(nd, 0, (int32_t)(thresh * Q21));
    nd->set_param(nd, 1, (int32_t)(ratio * Q21));
    nd->set_param(nd, 2, (int32_t)(attack * Q21));
    nd->set_param(nd, 3, (int32_t)(g_level * Q21));
    return nd;
}

/* run `n` samples of x(t) through the gate; returns output in y */
static void run(fx_node_t *nd, double (*x)(int, void *), void *ctx, int n, double *y) {
    int32_t buf[B];
    for (int t0 = 0; t0 < n; t0 += B) {
        int m = (n - t0 < B) ? n - t0 : B;
        for (int i = 0; i < B; i++) buf[i] = (i < m) ? (int32_t)(x(t0 + i, ctx) * Q21) : 0;
        nd->run(nd, buf, B);
        for (int i = 0; i < m; i++) y[t0 + i] = buf[i] / Q21;
    }
}

typedef struct { double f, a; int on, off; double decay_s; } sig_t;
static double sine(int t, void *c) {
    sig_t *s = (sig_t *)c;
    if (t < s->on || t >= s->off) return 0.0;
    double env = s->decay_s > 0 ? exp(-(t - s->on) / (s->decay_s * FS)) : 1.0;
    return s->a * env * sin(2 * PI * s->f * t / FS);
}

static double rms(const double *y, int a, int b) {
    double e = 0; for (int i = a; i < b; i++) e += y[i] * y[i]; return sqrt(e / (b - a));
}

#define N (FS * 2)
static double Y[N];

/* lowest amplitude (dBFS) of a steady sine at f that leaves the gate open (out/in > -3 dB) */
static double open_level_db(double f) {
    for (double db = -90; db <= -10; db += 0.5) {
        fx_node_t *nd = mk(0.35, 0.3, 0.0);
        sig_t s = { f, pow(10, db / 20), 0, N, 0 };
        run(nd, sine, &s, FS, Y);
        double ratio = rms(Y, FS / 2, FS) / (s.a / sqrt(2));
        nd->destroy(nd);
        if (ratio > 0.707) return db;
    }
    return 0;
}

int main(int argc, char **argv) {
    setvbuf(stdout, NULL, _IONBF, 0);
    if (argc > 1 && !strcmp(argv[1], "--report")) strict = 0;

    /* [1] detection flatness */
    const double fr[5] = { 82.4, 196, 660, 2000, 5000 };
    double lv[5], lo = 1e9, hi = -1e9;
    printf("[1] level that opens the gate (threshold knob 35 %%):");
    for (int k = 0; k < 5; k++) {
        lv[k] = open_level_db(fr[k]);
        printf("  %.0f Hz %.1f dB", fr[k], lv[k]);
        if (lv[k] < lo) lo = lv[k];
        if (lv[k] > hi) hi = lv[k];
    }
    printf("\n");
    CHECK(hi - lo < 6.0, "detection is flat across 82 Hz..5 kHz (spread %.1f dB)", hi - lo);
    double thr_db = lv[1];

    /* [2] never amplifies: a slow swell through the threshold region */
    {
        fx_node_t *nd = mk(0.35, 0.3, 0.0);
        double maxg = 0;
        for (double db = thr_db - 6; db <= thr_db + 6; db += 0.25) {
            sig_t s = { 196, pow(10, db / 20), 0, N, 0 };
            run(nd, sine, &s, FS / 4, Y);
            double g = rms(Y, FS / 8, FS / 4) / (s.a / sqrt(2));
            if (g > maxg) maxg = g;
        }
        nd->destroy(nd);
        CHECK(maxg <= 1.001, "the gate never amplifies (max gain %.3f = %+.2f dB)", maxg, 20 * log10(maxg));
    }

    /* [3] release time vs the RATIO/DECAY knob: note stops, time to fall 40 dB */
    double rel_ms[3];
    const double knob[3] = { 0.1, 0.5, 0.9 };
    for (int k = 0; k < 3; k++) {
        fx_node_t *nd = mk(0.35, knob[k], 0.0);
        sig_t s = { 196, pow(10, (thr_db + 15) / 20), 0, FS / 2, 0 };
        run(nd, sine, &s, N, Y);
        double ref = rms(Y, FS / 4, FS / 2);
        int t = FS / 2;
        /* envelope of the tail: the tail is the gated residue of a stopped tone, so measure
         * the gain applied to a probe instead - re-run with a tone that continues at -12 dB
         * under the threshold, and see how long it takes to be attenuated 40 dB */
        nd->destroy(nd);
        nd = mk(0.35, knob[k], 0.0);
        double a_hi = pow(10, (thr_db + 15) / 20), a_lo = pow(10, (thr_db - 12) / 20);
        int32_t buf[B];
        int t_hit = -1;
        for (int t0 = 0; t0 < N && t_hit < 0; t0 += B) {
            for (int i = 0; i < B; i++) {
                int tt = t0 + i;
                double a = tt < FS / 2 ? a_hi : a_lo;
                buf[i] = (int32_t)(a * sin(2 * PI * 196 * tt / FS) * Q21);
            }
            nd->run(nd, buf, B);
            if (t0 >= FS / 2 + 2 * B) {
                double e = 0; for (int i = 0; i < B; i++) { double v = buf[i] / Q21; e += v * v; }
                double g = sqrt(e / B) / (a_lo / sqrt(2));
                if (g < 0.01) t_hit = t0 - FS / 2;
            }
        }
        rel_ms[k] = t_hit < 0 ? 9999 : t_hit * 1000.0 / FS;
        (void)ref; (void)t;
        nd->destroy(nd);
    }
    printf("[3] release to -40 dB: knob 10 %% %.1f ms, 50 %% %.1f ms, 90 %% %.1f ms\n", rel_ms[0], rel_ms[1], rel_ms[2]);
    CHECK(rel_ms[2] > 4 * rel_ms[0] && rel_ms[0] >= 10.0,
          "release gets longer as the knob goes up, and never under 10 ms (%.1f -> %.1f ms)", rel_ms[0], rel_ms[2]);

    /* [4] chatter: a note decaying slowly through the threshold */
    {
        fx_node_t *nd = mk(0.35, 0.5, 0.0);
        sig_t s = { 196, pow(10, (thr_db + 20) / 20), 0, N, 0.35 };   /* -20 dB over ~1.6 s */
        run(nd, sine, &s, N, Y);
        int trans = 0, open = 1;
        for (int t0 = B; t0 + B < N; t0 += B) {
            double in = 0, out = 0;
            for (int i = 0; i < B; i++) { double x = sine(t0 + i, &s); in += x * x; out += Y[t0 + i] * Y[t0 + i]; }
            if (in < 1e-14) break;
            int o = (out / in) > 0.25;
            if (o != open) { trans++; open = o; }
        }
        nd->destroy(nd);
        CHECK(trans <= 1, "a decaying note closes the gate once, no chatter (%d transitions)", trans);
    }

    /* [5] a held low-E note 6 dB over the threshold passes untouched, sample for sample */
    double maxdev[2];
    for (int k = 0; k < 2; k++) {
        fx_node_t *nd = mk(0.35, k ? 0.95 : 0.3, 0.0);
        sig_t s = { 82.4, pow(10, (thr_db + 6) / 20), 0, N, 0 };
        run(nd, sine, &s, N, Y);
        maxdev[k] = 0;
        for (int t = FS; t < N; t++) {
            double x = (double)(int32_t)(sine(t, &s) * Q21) / Q21;     /* the exact Q21 input */
            double d = fabs(Y[t] - x);
            if (d > maxdev[k]) maxdev[k] = d;
        }
        nd->destroy(nd);
    }
    CHECK(maxdev[0] == 0.0 && maxdev[1] == 0.0,
          "a held low-E note just over the threshold passes bit-for-bit (max deviation %.2e / %.2e at RATIO 30 / 95 %%)",
          maxdev[0], maxdev[1]);

    /* [6] opening time (to -1 dB of the input) */
    {
        double ms[3];
        const int types[3] = { 0, 0, 2 };
        const double att[3] = { 0.0, 1.0, 1.0 };
        for (int k = 0; k < 3; k++) {
            g_type = types[k];
            fx_node_t *nd = mk(0.35, 0.3, att[k]);
            g_type = 0;
            sig_t s = { 660, pow(10, (thr_db + 20) / 20), FS / 4, N, 0 };
            run(nd, sine, &s, FS / 2, Y);
            int t90 = -1;
            for (int t = FS / 4; t < FS / 2 - 32; t++) {
                double in = 0, out = 0;
                for (int i = 0; i < 32; i++) { double x = sine(t + i, &s); in += x * x; out += Y[t + i] * Y[t + i]; }
                if (in > 0 && out / in > 0.81) { t90 = t; break; }
            }
            ms[k] = t90 < 0 ? 9999 : (t90 - FS / 4) * 1000.0 / FS;
            nd->destroy(nd);
        }
        CHECK(ms[0] < 1.0, "GATE, ATTACK 0 %%: opens in under 1 ms (%.2f ms)", ms[0]);
        CHECK(ms[1] >= 3.0 && ms[1] <= 15.0, "GATE, ATTACK 100 %%: a slow, swell-like opening of 3..15 ms (%.2f ms)", ms[1]);
        CHECK(ms[2] < 2.0, "COMP+GATE, ATTACK 100 %%: the compressor's attack does not slow the gate (%.2f ms)", ms[2]);
    }

    /* [7] below threshold is really closed */
    {
        fx_node_t *nd = mk(0.35, 0.3, 0.0);
        sig_t s = { 2000, pow(10, (thr_db - 15) / 20), 0, N, 0 };       /* hiss-like, 15 dB under */
        run(nd, sine, &s, N, Y);
        double att = 20 * log10(rms(Y, FS, N) / (s.a / sqrt(2)) + 1e-12);
        nd->destroy(nd);
        CHECK(att < -60.0, "a signal 15 dB under the threshold is gated (%.1f dB)", att);
    }

    /* [8] LEVEL = output volume on GATE (0) and AI GATE MS (3), measured on an open gate */
    {
        const int types[2] = { 0, 3 };
        const double lv[3] = { 1.0, 0.5, 0.0 };
        double db[2][3];
        for (int k = 0; k < 2; k++) {
            for (int j = 0; j < 3; j++) {
                g_type = types[k]; g_level = lv[j];
                fx_node_t *nd = mk(0.35, 0.3, 0.0);
                g_type = 0; g_level = 1.0;
                sig_t s = { 660, pow(10, (thr_db + 20) / 20), 0, N, 0 };
                run(nd, sine, &s, N, Y);
                db[k][j] = 20 * log10(rms(Y, FS, N) / (s.a / sqrt(2)) + 1e-12);
                nd->destroy(nd);
            }
        }
        for (int k = 0; k < 2; k++)
            CHECK(fabs(db[k][0]) < 0.01 && fabs(db[k][1] + 6.02) < 0.1 && db[k][2] < -100.0,
                  "%s: LEVEL is the output volume (100 %% %.2f dB, 50 %% %.2f dB, 0 %% %.0f dB)",
                  k ? "AI GATE MS" : "GATE", db[k][0], db[k][1], db[k][2]);
    }

    if (strict) printf("\n%s\n", fails ? "FAILED" : "GATE OK");
    return strict && fails ? 1 : 0;
}
