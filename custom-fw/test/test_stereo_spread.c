/* test/test_stereo_spread.c - the stereo spread (src/dsp/pa_stereo_spread.h): on a dual-mono guitar-like signal the left and right channels come out EQUALLY LOUD (within
 * 0.3 dB, for a low-heavy, a mid, a bright and a cabinet-filtered spectrum: the earlier design made one ear 4-7 dB louder on those), each channel keeps the level of the input,
 * the correlation between the channels falls as the width grows (fully uncorrelated at 100), the mono sum keeps the tone (only quieter by the decorrelation), a real stereo input
 * keeps its own side signal, and a loud or silent input never makes it run away. */
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "../src/dsp/pa_stereo_spread.h"

static int fails;
#define CHECK(c, ...) do { if (!(c)) { fails++; printf("FAIL: "); printf(__VA_ARGS__); printf("\n"); } } while (0)

static uint32_t seed = 12345u;
static double noise(void) { seed = seed * 1664525u + 1013904223u; return (double)(int32_t)seed / 2147483648.0; }

/* a simple 2-pole band-pass (RBJ) to shape the noise like a guitar signal */
typedef struct { double b0, b1, b2, a1, a2, x1, x2, y1, y2; } bq_t;
static void bq_bp(bq_t *q, double f0, double Q)
{
    const double w = 2 * M_PI * f0 / 44118.0, al = sin(w) / (2 * Q), a0 = 1 + al;
    q->b0 = al / a0; q->b1 = 0; q->b2 = -al / a0; q->a1 = -2 * cos(w) / a0; q->a2 = (1 - al) / a0; q->x1 = q->x2 = q->y1 = q->y2 = 0;
}
static double bq_run(bq_t *q, double x)
{
    const double y = q->b0 * x + q->b1 * q->x1 + q->b2 * q->x2 - q->a1 * q->y1 - q->a2 * q->y2;
    q->x2 = q->x1; q->x1 = x; q->y2 = q->y1; q->y1 = y; return y;
}

typedef struct { double bal_db, level_db, corr, mono_db; } res_t;

static res_t run(double f0, double Q, uint8_t width)
{
    pa_spread_t s; memset(&s, 0, sizeof s);
    bq_t q1, q2; bq_bp(&q1, f0, Q); bq_bp(&q2, f0, Q);
    int32_t L[64], R[64], X[64];
    double sl = 0, sr = 0, sx = 0, slr = 0, sm = 0;
    for (int b = 0; b < 700; b++) {
        for (int i = 0; i < 64; i++) { const double x = bq_run(&q2, bq_run(&q1, noise() * 3.0e6)); X[i] = L[i] = R[i] = (int32_t)x; }
        pa_spread_process(&s, L, R, 64, width);
        if (b >= 100) for (int i = 0; i < 64; i++) {
            sl += (double)L[i] * L[i]; sr += (double)R[i] * R[i]; sx += (double)X[i] * X[i]; slr += (double)L[i] * R[i];
            const double m = 0.5 * ((double)L[i] + R[i]); sm += m * m;
        }
    }
    res_t r;
    r.bal_db = 10 * log10(sr / sl);
    r.level_db = 10 * log10(sl / sx);
    r.corr = slr / sqrt(sl * sr);
    r.mono_db = 10 * log10(sm / sx);
    return r;
}

int main(void)
{
    /* balance and level over the spectra a guitar signal has: bass-heavy, mid, bright, and a cabinet's band */
    static const struct { double f0, Q; const char *name; } spec[] = {
        { 150, 0.8, "low heavy (150 Hz)" }, { 600, 0.8, "mid (600 Hz)" }, { 2500, 0.8, "presence (2.5 kHz)" }, { 5000, 1.2, "bright (5 kHz)" }, { 1000, 0.3, "wide (1 kHz, Q 0.3)" } };
    for (unsigned k = 0; k < sizeof spec / sizeof spec[0]; k++) {
        for (int w = 30; w <= 100; w += 35) {
            const res_t r = run(spec[k].f0, spec[k].Q, (uint8_t)w);
            CHECK(fabs(r.bal_db) < 0.3, "%s width %d: right vs left %.2f dB (must be within 0.3)", spec[k].name, w, r.bal_db);
            CHECK(fabs(r.level_db) < 0.5, "%s width %d: each channel vs input %.2f dB (must be within 0.5)", spec[k].name, w, r.level_db);
        }
    }

    /* the correlation falls with the width; at 100 the two channels are uncorrelated */
    {
        const res_t a = run(800, 0.5, 30), b = run(800, 0.5, 60), c = run(800, 0.5, 100);
        CHECK(a.corr > b.corr && b.corr > c.corr, "correlation falls with the width (%.2f, %.2f, %.2f)", a.corr, b.corr, c.corr);
        CHECK(fabs(c.corr) < 0.2, "uncorrelated at width 100 (%.2f)", c.corr);
        CHECK(b.corr < 0.8 && b.corr > 0.2, "width 60 is clearly wider but not detached (%.2f)", b.corr);
        /* folded to mono the level is what the decorrelation costs (1/(1+k^2)) and no more: no comb-filter hole */
        CHECK(fabs(b.mono_db - 10 * log10(1.0 / 1.36)) < 0.7, "mono sum at width 60 is %.2f dB (expected %.2f)", b.mono_db, 10 * log10(1.0 / 1.36));
        CHECK(fabs(c.mono_db - (-3.0)) < 0.7, "mono sum at width 100 is %.2f dB (expected -3.0)", c.mono_db);
    }

    /* a real stereo input: its side signal S = (L-R)/2 passes through untouched */
    {
        pa_spread_t s; memset(&s, 0, sizeof s);
        int32_t L[64], R[64];
        double ds = 0, dd = 0;
        for (int b = 0; b < 200; b++) {
            int32_t S0[64];
            for (int i = 0; i < 64; i++) { const double t = b * 64 + i; const int32_t m = (int32_t)(3.0e6 * sin(t * 0.03)); const int32_t sd = (int32_t)(1.0e6 * sin(t * 0.011 + 0.4)); L[i] = m + sd; R[i] = m - sd; S0[i] = sd; }
            pa_spread_process(&s, L, R, 64, 60);
            if (b >= 50) for (int i = 0; i < 64; i++) {
                const double so = 0.5 * ((double)L[i] - R[i]);                  /* L'-R' = 2S + 2g k u: the side is S plus the generated part */
                ds += (double)S0[i] * S0[i]; dd += so * so;
            }
        }
        CHECK(dd > ds * 0.9, "a stereo input keeps its own difference (%.3g -> %.3g)", ds, dd);
    }

    /* silence stays silence; a full-scale square wave and a burst of garbage stay bounded and recover */
    {
        pa_spread_t s; memset(&s, 0, sizeof s);
        int32_t L[64], R[64];
        for (int i = 0; i < 64; i++) { L[i] = R[i] = 0; }
        pa_spread_process(&s, L, R, 64, 100);
        int z = 1; for (int i = 0; i < 64; i++) if (L[i] || R[i]) z = 0;
        CHECK(z, "silence in, silence out");
        long peak = 0;
        for (int b = 0; b < 100; b++) {
            for (int i = 0; i < 64; i++) L[i] = R[i] = ((i / 8) & 1) ? 8388607 : -8388608;
            pa_spread_process(&s, L, R, 64, 100);
            for (int i = 0; i < 64; i++) { if (labs(L[i]) > peak) peak = labs(L[i]); if (labs(R[i]) > peak) peak = labs(R[i]); }
        }
        CHECK(peak < 4L * 8388608L, "a full-scale square stays bounded (peak %ld)", peak);
        for (int b = 0; b < 600; b++) { for (int i = 0; i < 64; i++) L[i] = R[i] = 0; pa_spread_process(&s, L, R, 64, 100); }
        z = 1; for (int i = 0; i < 64; i++) if (labs(L[i]) > 4 || labs(R[i]) > 4) z = 0;
        CHECK(z, "it rings out to silence after the loud input");
    }

    printf(fails ? "RESULT: FAILED (%d)\n" : "RESULT: ALL PASSED\n", fails);
    return fails ? 1 : 0;
}
