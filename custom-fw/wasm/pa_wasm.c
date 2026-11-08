/* SPDX-License-Identifier: GPL-3.0-only */
/* Copyright (C) 2026 The Onyx Authors (@28spawn) */
/* custom-fw/wasm/pa_wasm.c - WebAssembly board shim for the Pocket Amp firmware.
 *
 * This is NOT an emulator of the firmware. It is the firmware's own portable
 * core (src/dsp, src/nam, src/fx, src/sys, src/board/ui_app.c, src/board/ui_5x7.c)
 * compiled to WebAssembly, with ONLY the board layer replaced:
 *
 *   - the ST7735S panel becomes a 240x240 RGB565 framebuffer the page blits
 *   - the buttons/encoder become exported setters
 *   - the SPI NOR flash becomes a 1 MB in-memory NOR image (the real chip's size) the page can save
 *   - the DSP pool becomes a REAL 147,456 B budget, so "out of SRAM" happens in the
 *     browser exactly as it would on the device
 *   - the millisecond clock is driven by the host
 *
 * The audio path is the firmware's own: pa_audio_core_process_block() ->
 * pa_fx_chain_run_stereo() -> the Q21 NAM GRU with the 512-entry activation
 * LUTs, the Q18 partitioned cabinet FIR, and every effect module. The LCD you
 * see is the firmware's pa_ui_render(). The sound you hear is the firmware's
 * DSP. Nothing is re-implemented here.
 */
#include <stdint.h>
#include <string.h>
#include <math.h>

#include "../include/pa_audio_core.h"
#include "../include/pa_config.h"
#include "../include/pa_fx.h"
#include "../include/pa_model.h"
#include "../include/pa_ui.h"
#include "../include/pa_drums.h"
#include "../include/pa_usb_ble.h"
#include "../include/pa_cpu_meter.h"
#include "../include/pa_preset.h"
#include "../include/pa_settings.h"
#include "../include/pa_user_nam.h"
#include "../include/pa_stock_assets.h"
#include "../include/pa_preset.h"
#include "../include/pa_flash_map.h"
#include "../include/pa_spi_nor.h"
#include "../include/pa_audio.h"
#include "../include/pa_plat.h"
#include "../include/pa_hw_accel.h"
#include "../src/dsp/fixedpoint.h"

#define WASM_EXPORT(name) __attribute__((export_name(name)))

/* The allocator's budget: the same macro src/sys/pa_pool.c sizes its pool with. */
#define PA_WASM_POOL_BYTES PA_DSP_POOL_BYTES

/* ===================================================================== */
/* Runtime globals - the same set the device owns in src/board/main.c      */
/* ===================================================================== */
pa_fx_chain_t      g_fx_chain;
pa_nam_model_t     g_nam_model;
pa_wavenet_model_t g_wn_model;
int                g_active_amp_engine = 0;    /* 0 = GRU, 1 = WaveNet */
pa_ui_ctx_t        g_ui;
pa_usb_ble_ctx_t   g_usb_ble;
pa_drums_t         g_drums;
pa_cpu_meter_t     g_cpu_meter;
pa_audio_state_t   g_pa_audio;

static fx_node_t *g_cab_node;

/* ===================================================================== */
/* 1. Display: a 240x240 RGB565 framebuffer                                 */
/* ===================================================================== */
#define LCD_W 240
#define LCD_H 240
static uint16_t g_lcd[LCD_W * LCD_H];

WASM_EXPORT("pa_wasm_lcd_ptr")  uint8_t *pa_wasm_lcd_ptr(void) { return (uint8_t *)g_lcd; }
WASM_EXPORT("pa_wasm_lcd_w")    int pa_wasm_lcd_w(void) { return LCD_W; }
WASM_EXPORT("pa_wasm_lcd_h")    int pa_wasm_lcd_h(void) { return LCD_H; }

/* The ONLY drawing primitive the UI uses. Byte-swaps to the panel's endianness
 * exactly like the device driver does. */
void st7735s_fill_rect(uint16_t x, uint16_t y, uint16_t w, uint16_t h, uint16_t rgb565) {
    if (w > LCD_W) w = LCD_W;
    if (x >= LCD_W) return;
    if ((uint32_t)x + w > LCD_W) w = (uint16_t)(LCD_W - x);
    uint16_t swapped = (uint16_t)((rgb565 << 8) | (rgb565 >> 8));
    for (uint16_t row = 0; row < h; row++) {
        uint32_t py = (uint32_t)y + row;
        if (py >= LCD_H) break;
        uint16_t *dst = &g_lcd[py * LCD_W + x];
        for (uint16_t i = 0; i < w; i++) dst[i] = swapped;
    }
}

/* ===================================================================== */
/* 2. Clock, watchdog, backlight, battery                                  */
/* ===================================================================== */
static uint32_t s_now_ms = 0;
static uint8_t  s_buttons = 0;
static int32_t  s_enc = 0;
static uint16_t s_pots[4] = { 512u << 4, 512u << 4, 512u << 4, 512u << 4 };

uint32_t plat_ms(void)        { return s_now_ms; }
uint32_t plat_ticks24(void)   { return s_now_ms * 24u; }
void     plat_wdt_feed(void)  { }
void     plat_backlight_set(uint8_t pct) { (void)pct; }
int      plat_battery_is_brownout(void) { return 0; }
int      plat_battery_mv(void) { return 4100; }
int      plat_battery_bars(void) { return 3; }
void     plat_battery_poll(void) { }
/* ---- buttons --------------------------------------------------------------
 * The board layer only reports WHICH BUTTONS ARE DOWN. Turning that into
 * one-tick action pulses, debouncing the contacts and synthesising the
 * combo / HOLD bits is FIRMWARE logic and lives in src/sys/pa_input.c, so
 * the browser and the device behave identically. */
uint32_t plat_buttons(void)   { return s_buttons; }
int32_t  plat_enc_delta(void) { int32_t d = s_enc; s_enc = 0; return d; }
uint16_t plat_pot_q12(int ch) { return (ch >= 0 && ch < 4) ? s_pots[ch] : (uint16_t)(512u << 4); }

/* ---- the host-facing input API -------------------------------------------
 * A mouse click can be shorter than one 60 Hz tick, and shorter than the
 * firmware's 20 ms contact debounce - the press would be lost entirely, and
 * that is what made the buttons feel dead. A real button is never held for
 * less than ~50 ms, so the shim holds the contact for 60 ms (four 16 ms ticks, which is what
 * the firmware's debounce needs to see a stable contact) and
 * lets the firmware's debounce do the rest. This is an INPUT-DEVICE
 * adaptation, the same job the firmware's debounce does for a real switch.
 */
#define PA_WASM_MIN_CONTACT_MS 60u

static uint32_t s_press_t[6];       /* when each button went down          */
static uint32_t s_rel_due[6];       /* when a pending release may happen   */

static void s_buttons_service(void) {
    uint32_t now = plat_ms();
    for (int b = 0; b < 6; b++) {
        if (s_rel_due[b] && now >= s_rel_due[b]) {
            s_buttons &= ~(1u << b);
            s_rel_due[b] = 0;
        }
    }
}

WASM_EXPORT("pa_wasm_press")
void pa_wasm_press(uint32_t mask) {
    uint32_t now = plat_ms();
    for (int b = 0; b < 6; b++) {
        uint32_t bit = 1u << b;
        if ((mask & bit) && !(s_buttons & bit)) {
            s_press_t[b] = now;
            s_rel_due[b] = 0;
        }
    }
    s_buttons |= (mask & 0x3Fu);
}

WASM_EXPORT("pa_wasm_release")
void pa_wasm_release(uint32_t mask) {
    uint32_t now = plat_ms();
    for (int b = 0; b < 6; b++) {
        uint32_t bit = 1u << b;
        if ((mask & bit) && (s_buttons & bit)) {
            uint32_t held = now - s_press_t[b];
            if (held < PA_WASM_MIN_CONTACT_MS) s_rel_due[b] = s_press_t[b] + PA_WASM_MIN_CONTACT_MS;
            else { s_buttons &= ~bit; s_rel_due[b] = 0; }
        }
    }
}


/* The mute handshake (Blk-02) is FIRMWARE logic and lives in
 * src/sys/pa_audio_core.c - one owner for every platform. */

/* ===================================================================== */
/* 3b. USB / BLE - no-ops.
 * src/board/usb_ble.c drives the USB device registers directly, and a browser
 * has no USB audio device anyway. The USB path is therefore inert here; the
 * context is still zeroed so the firmware never reads uninitialised state.
 * ===================================================================== */
/* The USB audio loopback: the guitar to the host and the host's audio back.
 * A browser has no USB audio, so this only records the setting. */
void pa_usb_ble_set_loopback(pa_usb_ble_ctx_t *ctx, uint8_t enabled) {
    if (ctx) ctx->loopback_enabled = enabled ? 1u : 0u;
}
void pa_usb_ble_init(pa_usb_ble_ctx_t *ctx) {
    if (!ctx) return;
    memset(ctx, 0, sizeof(*ctx));
    ctx->sample_rate_hz = PA_SAMPLE_RATE;
    ctx->configured     = 1;
    ctx->loopback_enabled = 1;
}
void pa_usb_ble_poll(pa_usb_ble_ctx_t *ctx) { (void)ctx; }
void pa_usb_audio_mix_playback_stereo(pa_usb_ble_ctx_t *ctx, int32_t *L, int32_t *R, int n) { (void)ctx; (void)L; (void)R; (void)n; }
void pa_usb_audio_push_capture_stereo(pa_usb_ble_ctx_t *ctx, const int32_t *L, const int32_t *R, int n) { (void)ctx; (void)L; (void)R; (void)n; }
/* The Settings > USB Audio sample rate (44.1k / 48k) now reaches the USB
 * layer; a browser has no USB audio, so this just records it. */
void pa_usb_audio_set_rate(pa_usb_ble_ctx_t *ctx, uint32_t host_rate_hz) {
    if (ctx) ctx->sample_rate_hz = (host_rate_hz == 48000u) ? 48000u : 44100u;
}

extern uint32_t pa_pool_used_bytes(void);   /* src/sys/pa_pool.c */

/* ===================================================================== */
/* 4. DSP pool - the DEVICE's own allocator (src/sys/pa_pool.c).
 * The WASM build compiles it with -DPA_POOL_TEST, so the browser uses the very
 * same first-fit allocator, the same PA_DSP_POOL_BYTES budget and the same
 * coalescing as the hardware. "Out of SRAM" therefore happens in the browser
 * exactly as it would on the device.
 * ===================================================================== */
WASM_EXPORT("pa_wasm_pool_used") uint32_t pa_wasm_pool_used(void) { return pa_pool_used_bytes(); }
WASM_EXPORT("pa_wasm_pool_size") uint32_t pa_wasm_pool_size(void) { return PA_WASM_POOL_BYTES; }

/* ===================================================================== */
/* 5. SPI NOR flash - a 1 MB in-memory NOR image (erase = 0xFF, program    */
/*    can only clear bits), identical semantics to the device's driver.     */
/* ===================================================================== */
static uint8_t s_flash[PA_FLASH_SIZE];
static int     s_flash_ready = 0;

int pa_spi_nor_init(void) {
    if (!s_flash_ready) { memset(s_flash, 0xFF, PA_FLASH_SIZE); s_flash_ready = 1; }
    return 0;
}

int pa_spi_nor_erase_sector(uint32_t flash_addr) {
    if (!s_flash_ready) pa_spi_nor_init();
    if (flash_addr >= PA_FLASH_SIZE) return -1;
    if (!pa_flash_rw_ok(flash_addr & ~(uint32_t)(PA_FLASH_SECTOR_SIZE - 1u), PA_FLASH_SECTOR_SIZE)) return -3;   /* write-protected */
    if (plat_battery_is_brownout())  return -5;
    uint32_t a = flash_addr & ~(uint32_t)(PA_FLASH_SECTOR_SIZE - 1u);
    memset(&s_flash[a], 0xFF, PA_FLASH_SECTOR_SIZE);
    return 0;
}

int pa_spi_nor_write(uint32_t flash_addr, const void *data, uint32_t len) {
    if (!s_flash_ready) pa_spi_nor_init();
    if (flash_addr >= PA_FLASH_SIZE) return -1;
    if ((uint64_t)flash_addr + len > PA_FLASH_SIZE) return -1;
    if (!pa_flash_rw_ok(flash_addr, len)) return -3;
    if (plat_battery_is_brownout())  return -5;
    const uint8_t *src = (const uint8_t *)data;
    for (uint32_t i = 0; i < len; i++) {
        s_flash[flash_addr + i] &= src[i];      /* NOR program clears bits only */
    }
    return 0;
}

int pa_spi_nor_read(uint32_t flash_addr, void *dst, uint32_t len) {
    if (!s_flash_ready) pa_spi_nor_init();
    if (flash_addr >= PA_FLASH_SIZE) return -1;
    if ((uint64_t)flash_addr + len > PA_FLASH_SIZE) return -1;
    memcpy(dst, &s_flash[flash_addr], len);
    return 0;
}

void pa_spi_nor_sync(void) { }
void pa_spi_nor_begin(void) { }
void pa_spi_nor_end(void) { }

WASM_EXPORT("pa_wasm_flash_ptr")  uint8_t *pa_wasm_flash_ptr(void) { return s_flash; }
WASM_EXPORT("pa_wasm_flash_size") uint32_t pa_wasm_flash_size(void) { return PA_FLASH_SIZE; }

/* Read one preset's name out of the store (for the workbench's preset list). */
WASM_EXPORT("pa_wasm_preset_name")
int pa_wasm_preset_name(uint8_t idx, uint8_t *out, int out_len) {
    pa_preset_t p;
    if (!out || out_len < 1) return -1;
    if (pa_preset_load(idx, &p) != 0) { out[0] = 0; return -1; }
    int n = (int)sizeof(p.name);
    if (n > out_len) n = out_len;
    for (int i = 0; i < n; i++) out[i] = (uint8_t)p.name[i];
    out[n - 1] = 0;
    return (int)p.preset_vol;
}

/* ---- the whole preset record ---------------------------------------------
 *
 * The workbench's LIBRARY tab manages the preset store: it reads a slot's
 * record, writes one back, and stores the sound that is playing right now
 * into a slot. The record is pa_preset_t (include/pa_preset.h) - the same
 * 164 bytes the device stores, and the same bytes a cmd-25 session with the
 * preset target carries, so a preset written here can be pushed to a device
 * unchanged.
 *
 * The firmware recomputes the magic, the sequence number and the CRC32 when
 * it saves, so the host never has to. */
WASM_EXPORT("pa_wasm_preset_size") int pa_wasm_preset_size(void) { return (int)sizeof(pa_preset_t); }

/* Copy a slot's record out. 0 on success. */
WASM_EXPORT("pa_wasm_preset_get")
int pa_wasm_preset_get(uint8_t idx, uint8_t *out) {
    pa_preset_t p;
    if (!out) return -1;
    if (pa_preset_load(idx, &p) != 0) return -2;
    memcpy(out, &p, sizeof(p));
    return 0;
}

/* Write a slot's record back. The record's own CRC32 does not have to be
 * right: the firmware recomputes it (and the magic, and the sequence). */
WASM_EXPORT("pa_wasm_preset_put")
int pa_wasm_preset_put(uint8_t idx, const uint8_t *in) {
    if (!in) return -1;
    pa_preset_t p;
    memcpy(&p, in, sizeof(p));
    p.magic = PA_PRESET_MAGIC;
    p.seq = 0;
    return pa_preset_save(idx, &p);
}

/* Store the sound that is playing right now into a slot - the workbench's
 * "save this preset". The volume is the one the firmware shows. */
WASM_EXPORT("pa_wasm_preset_capture")
int pa_wasm_preset_capture(uint8_t idx, const char *name) {
    if (idx >= PA_PRESET_COUNT) return -1;
    pa_preset_t p;
    memset(&p, 0, sizeof(p));
    /* The chain and the UI's knobs are what the firmware would store. */
    pa_preset_capture_from_chain(&p, &g_fx_chain, (uint8_t)g_ui.preset_volume,
                                  g_ui.slot_params, g_ui.slot_type);
    if (name) {
        for (int i = 0; i < (int)sizeof(p.name) - 1 && name[i]; i++) p.name[i] = name[i];
    }
    return pa_preset_save(idx, &p);
}

/* Mark a slot empty again (the firmware's "EMPTY" state). */
WASM_EXPORT("pa_wasm_preset_clear")
int pa_wasm_preset_clear(uint8_t idx) {
    if (idx >= PA_PRESET_COUNT) return -1;
    pa_preset_t p;
    if (pa_preset_load(idx, &p) != 0) return -2;
    p.flags |= PA_PRESET_FLAG_EMPTY;
    return pa_preset_save(idx, &p);
}

/* ===================================================================== */
/* 6b. Audio scratch buffers - the page writes the input and reads the two
 *     outputs through these, so the host never has to guess the layout. */
/* ===================================================================== */
#define WASM_SCRATCH_FRAMES 4096
static float s_scratch_in[WASM_SCRATCH_FRAMES];
static float s_scratch_outL[WASM_SCRATCH_FRAMES];
static float s_scratch_outR[WASM_SCRATCH_FRAMES];

/* ---- loading a NAM capture --------------------------------------------------
 * The browser has the same firmware API the emulator uses: the host parses the
 * .nam, hands the model over, and the firmware registers it, stores it in the
 * flash and puts it in the DSP. So a capture loaded in the workbench is a
 * REAL capture in the flash image, and the FLASH tab can export it to the
 * device.
 *
 * (The SysEx path cannot be used for this: the M-VAVE framing is 7-bit, so a
 * binary model blob would be corrupted on the way through.) */
WASM_EXPORT("pa_wasm_add_nam")
int pa_wasm_add_nam(const uint8_t *blob, uint32_t len, const char *name) {
    if (!blob || len == 0u) return -1;
    pa_user_nam_meta_t meta;
    meta.input_trim = 1.0f;
    meta.makeup_db  = 0.0f;
    for (int k = 0; k < 6; k++) meta.amp_knobs[k] = 50;
    meta.flags = 0;

    uint8_t  type = 0;        /* 0 = the 3852 B Q21 GRU struct */
    uint8_t  has_cab = 0;
    float    head_scale = 1.0f;
    if (len >= 12u && blob[0] == 'W' && blob[1] == 'A' &&
        blob[2] == 'V' && blob[3] == 'E') {
        type = 1;             /* 1 = a WaveNet float container */
        uint32_t count = 0;
        uint32_t flags = 0;
        float input_trim = 1.0f;
        memcpy(&count, blob + 4, 4);
        memcpy(&head_scale, blob + 8, 4);
        if (len >= 20u + count * sizeof(float)) {
            memcpy(&flags, blob + 12, 4);
            memcpy(&input_trim, blob + 16, 4);
        } else if (len >= 16u + count * sizeof(float)) {
            memcpy(&flags, blob + 12, 4);
        }
        has_cab = (flags & 1u) ? 1 : 0;
        meta.flags = (uint8_t)flags;
        if (input_trim > 0.001f) meta.input_trim = input_trim;
    }
    int idx = pa_user_nam_add((name && name[0]) ? name : "USER NAM", type, 0, has_cab,
                              head_scale, blob, len, &meta);
    if (idx < 0) return idx;

    /* Verify load into DSP */
    int load_rc = pa_user_nam_load_into_dsp((uint8_t)idx, &g_nam_model, &g_wn_model);
    if (load_rc != 0) {
        pa_user_nam_rollback((uint8_t)idx);
        return load_rc;
    }

    if (has_cab) {
        pa_fx_chain_set_slot_enabled(&g_fx_chain, PA_SLOT_CAB, 0);
    }

    pa_audio_core_select_user_nam(&g_ui, &g_fx_chain, (uint8_t)idx);
    return idx;
}

WASM_EXPORT("pa_wasm_nam_count") int pa_wasm_nam_count(void) { return (int)pa_user_nam_get_count(); }

/* ---- the SysEx upload path -------------------------------------------------
 * The same firmware parser the device has: a cmd-25 upload session, with the
 * inner frame packed 8-to-7-bit. The browser build has no USB MIDI, but the
 * parser is the firmware's, so this is how the WASM build is checked against
 * it (custom-fw/wasm/test_sysex.js). */
extern void pa_sysex_input_byte(uint8_t b);

WASM_EXPORT("pa_wasm_feed_sysex")
void pa_wasm_feed_sysex(const uint8_t *bytes, uint32_t len) {
    if (!bytes) return;
    for (uint32_t i = 0; i < len; i++) pa_sysex_input_byte(bytes[i]);
}

/* Select an amp model the way the effect menu does: the AMP slot's type.
 * 0..19 are the stock models, 20+ are the user captures. The change is
 * applied by the audio thread at the next block boundary. */
WASM_EXPORT("pa_wasm_select_amp")
void pa_wasm_select_amp(uint8_t type) {
    pa_audio_core_request_model(&g_ui, &g_fx_chain, PA_SLOT_AMP, type);
}

WASM_EXPORT("pa_wasm_scratch_in")  float *pa_wasm_scratch_in(void)  { return s_scratch_in; }
WASM_EXPORT("pa_wasm_scratch_outl") float *pa_wasm_scratch_outl(void) { return s_scratch_outL; }
WASM_EXPORT("pa_wasm_scratch_outr") float *pa_wasm_scratch_outr(void) { return s_scratch_outR; }
WASM_EXPORT("pa_wasm_scratch_frames") int pa_wasm_scratch_frames(void) { return WASM_SCRATCH_FRAMES; }

/* ===================================================================== */
/* 6c. The "jack": the firmware's 24-bit audio I/O                        */
/*                                                                       */
/* The browser's audio (on Windows that IS Windows Audio / WASAPI, and it */
/* can be an ASIO interface behind a bridge driver) is converted to 24-bit */
/* at the edge and handed to the firmware as the device's jack input. The */
/* firmware's jack output is converted back to float for the browser.     */
/*                                                                       */
/* So the browser hears exactly what the device would play: the same input */
/* gain, the same master volume slew, the same 1-LSB TPDF dither and the  */
/* same fixed-point arithmetic - because it IS the device's code path.    */
/* The float<->24-bit conversion is here, in the shim, not in the        */
/* firmware: it is a property of the host's audio format, not of the      */
/* device.                                                               */
/* ===================================================================== */
#define WASM_JACK_FRAMES PA_AUDIO_BLOCK_FRAMES
static int32_t s_jack_in[WASM_JACK_FRAMES * 2];   /* 24-bit, stereo interleaved */
static int32_t s_jack_out[WASM_JACK_FRAMES * 2];  /* 24-bit, stereo interleaved */

WASM_EXPORT("pa_wasm_jack_in_ptr")  int32_t *pa_wasm_jack_in_ptr(void)  { return s_jack_in; }
WASM_EXPORT("pa_wasm_jack_out_ptr") int32_t *pa_wasm_jack_out_ptr(void) { return s_jack_out; }
WASM_EXPORT("pa_wasm_jack_frames")  int pa_wasm_jack_frames(void) { return WASM_JACK_FRAMES; }

/* Run one 64-frame jack quantum - the DEVICE's audio I/O path, verbatim. */
WASM_EXPORT("pa_wasm_audio_process")
void pa_wasm_audio_process(void) {
    pa_audio_core_process_half(s_jack_in, s_jack_out);
}

/* The drum engine's absolute step (0..PA_DRUM_MAX_STEPS-1, 48 per 4/4 bar). The Grove
 * Station render uses it to put the playhead in a chosen bar: the screen must not depend
 * on which bar of the loop it is ("the bar goes away after the 4th" bug). */
WASM_EXPORT("pa_wasm_drum_step")
uint32_t pa_wasm_drum_step(void) { return pa_drums_sounding_step(&g_drums); }   /* the step being heard */

/* ---- the board services the portable core needs -------------------------- */
static void     bsp_feed_wdt(void) { }
static int      bsp_battery_mv(void) { return 4100; }
static int      bsp_battery_brownout(void) { return 0; }
static void     bsp_set_backlight(uint8_t pct) { (void)pct; }
static uint32_t bsp_pool_used(void) { return pa_pool_used_bytes(); }
static uint32_t bsp_now_ms(void) { return s_now_ms; }
static const pa_board_services_t g_bsp = {
    bsp_feed_wdt, bsp_battery_mv, bsp_battery_brownout,
    bsp_set_backlight, bsp_pool_used, bsp_now_ms
};

/* ===================================================================== */
/* 7. Exported API - the browser drives the firmware through these          */
/* ===================================================================== */

WASM_EXPORT("pa_wasm_init")
int pa_wasm_init(void) {
    pa_stock_assets_init();
    pa_hw_accel_init();
    cpu_meter_init(&g_cpu_meter, PA_CPU_HZ, PA_BLOCK_N, PA_SAMPLE_RATE);

    fx_node_t *amp_nd = pa_amp_node_create(&g_nam_model, &g_wn_model, &g_active_amp_engine);
    g_cab_node = fx_cab_ir_new();
    pa_fx_chain_init(&g_fx_chain, amp_nd, g_cab_node);
    g_fx_chain.meter = &g_cpu_meter;

    pa_ui_init(&g_ui);
    pa_drums_init(&g_drums);
    pa_usb_ble_init(&g_usb_ble);

    /* The DEVICE's boot sequence (store, user NAM registry, settings, the
     * last-used preset) - the same code main.c runs. */
    pa_audio_core_boot(&g_fx_chain, &g_ui, &g_usb_ble, &g_drums, &g_cpu_meter, &g_bsp);
    /* Bind the runtime the portable core's jack path runs on, and give the
     * jack its power-on state (unity input gain, the UI's master level). */
    pa_audio_core_bind(&g_fx_chain, &g_ui, &g_drums, &g_usb_ble);
    pa_audio_core_init_jack(&g_ui);
    return 0;
}

WASM_EXPORT("pa_wasm_set_buttons") void pa_wasm_set_buttons(uint32_t b) { s_buttons = (uint8_t)b; }



WASM_EXPORT("pa_wasm_add_enc")    void pa_wasm_add_enc(int32_t d) { s_enc += d; }
WASM_EXPORT("pa_wasm_set_pot")    void pa_wasm_set_pot(int ch, uint16_t v) { if (ch >= 0 && ch < 4) s_pots[ch] = v; }

/* Advance the simulated clock and run the firmware's 60 Hz tick.
 * This is the DEVICE's tick (pa_audio_core_tick), not a copy of it, so the
 * browser gets the same input handling, timers, drum sync, auto-dim, settings
 * persistence and MASTER pot behaviour as the hardware. */
WASM_EXPORT("pa_wasm_tick")
void pa_wasm_tick(uint32_t dt_ms) {
    s_now_ms += dt_ms;
    s_buttons_service();
    pa_audio_core_tick(&g_fx_chain, &g_ui, &g_drums, &g_usb_ble, &g_cpu_meter, &g_bsp,
                       plat_buttons(), plat_enc_delta(), s_pots);
}

/* A float convenience wrapper around the jack, for callers that do not want
 * to deal with 24-bit samples (the page's own tests, the headless probes).
 * It still goes through the firmware's jack, so it is bit-identical to the
 * device. */
WASM_EXPORT("pa_wasm_process")
void pa_wasm_process(const float *in_mono, float *out_L, float *out_R, int nframes) {
    int offset = 0;
    while (offset < nframes) {
        int chunk = nframes - offset;
        if (chunk > WASM_JACK_FRAMES) chunk = WASM_JACK_FRAMES;

        /* float -> the 24-bit jack input (the guitar is on the Left slot,
         * exactly as on the device) */
        for (int i = 0; i < WASM_JACK_FRAMES; i++) {
            float x = (i < chunk && in_mono) ? in_mono[offset + i] : 0.0f;
            if (x >  1.5f) x =  1.5f;
            if (x < -1.5f) x = -1.5f;
            int32_t s24 = (int32_t)(x * 8388607.0f);
            if (s24 >  0x007FFFFF) s24 =  0x007FFFFF;
            if (s24 < -0x00800000) s24 = -0x00800000;
            s_jack_in[2 * i + 0] = s24 & 0x00FFFFFF;
            s_jack_in[2 * i + 1] = 0;
        }

        pa_audio_core_process_half(s_jack_in, s_jack_out);

        /* the 24-bit jack output -> float */
        for (int i = 0; i < chunk; i++) {
            int32_t l24 = (s_jack_out[2 * i + 0] << 8) >> 8;
            int32_t r24 = (s_jack_out[2 * i + 1] << 8) >> 8;
            float l = (float)l24 / 8388608.0f;
            float r = (float)r24 / 8388608.0f;
            if (l >  1.0f) l =  1.0f;
            if (l < -1.0f) l = -1.0f;
            if (r >  1.0f) r =  1.0f;
            if (r < -1.0f) r = -1.0f;
            if (out_L) out_L[offset + i] = l;
            if (out_R) out_R[offset + i] = r;
        }
        offset += chunk;
    }
}

/* ---- telemetry ---------------------------------------------------------- */
typedef struct {
    uint32_t view, preset_index, preset_volume, master_volume;
    uint32_t selected_slot, slot_active_mask;
    uint32_t sram_used, sram_peak, cpu_permille;
    uint32_t tuner_active, tuner_note, tuner_cents;
    uint32_t drum_playing, drum_bpm, drum_pattern;
    uint32_t bt_state, in_peak, out_peak;
    uint32_t cursor_slot, editor_cursor, view2;
} wasm_telemetry_t;

static wasm_telemetry_t s_tel;
WASM_EXPORT("pa_wasm_telemetry_ptr") uint8_t *pa_wasm_telemetry_ptr(void) { return (uint8_t *)&s_tel; }

WASM_EXPORT("pa_wasm_update_telemetry")
void pa_wasm_update_telemetry(void) {
    s_tel.view          = (uint32_t)g_ui.view;
    s_tel.preset_index  = g_ui.preset_index;
    s_tel.preset_volume = g_ui.preset_volume;
    s_tel.master_volume = g_ui.master_volume;
    s_tel.selected_slot = g_ui.selected_slot;
    s_tel.sram_used     = g_ui.sram_used_bytes;
    s_tel.sram_peak     = pa_pool_used_bytes();
    s_tel.cpu_permille  = g_ui.cpu_total_permille;
    s_tel.tuner_active  = g_ui.tuner.active;
    s_tel.tuner_note    = (uint32_t)g_ui.tuner.note_index;
    s_tel.tuner_cents   = (uint32_t)(g_ui.tuner.cents + 128);
    s_tel.drum_playing  = g_ui.drum_playing;
    s_tel.drum_bpm      = g_ui.drum_bpm;
    s_tel.drum_pattern  = g_ui.drum_pattern;
    s_tel.bt_state      = g_ui.bt_state;
    s_tel.in_peak       = g_ui.in_peak_pct;
    s_tel.out_peak      = g_ui.out_peak_pct;
    uint32_t mask = 0;
    for (int s = 0; s < PA_SLOT_COUNT; s++) {
        if (g_fx_chain.slots[s] && g_fx_chain.slots[s]->active) mask |= (1u << s);
    }
    s_tel.slot_active_mask = mask;
    s_tel.cursor_slot   = g_ui.cursor_slot_idx;
    s_tel.editor_cursor = (uint32_t)(int32_t)g_ui.editor_cursor;
}
