/* SPDX-License-Identifier: GPL-3.0-only */
/* Copyright (C) 2026 The Onyx Authors (@28spawn) */
/* include/pa_audio_core.h - Portable audio core shared by the device firmware,
 * the host emulator and the WebAssembly build, so all three produce the same
 * audio.
 *
 * Nothing here touches MMIO: the whole module runs on the PC and in a browser.
 *
 * Before this existed the emulator carried its OWN copy of the amp tone stack
 * (120 / 700 / 3 kHz / 5.5 kHz) while the device used the real one
 * (300 / 2 kHz / 4 kHz / 5 kHz, with per-model voicing and a per-model output
 * trim), and the emulator ran its OWN chain loop with no DC blocker, no global
 * noise gate, no Fizz filter and no output limiter. The two therefore never
 * sounded the same. Both now call this module.
 */
#ifndef PA_AUDIO_CORE_H
#define PA_AUDIO_CORE_H

#include <stdint.h>
#include "pa_config.h"
#include "pa_audio.h"
#include "pa_fx.h"
#include "pa_model.h"
#include "pa_wavenet.h"
#include "pa_ui.h"
#include "pa_drums.h"
#include "pa_usb_ble.h"

/* ---- AMP node (slot 2) ---------------------------------------------------- */

/* Builds the AMP effect node: pre-amp gain -> NAM/WaveNet core -> 4-band tone
 * stack -> per-model loudness trim -> master.
 * `engine_flag` points at the g_active_amp_engine selector (0 = GRU,
 * 1 = WaveNet) so the caller keeps ownership of it. */
fx_node_t *pa_amp_node_create(pa_nam_model_t *gru, pa_wavenet_model_t *wn, int *engine_flag);

/* Per-model voicing + output trim (Blk-01). Stock models 0..19, >= 20 = a user
 * NAM capture. Declared in pa_fx.h; the implementation lives here so the
 * device, the emulator and the WASM build all share it. */
void pa_amp_set_model_profile(uint8_t amp_idx);

/* ---- the "jack": the device's audio I/O path, made portable ----------------
 * 24-bit samples in, 24-bit samples out, with the input gain, the master
 * volume slew and the 1-LSB TPDF dither - the device's DAC output stage,
 * verbatim. On the device it is fed by the ALNK0 DMA; in the WebAssembly build
 * it is fed by the browser's audio (on Windows that IS Windows Audio / WASAPI,
 * including an ASIO interface behind a bridge driver); in the host emulator it
 * is fed by PortAudio.
 *
 * The firmware therefore sees exactly the same 24-bit samples, with the same
 * input gain, the same volume law and the same dither, in every build - so
 * what the emulator plays is what the device plays. */
void pa_audio_core_process_half(const int32_t *rx_half, int32_t *tx_half);

/* Bind the runtime the jack path runs on (the chain, the UI, the drum machine
 * and the USB/BLE context). Called once at boot by each platform. */
void pa_audio_core_bind(pa_fx_chain_t *chain, pa_ui_ctx_t *ui,
                        pa_drums_t *drums, pa_usb_ble_ctx_t *usb);

/* A custom block callback replaces the amp chain in the jack path (the device
 * allows this; the emulator and the browser do not use it). */
void pa_audio_core_set_block_cb(pa_audio_block_cb_t cb);

/* The jack's power-on state: unity input gain and the master level the UI
 * asks for. Every platform calls this before the first block. */
void pa_audio_core_init_jack(pa_ui_ctx_t *ui);

/* The power-on mute: the next quiet_blocks blocks go out as silence whatever the master level is, and the first rise after them is a fade (93 ms to full level)
 * instead of a one-block step. pa_audio_core_init_jack arms it on the device; the host tests arm it themselves. pa_audio_core_boot_muted: 1 while the silence lasts. */
void pa_audio_core_boot_mute_arm(uint32_t quiet_blocks);
int  pa_audio_core_boot_muted(void);

/* ---- shared block processing ----------------------------------------------- */

/* The device's audio block callback, verbatim:
 *   tuner (pre-FX, left channel) -> 8-slot chain -> drum mix
 *   (through the guitar + drums sum limiter, src/sys/pa_mix_limiter.h)
 *   -> USB playback/capture -> soft-knee ceiling (a safety net). */
void pa_audio_core_process_block(pa_fx_chain_t *chain, pa_ui_ctx_t *ui,
                                 pa_drums_t *drums, pa_usb_ble_ctx_t *usb,
                                 int32_t *buf_L, int32_t *buf_R, int n);

/* The device's final soft-knee ceiling: 4:1 compression above -1 dBFS. */
int32_t pa_soft_ceiling_q21(int32_t x);

/* The DAC master level, 0..100 %, on the audio taper (-40 dB .. 0 dB), as a
 * Q15 gain (0x8000 = unity). Shared by the Settings OUTPUT LEVEL and the
 * front-panel MASTER pot so the two never disagree. */
uint16_t pa_audio_core_master_q15(uint8_t pct);
/* The output routing (0 STEREO, 1 MONO, 2 LEFT, 3 RIGHT, 4 SWAP) and balance (-50..+50), applied after everything else, just before the DAC.
 * STEREO with balance 0 is the identity: the block is not touched. */
void pa_audio_core_set_routing(uint8_t route, int8_t balance);
/* The same transform on one block, for the host tests. */
void pa_audio_core_route_block(int32_t *L, int32_t *R, int n);

/* ---- the click-free mute handshake (Blk-02) -------------------------------
 * UI: request mute -> the audio thread ramps the gain to 0 over 4 blocks and
 * stops touching the chain -> the UI applies the preset -> the UI releases ->
 * the audio thread ramps back in. Without this every preset change clicks. */
void pa_audio_core_request_mute(void);
int  pa_audio_core_is_muted(void);
void pa_audio_core_release_mute(void);

/* ---- the platform services the portable core needs ------------------------
 * The core is free of MMIO, so the few platform-specific things it needs
 * (watchdog, battery, backlight, the pool usage, the clock) are injected. The
 * device passes the plat_* functions; the WebAssembly build passes stubs. */
typedef struct {
    void     (*feed_wdt)(void);
    int      (*battery_mv)(void);
    int      (*battery_brownout)(void);
    void     (*set_backlight)(uint8_t pct);
    uint32_t (*pool_used)(void);
    uint32_t (*now_ms)(void);
    int      (*battery_adc)(void);      /* the stock method: the averaged 10-bit SARADC count of channel 9 (0 = no reading yet); the percentage comes from it (pa_battery_pct_from_adc) */
    int      (*charging)(void);         /* 1 = a USB cable supplies power (the stock reads PMU status bit 16, active low) */
} pa_board_services_t;

/* ---- the 60 Hz firmware tick ----------------------------------------------
 * The whole tick - input handling, the MIDI footswitch events, the toast
 * timers, the drum sync, the auto-dim, the settings persistence and the
 * render - lives here, so the device, the host emulator and the WebAssembly
 * build all run the SAME firmware logic rather than three similar copies. */
void pa_audio_core_tick(pa_fx_chain_t *chain, pa_ui_ctx_t *ui, pa_drums_t *drums,
                        pa_usb_ble_ctx_t *usb, pa_cpu_meter_t *meter,
                        const pa_board_services_t *bsp,
                        uint32_t buttons, int32_t enc_delta, const uint16_t *pots);

/* The device's boot sequence: init the store, the user NAM registry and the
 * settings, then apply the last-used preset. */
void pa_audio_core_boot(pa_fx_chain_t *chain, pa_ui_ctx_t *ui, pa_usb_ble_ctx_t *usb,
                        pa_drums_t *drums, pa_cpu_meter_t *meter,
                        const pa_board_services_t *bsp);

/* ---- deferred swaps -------------------------------------------------------
 * The UI thread used to BLOCK waiting for the mute to finish (up to 30 ms - a
 * visible freeze on every preset change), and the wait was on a clock that
 * only advances when the caller is not blocked, so on a host or WebAssembly
 * build it could never finish.
 *
 * Instead the UI hands the change over and the AUDIO thread applies it, at a
 * block boundary, once the output gain has reached zero. The chain is never
 * mutated while the audio is running it, and nothing ever blocks. */
void pa_audio_core_request_preset(const pa_preset_t *p);
/* Select a user NAM capture through the model's apply path, so it gets the
 * same level as when it is picked from the effect menu. */
void pa_audio_core_select_user_nam(pa_ui_ctx_t *ui, pa_fx_chain_t *chain,
                                   uint8_t user_idx);
void pa_audio_core_request_model(pa_ui_ctx_t *ui, pa_fx_chain_t *chain,
                                 uint8_t slot, uint8_t type);

#endif /* PA_AUDIO_CORE_H */
