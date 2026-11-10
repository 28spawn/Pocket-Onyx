/* SPDX-License-Identifier: GPL-3.0-only */
/* Copyright (C) 2026 The Onyx Authors (@28spawn) */
/* test/test_usb_capture_tone.c - what the host records over USB is a clean copy of what the amp outputs, for as long as it runs.
 *
 * A 1 kHz sine from the codec side (64-frame blocks at 44,118 Hz, as the amp's audio interrupt produces them) goes through pa_usb_audio_push_capture_stereo / pa_usb_audio_ep3_in_fill
 * (src/board/usb_ble.c) read once per 1 ms host frame, for 12 seconds. The packets' 24-bit samples are cut into 10 ms windows (441 samples = ten whole cycles at 44.1 kHz); in each window
 * a 1 kHz sine and cosine are fitted and the rest is the error. Required after the first 2 seconds: every window within 1 dB of the input level and its error at least 45 dB below the tone (measured: ~60 dB; the 16-bit-position bug: -3 dB).
 *
 * Why it exists: the read position was one Q16 word, whose whole-frame part had 16 bits, so after 65,536 frames (~1.5 s) every packet re-centred and repeated or skipped frames: the host
 * recorded robotic, metallic audio (pack 161, found 2026-11-03 by simulating the path on the PC); the older test ran a few milliseconds and passed. The window after 1.5 s is the one that counts. */
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <math.h>
#include "pa_usb_ble.h"
#include "pa_config.h"
#include "pa_ota.h"
#include "pa_sysex.h"
#include "pa_diag.h"
void pa_ble_midi_tx_stream(pa_ble_emit_t emit, const uint8_t *msg, uint32_t len, uint32_t mtu) { (void)emit; (void)msg; (void)len; (void)mtu; }
uint32_t pa_ble_midi_unframe(const uint8_t *pkt, uint32_t len, uint8_t *out, uint32_t cap, uint8_t *state, uint8_t *in_sysex) { (void)pkt; (void)len; (void)out; (void)cap; (void)state; (void)in_sysex; return 0; }
void pa_sysex_set_reply_sink(pa_sysex_reply_sink_t sink) { (void)sink; }
void pa_sysex_input_byte(uint8_t b) { (void)b; }
void pa_diag_set_reply_sink(pa_diag_reply_fn fn) { (void)fn; }
void pa_diag_feed(uint8_t b) { (void)b; }
pa_ota_frame_t pa_ota_classify(const uint8_t *frame, uint32_t len) { (void)frame; (void)len; return PA_OTA_FRAME_NONE; }
uint32_t pa_ota_build_id_reply(uint8_t *out, uint32_t cap) { (void)out; (void)cap; return 0; }

static int fails;
#define CHECK(ok, ...) do { printf((ok) ? "  ok   " : "  FAIL "); printf(__VA_ARGS__); printf("\n"); if (!(ok)) fails++; } while (0)
static int32_t s24(const uint8_t *p) { return (int32_t)(((uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16)) << 8) >> 8; }

#define SECS 12
#define MAXS (SECS * 46000)
static double g_out[MAXS];

static void run(double codec_hz, const char *label) {
    static pa_usb_ble_ctx_t ctx;
    memset(&ctx, 0, sizeof ctx);
    pa_usb_audio_set_rate(&ctx, 44100u);
    const double dphi = 2.0 * M_PI * 1000.0 / codec_hz, blk_s = 64.0 / codec_hz;
    double phase = 0.0, t_prod = 0.0, t_cons = 0.0005;
    uint8_t pkt[300];
    size_t n = 0;
    while (t_cons < SECS) {
        if (t_prod <= t_cons) {
            int32_t L[64], R[64];
            for (int i = 0; i < 64; i++) { L[i] = R[i] = (int32_t)(0.25 * 2097152.0 * sin(phase)); phase += dphi; }
            pa_usb_audio_push_capture_stereo(&ctx, L, R, 64);
            t_prod += blk_s;
        } else {
            const uint32_t bytes = pa_usb_audio_ep3_in_fill(&ctx, pkt, sizeof pkt);
            for (uint32_t i = 0; i + 6 <= bytes && n < MAXS; i += 6) g_out[n++] = s24(&pkt[i]) / 8388608.0;
            t_cons += 0.001;
        }
    }
    /* windows of 441 samples after the first 2 s; each fits cos, sin and their first-order drift terms (t*cos, t*sin: a small constant pitch offset is not an error, a wobble is) */
    double worst_snr = 1e9, worst_lvl_err = 0.0;
    size_t windows = 0;
    for (size_t w0 = 2u * 44100u; w0 + 441u <= n; w0 += 441u) {
        double A[4][5] = {{0}};
        for (size_t i = 0; i < 441; i++) {
            const double ph = 2.0 * M_PI * 1000.0 * (double)i / 44100.0, tt = ((double)i - 220.0) / 220.0;
            const double bv[4] = { cos(ph), sin(ph), tt * cos(ph), tt * sin(ph) };
            for (int r = 0; r < 4; r++) { for (int c = 0; c < 4; c++) A[r][c] += bv[r] * bv[c]; A[r][4] += bv[r] * g_out[w0 + i]; }
        }
        for (int c = 0; c < 4; c++) {                         /* Gauss-Jordan */
            int piv = c; for (int r = c + 1; r < 4; r++) if (fabs(A[r][c]) > fabs(A[piv][c])) piv = r;
            for (int k = 0; k < 5; k++) { const double t = A[c][k]; A[c][k] = A[piv][k]; A[piv][k] = t; }
            for (int r = 0; r < 4; r++) if (r != c) { const double f = A[r][c] / A[c][c]; for (int k = c; k < 5; k++) A[r][k] -= f * A[c][k]; }
        }
        double x[4]; for (int r = 0; r < 4; r++) x[r] = A[r][4] / A[r][r];
        double err = 0;
        for (size_t i = 0; i < 441; i++) {
            const double ph = 2.0 * M_PI * 1000.0 * (double)i / 44100.0, tt = ((double)i - 220.0) / 220.0;
            const double e = g_out[w0 + i] - (x[0] * cos(ph) + x[1] * sin(ph) + x[2] * tt * cos(ph) + x[3] * tt * sin(ph));
            err += e * e;
        }
        const double amp = sqrt(x[0] * x[0] + x[1] * x[1]);
        const double snr = 20.0 * log10(amp / (sqrt(err / 441.0) * sqrt(2.0) + 1e-12));
        const double lvl = 20.0 * log10(amp / 0.25);
        if (snr < worst_snr) worst_snr = snr;
        if (fabs(lvl) > fabs(worst_lvl_err)) worst_lvl_err = lvl;
        windows++;
    }
    CHECK(n > (size_t)(SECS - 1) * 44000u, "%s: %zu samples in %d s of host frames (a frame lost or invented would show here)", label, n, SECS);
    CHECK(windows > 900, "%s: %zu windows measured after the first 2 s", label, windows);
    CHECK(worst_snr >= 45.0, "%s: the worst 10 ms window has the tone %.1f dB above its error (need 45; the broken reader gave -3)", label, worst_snr);
    CHECK(fabs(worst_lvl_err) <= 1.0, "%s: the level stays within %.2f dB of the input", label, worst_lvl_err);
}

int main(void) {
    run(44118.0, "codec 44,118 Hz");
    run(44100.0, "codec 44,100 Hz");
    run(44135.0, "codec 44,135 Hz (+0.08 %)");
    printf("%s\n", fails ? "RESULT: FAILED" : "RESULT: ALL PASSED");
    return fails ? 1 : 0;
}
