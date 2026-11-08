/* SPDX-License-Identifier: GPL-3.0-only */
/* Copyright (C) 2026 The Onyx Authors (@28spawn) */
/* test/test_usb_capture_missed.c - the recording survives host frames that found no packet (a late USB interrupt) without a re-centre.
 *
 * Every missed 1 ms host frame leaves the read position ~44 frames behind what the audio interrupt keeps writing; the slow fill control used to let the ring climb to its overflow line
 * and then skip ~270 frames at once (about once a second on the unit at 48 kHz: the clicks of 2026-11-05). Here the consumer skips one frame every 170 ms for 20 s (the unit measured 5 to 10 a second on 2026-11-05; the first version of this test used one a second and the rate control alone passed it), and tells the reader about each miss the way usb_dev.c does (the host frame-number gap), at 44.1 kHz and 48 kHz
 * host rates, and the ring must settle without a single re-centre or starvation (g_pa_cap_recentres counts both). */
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


extern uint32_t g_pa_cap_recentres, g_pa_cap_starved;
static int fails;
#define CHECK(ok, ...) do { printf((ok) ? "  ok   " : "  FAIL "); printf(__VA_ARGS__); printf("\n"); if (!(ok)) fails++; } while (0)

static void run(uint32_t host_hz, const char *label) {
    static pa_usb_ble_ctx_t ctx;
    memset(&ctx, 0, sizeof ctx);
    pa_usb_audio_set_rate(&ctx, host_hz);
    const double codec_hz = 44118.0, dphi = 2.0 * M_PI * 1000.0 / codec_hz, blk_s = 64.0 / codec_hz;
    double phase = 0.0, t_prod = 0.0, t_cons = 0.0005, next_miss = 3.0;
    uint8_t pkt[300];
    uint32_t misses = 0, rc0 = 0, st0 = 0, pending = 0;
    int warm = 0;
    while (t_cons < 20.0) {
        if (t_prod <= t_cons) {
            int32_t L[64], R[64];
            for (int i = 0; i < 64; i++) { L[i] = R[i] = (int32_t)(0.25 * 2097152.0 * sin(phase)); phase += dphi; }
            pa_usb_audio_push_capture_stereo(&ctx, L, R, 64);
            t_prod += blk_s;
        } else {
            if (t_cons >= next_miss) { next_miss += 0.17; misses++; pending++; }            /* the host frame found nothing: no packet read */
            else { if (pending) { pa_usb_audio_ep3_missed(&ctx, pending); pending = 0; } (void)pa_usb_audio_ep3_in_fill(&ctx, pkt, sizeof pkt); }
            t_cons += 0.001;
            if (!warm && t_cons >= 2.0) { warm = 1; rc0 = g_pa_cap_recentres; st0 = g_pa_cap_starved; }
        }
    }
    CHECK(misses >= 100, "%u Hz: %u host frames were missed", (unsigned)host_hz, (unsigned)misses);
    CHECK(g_pa_cap_recentres == rc0, "%s: no re-centre or starvation in 18 s with a missed frame every 170 ms (got %u)", label, (unsigned)(g_pa_cap_recentres - rc0));
    CHECK(g_pa_cap_starved == st0, "%s: no packet went out as silence (got %u)", label, (unsigned)(g_pa_cap_starved - st0));
}

int main(void) {
    run(44100u, "host 44.1 kHz");
    run(48000u, "host 48 kHz");
    printf("%s\n", fails ? "RESULT: FAILED" : "RESULT: ALL PASSED");
    return fails ? 1 : 0;
}
