/* SPDX-License-Identifier: GPL-3.0-only */
/* Copyright (C) 2026 The Onyx Authors (@28spawn) */
/* custom-fw/test/test_drum_codecs.c - the drum kit's decoders and synthesisers (src/dsp/drum_codec.h)
 *
 *   [1] G.711 mu-law: every code decodes to the standard value, and a round trip stays within the format's step
 *   [2] IMA-ADPCM: the decoder follows an encoder written here step for step (exactly), and a sine comes back clean
 *   [3] the synthesised kick and toms: peak at the kit's level, silent at both ends, a falling pitch, the same every time
 *   [4] the engine plays a voice the same way however the audio is cut into blocks (the decoders keep their place)
 *
 * clang -O2 -Icustom-fw/include -DPA_HOST_SIM=1 custom-fw/test/test_drum_codecs.c -lm
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>

#include "../src/dsp/drums_synth.c"

void *pa_alloc(uint32_t n) { return calloc(1, n); }
void  pa_free(void *p)     { free(p); }

static int fails = 0;
#define CHECK(ok, ...) do { printf((ok) ? "  ok   " : "  FAIL "); printf(__VA_ARGS__); printf("\n"); if (!(ok)) fails++; } while (0)

/* G.711 mu-law encoder (the standard one) */
static uint8_t ulaw_enc(int s) {
    const int sign = (s < 0) ? 0x80 : 0;
    if (s < 0) s = -s;
    if (s > 32635) s = 32635;
    s += 0x84;
    int e = 7;
    for (int mask = 0x4000; e > 0 && !(s & mask); e--, mask >>= 1) {}
    return (uint8_t)~(sign | (e << 4) | ((s >> (e + 3)) & 15));
}

static void test_ulaw(void) {
    printf("[1] mu-law\n");
    /* the standard's anchor values */
    uint8_t one[1];
    pa_dsrc_t st;
    float v;
    one[0] = 0xFF; pa_dsrc_start(&st, PA_DK_ULAW8, one); v = pa_dsrc_next(&st, PA_DK_ULAW8, one, 1);
    CHECK(v == 0.0f, "0xFF is zero (%g)", v);
    one[0] = 0x80; pa_dsrc_start(&st, PA_DK_ULAW8, one); v = pa_dsrc_next(&st, PA_DK_ULAW8, one, 1);
    CHECK(v == 32124.0f, "0x80 is +32124 (%g)", v);
    one[0] = 0x00; pa_dsrc_start(&st, PA_DK_ULAW8, one); v = pa_dsrc_next(&st, PA_DK_ULAW8, one, 1);
    CHECK(v == -32124.0f, "0x00 is -32124 (%g)", v);
    /* the round trip: within one quantisation step of the input (a step is 1/16 of the segment's width, so under 4.5 % + the bias) */
    double worst = 0;
    for (int x = -32768; x <= 32767; x += 7) {
        uint8_t b = ulaw_enc(x);
        pa_dsrc_start(&st, PA_DK_ULAW8, &b);
        float y = pa_dsrc_next(&st, PA_DK_ULAW8, &b, 1);
        const int xc = x > 32635 ? 32635 : (x < -32635 ? -32635 : x);
        double err = fabs(y - xc), lim = 16.0 + 0.0313 * abs(xc);
        if (err / lim > worst) worst = err / lim;
    }
    CHECK(worst <= 1.0, "a round trip stays inside the format's step (worst %.2f of the allowance)", worst);
}

/* IMA-ADPCM encoder, mirrors tools/gen_drum_kit.py */
static void adpcm_enc(const int16_t *x, int n, uint8_t *out, int16_t *recon) {
    int pred = 0, idx = 0;
    memset(out, 0, (size_t)(n + 1) / 2);
    for (int i = 0; i < n; i++) {
        int step = k_ima_step[idx], diff = x[i] - pred, code = 0;
        if (diff < 0) { code = 8; diff = -diff; }
        if (diff >= step) { code |= 4; diff -= step; }
        if (diff >= (step >> 1)) { code |= 2; diff -= step >> 1; }
        if (diff >= (step >> 2)) code |= 1;
        int vp = step >> 3;
        if (code & 4) vp += step;
        if (code & 2) vp += step >> 1;
        if (code & 1) vp += step >> 2;
        pred += (code & 8) ? -vp : vp;
        if (pred > 32767) pred = 32767; else if (pred < -32768) pred = -32768;
        idx += k_ima_idx[code & 7];
        if (idx < 0) idx = 0; else if (idx > 88) idx = 88;
        recon[i] = (int16_t)pred;
        out[i >> 1] |= (uint8_t)(code << ((i & 1) * 4));
    }
}

static void test_adpcm(void) {
    printf("[2] IMA-ADPCM\n");
    enum { N = 6001 };                                  /* odd: the last byte holds one sample */
    static int16_t x[N], rc[N];
    static uint8_t buf[(N + 1) / 2];
    for (int i = 0; i < N; i++) x[i] = (int16_t)(12000.0 * sin(0.03 * i + 1e-5 * i * i) * exp(-i / 4000.0));
    adpcm_enc(x, N, buf, rc);
    pa_dsrc_t st;
    pa_dsrc_start(&st, PA_DK_ADPCM4, buf);
    int mism = 0;
    double es = 0, ee = 0;
    for (int i = 0; i < N; i++) {
        float y = pa_dsrc_next(&st, PA_DK_ADPCM4, buf, N);
        if (y != (float)rc[i]) mism++;
        es += (double)x[i] * x[i]; ee += ((double)x[i] - y) * ((double)x[i] - y);
    }
    CHECK(mism == 0, "the decoder follows the encoder exactly (%d differences in %d samples)", mism, N);
    const double snr = 10 * log10(es / (ee + 1e-9));
    CHECK(snr > 25.0, "a decaying chirp comes back at %.1f dB SNR", snr);
}

static int crossings(const float *x, int a, int b) {
    int c = 0;
    for (int i = a + 1; i < b; i++) if ((x[i - 1] < 0) != (x[i] < 0)) c++;
    return c;
}

static void test_synth(void) {
    printf("[3] the synthesised voices\n");
    static const struct { const char *name; uint32_t v; double f_late, f_lo, f_hi; } k[] = {
        { "KICK",    PA_DRUM_V_KICK,    0, 0, 0 },
        { "TOM_LO",  PA_DRUM_V_TOM_LO,  95, 80, 115 },
        { "TOM_MID", PA_DRUM_V_TOM_MID, 130, 110, 160 },
        { "TOM_HI",  PA_DRUM_V_TOM_HI,  175, 150, 215 },
    };
    static float a[30000], b[30000];
    for (size_t i = 0; i < sizeof(k) / sizeof(k[0]); i++) {
        const uint32_t n = pa_drums_voice_decode(k[i].v, a, 30000u);
        pa_drums_voice_decode(k[i].v, b, 30000u);
        float pk = 0, rms_a = 0, rms_b = 0;
        for (uint32_t j = 0; j < n; j++) { if (fabsf(a[j]) > pk) pk = fabsf(a[j]); }
        for (uint32_t j = 0; j < n / 4; j++) rms_a += a[j] * a[j];
        for (uint32_t j = 3 * n / 4; j < n; j++) rms_b += a[j] * a[j];
        CHECK(pk > 28500.0f && pk < 29500.0f, "%s: peak %.0f is at the kit's level (29000)", k[i].name, pk);
        CHECK(fabsf(a[n - 1]) < 1.0f, "%s: ends silent (%g)", k[i].name, a[n - 1]);
        CHECK(rms_b < 0.05f * rms_a, "%s: dies away (last quarter %.1f %% of the first)", k[i].name, 100.0 * sqrt(rms_b / rms_a));
        CHECK(memcmp(a, b, n * sizeof(float)) == 0, "%s: the same every time", k[i].name);
        if (k[i].f_late > 0) {
            /* the pitch settles near the tom's own frequency: count crossings between 40 and 160 ms */
            const int c0 = (int)(0.04 * 22050), c1 = (int)(0.16 * 22050);
            const double f = crossings(a, c0, c1) / 2.0 / ((c1 - c0) / 22050.0);
            CHECK(f > k[i].f_lo && f < k[i].f_hi, "%s: %.0f Hz after the attack (expected about %.0f)", k[i].name, f, k[i].f_late);
        } else {
            const double early = crossings(a, (int)(0.004 * 22050), (int)(0.044 * 22050)) / 2.0 / 0.04;
            const double late  = crossings(a, (int)(0.20 * 22050), (int)(0.40 * 22050)) / 2.0 / 0.20;
            CHECK(early > late + 15.0 && late > 35.0 && late < 80.0, "KICK: pitch falls from %.0f to %.0f Hz", early, late);
        }
    }
}

/* the bus output of one voice played alone, the audio cut into blocks of `blk` samples */
static void play(uint32_t v, int blk, int32_t *out, int n) {
    static pa_drums_t d;
    pa_drums_init(&d);
    pa_drums_set_level(&d, 100);
    d.playing = 1;
    d.step_len_q16 = 0xFFFFFFFFu; d.step_pos_q16 = 0;
    d.style = 0;
    trigger_voice(&d, v, k_vel_tab[12]);
    memset(out, 0, sizeof(int32_t) * (size_t)n);
    for (int i = 0; i < n; i += blk) pa_drums_mix_block(&d, out + i, (n - i < blk) ? n - i : blk);
}

static void test_blocks(void) {
    printf("[4] block size does not change the sound\n");
    enum { NN = 4096 };
    static int32_t r1[NN], r2[NN], r3[NN];
    static const uint32_t voices[] = { PA_DRUM_V_KICK, PA_DRUM_V_SNARE, PA_DRUM_V_HAT, PA_DRUM_V_CRASH, PA_DRUM_V_TOM_MID, PA_DRUM_V_BELL };
    for (size_t i = 0; i < sizeof(voices) / sizeof(voices[0]); i++) {
        play(voices[i], 1, r1, NN);
        play(voices[i], 64, r2, NN);
        play(voices[i], 100, r3, NN);
        int pk = 0;
        for (int j = 0; j < NN; j++) if (abs(r1[j]) > pk) pk = abs(r1[j]);
        CHECK(pk > 1000 && memcmp(r1, r2, sizeof(r1)) == 0 && memcmp(r1, r3, sizeof(r1)) == 0,
              "voice %u: the same samples at 1, 64 and 100 per block (peak %d)", (unsigned)voices[i], pk);
    }
}

int main(void) {
    test_ulaw();
    test_adpcm();
    test_synth();
    test_blocks();
    printf("\n%s\n", fails == 0 ? "DRUM CODECS OK" : "DRUM CODECS FAILED");
    return fails == 0 ? 0 : 1;
}
