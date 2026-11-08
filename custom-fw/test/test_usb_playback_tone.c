/* SPDX-License-Identifier: GPL-3.0-only */
/* Copyright (C) 2026 The Onyx Authors (@28spawn) */
/* test/test_usb_playback_tone.c - the host's audio (playback, host -> unit) reaches the unit's output as a clean copy, and neither direction clicks when its ring runs dry.
 *
 * PLAYBACK: a 1 kHz sine (0.25 of full scale) is sent as the host sends it, 1 ms OUT packets of 44 or 45 frames at 44.1 kHz (pa_usb_audio_ep3_out_rx), and the audio interrupt takes it in
 * 64-frame blocks at the codec's own rate (pa_usb_audio_mix_playback_stereo ADDS it to the output) for 12 seconds. The output, in 10 ms windows after the first 2 s, must be the tone: each
 * window fits cos, sin and their drift terms (a constant pitch offset is not an error) and the error must be at least 45 dB below the tone, the level within 1 dB. Three codec rates: the
 * resampler has to absorb the difference to 44,100 Hz.
 *
 * STARVATION (both directions): 20 ms with no input in the middle of a 6 s run (the audio interrupt late for capture, the host silent for playback). The output must not jump: no sample
 * step larger than 0.12 of full scale anywhere (the tone's own steepest step is 0.036), and the tone must be back afterwards. Before pack 164 the capture reader re-centred on an
 * underrun and repeated ~190 frames, and the playback path did not exist (nothing ever called ep3_out_rx: the host's audio never reached the output).
 */
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
static int32_t s24(const uint8_t *p) { return (int32_t)((uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16)) << 8 >> 8; }

#define SECS 12
#define MAXS (SECS * 46000)
static double g_out[MAXS];

/* fit cos, sin and the drift terms of a 1 kHz tone at sample rate sr over 441-sample windows after 2 s; returns the worst signal-to-error (dB), the largest level error (dB) */
static void analyse(size_t n, double sr, double *worst_snr, double *worst_lvl, size_t *windows) {
    *worst_snr = 1e9; *worst_lvl = 0.0; *windows = 0;
    for (size_t w0 = (size_t)(2.0 * sr); w0 + 441u <= n; w0 += 441u) {
        double A[4][5] = {{0}};
        for (size_t i = 0; i < 441; i++) {
            const double ph = 2.0 * M_PI * 1000.0 * (double)i / sr, tt = ((double)i - 220.0) / 220.0;
            const double bv[4] = { cos(ph), sin(ph), tt * cos(ph), tt * sin(ph) };
            for (int r = 0; r < 4; r++) { for (int c = 0; c < 4; c++) A[r][c] += bv[r] * bv[c]; A[r][4] += bv[r] * g_out[w0 + i]; }
        }
        for (int c = 0; c < 4; c++) {
            int piv = c; for (int r = c + 1; r < 4; r++) if (fabs(A[r][c]) > fabs(A[piv][c])) piv = r;
            for (int k = 0; k < 5; k++) { const double t = A[c][k]; A[c][k] = A[piv][k]; A[piv][k] = t; }
            for (int r = 0; r < 4; r++) if (r != c) { const double f = A[r][c] / A[c][c]; for (int k = c; k < 5; k++) A[r][k] -= f * A[c][k]; }
        }
        double x[4]; for (int r = 0; r < 4; r++) x[r] = A[r][4] / A[r][r];
        double err = 0;
        for (size_t i = 0; i < 441; i++) {
            const double ph = 2.0 * M_PI * 1000.0 * (double)i / sr, tt = ((double)i - 220.0) / 220.0;
            const double e = g_out[w0 + i] - (x[0] * cos(ph) + x[1] * sin(ph) + x[2] * tt * cos(ph) + x[3] * tt * sin(ph));
            err += e * e;
        }
        const double amp = sqrt(x[0] * x[0] + x[1] * x[1]);
        const double snr = 20.0 * log10(amp / (sqrt(err / 441.0) * sqrt(2.0) + 1e-12));
        const double lvl = 20.0 * log10(amp / 0.25);
        if (snr < *worst_snr) *worst_snr = snr;
        if (fabs(lvl) > fabs(*worst_lvl)) *worst_lvl = lvl;
        (*windows)++;
    }
}

/* playback: gap_from..gap_to (s) is a stretch with no host packets */
static size_t run_playback(double codec_hz, double gap_from, double gap_to, double secs) {
    static pa_usb_ble_ctx_t ctx;
    memset(&ctx, 0, sizeof ctx);
    pa_usb_audio_set_rate(&ctx, 44100u);
    pa_usb_audio_playback_start(&ctx);
    const double dphi = 2.0 * M_PI * 1000.0 / 44100.0, blk_s = 64.0 / codec_hz;
    double phase = 0.0, t_host = 0.0, t_blk = blk_s;
    uint32_t acc = 0;
    size_t n = 0;
    while (t_blk < secs) {
        if (t_host <= t_blk) {                                         /* a host frame: 44 or 45 frames of the tone */
            uint32_t frames = 44u; acc += 100u; if (acc >= 1000u) { acc -= 1000u; frames = 45u; }
            uint8_t pkt[6 * 46];
            for (uint32_t i = 0; i < frames; i++) {
                const int32_t v = (int32_t)(0.25 * 8388608.0 * sin(phase)); phase += dphi;
                for (int c = 0; c < 2; c++) { pkt[i * 6 + c * 3] = (uint8_t)v; pkt[i * 6 + c * 3 + 1] = (uint8_t)(v >> 8); pkt[i * 6 + c * 3 + 2] = (uint8_t)(v >> 16); }
            }
            if (!(t_host >= gap_from && t_host < gap_to)) pa_usb_audio_ep3_out_rx(&ctx, pkt, frames * 6u);
            else phase += dphi * frames;                               /* the host's clock goes on while it says nothing */
            t_host += 0.001;
        } else {                                                       /* an audio block */
            int32_t L[64] = {0}, R[64] = {0};
            pa_usb_audio_mix_playback_stereo(&ctx, L, R, 64);
            for (int i = 0; i < 64 && n < MAXS; i++) g_out[n++] = L[i] / 2097152.0;
            t_blk += blk_s;
        }
    }
    return n;
}

/* capture: gap_from..gap_to (s) is a stretch in which the audio interrupt delivers nothing */
static size_t run_capture(double codec_hz, double gap_from, double gap_to, double secs) {
    static pa_usb_ble_ctx_t ctx;
    memset(&ctx, 0, sizeof ctx);
    pa_usb_audio_set_rate(&ctx, 44100u);
    pa_usb_audio_capture_start(&ctx);
    const double dphi = 2.0 * M_PI * 1000.0 / codec_hz, blk_s = 64.0 / codec_hz;
    double phase = 0.0, t_prod = 0.0, t_cons = 0.0005;
    uint8_t pkt[300];
    size_t n = 0;
    while (t_cons < secs) {
        if (t_prod <= t_cons) {
            int32_t L[64], R[64];
            for (int i = 0; i < 64; i++) { L[i] = R[i] = (int32_t)(0.25 * 2097152.0 * sin(phase)); phase += dphi; }
            if (!(t_prod >= gap_from && t_prod < gap_to)) pa_usb_audio_push_capture_stereo(&ctx, L, R, 64);
            t_prod += blk_s;
        } else {
            const uint32_t bytes = pa_usb_audio_ep3_in_fill(&ctx, pkt, sizeof pkt);
            for (uint32_t i = 0; i + 6 <= bytes && n < MAXS; i += 6) g_out[n++] = s24(&pkt[i]) / 8388608.0;
            t_cons += 0.001;
        }
    }
    return n;
}

static double max_step(size_t n, size_t from) {
    double m = 0.0; size_t at = 0;
    for (size_t i = from + 1; i < n; i++) { const double d = fabs(g_out[i] - g_out[i - 1]); if (d > m) { m = d; at = i; } }
    if (getenv("PA_TEST_VERBOSE") && at) { printf("    largest step at sample %zu (%.3f s):", at, (double)at / 44100.0); for (size_t i = at - 4; i < at + 4 && i < n; i++) printf(" %.3f", g_out[i]); printf("\n"); }
    return m;
}

/* playback with a source that changes: amp(t) is the amplitude (fraction of full scale) of the 1 kHz tone the host sends at time t; dither = 1 sends random LSBs of that amplitude instead of a tone */
typedef double (*amp_fn)(double t);
static size_t run_amp(double secs, amp_fn amp, int dither) {
    static pa_usb_ble_ctx_t ctx;
    memset(&ctx, 0, sizeof ctx);
    pa_usb_audio_set_rate(&ctx, 44100u);
    pa_usb_audio_playback_start(&ctx);
    const double dphi = 2.0 * M_PI * 1000.0 / 44100.0, blk_s = 64.0 / 44118.0;
    double phase = 0.0, t_host = 0.0, t_blk = blk_s;
    uint32_t acc = 0, rnd = 12345u;
    size_t n = 0;
    while (t_blk < secs) {
        if (t_host <= t_blk) {
            uint32_t frames = 44u; acc += 100u; if (acc >= 1000u) { acc -= 1000u; frames = 45u; }
            uint8_t pkt[6 * 46];
            for (uint32_t i = 0; i < frames; i++) {
                double x;
                if (dither) { rnd = rnd * 1664525u + 1013904223u; x = ((double)(int32_t)rnd / 2147483648.0) * amp(t_host); }
                else x = amp(t_host) * sin(phase);
                phase += dphi;
                const int32_t v = (int32_t)(x * 8388608.0);
                for (int c = 0; c < 2; c++) { pkt[i * 6 + c * 3] = (uint8_t)v; pkt[i * 6 + c * 3 + 1] = (uint8_t)(v >> 8); pkt[i * 6 + c * 3 + 2] = (uint8_t)(v >> 16); }
            }
            pa_usb_audio_ep3_out_rx(&ctx, pkt, frames * 6u);
            t_host += 0.001;
        } else {
            int32_t L[64] = {0}, R[64] = {0};
            pa_usb_audio_mix_playback_stereo(&ctx, L, R, 64);
            for (int i = 0; i < 64 && n < MAXS; i++) g_out[n++] = L[i] / 2097152.0;
            t_blk += blk_s;
        }
    }
    return n;
}
static double amp_dither(double t) { (void)t; return 3.2e-5; }                                   /* about -90 dBFS of dither, what a host streams when nothing plays */
static double amp_tone_then_silence(double t) { return t < 2.0 ? 0.25 : 0.0; }
static double amp_silence_then_tone(double t) { return t < 2.0 ? 0.0 : 0.25; }
static double peak_between(size_t n, double from_s, double to_s) {
    double m = 0.0; for (size_t i = (size_t)(from_s * 44100.0); i < (size_t)(to_s * 44100.0) && i < n; i++) if (fabs(g_out[i]) > m) m = fabs(g_out[i]);
    return m;
}


int main(void) {
    const double rates[3] = { 44118.0, 44100.0, 44135.0 };
    for (int k = 0; k < 3; k++) {
        double snr, lvl; size_t w;
        const size_t n = run_playback(rates[k], 1e9, 1e9, SECS);
        analyse(n, rates[k], &snr, &lvl, &w);
        CHECK(w > 800, "playback, codec %.0f Hz: %zu windows measured", rates[k], w);
        CHECK(snr >= 45.0, "playback, codec %.0f Hz: the worst 10 ms window has the tone %.1f dB above its error (need 45)", rates[k], snr);
        CHECK(fabs(lvl) <= 1.0, "playback, codec %.0f Hz: the level stays within %.2f dB of what the host sent", rates[k], lvl);
    }
    {   /* starvation */
        size_t n = run_playback(44118.0, 3.0, 3.020, 6.0);
        const double step = max_step(n, 44118);
        CHECK(step <= 0.12, "playback, 20 ms without host packets: the largest step between samples is %.3f (limit 0.12; the tone alone: 0.036)", step);
        double snr, lvl; size_t w;
        analyse(n, 44118.0, &snr, &lvl, &w);
        n = run_playback(44118.0, 3.0, 3.020, 12.0);
        analyse(n, 44118.0, &snr, &lvl, &w);
        (void)snr; (void)lvl; (void)w;
        double tail = 0.0; for (size_t i = n - 4410; i < n; i++) if (fabs(g_out[i]) > tail) tail = fabs(g_out[i]);
        CHECK(tail > 0.20 && tail < 0.30, "playback: the tone is back at the end of the run (peak %.3f, expect ~0.25)", tail);
        n = run_capture(44118.0, 3.0, 3.020, 6.0);
        const double cstep = max_step(n, 44100);
        CHECK(cstep <= 0.12, "capture, 20 ms with no audio from the amp: the largest step between samples is %.3f (limit 0.12)", cstep);
        tail = 0.0; for (size_t i = n - 4410; i < n; i++) if (fabs(g_out[i]) > tail) tail = fabs(g_out[i]);
        CHECK(tail > 0.20 && tail < 0.30, "capture: the tone is back at the end of the run (peak %.3f, expect ~0.25)", tail);
    }
    {   /* the gate on the host's level: Windows streams dither when nothing plays, and it must not reach the output */
        size_t n = run_amp(4.0, amp_dither, 1);
        CHECK(peak_between(n, 0.5, 4.0) == 0.0, "playback: -90 dBFS of host dither never reaches the output (peak %.7f)", peak_between(n, 0.5, 4.0));
        n = run_amp(8.0, amp_tone_then_silence, 0);
        CHECK(peak_between(n, 1.0, 2.0) > 0.20, "playback: the tone passes while it plays (peak %.3f)", peak_between(n, 1.0, 2.0));
        CHECK(peak_between(n, 3.0, 8.0) == 0.0, "playback: after the host goes silent the output is silent once the hold (0.5 s) has passed (peak %.7f)", peak_between(n, 3.0, 8.0));
        n = run_amp(6.0, amp_silence_then_tone, 0);
        CHECK(peak_between(n, 0.5, 1.9) == 0.0 && peak_between(n, 2.1, 6.0) > 0.20, "playback: the gate opens when the tone starts (silence before: %.6f, tone after: %.3f)", peak_between(n, 0.5, 1.9), peak_between(n, 2.1, 6.0));
        double first = -1.0; for (size_t i = (size_t)(2.0 * 44100.0); i < n; i++) if (fabs(g_out[i]) > 0.05) { first = (double)i / 44100.0 - 2.0; break; }
        CHECK(first >= 0.0 && first < 0.030, "playback: the tone is audible %.1f ms after the host starts it (limit 30 ms)", first * 1000.0);
    }
    {   /* the Schmitt trigger: after a starvation the output waits until the ring holds PA_PB_REPRIME frames again, it does not chatter back after a packet or two */
        size_t n = run_playback(44118.0, 3.0, 3.020, 6.0);
        const size_t from = (size_t)(3.0 * 44118.0);
        double silent = 0.0, run = 0.0;
        for (size_t i = from; i < from + 4410 && i < n; i++) { if (fabs(g_out[i]) < 1e-6) { run += 1.0; if (run > silent) silent = run; } else run = 0.0; }
        CHECK(silent / 44.118 >= 15.0, "playback: after a 20 ms host gap the output stays silent for %.1f ms (the gap and the ring buffering again; at least 15 ms)", silent / 44.118);
    }
    printf("%s\n", fails ? "RESULT: FAILED" : "RESULT: ALL PASSED");
    return fails ? 1 : 0;
}
