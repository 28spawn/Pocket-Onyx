/* SPDX-License-Identifier: GPL-3.0-only */
/* Copyright (C) 2026 The Onyx Authors (@28spawn) */
/* custom-fw/test/test_comp.c - the DYN block's compressors, measured.
 *
 * Types: 2 = COMP+GATE (the compressor after the noise gate), 4 = COMP (the compressor alone,
 * with its own THRESH). Both use the same compressor.
 *
 *   [1] static curve: the measured ratio above the threshold matches the RATIO knob
 *   [2] attack and release times are what they should be, and RELEASE does not follow RATIO
 *   [3] makeup: engaging the compressor keeps a typical note (-18 dBFS) at its level
 *   [4] no pumping / ripple: a held low E under 10 dB of gain reduction stays clean (THD)
 *   [5] COMP's THRESH sets where compression starts (the value the screen shows)
 *   [6] COMP has no gate; COMP+GATE still gates
 *   [7] RATIO 0 % + LEVEL 100 % is bit-exact; type changes stay clean
 *
 *   --report   print the measurements without failing
 *
 * clang -O2 -Icustom-fw/include -DPA_HOST_SIM=1 custom-fw/test/test_comp.c \
 *       custom-fw/src/fx/fx_dynamics.c -lm
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
#define Q21 2097152.0
#define PI  3.14159265358979

static int fails = 0, strict = 1;
#define CHECK(ok, ...) do { printf(!strict ? "  info " : (ok) ? "  ok   " : "  FAIL "); printf(__VA_ARGS__); printf("\n"); if (strict && !(ok)) fails++; } while (0)

static int g_type = 2;
/* knobs: THRESH, RATIO, ATTACK, LEVEL (0..1). THRESH on COMP+GATE is the gate's: 0 = open */
static fx_node_t *mk(double thr, double ratio, double attack) {
    fx_node_t *nd = fx_dynamics_new();
    nd->create(nd);
    nd->set_param(nd, 10, g_type);
    nd->set_param(nd, 0, (int32_t)(thr * Q21));
    nd->set_param(nd, 1, (int32_t)(ratio * Q21));
    nd->set_param(nd, 2, (int32_t)(attack * Q21));
    nd->set_param(nd, 3, (int32_t)Q21);
    return nd;
}

#define NMAX (FS * 4)
static double X[NMAX], Y[NMAX];
static void run(fx_node_t *nd, int n) {
    int32_t b[PA_BLOCK_N];
    for (int i = 0; i + PA_BLOCK_N <= n; i += PA_BLOCK_N) {
        for (int k = 0; k < PA_BLOCK_N; k++) b[k] = (int32_t)(X[i + k] * Q21);
        nd->run(nd, b, PA_BLOCK_N);
        for (int k = 0; k < PA_BLOCK_N; k++) Y[i + k] = b[k] / Q21;
    }
}
static double rms(const double *v, int a, int b) { double s = 0; for (int i = a; i < b; i++) s += v[i] * v[i]; return sqrt(s / (b - a)); }
static double db(double v) { return 20 * log10(v + 1e-15); }

/* steady output level (dBFS rms) for a sine of `f` at `in_db` (rms dBFS) */
static double out_db(fx_node_t *nd, double f, double in_db) {
    int n = FS - FS % PA_BLOCK_N;
    double a = pow(10, in_db / 20) * sqrt(2);
    for (int i = 0; i < n; i++) X[i] = a * sin(2 * PI * f * i / FS);
    run(nd, n);
    return db(rms(Y, n / 2, n));
}

/* the gain the compressor applies (dB), read sample by sample near the tone's peaks (the gain
 * multiplies the signal, so y/x is the gain). Returns the time (ms) after the level step for the
 * gain to cover 90 % of its change; *change gets the size of the change (dB). */
static double settle_ms(fx_node_t *nd, double lo_db, double hi_db, int up, double *change) {
    int pre = FS - FS % PA_BLOCK_N, post = 2 * FS - (2 * FS) % PA_BLOCK_N, n = pre + post;
    double l0 = pow(10, (up ? lo_db : hi_db) / 20) * sqrt(2), l1 = pow(10, (up ? hi_db : lo_db) / 20) * sqrt(2);
    for (int i = 0; i < n; i++) X[i] = (i < pre ? l0 : l1) * sin(2 * PI * 1000 * i / FS);
    run(nd, n);
    double g0 = 0, g1 = 0; int c0 = 0, c1 = 0;
    for (int i = pre - FS / 20; i < pre; i++) if (fabs(X[i]) > 0.7 * l0) { g0 += db(fabs(Y[i] / X[i])); c0++; }
    for (int i = n - FS / 20; i < n; i++)     if (fabs(X[i]) > 0.7 * l1) { g1 += db(fabs(Y[i] / X[i])); c1++; }
    g0 /= c0; g1 /= c1;
    *change = g1 - g0;
    if (fabs(g1 - g0) < 0.5) return 0;
    int t = n;
    for (int i = n - 1; i >= pre; i--)
        if (fabs(X[i]) > 0.7 * l1 && fabs(db(fabs(Y[i] / X[i])) - g1) > 0.1 * fabs(g1 - g0)) { t = i; break; }
    return (t - pre) * 1000.0 / FS;
}

static double thd_lowE(fx_node_t *nd, double in_db) {
    int n = 2 * FS - (2 * FS) % PA_BLOCK_N;
    double f = 82.4, a = pow(10, in_db / 20) * sqrt(2);
    for (int i = 0; i < n; i++) X[i] = a * sin(2 * PI * f * i / FS);
    run(nd, n);
    /* fit the fundamental over the second second (82.4 Hz: 82.4 cycles in a second - use a whole
     * number of cycles: 82 cycles = 43,895 samples) */
    int a0 = n - 43895, b0 = n;
    double re = 0, im = 0, e = 0;
    for (int i = a0; i < b0; i++) { re += Y[i] * cos(2 * PI * f * i / FS); im += Y[i] * sin(2 * PI * f * i / FS); e += Y[i] * Y[i]; }
    double n2 = b0 - a0, f1 = 2 * (re * re + im * im) / (n2 * n2) * n2 / 2;   /* fundamental energy * n2/2 ... */
    double p1 = (re * re + im * im) * 2 / n2;            /* energy of the fundamental over the window */
    (void)f1;
    double rest = e - p1;
    return sqrt(fmax(rest, 0) / p1) * 100;
}

/* output peak (dBFS) of a steady sine whose peak is in_pk (dBFS) */
static double out_pk(fx_node_t *nd, double f, double in_pk) { return out_db(nd, f, in_pk - 3.0103) + 3.0103; }

static fx_node_t *mk_t(int type, double thr, double ratio, double attack) {
    int keep = g_type; g_type = type;
    fx_node_t *nd = mk(thr, ratio, attack);
    g_type = keep;
    return nd;
}

int main(int argc, char **argv) {
    for (int i = 1; i < argc; i++) if (!strcmp(argv[i], "--report")) strict = 0;
    int u;

    printf("[1] static curve (1 kHz, peak levels; COMP THRESH 50 %% = -30 dBFS)\n");
    {
        const double rk[3] = { 0.25, 0.5, 1.0 };
        int ok = 1;
        for (int r = 0; r < 3; r++) {
            for (int ty = 2; ty <= 4; ty += 2) {
                fx_node_t *nd = mk_t(ty, ty == 4 ? 0.5 : 0.0, rk[r], 0.3);
                double a = out_pk(nd, 1000, -20), b = out_pk(nd, 1000, -4);
                nd->destroy(nd);
                double meas = 16.0 / (b - a), want = pa_dyn_display(ty, 1, (int32_t)(rk[r] * Q21), &u) / 10.0;
                printf("     %-9s RATIO %3.0f %%: screen %4.1f:1, measured %5.2f:1 (-20 -> %5.1f, -4 -> %5.1f dBFS)\n",
                       ty == 2 ? "COMP+GATE" : "COMP", rk[r] * 100, want, meas, a, b);
                if (fabs(meas - want) > 0.08 * want) ok = 0;
            }
        }
        CHECK(ok, "the measured ratio is the one the screen shows (within 8 %%)");
        fx_node_t *nd = mk_t(4, 0.5, 0.5, 0.3);
        double g1 = out_pk(nd, 1000, -60) + 60, g2 = out_pk(nd, 1000, -45) + 45;
        nd->destroy(nd);
        CHECK(fabs(g1 - g2) < 0.1, "below the threshold the gain is constant (%+.2f / %+.2f dB)", g1, g2);
    }

    printf("[2] attack / release (COMP, -40 -> -4 dBFS peak steps, 1 kHz)\n");
    {
        const double att[2] = { 0.0, 1.0 };
        int ok = 1;
        for (int a = 0; a < 2; a++) {
            fx_node_t *nd = mk_t(4, 0.5, 0.75, att[a]);
            double ca, cr;
            double ta = settle_ms(nd, -43, -7, 1, &ca);
            double tr = settle_ms(nd, -43, -7, 0, &cr);
            nd->destroy(nd);
            double sa = pa_dyn_display(4, 2, (int32_t)(att[a] * Q21), &u) / 10.0, sr = 80 + 12 * sa;
            printf("     ATTACK %3.0f %% (screen %4.1f ms): attack %5.1f ms (%.1f dB), release %5.1f ms (%.1f dB),"
                   " release law %.0f ms + up to 23 ms hold\n", att[a] * 100, sa, ta, ca, tr, cr, sr);
            if (ta > sa * 1.3 + 0.6 || ta < sa * 0.6) ok = 0;
            if (tr < sr * 0.85 || tr > sr * 1.1 + 25) ok = 0;
        }
        CHECK(ok, "attack and release take the times the knob says (90 %% of the change)");
        double trel[2]; const double rat[2] = { 0.3, 0.9 };
        for (int r = 0; r < 2; r++) {
            fx_node_t *nd = mk_t(4, 0.5, rat[r], 0.3);
            double c;
            trel[r] = settle_ms(nd, -43, -7, 0, &c);
            nd->destroy(nd);
        }
        CHECK(fabs(trel[0] - trel[1]) < 0.15 * fmax(trel[0], trel[1]),
              "RELEASE does not follow RATIO (%.0f ms at 30 %%, %.0f ms at 90 %%)", trel[0], trel[1]);
    }

    printf("[3] makeup\n");
    {
        int ok = 1;
        for (int ty = 2; ty <= 4; ty += 2)
            for (int r = 0; r < 2; r++) {
                fx_node_t *nd = mk_t(ty, ty == 4 ? 0.5 : 0.0, r ? 1.0 : 0.5, 0.3);
                double o = out_db(nd, 220, -18);
                nd->destroy(nd);
                printf("     %-9s RATIO %3d %%: a -18 dBFS rms note comes out at %.1f dBFS (%+.1f dB)\n",
                       ty == 2 ? "COMP+GATE" : "COMP", r ? 100 : 50, o, o + 18);
                if (fabs(o + 18) > 1.5) ok = 0;
            }
        CHECK(ok, "engaging the compressor keeps a -18 dBFS note at its level (within 1.5 dB)");
    }

    printf("[4] pumping / ripple on a held low E\n");
    {
        double worst = 0;
        for (int ty = 2; ty <= 4; ty += 2) {
            fx_node_t *nd = mk_t(ty, ty == 4 ? 0.3 : 0.0, 1.0, 0.0);
            double t = thd_lowE(nd, -6);
            nd->destroy(nd);
            printf("     %-9s low E at -6 dBFS, RATIO 100 %%, ATTACK 0 %%: THD %.3f %%\n", ty == 2 ? "COMP+GATE" : "COMP", t);
            if (t > worst) worst = t;
        }
        CHECK(worst < 0.3, "the gain does not ride the waveform (worst THD %.3f %%, the old one: 3.4 %%)", worst);
    }

    printf("[5] COMP's THRESH\n");
    {
        int ok = 1;
        const double tk[3] = { 0.0, 0.5, 1.0 };
        for (int i = 0; i < 3; i++) {
            double T = pa_dyn_display(4, 0, (int32_t)(tk[i] * Q21), &u) / 10.0;
            fx_node_t *nd = mk_t(4, tk[i], 0.5, 0.0);
            out_pk(nd, 1000, T - 6); double g_lo = pa_dyn_comp_gr_db(nd);
            out_pk(nd, 1000, T + 10); double g_hi = pa_dyn_comp_gr_db(nd);
            nd->destroy(nd);
            double want = -10 * (1 - 1 / 4.4721);
            printf("     THRESH %3.0f %% (screen %.0f dBFS): reduction %.2f dB 6 dB under it, %.1f dB 10 dB over (want %.1f)\n",
                   tk[i] * 100, T, g_lo, g_hi, want);
            if (g_lo < -0.05 || fabs(g_hi - want) > 0.5) ok = 0;
        }
        CHECK(ok, "compression starts at the THRESH the screen shows");
    }

    printf("[6] COMP has no gate; COMP+GATE still gates\n");
    {
        /* (fx_dynamics_new() hands out one static node: one at a time) */
        fx_node_t *a = mk_t(4, 0.3, 0.5, 0.3);
        double oa = out_pk(a, 1000, -66);
        a->destroy(a);
        fx_node_t *b = mk_t(2, 0.3, 0.5, 0.3);
        double ob = out_pk(b, 1000, -66);
        b->destroy(b);
        CHECK(oa > -66 && ob < -110, "a -66 dBFS tone: COMP passes it (%.1f dBFS), COMP+GATE at THRESH 30 %% shuts (%.0f dBFS)", oa, ob);
    }

    printf("[7] RATIO 0 %% with LEVEL 100 %% is bit-exact; type changes are clean\n");
    {
        fx_node_t *nd = mk_t(4, 0.5, 0.0, 0.3);
        int32_t b[PA_BLOCK_N], diff = 0;
        for (int k = 0; k < 200; k++) {
            for (int i = 0; i < PA_BLOCK_N; i++) b[i] = (int32_t)(0.7 * Q21 * sin(0.05 * (k * PA_BLOCK_N + i)));
            nd->run(nd, b, PA_BLOCK_N);
            for (int i = 0; i < PA_BLOCK_N; i++) if (b[i] != (int32_t)(0.7 * Q21 * sin(0.05 * (k * PA_BLOCK_N + i)))) diff++;
        }
        CHECK(diff == 0, "%d samples changed", diff);
        int bad = 0;
        nd->set_param(nd, 1, (int32_t)(0.8 * Q21));
        for (int r = 0; r < 15; r++) {
            nd->set_param(nd, 10, r % 5);
            double o = out_pk(nd, 440, -10);
            if (!(o == o) || o > 6) bad++;
        }
        nd->destroy(nd);
        CHECK(!bad, "15 type changes under signal stay finite and under +6 dBFS (%d bad)", bad);
    }

    printf("\n%s\n", fails ? "FAILED" : "COMP OK");
    return strict && fails ? 1 : 0;
}
