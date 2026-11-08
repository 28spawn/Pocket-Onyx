/* SPDX-License-Identifier: GPL-3.0-only */
/* Copyright (C) 2026 The Onyx Authors (@28spawn) */
/* custom-fw/test/test_mix_limiter.c - the limiter on the guitar + drums sum
 *
 * The real drum engine (drums_synth.c) and the real limiter (src/sys/pa_mix_limiter.h),
 * followed by the output's soft ceiling (copied from pa_audio_core.c), as the device runs
 * them: guitar = the chain's output, drums added through the limiter.
 *
 *   [1] drums off, or playing silence: a guitar at the chain's ceiling passes bit-exact
 *   [2] every pattern under a loud preset (-0.2 dBFS, the chain limiter's ceiling) and a
 *       normal one (-6 dBFS), steady and strummed: the output never hard-clips; with a steady
 *       guitar the drums never push the sum into the soft ceiling past where the guitar alone
 *       already was; a strum that jumps from silence to full on a hit (unpredictable) is
 *       caught within 1 ms, less than 1 dB over the knee (the soft ceiling's mild 4:1 part, far from
 *       its 0 dBFS clip); the gain reduction
 *       stays moderate
 *   [3] no latency on the guitar: a click on the guitar comes out on the same sample
 *   [4] after the drums stop the limiter drains and the guitar is bit-exact again
 *
 * clang -O2 -Icustom-fw/include -DPA_HOST_SIM=1 custom-fw/test/test_mix_limiter.c -lm
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>

#include "../src/dsp/drums_synth.c"
#include "../src/sys/pa_mix_limiter.h"

void *pa_alloc(uint32_t n) { return calloc(1, n); }
void  pa_free(void *p)     { free(p); }

static int32_t soft_ceiling(int32_t x) {         /* pa_audio_core.c pa_soft_ceiling_q21 */
    const int32_t knee = 1869214, ceil = PA_Q21_ONE - 1;
    if (x >  knee) { x = knee + ((x - knee) >> 2); if (x >  ceil) x =  ceil; }
    if (x < -knee) { x = -knee - ((-knee - x) >> 2); if (x < -ceil) x = -ceil; }
    return x;
}

static int fails = 0;
#define CHECK(ok, ...) do { printf((ok) ? "  ok   " : "  FAIL "); printf(__VA_ARGS__); printf("\n"); if (!(ok)) fails++; } while (0)

#define FS   44100
#define Q21  2097152.0
#define PI   3.14159265358979
#define KNEE 1869214

/* the guitar: 0 = steady 196 Hz sine, 1 = an E-major-ish chord strummed every 0.5 s,
 * decaying - both peaking at `pk` */
static int32_t guitar(int kind, double pk, int i) {
    double t = (double)i / FS;
    if (kind == 0) return (int32_t)(pk * Q21 * sin(2 * PI * 196.0 * t));
    double tt = fmod(t, 0.5), e = exp(-tt / 0.25);
    double s = (sin(2 * PI * 82.4 * t) + 0.7 * sin(2 * PI * 123.5 * t + 1) + 0.5 * sin(2 * PI * 164.8 * t + 2)) / 2.2;
    return (int32_t)(pk * Q21 * e * s);
}

int main(void) {
    static pa_drums_t d;
    static pa_mixlim_t m;
    int32_t L[PA_BLOCK_N], R[PA_BLOCK_N], D[PA_BLOCK_N];

    printf("[1] drums off / silent: the guitar is untouched\n");
    {
        int diff = 0;
        memset(&m, 0, sizeof m);
        for (int b = 0; b < 400; b++) {
            for (int i = 0; i < PA_BLOCK_N; i++) { L[i] = R[i] = guitar(0, 0.98, b * PA_BLOCK_N + i); D[i] = 0; }
            pa_mixlim_process(&m, L, R, (b & 1) ? D : 0, PA_BLOCK_N);    /* off, and playing silence */
            for (int i = 0; i < PA_BLOCK_N; i++) if (L[i] != guitar(0, 0.98, b * PA_BLOCK_N + i) || R[i] != L[i]) diff++;
        }
        CHECK(diff == 0, "a -0.2 dBFS guitar passes bit-exact (%d samples changed)", diff);
    }

    printf("[2] every pattern, 2 loops at 120 BPM, LEVEL 100 %%, under four guitars\n");
    const char *gname[4] = { "-0.2 dBFS steady", "-0.2 dBFS strum", "-6 dBFS steady", "-6 dBFS strum" };
    const double gpk[4] = { 0.977, 0.977, 0.501, 0.501 };
    for (int gi = 0; gi < 4; gi++) {
        int hard = 0, pushed = 0, hard_old = 0, onsets = 0, run = 0, worst_run = 0;
        double worst_over = 0;
        double worst_gr = 0, sum_gr = 0; int wg = 0;
        for (int st = 0; st < PA_DRUM_STYLE_COUNT; st++) {
            pa_drums_init(&d);
            pa_drums_set_level(&d, 100);
            pa_drums_set_bpm(&d, 120);
            pa_drums_set_style(&d, (uint8_t)st);
            pa_drums_set_playing(&d, 1);
            memset(&m, 0, sizeof m);
            int nb = (int)(2.0 * pa_drums_style_length((uint8_t)st) * (220500.0 / 120)) / PA_BLOCK_N;
            if (gi & 1) onsets += (int)(nb * PA_BLOCK_N / (0.5 * FS)) + 1;
            for (int b = 0; b < nb; b++) {
                for (int i = 0; i < PA_BLOCK_N; i++) { L[i] = R[i] = guitar(gi & 1, gpk[gi], b * PA_BLOCK_N + i); D[i] = 0; }
                pa_drums_mix_block(&d, D, PA_BLOCK_N);
                int32_t G[PA_BLOCK_N];
                memcpy(G, L, sizeof G);
                /* the old output stage, for comparison: plain sum into the soft ceiling */
                for (int i = 0; i < PA_BLOCK_N; i++) {
                    int64_t s = (int64_t)G[i] + D[i];
                    if (s > INT32_MAX) s = INT32_MAX;
                    if (s < INT32_MIN) s = INT32_MIN;
                    int32_t o = soft_ceiling((int32_t)s);
                    if (o >= PA_Q21_ONE - 1 || o <= -(PA_Q21_ONE - 1)) hard_old++;
                }
                pa_mixlim_process(&m, L, R, D, PA_BLOCK_N);
                for (int i = 0; i < PA_BLOCK_N; i++) {
                    int32_t a = L[i] < 0 ? -L[i] : L[i], ga = G[i] < 0 ? -G[i] : G[i];
                    int32_t lim = ga > KNEE ? ga : KNEE;
                    if (a > lim + lim / 100) {                            /* drums pushed it over */
                        pushed++;
                        if (++run > worst_run) worst_run = run;
                        double over = 20 * log10((double)a / lim);
                        if (over > worst_over) worst_over = over;
                    } else run = 0;
                    int32_t o = soft_ceiling(L[i]);
                    if (o >= PA_Q21_ONE - 1 || o <= -(PA_Q21_ONE - 1)) hard++;
                }
            }
            double gr = -20 * log10(m.gmin);
            sum_gr += gr;
            if (gr > worst_gr) { worst_gr = gr; wg = st; }
        }
        printf("     guitar %-17s 0 dBFS clips: %5d -> %d | over the knee: %d samples, longest %d, at most %.2f dB |"
               " gain reduction avg %.1f dB, most %.1f dB (%s)\n",
               gname[gi], hard_old, hard, pushed, worst_run, worst_over, sum_gr / PA_DRUM_STYLE_COUNT, worst_gr,
               pa_drums_get_style_name((uint8_t)wg));
        CHECK(hard == 0, "guitar %s: the output never hard-clips (it did %d times)", gname[gi], hard_old);
        if (gi & 1)   /* a strum is a step from silence to full: unpredictable, caught within a chunk */
            CHECK(worst_run <= FS / 1000 && worst_over < 1.0,
                  "guitar %s: when a strum lands on a hit the sum is over the knee for at most 1 ms, < 1 dB"
                  " (%d samples, %.2f dB; %.1f samples per strum on average)", gname[gi], worst_run, worst_over,
                  (double)pushed / onsets);
        else
            CHECK(pushed == 0, "guitar %s: the drums never push the sum into the soft ceiling (%d samples)", gname[gi], pushed);
        CHECK(worst_gr < (gi < 2 ? 6.5 : 3.5), "guitar %s: gain reduction at most %.1f dB", gname[gi], worst_gr);
    }

    printf("[3] no latency on the guitar\n");
    {
        pa_drums_init(&d); pa_drums_set_level(&d, 100); pa_drums_set_playing(&d, 1);
        memset(&m, 0, sizeof m);
        int at = -1;
        for (int b = 0; b < 20 && at < 0; b++) {
            for (int i = 0; i < PA_BLOCK_N; i++) { L[i] = R[i] = 0; D[i] = 0; }
            if (b == 10) L[5] = R[5] = (int32_t)(0.3 * Q21);
            pa_drums_mix_block(&d, D, PA_BLOCK_N);
            for (int i = 0; i < PA_BLOCK_N; i++) D[i] = 0;          /* the guitar alone */
            pa_mixlim_process(&m, L, R, D, PA_BLOCK_N);
            for (int i = 0; i < PA_BLOCK_N; i++) if (L[i] > (int32_t)(0.2 * Q21)) at = b * PA_BLOCK_N + i;
        }
        CHECK(at == 10 * PA_BLOCK_N + 5, "a guitar click comes out on the sample it went in (%d, want %d)", at, 10 * PA_BLOCK_N + 5);
    }

    printf("[4] the drums stop: the limiter drains and the guitar is bit-exact again\n");
    {
        pa_drums_init(&d); pa_drums_set_level(&d, 100); pa_drums_set_style(&d, 12); pa_drums_set_playing(&d, 1);
        memset(&m, 0, sizeof m);
        int b = 0, idle_at = -1, diff_after = 0;
        for (; b < 2000; b++) {
            for (int i = 0; i < PA_BLOCK_N; i++) { L[i] = R[i] = guitar(0, 0.98, b * PA_BLOCK_N + i); D[i] = 0; }
            int playing = b < 400;
            if (playing) pa_drums_mix_block(&d, D, PA_BLOCK_N);
            pa_mixlim_process(&m, L, R, playing ? D : 0, PA_BLOCK_N);
            if (!playing && idle_at < 0 && pa_mixlim_idle(&m)) idle_at = b;
            if (idle_at >= 0 && b > idle_at)
                for (int i = 0; i < PA_BLOCK_N; i++) if (L[i] != guitar(0, 0.98, b * PA_BLOCK_N + i)) diff_after++;
        }
        double ms = idle_at < 0 ? -1 : (idle_at - 400) * PA_BLOCK_N * 1000.0 / FS;
        CHECK(idle_at >= 0 && ms < 1500 && diff_after == 0, "idle %.0f ms after the stop, then bit-exact (%d samples changed)", ms, diff_after);
    }

    printf("\n%s\n", fails ? "FAILED" : "MIX LIMITER OK");
    return fails ? 1 : 0;
}
