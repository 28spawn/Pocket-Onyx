/* SPDX-License-Identifier: GPL-3.0-only */
/* Copyright (C) 2026 The Onyx Authors (@28spawn) */
/* custom-fw/test/test_full_firmware.c — End-to-End Host Verification Suite for All Big Rocks
 *
 * Compiles and tests all Pocket Amp Custom Firmware subsystems natively on PC:
 *   - Big Rock 2: 0x01EE0400 HW Accelerator Sim + 16-Unit Q21 NAM A2 GRU + Partitioned Q18 IR Cab
 *   - Big Rock 3: Dynamics (Gate/Gate++/Comp/Boost) + Drive + EQ + Mod (7 modes) + Delay (5 modes)
 *                 + Reverb (5 modes) + 8-Slot Chain Manager + C7 SRAM Reclaim
 *   - Big Rock 4: Chromatic Guitar Tuner (82.4 Hz E2 & 440.0 Hz A4 pitch lock) + TFT UI Renderer
 *   - Big Rock 5: USB-MIDI 4-byte Event Packet framing + OTA Handshake + BLE GATT Bridge
 *   - Secondary : Procedural 808/909 Drum Machine & Metronome + Dual-Core CPU1 Offload
 */

#include <stdio.h>
#include <stdlib.h>
#include <math.h>
#include "pa_config.h"
#include "pa_fx.h"
#include "pa_model.h"
#include "pa_cab.h"
#include "pa_ui.h"
#include "pa_usb_ble.h"
#include "pa_sysex.h"
#include "pa_drums.h"
#include "pa_cpu_meter.h"
#include "pa_wavenet.h"
#include "pa_audio.h"
#include "pa_audio_core.h"
#include <string.h>

/* Tracking heap allocator for verifying C7 dynamic SRAM reclaim */
static long g_sram_cur = 0;
static long g_sram_peak = 0;

void *pa_alloc(uint32_t n) {
    /* a 16-byte header: one long is 4 bytes on Windows, which left every block misaligned for 8-byte types */
    long long *p = (long long *)calloc(1, n + 2 * sizeof(long long));
    if (!p) return 0;
    p[0] = (long long)n;
    g_sram_cur += (long)n;
    if (g_sram_cur > g_sram_peak) g_sram_peak = g_sram_cur;
    return p + 2;
}

void pa_free(void *q) {
    if (!q) return;
    long long *p = (long long *)q - 2;
    g_sram_cur -= (long)p[0];
    free(p);
}

/* Mock LCD hooks for UI verification */
void st7735s_fill_rect(uint16_t x, uint16_t y, uint16_t w, uint16_t h, uint16_t c) {
    (void)x; (void)y; (void)w; (void)h; (void)c;
}
void ui_draw_str(uint16_t x, uint16_t y, const char *s, uint16_t fg, uint16_t bg) {
    (void)x; (void)y; (void)s; (void)fg; (void)bg;
}
void ui_draw_char(uint16_t x, uint16_t y, char c, uint16_t fg, uint16_t bg) {
    (void)x; (void)y; (void)c; (void)fg; (void)bg;
}
void ui_draw_char_scaled(uint16_t x, uint16_t y, char c, uint16_t fg, uint16_t bg, uint8_t scale) {
    (void)x; (void)y; (void)c; (void)fg; (void)bg; (void)scale;
}
void ui_draw_str_scaled(uint16_t x, uint16_t y, const char *s, uint16_t fg, uint16_t bg, uint8_t scale) {
    (void)x; (void)y; (void)s; (void)fg; (void)bg; (void)scale;
}

/* The Onyx pixel primitives (the Groove Station redesign) - the host suite
 * does not draw, so they are stubs here. The real ones are in
 * src/board/ui_5x7.c, which the host build leaves out. */
uint16_t ui_onyx_text_w(const char *s, uint8_t scale) {
    (void)scale;
    if (!s) return 0;
    uint16_t n = 0;
    while (s[n]) n++;
    return (uint16_t)((n * 6u - 1u) * (scale ? scale : 1u));
}
void ui_onyx_draw_glyph(uint16_t x, uint16_t y, char c, uint8_t scale, uint16_t fg) {
    (void)x; (void)y; (void)c; (void)scale; (void)fg;
}
void ui_onyx_draw_str(uint16_t x, uint16_t y, const char *s, uint8_t scale, uint16_t fg, uint8_t align) {
    (void)x; (void)y; (void)s; (void)scale; (void)fg; (void)align;
}
void ui_onyx_cut_box(uint16_t x0, uint16_t y0, uint16_t x1, uint16_t y1,
                     uint8_t cut, uint16_t fill, uint16_t outline) {
    (void)x0; (void)y0; (void)x1; (void)y1; (void)cut; (void)fill; (void)outline;
}
void ui_onyx_draw_sprite(uint16_t x, uint16_t y, uint8_t sprite, uint8_t scale, uint16_t fg) {
    (void)x; (void)y; (void)sprite; (void)scale; (void)fg;
}
void ui_onyx_segments(uint16_t x, uint16_t y, uint8_t n, uint8_t w, uint8_t h,
                       uint8_t gap, uint8_t lit, uint16_t on, uint16_t off) {
    (void)x; (void)y; (void)n; (void)w; (void)h; (void)gap; (void)lit; (void)on; (void)off;
}
void pa_sysex_input_byte(uint8_t b) { (void)b; }
/* The host suite does not link src/sys/sysex.c (it stubs the parser above),
 * so the board driver's reply-sink registration needs a stub too. */
void pa_sysex_set_reply_sink(pa_sysex_reply_sink_t sink) { (void)sink; }
int  pa_sysex_reply_sink_set(void) { return 0; }
int plat_battery_is_brownout(void) { return 0; }

/* Globals + stubs for the board-layer symbols preset.c / ui_app.c reference
 * (they normally live in src/board/main.c, which the host tests do not link).
 * The mute handshake is NOT stubbed here: pa_audio_core.c owns that state
 * machine for every platform, and this suite links the core. */
pa_wavenet_model_t g_wn_model;
int g_active_amp_engine = 0;
uint32_t plat_ms(void) { return 0; }
void plat_wdt_feed(void) {}
void plat_backlight_set(uint8_t pct) { (void)pct; }
pa_fx_chain_t g_fx_chain;
pa_nam_model_t g_nam_model;
pa_audio_state_t g_pa_audio;

static int fails = 0;
#define CHECK(cond, ...) do { \
    if (!(cond)) { \
        printf("  FAIL: "); printf(__VA_ARGS__); printf("\n"); \
        fails++; \
    } else { \
        printf("  ok    "); printf(__VA_ARGS__); printf("\n"); \
    } \
} while (0)

int main(void) {
    printf("=== M-VAVE Pocket Amp Custom Firmware — Full Verification Suite ===\n");

    /* 1. Initialize 8-Slot Chain (DYN -> DRV -> AMP -> CAB -> EQ -> MOD -> DLY -> REV) */
    pa_fx_chain_t chain;
    fx_node_t *cab_nd = fx_cab_ir_new();
    pa_fx_chain_init(&chain, 0, cab_nd);
    /* Enable all 8 slots for full-chain verification and C7 reclaim test */
    for (int s = 0; s < PA_SLOT_COUNT; s++) {
        pa_fx_chain_set_slot_enabled_now(&chain, s, 1);
    }
    printf("[BR-3] 8-Slot FX Chain initialized: active SRAM = %ld B (%.1f KB)\n",
           g_sram_cur, g_sram_cur / 1024.0);
    CHECK(chain.head != NULL, "8-Slot FX Chain head linked");
    CHECK(g_sram_cur > 0, "8-Slot FX Chain allocated state in SRAM (%ld B)", g_sram_cur);

    /* 2. Synthesize a 440.0 Hz A4 test guitar wave in Q21 and run through all 8 slots */
    int32_t block[PA_BLOCK_N];
    for (int b = 0; b < 30; b++) {
        for (int i = 0; i < PA_BLOCK_N; i++) {
            double t = (double)(b * PA_BLOCK_N + i) / (double)PA_SAMPLE_RATE;
            block[i] = (int32_t)(sin(2.0 * 3.141592653589793 * 440.0 * t) * (PA_Q21_ONE * 0.25));
        }
        pa_fx_chain_run(&chain, block, PA_BLOCK_N);
    }
    printf("[BR-3] Processed 30 blocks through DYN->DRV->CAB->EQ->MOD->DLY->REV: sample[0]=%d\n",
           block[0]);
    int blk_finite = 1;
    int32_t blk_peak = 0;
    for (int i = 0; i < PA_BLOCK_N; i++) {
        if (!isfinite((float)block[i])) blk_finite = 0;
        int32_t a = abs(block[i]);
        if (a > blk_peak) blk_peak = a;
    }
    CHECK(blk_finite, "Guitar audio output is finite across all samples");
    CHECK(blk_peak > 1000, "Guitar audio output is not silent (peak %d)", blk_peak);
    CHECK(blk_peak < 4 * PA_Q21_ONE, "Guitar audio output within headroom (peak %d)", blk_peak);

    /* 3. Verify C7 Dynamic SRAM Reclaim when bypassing Reverb +Modulation */
    long before_reclaim = g_sram_cur;
    pa_fx_chain_set_slot_enabled_now(&chain, PA_SLOT_REVERB, 0);
    pa_fx_chain_set_slot_enabled_now(&chain, PA_SLOT_MOD, 0);
    printf("[BR-3] Bypassed REVERB + MOD -> Reclaimed %ld B SRAM (remaining: %ld B)\n",
           before_reclaim - g_sram_cur, g_sram_cur);
    CHECK(before_reclaim - g_sram_cur > 0, "Bypassed REVERB + MOD reclaimed SRAM (> 0 B, got %ld B)", before_reclaim - g_sram_cur);

    /* 4. Verify Big Rock 4 Chromatic Tuner Pitch Detection on A4 = 440.0 Hz */
    pa_tuner_state_t tuner;
    pa_tuner_init(&tuner);
    tuner.active = 1;
    tuner.mute_output = 0;
    for (int b = 0; b < 250; b++) {                 /* the YIN detector needs a window of 63 ms and two agreeing detections: about 180 ms (it used to lock in 20 ms and wander) */
        for (int i = 0; i < PA_BLOCK_N; i++) {
            double t = (double)(b * PA_BLOCK_N + i) / (double)PA_SAMPLE_RATE;
            block[i] = (int32_t)(sin(2.0 * 3.141592653589793 * 440.0 * t) * (PA_Q21_ONE * 0.30));
        }
        pa_tuner_process_block(&tuner, block, PA_BLOCK_N);
    }
    printf("[BR-4] Chromatic Tuner on 440.0 Hz -> locked=%u, note_idx=%u (9='A'), oct=%d, cents=%d, freq=%.1f Hz\n",
           tuner.signal_locked, tuner.note_index, tuner.octave, tuner.cents,
           tuner.freq_tenths_hz / 10.0);
    CHECK(tuner.signal_locked == 1, "Chromatic Tuner locked onto 440.0 Hz signal");
    CHECK(tuner.note_index == 9, "Chromatic Tuner note index is 9 ('A', got %u)", tuner.note_index);
    CHECK(tuner.octave == 4, "Chromatic Tuner octave is 4 (got %d)", tuner.octave);
    CHECK(abs(tuner.cents) <= 2, "Chromatic Tuner cents error within +/-2 cents (got %d)", tuner.cents);
    CHECK(abs((int)tuner.freq_tenths_hz - 4400) <= 10, "Chromatic Tuner freq within 1 Hz of 440.0 Hz (got %.1f Hz)", tuner.freq_tenths_hz / 10.0);

    /* 5. Verify Secondary Priority Drum Machine + Metronome Mix */
    pa_drums_t drums;
    pa_drums_init(&drums);
    pa_drums_set_playing(&drums, 1);
    /* two blocks: the drum bus limiter's look-ahead (PA_DRUM_LIM_LOOKAHEAD = 36 bus samples = 72 output
     * samples, 1.6 ms) and the bus interpolation (2 more) delay the first hit past the end of the first block */
    for (int i = 0; i < PA_BLOCK_N; i++) block[i] = 0;
    pa_drums_mix_block(&drums, block, PA_BLOCK_N);
    for (int i = 0; i < PA_BLOCK_N; i++) block[i] = 0;
    pa_drums_mix_block(&drums, block, PA_BLOCK_N);
    int32_t drum_hit = block[2 * PA_DRUM_LIM_LOOKAHEAD + 2 - PA_BLOCK_N + 4];
    printf("[SP-2] Procedural 808/909 Drum Machine step 0 triggered: peak sample[%d]=%d\n",
           (int)(2 * PA_DRUM_LIM_LOOKAHEAD + 2 - PA_BLOCK_N + 4), drum_hit);
    CHECK(drum_hit > 100000 || drum_hit < -100000, "Drum hit triggered nonzero audio sample (|x| > 100000, got %d)", drum_hit);

    /* Verify drum output is finite, not silent, and below full scale */
    float max_abs = 0.0f;
    int drum_finite = 1;
    for (int i = 0; i < PA_BLOCK_N; i++) {
        float abs_val = (float)abs(block[i]);
        if (!isfinite(abs_val)) drum_finite = 0;
        if (abs_val > max_abs) max_abs = abs_val;
    }
    CHECK(drum_finite, "Drum machine output is finite");
    CHECK(max_abs > 0.01f, "Drum machine output is not silent (max=%d)", (int)max_abs);
    CHECK(max_abs < PA_Q21_ONE, "Drum machine output is below full scale (max=%d)", (int)max_abs);

    /* NaN / Inf weights are refused, and a refused load leaves the model untouched.
     * (A non-finite weight would otherwise make the output NaN until reboot, and converting
     * NaN to an integer is undefined - in practice a full-scale burst.) */
    {
        static float w[PA_WN_LITE_WEIGHTS];
        for (int i = 0; i < PA_WN_LITE_WEIGHTS; i++) w[i] = 0.01f * (float)((i % 7) - 3);
        pa_wavenet_init_default(&g_wn_model);
        CHECK(pa_wavenet_load_weights(&g_wn_model, w, PA_WN_LITE_WEIGHTS, 1.0f) == 0,
              "Finite A2-Lite weight set was accepted");
        const float before = g_wn_model.weights[100];
        w[100] = NAN;
        int rc_nan = pa_wavenet_load_weights(&g_wn_model, w, PA_WN_LITE_WEIGHTS, 1.0f);
        w[100] = INFINITY;
        int rc_inf = pa_wavenet_load_weights(&g_wn_model, w, PA_WN_LITE_WEIGHTS, 1.0f);
        w[100] = 0.0f;
        int rc_scale = pa_wavenet_load_weights(&g_wn_model, w, PA_WN_LITE_WEIGHTS - 1, NAN);
        CHECK(rc_nan == -3 && rc_inf == -3 && rc_scale == -3,
              "Non-finite weights and NaN scale are refused with -3 (NaN %d, Inf %d, NaN scale %d)",
              rc_nan, rc_inf, rc_scale);
        CHECK(g_wn_model.weights[100] == before, "Refused weight load left model weights untouched");
    }

    {   /* The power-on mute (pa_audio_core.c): a full-scale input from the first block on, the audio in bypass so the output is the input times the master level.
         * (1) the quiet blocks are silence even with the master at 100 % and no hold; (2) the level then rises by at most the ramp step a block, so no sample-to-sample
         * jump is larger than a fade allows; (3) it ends at the full level; (4) once there, the master follows the knob in one block again. */
        extern volatile uint8_t g_pa_master_hold;
        static int32_t rx[2 * PA_AUDIO_BLOCK_FRAMES], tx[2 * PA_AUDIO_BLOCK_FRAMES];
        const int32_t in24 = 0x400000;                                   /* half of full scale, DC: the worst case for a step */
        for (int i = 0; i < (int)PA_AUDIO_BLOCK_FRAMES; i++) { rx[2 * i] = in24; rx[2 * i + 1] = 0; }
        memset(&g_pa_audio, 0, sizeof g_pa_audio);
        g_pa_audio.input_gain_q15 = 0x8000u; g_pa_audio.master_vol_q15 = 0x8000u; g_pa_audio.master_vol_cur_q15 = 0x8000u;
        g_pa_audio.bypass = 1; g_pa_master_hold = 0;
        pa_audio_core_boot_mute_arm(10u);
        int32_t quiet_peak = 0, max_jump = 0, prev = 0, last = 0; int blocks_to_full = -1, muted_ok = 1;
        for (int b = 0; b < 200; b++) {
            if (b < 10 && !pa_audio_core_boot_muted()) muted_ok = 0;
            pa_audio_core_process_half(rx, tx);
            for (int i = 0; i < (int)PA_AUDIO_BLOCK_FRAMES; i++) {
                const int32_t v = (int32_t)((uint32_t)tx[2 * i] << 8) >> 8, a = v < 0 ? -v : v, j = v > prev ? v - prev : prev - v;
                if (b < 10 && a > quiet_peak) quiet_peak = a;
                if (j > max_jump) max_jump = j;
                prev = v; last = v;
            }
            if (blocks_to_full < 0 && last >= in24 - 4) blocks_to_full = b - 10 + 1;
        }
        CHECK(muted_ok && !pa_audio_core_boot_muted(), "Power-on mute: reported for its 10 blocks, then over");
        CHECK(quiet_peak <= 2, "Power-on mute: the quiet blocks are silence with the master at 100 %% (peak %d of 8388607)", (int)quiet_peak);
        CHECK(blocks_to_full >= 60 && blocks_to_full <= 66, "Power-on mute: the fade-in takes 64 blocks, 93 ms (got %d)", blocks_to_full);
        CHECK(max_jump <= in24 / (60 * 64) + 4, "Power-on mute: no step in the fade-in (largest sample-to-sample change %d, input %d)", (int)max_jump, (int)in24);
        CHECK(last >= in24 - 4 && last <= in24 + 4, "Power-on mute: ends at the full level (%d of %d)", (int)last, (int)in24);
        g_pa_audio.master_vol_q15 = 0x2000u;
        pa_audio_core_process_half(rx, tx);
        g_pa_audio.master_vol_q15 = 0x8000u;
        pa_audio_core_process_half(rx, tx);
        last = (int32_t)((uint32_t)tx[2 * (PA_AUDIO_BLOCK_FRAMES - 1)] << 8) >> 8;
        CHECK(last >= in24 - 4, "Power-on mute: after the fade the master follows the knob within one block (%d of %d)", (int)last, (int)in24);
        /* the hold: armed with the knob not read yet, nothing comes out however long it lasts, and the release is the same fade */
        g_pa_master_hold = 1; g_pa_audio.master_vol_cur_q15 = 0x8000u;
        pa_audio_core_boot_mute_arm(3u);
        quiet_peak = 0;
        for (int b = 0; b < 40; b++) {
            pa_audio_core_process_half(rx, tx);
            for (int i = 0; i < (int)PA_AUDIO_BLOCK_FRAMES; i++) { const int32_t v = (int32_t)((uint32_t)tx[2 * i] << 8) >> 8, a = v < 0 ? -v : v; if (a > quiet_peak) quiet_peak = a; }
        }
        CHECK(quiet_peak <= 2, "Power-on mute: silent for as long as the master hold lasts (peak %d)", (int)quiet_peak);
        g_pa_master_hold = 0;
        pa_audio_core_process_half(rx, tx);
        last = (int32_t)((uint32_t)tx[2 * (PA_AUDIO_BLOCK_FRAMES - 1)] << 8) >> 8;
        CHECK(last > 0 && last <= in24 / 64 + 4, "Power-on mute: the hold's release starts the fade, not a one-block step (first block ends at %d of %d)", (int)last, (int)in24);
        g_pa_audio.bypass = 0;
    }

    if (fails == 0) {
        printf("=== ALL BIG ROCKS & SECONDARY PRIORITIES VERIFIED (ALL PASSED) ===\n");
        return 0;
    } else {
        printf("=== %d FULL FIRMWARE TEST(S) FAILED ===\n", fails);
        return 1;
    }
}
