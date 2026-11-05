/* test/test_usb_stereo.c - the USB audio is stereo end to end: what the host plays comes back on its own channel (the playback ring keeps L and R apart,
 * and mixes into the two output channels), and the capture sent to the host carries the left and right outputs on the two channels of each frame
 * (44.1 kHz and 48 kHz). With the stereo cabinet on, the host therefore records the same two channels the headphones get. */
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include "pa_usb_ble.h"
#include "pa_config.h"

/* the MIDI side of usb_ble.c is not under test */
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

int main(void) {
    static pa_usb_ble_ctx_t ctx;
    memset(&ctx, 0, sizeof ctx);

    /* playback: the host sends L = +40000, R = -120000 (24-bit) as 1 ms packets; the playback ring primes (192 frames), then the outputs get them on their own channels (Q21 = 24-bit >> 2) */
    uint8_t pkt[6 * 44];
    for (int i = 0; i < 44; i++) {
        const int32_t l = 40000, r = -120000;
        pkt[i * 6 + 0] = (uint8_t)l; pkt[i * 6 + 1] = (uint8_t)(l >> 8); pkt[i * 6 + 2] = (uint8_t)(l >> 16);
        pkt[i * 6 + 3] = (uint8_t)r; pkt[i * 6 + 4] = (uint8_t)(r >> 8); pkt[i * 6 + 5] = (uint8_t)(r >> 16);
    }
    for (int k = 0; k < 10; k++) pa_usb_audio_ep3_out_rx(&ctx, pkt, sizeof pkt);       /* 440 frames */
    int32_t L[64] = {0}, R[64] = {0};
    pa_usb_audio_mix_playback_stereo(&ctx, L, R, 64);                                   /* primes and fades in */
    memset(L, 0, sizeof L); memset(R, 0, sizeof R);
    pa_usb_audio_mix_playback_stereo(&ctx, L, R, 64);
    CHECK(L[0] == 10000 && L[63] == 10000, "playback left channel: %d (expect 10000)", (int)L[0]);
    CHECK(R[0] == -30000 && R[63] == -30000, "playback right channel: %d (expect -30000)", (int)R[0]);

    /* capture: different left and right outputs arrive on different channels of the host's frames, at both host rates */
    for (int pass = 0; pass < 2; pass++) {
        pa_usb_audio_set_rate(&ctx, pass ? 48000u : 44100u);
        int32_t cl[64], cr[64];
        for (int blk = 0; blk < 8; blk++) {                   /* fill half the ring so the reader does not re-centre */
            for (int i = 0; i < 64; i++) { cl[i] = 200000; cr[i] = -400000; }
            pa_usb_audio_push_capture_stereo(&ctx, cl, cr, 64);
        }
        uint8_t out[300];
        uint32_t n = 0;
        for (int ms = 0; ms < 4; ms++) n += pa_usb_audio_ep3_in_fill(&ctx, out, sizeof out) ? 1u : 0u;
        const uint32_t bytes = pa_usb_audio_ep3_in_fill(&ctx, out, sizeof out);
        CHECK(bytes >= 6u * 44u, "%s: a 1 ms packet holds %u bytes", pass ? "48 kHz" : "44.1 kHz", (unsigned)bytes);
        const int32_t l = s24(&out[0]), r = s24(&out[3]);
        CHECK(l == 800000 && r == -1600000, "%s: frame 0 left %d right %d (expect 800000 / -1600000)", pass ? "48 kHz" : "44.1 kHz", (int)l, (int)r);
        (void)n;
    }
    printf("%s\n", fails ? "RESULT: FAILED" : "RESULT: ALL PASSED");
    return fails ? 1 : 0;
}
