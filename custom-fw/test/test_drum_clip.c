/* SPDX-License-Identifier: GPL-3.0-only */
/* Copyright (C) 2026 The Onyx Authors (@28spawn) */
/* custom-fw/test/test_drum_clip.c - does any Groove Station voice clip or distort?
 *
 * The real engine (drums_synth.c is included, so the test can trigger one voice on its own)
 * and the real output stage (pa_soft_ceiling_q21, copied from pa_audio_core.c below).
 *
 *   [1] each voice alone at full velocity: its peak at the drum bus and at the output, and
 *       how far the engine's output is from a clean, scaled copy of the recording
 *       (distortion added by the engine, in dB under the signal)
 *   [2] every groove and METRO pattern, 2 loops at 120 BPM, LEVEL 100 %: the drums never
 *       go over their ceiling, the bus limiter never takes more than 6 dB, and mixed with a
 *       guitar at -6 dBFS the output never reaches its 0 dBFS clip (a guitar at -1 dBFS,
 *       a preset at the chain's limiter ceiling, is reported)
 *
 *   --report  print only, never fail
 *
 * clang -O2 -Icustom-fw/include -DPA_HOST_SIM=1 custom-fw/test/test_drum_clip.c -lm
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>

#include "../src/dsp/drums_synth.c"

void *pa_alloc(uint32_t n) { return calloc(1, n); }
void  pa_free(void *p)     { free(p); }

/* pa_audio_core.c's output guard (kept in step by hand: the test checks the same law) */
static int32_t soft_ceiling(int32_t x) {
    const int32_t knee = 1869214, ceil = PA_Q21_ONE - 1;
    if (x >  knee) { x = knee + ((x - knee) >> 2); if (x >  ceil) x =  ceil; }
    if (x < -knee) { x = -knee - ((-knee - x) >> 2); if (x < -ceil) x = -ceil; }
    return x;
}

static int fails = 0, strict = 1;
#define CHECK(ok, ...) do { printf(!strict ? "  info " : (ok) ? "  ok   " : "  FAIL "); printf(__VA_ARGS__); printf("\n"); if (strict && !(ok)) fails++; } while (0)

#define Q21 2097152.0
static const char *k_vname[PA_DRUM_VOICE_COUNT] = {
    "KICK", "SNARE", "RIM", "HAT", "PEDAL", "OPEN", "CRASH", "RIDE", "BELL",
    "TOM LO", "TOM MID", "TOM HI", "CLICK" };

/* every voice decoded once (the kit is stored compressed / synthesised), then read like a recording */
static float g_ref[PA_DRUM_VOICE_COUNT][30000];

/* the clean read of one voice, one value per bus tick (22.05 kHz): the velocity and the trim, nothing after it */
static double clean_voice(uint32_t v, uint32_t t, int32_t vel) {
    if (t >= k_voice[v].frames) return 0;
    return g_ref[v][t] * 64.0 * (((int64_t)vel * k_voice[v].trim_q15) >> 15) / 32768.0 / Q21;
}

/* the engine's output stage on a clean bus: an even output is the previous bus sample, an odd one the mean of the previous and the new */
static double up_out(const double *bt, int j) {
    if ((j & 1) == 0) return j ? bt[j / 2 - 1] : 0.0;
    const int p = (j - 1) / 2;
    return 0.5 * ((p ? bt[p - 1] : 0.0) + bt[p]);
}

/* distortion (dB under the signal) of y against the best scaled copy of x; y comes out
 * PA_DRUM_LIM_LOOKAHEAD bus samples (two outputs each) late: the bus limiter's look-ahead */
#define LAG (2 * (int)PA_DRUM_LIM_LOOKAHEAD)
static double dist_db(const double *x, const double *y, int n) {
    double xy = 0, xx = 0;
    n -= LAG;
    for (int i = 0; i < n; i++) { xy += x[i] * y[i + LAG]; xx += x[i] * x[i]; }
    double g = xx > 0 ? xy / xx : 0, e = 0, yy = 0;
    for (int i = 0; i < n; i++) { double d = y[i + LAG] - g * x[i]; e += d * d; yy += y[i + LAG] * y[i + LAG]; }
    return 10 * log10((e + 1e-30) / (yy + 1e-30));
}

#define NMAX (44100 * 30)
static double X[NMAX], Y[NMAX], BT[NMAX];
static int32_t B[NMAX];

int main(int argc, char **argv) {
    for (int i = 1; i < argc; i++) if (!strcmp(argv[i], "--report")) strict = 0;
    static pa_drums_t d;
    for (uint32_t v = 0; v < PA_DRUM_VOICE_COUNT; v++) pa_drums_voice_decode(v, g_ref[v], 30000u);

    printf("[1] each voice alone, full velocity, LEVEL 100 %%\n");
    printf("     voice     bus peak   out peak   engine distortion\n");
    double worst_v = -200; int worst_vi = 0;
    for (uint32_t v = 0; v < PA_DRUM_VOICE_COUNT; v++) {
        pa_drums_init(&d);
        pa_drums_set_level(&d, 100);
        d.playing = 1;
        d.step_len_q16 = 0xFFFFFFFFu; d.step_pos_q16 = 0;      /* no pattern steps: we play it */
        d.style = 0;
        const int32_t vel = k_vel_tab[15];
        trigger_voice(&d, v, vel);
        d.target_alpha_q15 = d.lpf_alpha_q15 = LPF_ALPHA_MIN + 15 * LPF_ALPHA_STEP;
        int n = (int)(2u * k_voice[v].frames + 64 + LAG); n -= n % 64;
        double pk_bus = 0;
        for (int t = 0; t < n / 2; t++) { BT[t] = clean_voice(v, (uint32_t)t, vel); if (fabs(BT[t]) > pk_bus) pk_bus = fabs(BT[t]); }
        for (int i = 0; i < n; i++) X[i] = up_out(BT, i);
        memset(B, 0, sizeof(int32_t) * n);
        for (int i = 0; i < n; i += 64) pa_drums_mix_block(&d, B + i, 64);
        double pk_out = 0;
        for (int i = 0; i < n; i++) { Y[i] = B[i] / Q21; if (fabs(Y[i]) > pk_out) pk_out = fabs(Y[i]); }
        double dd = dist_db(X, Y, n);
        printf("     %-8s  %5.1f dBFS %5.1f dBFS  %6.1f dB\n", k_vname[v], 20 * log10(pk_bus), 20 * log10(pk_out), dd);
        if (dd > worst_v) { worst_v = dd; worst_vi = (int)v; }
    }
    CHECK(worst_v < -60, "no voice is distorted by the engine (worst: %s, %.1f dB under the signal)", k_vname[worst_vi], worst_v);

    printf("[2] every pattern, 2 loops at 120 BPM, LEVEL 100 %%\n");
    printf("     (clean = the voices simply summed; the bus limiter holds the sum to -1 dBFS before\n"
           "      LEVEL, -4.1 dBFS after it; then the output sum with a guitar at -6 / -1 dBFS)\n");
    const double ceil_out = 0.89125 * (0.7 * PA_Q21_ONE) / Q21;     /* -1 dBFS x LEVEL 100 % */
    int overs = 0, deep = 0;
    double worst_pk = 0, worst_gr = 0; int wp = 0, wg = 0;
    int hard6_total = 0, hard1_pat = 0;
    for (int st = 0; st < PA_DRUM_STYLE_COUNT; st++) {
        pa_drums_init(&d);
        pa_drums_set_level(&d, 100);
        pa_drums_set_bpm(&d, 120);
        pa_drums_set_style(&d, (uint8_t)st);
        pa_drums_set_playing(&d, 1);
        int n = (int)(2.0 * pa_drums_style_length((uint8_t)st) * (220500.0 / 120)); n -= n % 64;
        if (n > NMAX) n = NMAX;
        /* the clean sum of the voices, on the same clock */
        uint32_t spos = d.step_pos_q16; uint16_t sstep = 0;
        uint32_t vpos[PA_DRUM_VOICE_COUNT]; int32_t vvel[PA_DRUM_VOICE_COUNT];
        for (uint32_t v = 0; v < PA_DRUM_VOICE_COUNT; v++) { vpos[v] = k_voice[v].frames; vvel[v] = 0; }
        uint16_t len = k_groove_len[st];
        double pk_clean = 0;
        for (int i = 0; i < n / 2; i++) {                              /* a bus tick is two output samples */
            spos += 0x20000u;
            if (spos >= d.step_len_q16) {
                spos -= d.step_len_q16;
                uint32_t w = k_groove_steps[k_groove_off[st] + sstep];
                uint32_t lanes = w & PA_DRUM_L_MASK;
                int32_t vel = k_vel_tab[(w >> PA_DRUM_VEL_SHIFT) & PA_DRUM_VEL_MASK];
                if (lanes) {
                    if (st >= (int)PA_DRUM_GROOVE_STYLE_COUNT) { vpos[PA_DRUM_V_CLICK] = 0; vvel[PA_DRUM_V_CLICK] = vel; }
                    else {
                        for (uint32_t v = 0; v <= PA_DRUM_V_TOM_HI; v++) if (lanes & (1u << v)) { vpos[v] = 0; vvel[v] = vel; }
                        if ((lanes & (PA_DRUM_L_HAT | PA_DRUM_L_PEDAL)) && !(lanes & PA_DRUM_L_OPEN)) vpos[PA_DRUM_V_OPEN] = k_voice[PA_DRUM_V_OPEN].frames;
                    }
                }
                sstep = (uint16_t)((sstep + 1u) % len);
            }
            double s = 0;
            for (uint32_t v = 0; v < PA_DRUM_VOICE_COUNT; v++)
                if (vpos[v] < k_voice[v].frames) { s += clean_voice(v, vpos[v], vvel[v]); vpos[v]++; }
            if (fabs(s) > pk_clean) pk_clean = fabs(s);
        }
        /* the engine, 8 samples at a time so the limiter's lowest gain is seen */
        memset(B, 0, sizeof(int32_t) * n);
        float gmin = 1.0f;
        for (int i = 0; i < n; i += 8) { pa_drums_mix_block(&d, B + i, 8); if (d.lim_gain < gmin) gmin = d.lim_gain; }
        double pk = 0; int hard6 = 0, hard1 = 0;
        for (int i = 0; i < n; i++) {
            double y = B[i] / Q21;
            if (fabs(y) > pk) pk = fabs(y);
            for (int gl = 0; gl < 2; gl++) {
                double ga = gl ? 0.891 : 0.501;
                int64_t sum = (int64_t)(ga * Q21 * sin(2 * 3.14159265 * 196.0 * i / 44100.0)) + B[i];
                if (sum > INT32_MAX) sum = INT32_MAX;
                if (sum < INT32_MIN) sum = INT32_MIN;
                int32_t o = soft_ceiling((int32_t)sum);
                if (o >= PA_Q21_ONE - 1 || o <= -(PA_Q21_ONE - 1)) { if (gl) hard1++; else hard6++; }
            }
        }
        double gr = -20 * log10(gmin);
        printf("     %2d %-14s clean %5.1f -> out %5.1f dBFS, limiter %4.1f dB | 0 dBFS clips: %d (-6 dBFS guitar), %d (-1)\n",
               st + 1, pa_drums_get_style_name((uint8_t)st), 20 * log10(pk_clean), 20 * log10(pk), gr, hard6, hard1);
        if (pk > ceil_out * 1.002) overs++;
        if (gr > 6.0) deep++;
        if (pk > worst_pk) { worst_pk = pk; wp = st; }
        if (gr > worst_gr) { worst_gr = gr; wg = st; }
        hard6_total += hard6;
        if (hard1) hard1_pat++;
    }
    CHECK(!overs, "the drums never go over their ceiling (%d patterns; loudest %s at %.2f dBFS, ceiling %.2f)",
          overs, pa_drums_get_style_name((uint8_t)wp), 20 * log10(worst_pk), 20 * log10(ceil_out));
    CHECK(!deep, "the limiter never has to take more than 6 dB (most: %s, %.1f dB)", pa_drums_get_style_name((uint8_t)wg), worst_gr);
    CHECK(!hard6_total, "with a guitar at -6 dBFS the output never hard-clips (%d samples)", hard6_total);
    printf("  info with a guitar at -1 dBFS (a loud preset at the chain's ceiling), %d patterns reach the output's\n"
           "       0 dBFS clip in a PLAIN sum - the device sums through src/sys/pa_mix_limiter.h\n"
           "       instead (measured by test_mix_limiter.c)\n", hard1_pat);

    printf("\n%s\n", fails ? "FAILED" : "DRUM CLIP OK");
    return strict && fails ? 1 : 0;
}
