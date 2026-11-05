/*
 * custom-fw/emu/emu_bridge.c — Pocket Amp Hardware-in-the-Loop (HIL) Native C Emulator Bridge
 *
 * Compiles the actual Pocket Amp custom firmware C modules into a high-speed shared library
 * (pocket_amp_emu.dll) for:
 *   - Real-time 44.1 kHz ASIO audio streaming (ASIO In -> Pocket Amp DSP -> ASIO Out)
 *   - Pixel-accurate ST7789/ST7735S RGB565 LCD framebuffer capture -> HTML5 Canvas
 *   - Live physical button / chord combo / PARA rotary encoder / expression pot simulation
 *   - Hot-loading of .nam (Neural Amp Modeler) models into the 16-unit Q21 GRU engine
 *   - Hot-loading of .wav Cabinet IRs into the Partitioned Q18 Hardware FIR Convolver (0x01EE0400)
 *   - Live per-stage CPU cycle telemetry & C7 Dynamic DSP SRAM Pool Reclaim inspection
 *     (the pool is PA_DSP_POOL_BYTES = 147,456 B, the same budget the firmware has)
 */

#include <stdint.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <math.h>
#ifdef _WIN32
#include <windows.h>
#else
#include <time.h>
#endif

/* ---- the firmware's clock -------------------------------------------------
 * On the device ONE hardware clock drives everything: the 44.1 kHz sample
 * clock, the 60 Hz tick interrupt and the contact debounce. The emulator has
 * to do the same, or the debounce and the tick disagree - which is exactly
 * what happened when the tick was clocked by the audio block count while the
 * debounce used the host's wall clock: a press that never completed its
 * 20 ms debounce, i.e. no buttons at all.
 *
 * So the emulator counts the samples it has processed. 64 frames at 44.1 kHz
 * is 1451 us, and 11 blocks are 15.96 ms - the firmware's 60 Hz tick. When
 * no audio is running, the host's clock is folded in instead, so the timers
 * keep running. */
static uint32_t s_emu_ms = 0;      /* the firmware's millisecond clock */
static uint32_t s_emu_us = 0;      /* the sub-millisecond remainder     */

static uint32_t emu_host_ms(void) {
#ifdef _WIN32
    return (uint32_t)GetTickCount();
#else
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint32_t)(ts.tv_sec * 1000 + ts.tv_nsec / 1000000);
#endif
}

uint32_t plat_ms(void) { return s_emu_ms; }

#include "../include/pa_config.h"
#include "../include/pa_plat.h"
#include "../include/pa_hw_accel.h"
#include "../include/pa_model.h"
#include "../include/pa_cab.h"
#include "../include/pa_fx.h"
#include "../include/pa_ui.h"
#include "../include/pa_usb_ble.h"
#include "../include/pa_drums.h"
#include "../include/pa_preset.h"
#include "../include/pa_sysex.h"
#include "../include/pa_ota.h"
#include "../include/pa_diag.h"
#include "../include/pa_spi_nor.h"
#include "../include/pa_cpu_meter.h"
#include "../include/pa_wavenet.h"
#include "../include/pa_audio_core.h"
#include "../include/pa_stock_assets.h"
#include "../include/pa_user_nam.h"
#include "../src/dsp/fixedpoint.h"

#ifdef _WIN32
#define EMU_EXPORT __declspec(dllexport)
#else
#define EMU_EXPORT __attribute__((visibility("default")))
#endif

#define LCD_W 240
#define LCD_H 240

/* Firmware Global Instances */
pa_fx_chain_t       g_fx_chain;
pa_nam_model_t      g_nam_model __attribute__((aligned(32)));
pa_wavenet_model_t  g_wn_model __attribute__((aligned(32)));
int                 g_active_amp_engine = 0; /* 0: GRU, 1: WaveNet */
static fx_node_t        g_nam_node;
static fx_node_t       *g_cab_node = 0;
static pa_ui_ctx_t      g_ui;
static pa_usb_ble_ctx_t g_usb_ble;
static pa_drums_t       g_drums;
static pa_cpu_meter_t   g_cpu_meter;

/* Emulated LCD RGB565 Framebuffer (160x80 logical UI resolution) */
static uint16_t g_lcd_fb[LCD_W * LCD_H];

/* C7 Dynamic SRAM Pool Tracker. The device's pool is PA_DSP_POOL_BYTES
 * (147,456 B, src/sys/pa_pool.c); the emulator allocates on the host heap but
 * counts the same bytes, so the badge and the firmware agree. */
static int32_t g_sram_cur  = 0;
static int32_t g_sram_peak = 0;

/* Audio Peak & Oscilloscope Telemetry */
static float   g_in_peak   = 0.0f;
static float   g_out_peak  = 0.0f;
static float   g_scope_in[256];
static float   g_scope_out[256];
static int     g_scope_wr  = 0;
static float   g_dc_x1     = 0.0f;
static float   g_dc_y1     = 0.0f;
static void emu_adapter_reset(void);

void *pa_alloc(uint32_t n) {
    uint32_t aligned = (n + 7u) & ~7u;
    uint32_t *p = (uint32_t *)calloc(1, aligned + 8u);
    if (!p) return 0;
    p[0] = aligned;
    g_sram_cur += (int32_t)aligned;
    if (g_sram_cur > g_sram_peak) g_sram_peak = g_sram_cur;
    g_ui.sram_used_bytes = (uint32_t)g_sram_cur;
    return (void *)(p + 2);
}

void pa_free(void *q) {
    if (!q) return;
    uint32_t *p = ((uint32_t *)q) - 2;
    g_sram_cur -= (int32_t)p[0];
    if (g_sram_cur < 0) g_sram_cur = 0;
    g_ui.sram_used_bytes = (uint32_t)g_sram_cur;
    free(p);
}

/* Intercept ST7789/ST7735S rectangle fills into g_lcd_fb */
void st7735s_fill_rect(uint16_t x, uint16_t y, uint16_t w, uint16_t h, uint16_t rgb565) {
    for (uint16_t r = 0; r < h; r++) {
        uint16_t py = (uint16_t)(y + r);
        if (py >= LCD_H) break;
        for (uint16_t c = 0; c < w; c++) {
            uint16_t px = (uint16_t)(x + c);
            if (px >= LCD_W) break;
            g_lcd_fb[py * LCD_W + px] = rgb565;
        }
    }
}

/* 5x7 Font Renderer (shared with ui_5x7.c) */
extern void ui_draw_char(uint16_t x, uint16_t y, char c, uint16_t fg, uint16_t bg);
extern void ui_draw_str(uint16_t x, uint16_t y, const char *s, uint16_t fg, uint16_t bg);

/* The AMP tone stack, the per-model voicing and the per-model output trim now
 * live in the portable module src/sys/pa_audio_core.c - the SAME code the
 * device runs. The emulator used to carry its own copy at 120 / 700 / 3 kHz /
 * 5.5 kHz while the device used 300 / 2 kHz / 4 kHz / 5 kHz, so the two never
 * sounded the same. */

/* NAM Slot 2 Wrapper - the DEVICE's implementation, from pa_audio_core.c. */

static fx_node_t *nam_slot_init(pa_nam_model_t *m) {
    /* The DEVICE's AMP node (same tone stack, same per-model trim, same Q21
     * arithmetic) - see src/sys/pa_audio_core.c. */
    return pa_amp_node_create(m, &g_wn_model, &g_active_amp_engine);
}

/* Build a realistic default tube overdrive GRU profile on init */
static void init_rich_tube_gru(pa_nam_model_t *m, float drive, float asym, float warmth) {
    pa_nam_init_default(m);
    const float q21 = (float)(1 << PA_NAM_Q_SHIFT);
    for (int i = 0; i < PA_NAM_HIDDEN; i++) {
        float sign = (i & 1) ? -1.0f : 1.0f;
        float decay = 0.25f + 0.65f * ((float)i / (float)PA_NAM_HIDDEN) * warmth;
        float w_in_f = drive * (1.4f / (1.0f + 0.18f * (float)i)) * sign;
        float w_out_f = (0.38f / sqrtf((float)(i + 1))) * sign;

        m->w_in[i]  = (int32_t)(w_in_f * q21);
        m->w_out[i] = (int32_t)(w_out_f * q21);
        m->b_r[i]   = (int32_t)(1.2f * q21);
        m->b_z[i]   = (int32_t)((decay * 3.0f - 1.5f) * q21);
        m->b_ni[i]  = (int32_t)(asym * 0.08f * sign * q21);
        m->b_nh[i]  = 0;

        if (i < PA_NAM_HW_ROWS) {
            m->w_z_hw[i][i] = (int32_t)(decay * q21);
            m->w_r_hw[i][i] = (int32_t)(0.15f * q21);
            m->w_n_hw[i][i] = (int32_t)(0.22f * q21);
            m->w_z_hw[i][16] = (int32_t)(0.10f * q21);
            m->w_r_hw[i][16] = (int32_t)(0.15f * sign * q21);
        } else {
            int r = i - PA_NAM_HW_ROWS;
            m->w_z_cpu[r][i] = (int32_t)(decay * q21);
            m->w_r_cpu[r][i] = (int32_t)(0.15f * q21);
            m->w_n_cpu[r][i] = (int32_t)(0.22f * q21);
            m->w_z_cpu[r][16] = (int32_t)(0.10f * q21);
            m->w_r_cpu[r][16] = (int32_t)(0.15f * sign * q21);
        }
    }
    m->b_out     = 0;
    m->scale_q21 = (int32_t)(1.15f * q21);
    m->active    = 1;
}

static uint32_t g_emu_cycles = 0;
static uint32_t emu_cycle_clock(void) {
    return g_emu_cycles;
}

/* The per-slot cycle cost now lives in include/pa_dsp_cost.h and is
 * applied by the chain itself, so the emulator, the WASM build and the device
 * report the same DSP load. */

static void emu_sysex_init(void);
static void emu_ble_init(void);

EMU_EXPORT void emu_init(void) {
    g_sram_cur   = 0;
    g_sram_peak  = 0;
    g_emu_cycles = 0;
    g_dc_x1      = 0.0f;
    g_dc_y1      = 0.0f;
    memset(g_lcd_fb, 0, sizeof(g_lcd_fb));
    emu_adapter_reset();

    pa_hw_accel_init();
    cpu_meter_init(&g_cpu_meter, PA_CPU_HZ, PA_BLOCK_N, PA_SAMPLE_RATE);
    cpu_meter_set_clock(&g_cpu_meter, emu_cycle_clock);

    fx_node_t *amp_nd = nam_slot_init(&g_nam_model);
    init_rich_tube_gru(&g_nam_model, 1.35f, 0.25f, 0.7f);

    g_cab_node = fx_cab_ir_new();
    pa_fx_chain_init(&g_fx_chain, amp_nd, g_cab_node);
    g_fx_chain.meter = &g_cpu_meter;

    pa_ui_init(&g_ui);
    g_ui.sram_used_bytes = (uint32_t)g_sram_cur;
    /* Push the Settings > Input / Output into the DSP. The device's boot does
     * this (pa_audio_core_boot -> pa_ui_apply_io_settings); the emulator did
     * not, so the chain ran with the DEFAULT input trim, gate threshold and
     * speaker routing until something happened to change the OUTPUT LEVEL -
     * and then the sound jumped. */
    pa_ui_apply_io_settings(&g_ui, &g_fx_chain);
    pa_drums_init(&g_drums);
    pa_usb_ble_init(&g_usb_ble);

    /* The SysEx replies are captured instead of going out over USB - and this
     * has to be AFTER pa_usb_ble_init(), because the USB driver registers the
     * device's real TX path as the reply sink, and the emulator wants the
     * capture instead. */
    emu_sysex_init();
    pa_sysex_init();
    /* The private diagnostic channel reads the emulated SPI NOR, so the flash
     * dump (custom-fw/tools/dump_flash.py) can be exercised without a device. */
    pa_diag_set_flash_reader(pa_spi_nor_read);
    /* The BLE replies are captured as BLE-MIDI 1.0 packets, the way the
     * device would notify them on the MIDI characteristic. */
    emu_ble_init();
    /* Bind the runtime the portable core's jack path runs on and give the
     * jack its power-on state (unity input gain, the UI's master level). */
    pa_audio_core_bind(&g_fx_chain, &g_ui, &g_drums, &g_usb_ble);
    pa_audio_core_init_jack(&g_ui);

    /* Initialize stock factory models, persistent user NAMs, and SPI NOR flash persistence */
    pa_stock_assets_init();
    pa_user_nam_init();
    if (pa_user_nam_get_count() > 0) {
        g_ui.user_nam_loaded = 1;
        strncpy(g_ui.user_nam_name, pa_user_nam_get_name(0), sizeof(g_ui.user_nam_name) - 1);
    }
    pa_preset_init();
    pa_preset_t initial_p;
    if (pa_preset_load(g_ui.preset_index, &initial_p) == 0) {
        pa_preset_apply_to_chain(&initial_p, &g_fx_chain);
        g_ui.preset_volume = initial_p.preset_vol;
        g_ui.master_volume = initial_p.preset_vol;
        /* Load corresponding factory stock amp model or user NAM capture & cabinet IR */
        if (initial_p.amp_model_idx < PA_STOCK_AMP_COUNT) {
            pa_stock_load_amp(initial_p.amp_model_idx, &g_nam_model, NULL);
            g_active_amp_engine = 0;
        } else {
            pa_user_nam_load_into_dsp(initial_p.amp_model_idx - PA_STOCK_AMP_COUNT, &g_nam_model, &g_wn_model);
        }
        g_nam_node.active   = 1;
        int32_t c_taps[PA_STOCK_CAB_TAPS];
        uint32_t n_taps = PA_STOCK_CAB_TAPS;
        if (pa_stock_load_cab(initial_p.cab_model_idx, c_taps, &n_taps) == 0) {
            fx_cab_ir_load_q18(g_cab_node, c_taps, n_taps);
            g_cab_node->active = 1;
        }
    }
    pa_fx_chain_set_preset_vol(&g_fx_chain, g_ui.preset_volume);

    g_ui.dirty = 1;
    pa_ui_render(&g_ui, &g_fx_chain, &g_cpu_meter);
}

/* ---- the jack: the firmware's 24-bit audio I/O ----------------------------
 * The emulator does NOT run its own audio loop. It converts the host's
 * float samples to 24-bit at the edge and hands them to the firmware's jack
 * (src/sys/pa_audio_core.c), which is the DEVICE's audio I/O path verbatim:
 * the same input gain, the same master volume slew, the same 1-LSB TPDF
 * dither and the same fixed-point arithmetic. The float<->24-bit conversion
 * is the only host-specific part, and it lives here, in the shim. */
static int32_t s_emu_jack_in[PA_BLOCK_N * 2];
static int32_t s_emu_jack_out[PA_BLOCK_N * 2];

/* The tuner's input level meter (0..7 dots), read from the 24-bit jack
 * input - the device reads it from the ADC, the emulator from the host's
 * samples, but it is the same number. */
static void update_input_dots(const int32_t *jack_in) {
    int32_t max_abs = 0;
    for (int i = 0; i < PA_BLOCK_N; i++) {
        int32_t s24 = (jack_in[2 * i] << 8) >> 8;
        int32_t a = s24 < 0 ? -s24 : s24;
        if (a > max_abs) max_abs = a;
    }
    uint8_t dots = 0;
    if (max_abs > (0x007FFFFF / 256)) dots = 1;
    if (max_abs > (0x007FFFFF / 128)) dots = 2;
    if (max_abs > (0x007FFFFF / 64))  dots = 3;
    if (max_abs > (0x007FFFFF / 32))  dots = 4;
    if (max_abs > (0x007FFFFF / 16))  dots = 5;
    if (max_abs > (0x007FFFFF / 8))   dots = 6;
    if (max_abs > (0x007FFFFF / 4))   dots = 7;
    g_ui.tuner.input_level_dots = dots;
}

/* The firmware's 60 Hz tick (defined below, next to the input latch). */
static void emu_tick(void);

/* One firmware block: exactly PA_BLOCK_N samples in, PA_BLOCK_N out. The device
 * never sees anything else, so the emulator must not either. */
static void emu_run_block(const float *in_mono, float *out_L, float *out_R,
                          float *in_peak, float *out_peak) {
    /* float -> the 24-bit jack input (the guitar is on the Left slot, exactly as on
     * the device) */
    for (int i = 0; i < PA_BLOCK_N; i++) {
        float raw_x = in_mono ? in_mono[i] : 0.0f;
        /* 1-pole DC blocker (~7 Hz @ 44.1 kHz): removes interface DC offset completely */
        float x = raw_x - g_dc_x1 + 0.999f * g_dc_y1;
        g_dc_x1 = raw_x;
        g_dc_y1 = x;

        float ax = x < 0.0f ? -x : x;
        if (ax > *in_peak) *in_peak = ax;
        if (x >  1.5f)  x =  1.5f;
        if (x < -1.5f)  x = -1.5f;

        int32_t s24 = (int32_t)(x * 8388607.0f);
        if (s24 >  0x007FFFFF) s24 =  0x007FFFFF;
        if (s24 < -0x00800000) s24 = -0x00800000;
        s_emu_jack_in[2 * i + 0] = s24 & 0x00FFFFFF;
        s_emu_jack_in[2 * i + 1] = 0;
    }

    update_input_dots(s_emu_jack_in);

    /* the DEVICE's audio I/O path, verbatim */
    pa_audio_core_process_half(s_emu_jack_in, s_emu_jack_out);
    emu_tick();          /* the firmware's 60 Hz tick, clocked by the host */

    /* the 24-bit jack output -> float */
    for (int i = 0; i < PA_BLOCK_N; i++) {
        int32_t l24 = (s_emu_jack_out[2 * i + 0] << 8) >> 8;
        int32_t r24 = (s_emu_jack_out[2 * i + 1] << 8) >> 8;
        float l = (float)l24 / 8388608.0f;
        float r = (float)r24 / 8388608.0f;
        if (l >  1.0f) l =  1.0f;
        if (l < -1.0f) l = -1.0f;
        if (r >  1.0f) r =  1.0f;
        if (r < -1.0f) r = -1.0f;

        float ay = l < 0.0f ? -l : l;
        if (ay > *out_peak) *out_peak = ay;

        out_L[i] = l;
        out_R[i] = r;

        g_scope_in[g_scope_wr]  = in_mono ? in_mono[i] : 0.0f;
        g_scope_out[g_scope_wr] = l;
        g_scope_wr = (g_scope_wr + 1) & 255;
    }
}

/* ---- the block adapter ------------------------------------------------------
 *
 * The firmware processes 64-sample blocks; the host hands over whatever its audio
 * API delivers. The old loop cut a call into 64-sample chunks and, for the last
 * chunk of a call that was NOT a multiple of 64, still ran a full 64-sample block
 * with the missing input padded with zeros and threw the extra output away. The
 * DSP then moved 64 samples on while the stream moved `chunk`: zeros spliced into
 * the signal and the output cut short, on every call - a steady crackle whenever
 * the host's frame count is not a multiple of 64 (a 48 kHz interface through the
 * resampler always; ASIO buffers like 441 or 480; "native" WASAPI/MME blocks).
 * Measured: 100-frame calls differed from 64-frame calls by up to 0.4 full scale,
 * with thousands of discontinuities in 4 s.
 *
 * Now: while every call is a multiple of 64, blocks run straight through, with no
 * added latency (the tests and the effect matrix depend on that). The first call
 * that is not, switches to a 64-sample input queue and an output queue primed with
 * 64 samples of silence. From then on the firmware only ever sees whole blocks and
 * every call returns exactly as many samples as it was given. (Queue invariant:
 * queued output + pending input == 64 at every call boundary, so the output queue
 * can never run dry.) The cost is a constant 64-sample (1.45 ms) delay, in that
 * mode only. */
#define EMU_MAX_CALL   4096
#define EMU_OUTQ_LEN   (EMU_MAX_CALL + 2 * PA_BLOCK_N)
static int   s_adapt = 0;                 /* 1 once a non-multiple-of-64 call was seen */
static float s_inq[PA_BLOCK_N];
static int   s_inq_n = 0;
static float s_outq_L[EMU_OUTQ_LEN], s_outq_R[EMU_OUTQ_LEN];
static int   s_outq_rd = 0, s_outq_n = 0;
static uint32_t s_outq_underruns = 0;     /* must stay 0; exported for the tests */

static void outq_push(const float *l, const float *r, int n) {
    for (int i = 0; i < n; i++) {
        int w = (s_outq_rd + s_outq_n) % EMU_OUTQ_LEN;
        s_outq_L[w] = l[i];
        s_outq_R[w] = r[i];
        s_outq_n++;
    }
}

/* Switch to the queued mode, once: prime the output with one block of silence. */
static void emu_adapter_enter(void) {
    static const float zeros[PA_BLOCK_N];
    s_adapt = 1;
    s_inq_n = 0;
    s_outq_rd = 0;
    s_outq_n = 0;
    outq_push(zeros, zeros, PA_BLOCK_N);
}

static void emu_adapter_reset(void) {
    s_adapt = 0;
    s_inq_n = 0;
    s_outq_rd = 0;
    s_outq_n = 0;
    s_outq_underruns = 0;
}

EMU_EXPORT uint32_t emu_get_block_adapter_state(void) {
    /* bit 0: the adapter is active; bits 8..: output-queue underruns (must be 0) */
    return (uint32_t)s_adapt | (s_outq_underruns << 8);
}

static void emu_process_slice(const float *in_mono, float *out_L, float *out_R, int nframes,
                              float *in_peak, float *out_peak) {
    if (!s_adapt && (nframes % PA_BLOCK_N) == 0) {
        for (int off = 0; off < nframes; off += PA_BLOCK_N) {
            float tl[PA_BLOCK_N], tr[PA_BLOCK_N];
            emu_run_block(in_mono ? in_mono + off : NULL, tl, tr, in_peak, out_peak);
            if (out_L) memcpy(out_L + off, tl, sizeof tl);
            if (out_R) memcpy(out_R + off, tr, sizeof tr);
        }
        return;
    }
    if (!s_adapt) emu_adapter_enter();
    for (int i = 0; i < nframes; i++) {
        s_inq[s_inq_n++] = in_mono ? in_mono[i] : 0.0f;
        if (s_inq_n == PA_BLOCK_N) {
            float tl[PA_BLOCK_N], tr[PA_BLOCK_N];
            emu_run_block(s_inq, tl, tr, in_peak, out_peak);
            outq_push(tl, tr, PA_BLOCK_N);
            s_inq_n = 0;
        }
    }
    for (int i = 0; i < nframes; i++) {
        float l = 0.0f, r = 0.0f;
        if (s_outq_n > 0) {
            l = s_outq_L[s_outq_rd];
            r = s_outq_R[s_outq_rd];
            s_outq_rd = (s_outq_rd + 1) % EMU_OUTQ_LEN;
            s_outq_n--;
        } else {
            s_outq_underruns++;           /* cannot happen (see the invariant) */
        }
        if (out_L) out_L[i] = l;
        if (out_R) out_R[i] = r;
    }
}

/*
 * Real-Time Float32 Audio Callback Entrypoint: any frame count.
 */
EMU_EXPORT void emu_process_audio_f32(const float *in_mono, float *out_L, float *out_R, int nframes) {
    float blk_in_peak  = 0.0f;
    float blk_out_peak = 0.0f;

    /* Decide the mode from the WHOLE call, before it is cut into slices: a 5,000-frame
     * call must not run its first 4,096 frames straight through and then switch modes
     * (and insert the priming silence) in the middle of the call. */
    if (!s_adapt && nframes > 0 && (nframes % PA_BLOCK_N) != 0) emu_adapter_enter();

    for (int off = 0; off < nframes; off += EMU_MAX_CALL) {
        int n = nframes - off;
        if (n > EMU_MAX_CALL) n = EMU_MAX_CALL;
        emu_process_slice(in_mono ? in_mono + off : NULL, out_L ? out_L + off : NULL,
                          out_R ? out_R + off : NULL, n, &blk_in_peak, &blk_out_peak);
    }

    g_in_peak  = (blk_in_peak  > g_in_peak * 0.90f) ? blk_in_peak  : (g_in_peak * 0.90f);
    g_out_peak = (blk_out_peak > g_out_peak * 0.90f) ? blk_out_peak : (g_out_peak * 0.90f);
    if (isnan(g_in_peak) || isinf(g_in_peak) || g_in_peak < 0.0f) g_in_peak = 0.0f;
    if (isnan(g_out_peak) || isinf(g_out_peak) || g_out_peak < 0.0f) g_out_peak = 0.0f;
}

/* ---- the 60 Hz firmware tick ----------------------------------------------
 *
 * The emulator used to call pa_ui_handle_inputs() directly and then render
 * the LCD itself. That meant it ran its OWN copy of the UI logic: no contact
 * debounce, no HOLD bits, no chord combos, no battery model, no toast timers,
 * no auto-dim, no settings persistence and no deferred preset swap. The
 * device and the browser ran the real firmware's tick; the emulator ran a
 * paraphrase of it.
 *
 * Now the emulator runs the SAME tick - pa_audio_core_tick(), the one the
 * device's main loop runs and the browser's worklet runs - driven by the
 * host's clock. The input layer, the timers and the render all live in the
 * firmware, so the three platforms cannot drift apart any more.
 *
 * The pending input is latched here and consumed by the next tick, exactly
 * as the device latches the GPIO scanner's result. */
static uint32_t s_btn_mask = 0;
static int32_t  s_enc_accum = 0;
/* The front-panel MASTER pot, a 12-bit SARADC (0..4095).
 *
 * It used to start at 2048 - the middle of the range - so the emulator ran
 * the firmware's master volume at 50 %, which on the audio taper (-40 dB at
 * 0 % .. 0 dB at 100 %) is TWENTY DECIBELS of attenuation. The emulator's own
 * UI showed the knob at 100 %, so the panel said one thing and the firmware
 * did another, and the emulator was that much quieter than the browser build
 * (the WASM's pot starts at full scale) and than the device's factory
 * default (the settings' OUTPUT LEVEL is 100 %).
 *
 * A real unit has a pot somebody turned; the emulator's default is the
 * firmware's factory default: full scale. */
static uint16_t s_pot[4] = { 4095u, 4095u, 4095u, 4095u };

/* ---- the host-facing button API --------------------------------------------
 *
 * The firmware's input layer (src/sys/pa_input.c) turns the RAW LEVEL of the
 * front-panel buttons into one-tick action pulses, and it decides a PARA or a
 * HOME CLICK from the RELEASE edge - a button that is never released never
 * clicks and never holds.
 *
 * The emulator used to expose a single LEVEL setter and the web UI never
 * released anything, so half of the front panel was dead:
 *   - HOME and PARA clicks never fired (they are emitted on the release), so
 *     HOME/Settings and the encoder click did nothing at all;
 *   - the HOLD actions were unreachable: Live Stomp Lock (HOME 1.5 s), Save to
 *     (PARA 1 s), Moving mode (PARA 2 s) and Factory Reset (PARA 3 s);
 *   - a second tap of the same button did nothing (there was no new edge) and
 *     the button stayed latched down, so VOL-/VOL+ auto-repeated forever and a
 *     chord could never be released.
 *
 * This is the same input-device adaptation the WASM shim makes
 * (wasm/pa_wasm.c): a mouse click can be shorter than the firmware's 20 ms
 * contact debounce, so the contact is held for at least
 * PA_EMU_MIN_CONTACT_MS and the firmware's debounce does the rest. */
#define PA_EMU_MIN_CONTACT_MS 60u
/* Watchdog: if the mouse-up never arrives (the pointer left the window, the
 * tab was closed mid-drag) the button would stay latched down forever. */
#define PA_EMU_MAX_CONTACT_MS 8000u

static uint32_t s_press_t[6];    /* when each button went down            */
static uint32_t s_rel_due[6];    /* when a pending release may happen     */
static uint32_t s_tap_mask = 0;  /* the buttons of a complete gesture     */
static uint32_t s_tap_due = 0;   /* when that gesture lets go             */

/* The host clock, and when the AUDIO callback last ran. The audio callback
 * is the firmware's clock while it is running, so the idle path must keep
 * its hands off it (see emu_tick_idle). */
static uint32_t s_last_host_ms = 0;
static uint32_t s_last_audio_host_ms = 0;
static int      s_audio_seen = 0;

/* Service the pending releases. Called from the tick, so the release happens
 * on the firmware's clock and not on the host's. */
static void emu_buttons_service(void) {
    uint32_t now = plat_ms();

    /* The end of a complete gesture (tap / hold): the emulator does the
     * waiting, on the firmware's clock. */
    if (s_tap_mask && (int32_t)(now - s_tap_due) >= 0) {
        s_btn_mask &= ~s_tap_mask;
        s_tap_mask = 0;
        for (int b = 0; b < 6; b++) s_rel_due[b] = 0;
    }

    for (int b = 0; b < 6; b++) {
        uint32_t bit = 1u << b;
        if (!(s_btn_mask & bit)) continue;
        if (s_rel_due[b]) {
            if ((int32_t)(now - s_rel_due[b]) >= 0) {
                s_btn_mask &= ~bit;
                s_rel_due[b] = 0;
            }
        } else if ((uint32_t)(now - s_press_t[b]) >= PA_EMU_MAX_CONTACT_MS) {
            s_btn_mask &= ~bit;      /* the mouse-up was lost */
        }
    }
}

static void     bsp_feed_wdt(void)          { }
static int      g_emu_batt_adc = 620, g_emu_charging = 0;                                  /* the stock method: the averaged SARADC channel 9 count, and the USB power state */
EMU_EXPORT void emu_set_battery_adc(int adc)   { g_emu_batt_adc = adc; }
EMU_EXPORT void emu_set_charging(int on)       { g_emu_charging = on ? 1 : 0; }
static int      bsp_battery_mv(void)        { return g_emu_batt_adc * 66 / 10; }
static int      bsp_battery_adc(void)       { return g_emu_batt_adc; }
static int      bsp_charging(void)          { return g_emu_charging; }
static int      bsp_battery_brownout(void)  { return 0; }
static void     bsp_set_backlight(uint8_t pct) { (void)pct; }
static uint32_t bsp_pool_used(void)         { return (uint32_t)g_sram_cur; }
static uint32_t bsp_now_ms(void)            { return plat_ms(); }
static const pa_board_services_t g_bsp = {
    bsp_feed_wdt, bsp_battery_mv, bsp_battery_brownout,
    bsp_set_backlight, bsp_pool_used, bsp_now_ms,
    bsp_battery_adc, bsp_charging
};

/* Run the firmware's tick. */
static void emu_run_tick(void) {
    emu_buttons_service();
    pa_audio_core_tick(&g_fx_chain, &g_ui, &g_drums, &g_usb_ble, &g_cpu_meter, &g_bsp,
                       s_btn_mask, s_enc_accum, s_pot);
    s_enc_accum = 0;
}

/* The emulator's 60 Hz timer, driven by the AUDIO BLOCK COUNT - the same
 * thing the device's timer interrupt does. 64 frames at 44.1 kHz is
 * 1451 us, so a tick every 11 blocks is 15.96 ms. Using the sample clock
 * rather than the host's wall clock keeps the tick deterministic: the
 * emulator behaves the same whether PortAudio is pacing the callback or a
 * test is calling it as fast as it can. */
#define EMU_TICK_BLOCKS 11u
#define EMU_BLOCK_US    (1000000u * PA_BLOCK_N / PA_SAMPLE_RATE)   /* 1451 us */
static uint32_t s_tick_block = EMU_TICK_BLOCKS;

static void emu_tick(void) {
    /* the sample clock advances the firmware's clock */
    s_emu_us += EMU_BLOCK_US;
    while (s_emu_us >= 1000u) { s_emu_us -= 1000u; s_emu_ms++; }
    /* The audio callback is the firmware's clock while it runs, so the idle
     * path must keep its hands off it (see emu_tick_idle). */
    s_last_audio_host_ms = emu_host_ms();
    s_audio_seen = 1;

    if (--s_tick_block) return;
    s_tick_block = EMU_TICK_BLOCKS;
    emu_run_tick();
}

/* The idle path (no audio running): fold the host's clock into the
 * firmware's clock and tick, so the timers keep running.
 *
 * It must ONLY do that when the audio callback is not running. The audio
 * callback IS the firmware's clock when it is running (it advances s_emu_ms
 * by the sample time), and folding the host clock in as well made the
 * firmware's clock run at about twice the speed - every timer, the auto
 * repeat and the HOLD thresholds included. It also swallowed a button
 * contact whole: a tap is held for 60 ms and released by the tick, but an
 * idle tick could advance the clock by 100 ms in between and release the
 * button before the firmware's 20 ms debounce ever saw it - which is why
 * some taps worked and some did nothing at all. */
static void emu_tick_idle(void) {
    uint32_t now = emu_host_ms();
    if (s_audio_seen) {
        /* The audio callback has the last word on the clock while it runs. */
        if ((uint32_t)(now - s_last_audio_host_ms) < 500u) return;
        s_audio_seen = 0;   /* the audio stopped - take the clock back */
    }
    if (s_last_host_ms && (uint32_t)(now - s_last_host_ms) < 100u) return;
    uint32_t dt = s_last_host_ms ? (uint32_t)(now - s_last_host_ms) : 16u;
    s_last_host_ms = now;
    s_emu_ms += dt;
    emu_run_tick();
}

/* Latch the front-panel input for the next tick.
 *
 * This is the LEVEL api: it is what the host tests use (they drive a press
 * and then an explicit release themselves) and it is kept unchanged for them.
 * The web UI uses the press/release api below instead, because the firmware
 * needs the release edge to turn a press into a click or a hold. */
EMU_EXPORT void emu_ui_input(uint32_t buttons_mask, int32_t enc_delta, uint16_t master_pot) {
    /* Stamp the buttons that go down now. Without this s_press_t kept its old
     * value (0 at start), so once the emulated clock passed 8 s the stuck-button
     * watchdog in emu_buttons_service released every new press before the
     * firmware saw it: buttons sent through this call stopped working. */
    uint32_t now = plat_ms();
    for (int b = 0; b < 6; b++) {
        uint32_t bit = 1u << b;
        if ((buttons_mask & bit) && !(s_btn_mask & bit)) {
            s_press_t[b] = now;
            s_rel_due[b] = 0;
        }
    }
    s_tap_mask = 0;               /* a level update supersedes a gesture */
    s_btn_mask = buttons_mask;
    s_enc_accum += enc_delta;
    /* The MASTER pot is a 12-bit SARADC on the device (0..4095), the same
     * range the firmware scales it in. Anything above that means "no pot"
     * (the server sends 0xFFFF when the front panel is not simulated).
     * The old 10-bit limit made the emulator's MASTER knob reach only 25 %. */
    if (master_pot <= 4095u) s_pot[0] = master_pot;
    emu_tick_idle();
}

/* ---- the press / release api the web UI uses ------------------------------
 * Each of these changes ONE thing, so the UI can turn the encoder or the
 * master pot without disturbing the buttons (the old single level setter
 * could not: sending the encoder released the button that was down). */

/* Buttons go DOWN. */
EMU_EXPORT void emu_ui_press(uint32_t mask) {
    uint32_t now = plat_ms();
    s_tap_mask = 0;               /* a manual press supersedes a gesture */
    for (int b = 0; b < 6; b++) {
        uint32_t bit = 1u << b;
        if ((mask & bit) && !(s_btn_mask & bit)) {
            s_press_t[b] = now;
            s_rel_due[b] = 0;
        }
    }
    s_btn_mask |= (mask & 0x3Fu);
    emu_tick_idle();
}

/* Buttons come UP.
 * A contact shorter than the firmware's 20 ms debounce would be lost, so a
 * short press is held until PA_EMU_MIN_CONTACT_MS after it started. */
EMU_EXPORT void emu_ui_release(uint32_t mask) {
    uint32_t now = plat_ms();
    for (int b = 0; b < 6; b++) {
        uint32_t bit = 1u << b;
        if ((mask & bit) && (s_btn_mask & bit)) {
            uint32_t held = now - s_press_t[b];
            if (held < PA_EMU_MIN_CONTACT_MS) {
                s_rel_due[b] = s_press_t[b] + PA_EMU_MIN_CONTACT_MS;
            } else {
                s_btn_mask &= ~bit;
                s_rel_due[b] = 0;
            }
        }
    }
    s_tap_mask &= ~mask;          /* the gesture ends here */
    emu_tick_idle();
}

/* A COMPLETE gesture: press the buttons, hold them for `hold_ms`, release.
 *
 * The browser cannot be trusted with the timing of a HOLD - a background tab
 * throttles its timers, so a 1 s hold can turn into a 2 s hold and select the
 * wrong action ("Save to" instead of "Move block"). The emulator does the
 * waiting, on the firmware's own clock.
 *
 * One request is one gesture, so a press and its release can never be
 * reordered or lost. */
EMU_EXPORT void emu_ui_tap(uint32_t mask, uint32_t hold_ms) {
    uint32_t now = plat_ms();
    uint32_t ms = hold_ms ? hold_ms : PA_EMU_MIN_CONTACT_MS;
    for (int b = 0; b < 6; b++) {
        uint32_t bit = 1u << b;
        if ((mask & bit) && !(s_btn_mask & bit)) {
            s_press_t[b] = now;
            s_rel_due[b] = 0;
        }
    }
    s_btn_mask |= (mask & 0x3Fu);
    s_tap_mask = mask & 0x3Fu;
    s_tap_due = now + ms;
    emu_tick_idle();
}

/* The encoder only - it must not touch the button state. */
EMU_EXPORT void emu_ui_enc(int32_t delta) {
    s_enc_accum += delta;
    emu_tick_idle();
}

/* The MASTER pot only. */
EMU_EXPORT void emu_ui_pot(uint16_t master_pot) {
    if (master_pot <= 4095u) s_pot[0] = master_pot;
    emu_tick_idle();
}

/* Export 160x80 RGB565 framebuffer as RGBA8888 (640 * 80 = 51,200 bytes) */
EMU_EXPORT void emu_get_lcd_rgba(uint8_t *rgba_out) {
    if (!rgba_out) return;
    emu_tick_idle();     /* the UI keeps ticking even when the audio is idle */

    /* The IN/OUT level meters are the firmware's own now: the portable core's
     * tick reads them from g_pa_audio.input_peak_s24 / output_peak_s24, which
     * the jack fills on every platform. The emulator used to have a private
     * copy, computed from its float peaks - which is why the meters were
     * empty on the device and in the browser. */

    /* The LCD is drawn by the firmware's own render, inside its tick. */
    /* Sync real-time drum sequencer step (0..63) for beat LEDs */
    g_ui.drum_step = pa_drums_sounding_step(&g_drums);   /* the step being heard */

    /* DSP SRAM: the emulator's pa_alloc wrapper (above) tracks every byte the
     * effects actually ask for - the same number the device's pool reports
     * through bsp_pool_used(). This used to add up hard-coded per-slot guesses
     * instead (reverb 56,320, delay 8,192, cab 4,096 ...); the real state is
     * 49,384 / 65,624 / 16,720, so the badge drifted further from the truth with
     * every tank change. */
    g_ui.sram_used_bytes = (uint32_t)g_sram_cur;

    /* Dynamic CPU load calculation based on active effect nodes */
    uint32_t active_cpu = 75u; /* ISR + DAC/ADC overhead */
    if (g_fx_chain.slots[PA_SLOT_AMP] && g_fx_chain.slots[PA_SLOT_AMP]->active) active_cpu += 290u;
    if (g_fx_chain.slots[PA_SLOT_CAB] && g_fx_chain.slots[PA_SLOT_CAB]->active) active_cpu += 82u;
    if (g_fx_chain.slots[PA_SLOT_DRIVE] && g_fx_chain.slots[PA_SLOT_DRIVE]->active) active_cpu += 32u;
    if (g_fx_chain.slots[PA_SLOT_DYNAMICS] && g_fx_chain.slots[PA_SLOT_DYNAMICS]->active) active_cpu += 30u;
    if (g_fx_chain.slots[PA_SLOT_EQ] && g_fx_chain.slots[PA_SLOT_EQ]->active) active_cpu += 35u;
    if (g_fx_chain.slots[PA_SLOT_MOD] && g_fx_chain.slots[PA_SLOT_MOD]->active) active_cpu += 47u;
    if (g_fx_chain.slots[PA_SLOT_DELAY] && g_fx_chain.slots[PA_SLOT_DELAY]->active) active_cpu += 52u;
    if (g_fx_chain.slots[PA_SLOT_REVERB] && g_fx_chain.slots[PA_SLOT_REVERB]->active) active_cpu += 112u;
    if (g_drums.playing) active_cpu += 25u;
    g_ui.cpu_total_permille = active_cpu;

    /* Popup & toast timers are serviced via real-time clock in pa_audio_core_tick() */

    g_ui.dirty = 1;
    pa_ui_render(&g_ui, &g_fx_chain, &g_cpu_meter);

    for (int i = 0; i < LCD_W * LCD_H; i++) {
        uint16_t c = g_lcd_fb[i];
        uint8_t r5 = (uint8_t)((c >> 11) & 0x1Fu);
        uint8_t g6 = (uint8_t)((c >> 5)  & 0x3Fu);
        uint8_t b5 = (uint8_t)(c & 0x1Fu);
        rgba_out[i * 4 + 0] = (uint8_t)((r5 << 3) | (r5 >> 2));
        rgba_out[i * 4 + 1] = (uint8_t)((g6 << 2) | (g6 >> 4));
        rgba_out[i * 4 + 2] = (uint8_t)((b5 << 3) | (b5 >> 2));
        rgba_out[i * 4 + 3] = 255u;
    }
}

static uint8_t  s_last_nam_blob[PA_NAM_BLOB_MAX];
static uint32_t s_last_nam_len = 0;
static uint8_t  s_last_nam_type = 0;
static uint8_t  s_last_nam_has_cab = 0;
static uint8_t  s_last_nam_voicing = 0;
static float    s_last_nam_head_scale = 1.0f;

/* Hot-load a 3,852-byte pa_nam_model_t weight struct OR a WAVE container into Slot 2 (AMP) */
EMU_EXPORT int emu_load_nam_struct(const uint8_t *blob, uint32_t len) {
    if (!blob || len < 4u || len > sizeof(s_last_nam_blob)) return -1;

    /* Check for WaveNet container format: Magic 'WAVE' (0x45564157) */
    if (len >= 12u && blob[0] == 'W' && blob[1] == 'A' && blob[2] == 'V' && blob[3] == 'E') {
        uint32_t count = 0;
        float head_scale = 1.0f;
        uint32_t flags = 0;
        float input_trim = 1.0f;
        memcpy(&count, blob + 4, 4);
        memcpy(&head_scale, blob + 8, 4);
        uint32_t data_off = 12u;
        if (len >= 20u + count * sizeof(float)) {
            data_off = 20u;
            memcpy(&flags, blob + 12, 4);
            memcpy(&input_trim, blob + 16, 4);
        } else if (len >= 16u + count * sizeof(float)) {
            data_off = 16u;
            memcpy(&flags, blob + 12, 4);
        }
        if (len >= data_off + count * sizeof(float)) {
            const float *weights = (const float *)(blob + data_off);
            int ret = pa_wavenet_load_weights(&g_wn_model, weights, count, head_scale);
            if (ret != 0) return ret;
            g_wn_model.input_trim = (input_trim > 0.001f) ? input_trim : 1.0f;
            g_wn_model.active   = 1;
            g_wn_model.is_amp_cab = (flags & 1u) ? 1 : 0;
            g_active_amp_engine = 1;
            g_nam_node.active   = 1;
            pa_fx_chain_set_slot_enabled(&g_fx_chain, PA_SLOT_AMP, 1);
            if (flags & 1u) {
                pa_fx_chain_set_slot_enabled(&g_fx_chain, PA_SLOT_CAB, 0);
            }
            /* Cache blob for flash registration */
            memcpy(s_last_nam_blob, blob, len);
            s_last_nam_len        = len;
            s_last_nam_type       = 1;
            s_last_nam_has_cab    = (flags & 1u) ? 1 : 0;
            s_last_nam_head_scale = head_scale;
            g_ui.dirty            = 1;
            return 0;
        }
    }

    /* Standard GRU model blob */
    if (len >= 3852u) {
        memcpy(&g_nam_model, blob, 3852u);
        g_nam_model.scale_q21 = (1 << PA_NAM_Q_SHIFT);
        g_nam_model.active    = 1;
        g_active_amp_engine   = 0;
        g_nam_node.state      = &g_nam_model;
        g_nam_node.active     = 1;
        pa_fx_chain_set_slot_enabled(&g_fx_chain, PA_SLOT_AMP, 1);
        /* Cache blob for flash registration */
        memcpy(s_last_nam_blob, blob, 3852u);
        s_last_nam_len        = 3852u;
        s_last_nam_type       = 0;
        s_last_nam_has_cab    = 0;
        s_last_nam_head_scale = 1.0f;
        g_ui.dirty            = 1;
        return 0;
    }
    return -2;
}

/* Register custom user NAM capture into persistent flash and add to AMP model list */
/* ---- Shims for firmware entry points that live in main.c / audio_alnk.c / gpio_scan.c ----
 * The DLL does not link the device audio path (see Blk-01 note: it should - the emulator's own
 * process_block_64() has no output limiter, no fizz filter, mono only and an extra 0.85 soft
 * saturator, which is why the device-only loudness problems never showed up here).
 * DSP and UI both run under emu_lock, so the mute handshake can complete immediately. */
#include "../include/pa_audio.h"
pa_audio_state_t g_pa_audio;
/* The mute handshake (pa_audio_request_mute / pa_audio_is_muted /
 * pa_audio_release_mute) is NOT defined here: pa_audio_core.c owns that state
 * machine for every platform, and the emulator links the core, so the preset
 * swap fades the same way here as it does on the device. */
void plat_backlight_set(uint8_t pct) { (void)pct; }
void plat_wdt_feed(void) {}
int  plat_battery_is_brownout(void) { return 0; }

/* IMP-01: import metadata computed by nam_loader.py (amp knobs, makeup, input trim) */
static pa_user_nam_meta_t s_last_nam_meta = { 1.0f, 0.0f, { 50, 50, 50, 50, 50, 50 }, 0 };
EMU_EXPORT void emu_set_pending_nam_meta(float input_trim, float makeup_db, const uint8_t *knobs6, uint8_t flags) {
    s_last_nam_meta.input_trim = input_trim;
    s_last_nam_meta.makeup_db  = makeup_db;
    for (int k = 0; k < 6; k++) s_last_nam_meta.amp_knobs[k] = knobs6 ? knobs6[k] : 50u;
    s_last_nam_meta.flags = flags;
}

/* Register a capture and select it, so it comes up at the same level as
 * when it is picked from the effect menu.
 *
 * It returns the AMP ID of the capture (PA_STOCK_AMP_COUNT + the slot), or
 * -1 if it could not be registered. The caller needs the real id: a capture
 * that is loaded a second time OVERWRITES the entry with the same name, so
 * it is not necessarily the last one in the list. The UI used to assume it
 * was and selected the wrong capture. */
EMU_EXPORT int emu_register_user_nam(const char *name) {
    if (!name || s_last_nam_len == 0) return -1;
    int slot = pa_user_nam_add(name, s_last_nam_type, s_last_nam_voicing, s_last_nam_has_cab,
                               s_last_nam_head_scale, s_last_nam_blob, s_last_nam_len, &s_last_nam_meta);
    if (slot >= 0) {
        g_ui.user_nam_loaded = 1;
        strncpy(g_ui.user_nam_name, pa_user_nam_get_name((uint8_t)slot), sizeof(g_ui.user_nam_name) - 1);
        g_ui.user_nam_name[sizeof(g_ui.user_nam_name) - 1] = '\0';
        g_ui.slot_type[PA_SLOT_AMP] = (uint8_t)(PA_STOCK_AMP_COUNT + slot);
        /* Select it through the model's apply path, so it comes up at the
         * same level as when it is picked from the effect menu: that path
         * resets the per-model trim, which a capture carries its own makeup
         * gain for. */
        pa_audio_core_select_user_nam(&g_ui, &g_fx_chain, (uint8_t)slot);
        g_ui.dirty = 1;
        return (int)(PA_STOCK_AMP_COUNT + slot);
    }
    return -1;
}

EMU_EXPORT int emu_get_user_nam_count(void) {
    return (int)pa_user_nam_get_count();
}

EMU_EXPORT const char *emu_get_user_nam_name(int user_idx) {
    return pa_user_nam_get_name((uint8_t)user_idx);
}

EMU_EXPORT int emu_get_total_amp_count(void) {
    return (int)(PA_STOCK_AMP_COUNT + pa_user_nam_get_count());
}

EMU_EXPORT int emu_select_user_nam(int user_idx) {
    if (user_idx < 0 || user_idx >= (int)pa_user_nam_get_count()) return -1;
    int rc = pa_user_nam_load_into_dsp((uint8_t)user_idx, &g_nam_model, &g_wn_model);
    if (rc == 0) {
        g_ui.slot_type[PA_SLOT_AMP] = (uint8_t)(PA_STOCK_AMP_COUNT + user_idx);
        g_ui.user_nam_loaded = 1;
        strncpy(g_ui.user_nam_name, pa_user_nam_get_name((uint8_t)user_idx), sizeof(g_ui.user_nam_name) - 1);
        g_nam_node.active = 1;
        pa_fx_chain_set_slot_enabled(&g_fx_chain, PA_SLOT_AMP, 1);
        g_ui.dirty = 1;
    }
    return rc;
}

/* Explicit WaveNet float array loader */
EMU_EXPORT int emu_load_wavenet_floats(const float *weights, uint32_t count, float head_scale) {
    if (!weights) return -1;
    int ret = pa_wavenet_load_weights(&g_wn_model, weights, count, head_scale);
    if (ret != 0) return ret;
    g_wn_model.active   = 1;
    g_active_amp_engine = 1;
    g_nam_node.active   = 1;
    pa_fx_chain_set_slot_enabled(&g_fx_chain, PA_SLOT_AMP, 1);
    g_ui.dirty          = 1;
    return 0;
}

/* Direct WaveNet processing test bypass */
EMU_EXPORT void emu_test_wavenet_direct(const float *in, float *out, int nframes) {
    if (!in || !out || nframes <= 0) return;
    memcpy(out, in, nframes * sizeof(float));
    pa_wavenet_process_block_float(&g_wn_model, out, nframes);
}

EMU_EXPORT void emu_reset_wavenet_state(void) {
    pa_wavenet_reset(&g_wn_model);
}

EMU_EXPORT void emu_set_wavenet_voicing(int profile) {
    pa_wavenet_set_voicing(&g_wn_model, (pa_voicing_profile_t)profile);
    s_last_nam_voicing = (uint8_t)profile;
}

EMU_EXPORT void emu_wavenet_set_input_trim(float trim) {
    pa_wavenet_set_input_trim(&g_wn_model, trim);
}

EMU_EXPORT float emu_wavenet_get_input_trim(void) {
    return pa_wavenet_get_input_trim(&g_wn_model);
}

EMU_EXPORT int emu_get_wavenet_voicing(void) {
    return (int)g_wn_model.voicing_profile;
}

/* Hot-load one of the stock amp models or persistent user NAM captures */
EMU_EXPORT int emu_select_stock_amp(int amp_idx) {
    if (amp_idx < 0) return -1;
    if (amp_idx < (int)PA_STOCK_AMP_COUNT) {
        float dc_bias = 0.0f;
        if (pa_stock_load_amp((uint8_t)amp_idx, &g_nam_model, &dc_bias) != 0) return -2;
        g_active_amp_engine = 0;
        g_nam_node.active   = 1;
        g_ui.slot_type[PA_SLOT_AMP] = (uint8_t)amp_idx;
        /* as the device does (pa_audio_core_apply_model): the model's own tone-stack voicing
         * and output trim, then the knobs again. Without it a type change kept the previous
         * model's trim - Amp 2 (+24 dB) played at another model's level, 20+ dB too quiet. */
        pa_amp_set_model_profile((uint8_t)amp_idx);
        for (int k = 0; k < 6; k++)
            pa_fx_chain_set_param(&g_fx_chain, PA_SLOT_AMP, k, g_ui.slot_params[PA_SLOT_AMP][k]);
        pa_fx_chain_set_slot_enabled(&g_fx_chain, PA_SLOT_AMP, 1);
        g_ui.dirty          = 1;
        return 0;
    }
    return emu_select_user_nam(amp_idx - PA_STOCK_AMP_COUNT);
}

/* Hot-load one of the 20 official factory 1024-tap cabinet IRs */
EMU_EXPORT int emu_select_stock_cab(int cab_idx) {
    if (cab_idx < 0 || cab_idx >= (int)PA_STOCK_CAB_COUNT || !g_cab_node) return -1;
    int32_t taps[PA_STOCK_CAB_TAPS];
    uint32_t num_taps = PA_STOCK_CAB_TAPS;
    if (pa_stock_load_cab((uint8_t)cab_idx, taps, &num_taps) != 0) return -2;
    fx_cab_ir_load_q18(g_cab_node, taps, num_taps);
    fx_cab_ir_set_stock_trim(g_cab_node, (uint8_t)cab_idx);   /* the IR's loudness trim, as on the device */
    g_ui.slot_type[PA_SLOT_CAB] = (uint8_t)cab_idx;
    g_cab_node->active = 1;
    pa_fx_chain_set_slot_enabled(&g_fx_chain, PA_SLOT_CAB, 1);
    g_ui.dirty         = 1;
    return 0;
}

EMU_EXPORT const char *emu_get_stock_amp_name(int amp_idx) {
    if (amp_idx < 0) return "";
    if (amp_idx < (int)PA_STOCK_AMP_COUNT) {
        return pa_stock_get_amp_name((uint8_t)amp_idx);
    }
    return pa_user_nam_get_name((uint8_t)(amp_idx - PA_STOCK_AMP_COUNT));
}

EMU_EXPORT const char *emu_get_stock_cab_name(int cab_idx) {
    return pa_stock_get_cab_name((uint8_t)cab_idx);
}

/* Select built-in voiced tube/solid-state amp profile (0..19: factory models, 20+: ToneHunt WaveNet) */
EMU_EXPORT void emu_select_amp_profile(int profile_id) {
    if (profile_id >= 0 && profile_id < (int)PA_STOCK_AMP_COUNT) {
        emu_select_stock_amp(profile_id);
        return;
    }
    if (profile_id == 20) {
        /* Bogner WaveNet default */
        pa_wavenet_init_default(&g_wn_model);
        g_wn_model.head_scale = 1.15f;
        g_wn_model.active     = 1;
        g_active_amp_engine   = 1;
        g_nam_node.active     = 1;
        pa_fx_chain_set_slot_enabled(&g_fx_chain, PA_SLOT_AMP, 1);
        g_ui.dirty            = 1;
        return;
    }
    init_rich_tube_gru(&g_nam_model, 1.50f, 0.25f, 0.65f);
    g_active_amp_engine = 0;
    g_nam_node.active   = 1;
    pa_fx_chain_set_slot_enabled(&g_fx_chain, PA_SLOT_AMP, 1);
    g_ui.dirty          = 1;
}

/* Hot-load float32 Cabinet IR taps [-1.0..+1.0] into the Q18 Partitioned Convolver */
EMU_EXPORT int emu_load_cab_ir_floats(const float *taps, uint32_t num_taps) {
    if (!taps || num_taps == 0 || !g_cab_node) return -1;
    if (num_taps > PA_IR_MAX_TAPS) num_taps = PA_IR_MAX_TAPS;
    int32_t q18_taps[PA_IR_MAX_TAPS];
    for (uint32_t i = 0; i < PA_IR_MAX_TAPS; i++) {
        float v = (i < num_taps) ? taps[i] : 0.0f;
        if (v > 2.0f)  v = 2.0f;
        if (v < -2.0f) v = -2.0f;
        q18_taps[i] = (int32_t)(v * 262144.0f);
    }
    fx_cab_ir_load_q18(g_cab_node, q18_taps, num_taps);
    g_cab_node->active = 1;
    pa_fx_chain_set_slot_enabled(&g_fx_chain, PA_SLOT_CAB, 1);
    g_ui.dirty = 1;
    return 0;
}

/* Toggle any of the 8 effect slots (0..7) and trigger C7 Dynamic SRAM Reclaim */
EMU_EXPORT void emu_set_slot_enabled(int slot, int enabled) {
    if (slot < 0 || slot >= PA_SLOT_COUNT) return;
    pa_fx_chain_set_slot_enabled(&g_fx_chain, (pa_fx_slot_id_t)slot, enabled ? 1 : 0);
    g_ui.dirty = 1;
}

/* Set the ORDER the eight effects run in.
 *
 * The order is not cosmetic: a drive after the cabinet runs on the cabinet's
 * high-passed output (so its harmonics get cut), a reverb before the amp
 * feeds the amp (so the reverb is distorted), and a delay before the drive
 * smears the drive's attack. This is what the effect-matrix stress test
 * drives, so the order has to be settable from the host. */
EMU_EXPORT void emu_set_slot_order(const uint8_t *order) {
    if (!order) return;
    for (int i = 0; i < PA_SLOT_COUNT; i++) {
        uint8_t s = order[i];
        if (s >= PA_SLOT_COUNT) return;      /* reject the whole thing */
    }
    for (int i = 0; i < PA_SLOT_COUNT; i++) g_fx_chain.slot_order[i] = order[i];
    g_ui.dirty = 1;
}

/* Read the order back (for the test's report). */
EMU_EXPORT void emu_get_slot_order(uint8_t *out) {
    if (!out) return;
    for (int i = 0; i < PA_SLOT_COUNT; i++) out[i] = g_fx_chain.slot_order[i];
}

/* Set a slot's TYPE (the drive model, the cabinet IR, the modulation type,
 * the reverb type, ...). The type index is the one the UI uses. */
EMU_EXPORT void emu_set_slot_type(int slot, int type) {
    if (slot < 0 || slot >= PA_SLOT_COUNT) return;
    if (slot == PA_SLOT_AMP) { emu_select_amp_profile(type); return; }
    /* The CAB's "model" is an impulse response, not a set_param id, so it has
     * to go through the stock IR loader. */
    if (slot == PA_SLOT_CAB) { emu_select_stock_cab(type); return; }
    pa_fx_chain_set_slot_type(&g_fx_chain, (pa_fx_slot_id_t)slot, (uint8_t)type);
    /* the screen reads the type from the UI context (model name, DYN knob labels):
     * without this the web panel changed the sound and the LCD kept the old type */
    g_ui.slot_type[slot] = (uint8_t)type;
    g_ui.dirty = 1;
}

/* Switch the global 5-band EQ on or off. The stress test uses this to show
 * whether a left/right difference comes from the global EQ. */
EMU_EXPORT void emu_set_global_eq_enabled(int enabled) {
    g_ui.eq_enabled = enabled ? 1 : 0;
    pa_ui_apply_io_settings(&g_ui, &g_fx_chain);
    g_ui.dirty = 1;
}

/* Set a slot parameter (slot 0..7, param 0..5, value 0..100) */
EMU_EXPORT void emu_set_slot_param(int slot, int param_id, int val_0_to_100) {
    if (slot < 0 || slot >= PA_SLOT_COUNT || param_id < 0 || param_id >= 6) return;
    if (val_0_to_100 < 0)   val_0_to_100 = 0;
    if (val_0_to_100 > 100) val_0_to_100 = 100;
    int32_t q21_val = (int32_t)(((int64_t)val_0_to_100 * PA_Q21_ONE) / 100);
    g_ui.slot_params[slot][param_id] = q21_val;
    if (slot == PA_SLOT_EQ) {
        int db = (val_0_to_100 * 24) / 100 - 12;
        g_ui.slot_bands_db[param_id] = (int8_t)db;
    }
    pa_fx_chain_set_param(&g_fx_chain, (pa_fx_slot_id_t)slot, param_id, q21_val);
    g_ui.dirty = 1;
}

/* Control procedural 808/909 Drum Machine */
EMU_EXPORT void emu_set_drums(int playing, int bpm, int pattern) {
    g_ui.drum_playing = (uint8_t)(playing ? 1 : 0);
    g_ui.drum_bpm     = (uint16_t)bpm;
    g_ui.drum_pattern = (uint8_t)pattern;
    pa_drums_set_bpm(&g_drums, g_ui.drum_bpm);
    pa_drums_set_style(&g_drums, g_ui.drum_pattern);
    pa_drums_set_playing(&g_drums, g_ui.drum_playing);
    g_ui.dirty = 1;
}

/* Control Master Volume Knob (0..100) */
EMU_EXPORT void emu_set_master_volume(int vol_0_to_100) {
    if (vol_0_to_100 < 0)   vol_0_to_100 = 0;
    if (vol_0_to_100 > 100) vol_0_to_100 = 100;
    g_ui.master_volume = (uint8_t)vol_0_to_100;
    g_ui.preset_volume = (uint8_t)vol_0_to_100;
    pa_fx_chain_set_preset_vol(&g_fx_chain, g_ui.preset_volume);
    if (g_ui.view != PA_UI_VIEW_MAIN_GRID && g_ui.view != PA_UI_VIEW_BANK_MODE && g_ui.view != PA_UI_VIEW_DRUMS) {
        g_ui.show_vol_popup = 1;
        g_ui.vol_popup_timer_ms = 1000;
    }
    g_ui.dirty = 1;
}

/* Directly switch preset 0..79 */
EMU_EXPORT void emu_select_preset(int preset_0_to_79) {
    if (preset_0_to_79 < 0) preset_0_to_79 = 0;
    if (preset_0_to_79 >= (int)PA_PRESET_COUNT) preset_0_to_79 = (int)PA_PRESET_COUNT - 1;
    g_ui.preset_index = (uint8_t)preset_0_to_79;
    g_ui.bank_group = g_ui.preset_index / 4u;
    pa_preset_t pr;
    if (pa_preset_load(g_ui.preset_index, &pr) == 0) {
        pa_preset_apply_to_chain(&pr, &g_fx_chain);
        /* the firmware's own sync: knobs converted Q16 -> Q21, every slot's type, the name,
         * the stomp map. It used to copy the raw Q16 values (the screen then read every
         * knob as ~2 %, and the first turn sent ~0 to the DSP) and skipped the types. */
        pa_ui_sync_from_preset(&g_ui, &pr);
    }
    g_ui.dirty = 1;
}

/* The SysEx replies the firmware sends, captured so the host can check them
 * (the editor reads them on the same MIDI port it writes on). */
#define EMU_REPLY_MAX 32
#define EMU_REPLY_LEN 512
static uint8_t  g_emu_reply[EMU_REPLY_MAX][EMU_REPLY_LEN];
static uint32_t g_emu_reply_len[EMU_REPLY_MAX];
static uint32_t g_emu_reply_count = 0;

static void emu_sysex_reply_sink(const uint8_t *data, uint32_t len) {
    if (!data || len == 0u || len > EMU_REPLY_LEN) return;
    /* The reply also goes back over the BLE link, framed as BLE-MIDI 1.0
     * packets - that is what the device would notify on the MIDI
     * characteristic. The USB path is left alone (the emulator has no USB). */
    pa_usb_ble_tx_ble(&g_usb_ble, data, len);
    if (g_emu_reply_count >= EMU_REPLY_MAX) return;
    memcpy(g_emu_reply[g_emu_reply_count], data, len);
    g_emu_reply_len[g_emu_reply_count] = len;
    g_emu_reply_count++;
}

static void emu_sysex_init(void) {
    g_emu_reply_count = 0;
    pa_sysex_set_reply_sink(emu_sysex_reply_sink);
}

/* 1 = the reply sink is registered, 2 = a reply is waiting */
EMU_EXPORT int emu_sysex_debug(void) {
    return (pa_sysex_reply_sink_set() ? 1 : 0) | (g_emu_reply_count ? 2 : 0);
}

/* The replies, one per call, as hex. 0 when there are none left. */
EMU_EXPORT int emu_next_sysex_reply(char *out, int cap) {    if (g_emu_reply_count == 0) return 0;
    uint32_t n = g_emu_reply_len[0];
    if ((int)n > cap) n = (uint32_t)cap;
    memcpy(out, g_emu_reply[0], n);
    for (uint32_t i = 1; i < g_emu_reply_count; i++) {
        memcpy(g_emu_reply[i - 1], g_emu_reply[i], g_emu_reply_len[i]);
        g_emu_reply_len[i - 1] = g_emu_reply_len[i];
    }
    g_emu_reply_count--;
    return (int)n;
}

/* Feed raw SysEx byte stream into the firmware dispatcher */
EMU_EXPORT void emu_feed_sysex_bytes(const uint8_t *bytes, uint32_t len) {
    if (!bytes) return;
    for (uint32_t i = 0; i < len; i++) {
        pa_sysex_input_byte(bytes[i]);
    }
}

/* The upload session's state: the host test uses it to prove that an oversized
 * transfer was REFUSED (armed = 0, status = 1) instead of being half-written.
 * Returns the armed flag; the pointers are optional. */
EMU_EXPORT int emu_upload_state(uint32_t *declared, uint32_t *rx, int *status) {
    const pa_upload_session_t *s = pa_upload_get_session();
    if (!s) return 0;
    if (declared) *declared = s->declared_len;
    if (rx)       *rx       = s->rx_bytes;
    if (status)   *status   = (int)s->status;
    return (int)s->armed;
}

/* The BLE-MIDI framed replies, captured so the host can check them: this is
 * what the device would notify on the MIDI characteristic. */
static uint8_t  g_emu_ble_tx[EMU_REPLY_MAX][EMU_REPLY_LEN];
static uint32_t g_emu_ble_tx_len[EMU_REPLY_MAX];
static uint32_t g_emu_ble_tx_count = 0;

static void emu_ble_tx_sink(const uint8_t *pkt, uint32_t len) {
    if (!pkt || len == 0u || len > EMU_REPLY_LEN) return;
    if (g_emu_ble_tx_count >= EMU_REPLY_MAX) return;
    memcpy(g_emu_ble_tx[g_emu_ble_tx_count], pkt, len);
    g_emu_ble_tx_len[g_emu_ble_tx_count] = len;
    g_emu_ble_tx_count++;
}

static void emu_ble_init(void) {
    g_emu_ble_tx_count = 0;
    pa_usb_ble_set_ble_tx_sink(&g_usb_ble, emu_ble_tx_sink);
}

/* The BLE-MIDI packets the device sent, one per call, as hex. 0 when there
 * are none left. */
EMU_EXPORT int emu_next_ble_tx(char *out, int cap) {
    if (g_emu_ble_tx_count == 0) return 0;
    uint32_t n = g_emu_ble_tx_len[0];
    if ((int)n > cap) n = (uint32_t)cap;
    memcpy(out, g_emu_ble_tx[0], n);
    for (uint32_t i = 1; i < g_emu_ble_tx_count; i++) {
        memcpy(g_emu_ble_tx[i - 1], g_emu_ble_tx[i], g_emu_ble_tx_len[i]);
        g_emu_ble_tx_len[i - 1] = g_emu_ble_tx_len[i];
    }
    g_emu_ble_tx_count--;
    return (int)n;
}

/* Feed a BLE-MIDI 1.0 packet, exactly as a real BLE write on the MIDI
 * characteristic would arrive it: the framing is stripped, the bytes go into
 * the same ring the USB MIDI path uses, and the ring is drained into the
 * firmware's SysEx assembler. So a BLE peer and a USB peer drive the device
 * through one protocol. */
EMU_EXPORT void emu_feed_ble_midi(const uint8_t *bytes, uint32_t len) {
    if (!bytes) return;
    pa_ble_gatt_rx_write(&g_usb_ble, bytes, len);
    pa_usb_ble_poll(&g_usb_ble);
}

typedef struct {
    uint32_t view;
    uint32_t preset_index;
    uint32_t preset_volume;
    uint32_t master_volume;
    uint32_t selected_slot;
    uint32_t selected_param;
    uint32_t slot_active_mask;
    uint32_t sram_used_bytes;
    uint32_t sram_peak_bytes;
    uint32_t cpu_total_permille;
    uint32_t cpu_stage_permille[8];
    uint32_t tuner_active;
    uint32_t tuner_locked;
    uint32_t tuner_note;
    int32_t  tuner_octave;
    int32_t  tuner_cents;
    uint32_t tuner_freq_tenths;
    uint32_t tuner_dots;
    uint32_t drum_playing;
    uint32_t drum_bpm;
    uint32_t drum_pattern;
    float    in_peak;
    float    out_peak;
    uint32_t bt_state;
    /* the cursor, so a host can see WHICH tile is selected (the WASM build
     * reports the same field) */
    uint32_t cursor_slot;
    uint32_t editor_cursor;
} emu_telemetry_t;

EMU_EXPORT void emu_set_bt_state(int state) {
    if (state < 0) state = 0;
    if (state > 2) state = 2;
    g_ui.bt_state = (uint8_t)state;
    g_ui.dirty = 1;
}

EMU_EXPORT int emu_get_bt_state(void) {
    return (int)g_ui.bt_state;
}

EMU_EXPORT void emu_get_telemetry(emu_telemetry_t *out) {
    if (!out) return;
    out->view          = (uint32_t)g_ui.view;
    out->preset_index  = (uint32_t)g_ui.preset_index;
    out->preset_volume = (uint32_t)g_ui.preset_volume;
    out->master_volume = (uint32_t)g_ui.master_volume;
    out->selected_slot = (uint32_t)g_ui.selected_slot;
    out->selected_param= (uint32_t)g_ui.selected_param;

    uint32_t mask = 0;
    for (int s = 0; s < PA_SLOT_COUNT; s++) {
        if (g_fx_chain.slots[s] && g_fx_chain.slots[s]->active) {
            mask |= (1u << s);
        }
    }
    out->slot_active_mask   = mask;
    out->sram_used_bytes    = (uint32_t)g_sram_cur;
    out->sram_peak_bytes    = (uint32_t)g_sram_peak;
    out->cpu_total_permille = cpu_meter_total_permille(&g_cpu_meter);
    for (int i = 0; i < 8; i++) {
        out->cpu_stage_permille[i] = cpu_meter_stage_permille(&g_cpu_meter, (enum pa_stage)i);
    }
    out->tuner_active      = g_ui.tuner.active;
    out->tuner_locked      = g_ui.tuner.signal_locked;
    out->tuner_note        = g_ui.tuner.note_index;
    out->tuner_octave      = g_ui.tuner.octave;
    out->tuner_cents       = g_ui.tuner.cents;
    out->tuner_freq_tenths = g_ui.tuner.freq_tenths_hz;
    out->tuner_dots        = g_ui.tuner.input_level_dots;
    out->drum_playing      = g_ui.drum_playing;
    out->drum_bpm          = g_ui.drum_bpm;
    out->drum_pattern      = g_ui.drum_pattern;
    out->in_peak           = g_in_peak;
    out->out_peak          = g_out_peak;
    out->bt_state          = (uint32_t)g_ui.bt_state;
    out->cursor_slot     = (uint32_t)g_ui.cursor_slot_idx;
    out->editor_cursor   = (uint32_t)(int32_t)g_ui.editor_cursor;
}

/* The menu cursors, for the tests: out[0] = the Settings row, out[1] = the sub-page row (Input/Output, Display) */
/* MIDI: one channel message through the REAL handler (usb_ble.c), as a USB-MIDI event packet; the same handler serves USB and BLE. Applied at the next firmware tick. */
EMU_EXPORT void emu_midi_event(int status, int d1, int d2) {
    const uint8_t pkt[4] = { (uint8_t)((status >> 4) & 0x0F), (uint8_t)status, (uint8_t)d1, (uint8_t)d2 };
    pa_usb_midi_rx_packet(&g_usb_ble, pkt, 4u);
}

/* tests: read one value of the UI context by number (the controls MIDI changes) */
EMU_EXPORT int emu_get_ui(int what) {
    switch (what) {
    case 0:  return g_ui.preset_volume;      case 1:  return g_ui.input_trim_db;       case 2:  return g_ui.gate_thresh_db;
    case 3:  return g_ui.out_level_pct;      case 4:  return g_ui.io_output_type;      case 5:  return g_ui.io_cab_on_line;
    case 6:  return g_ui.output_limit_mode;  case 7:  return g_ui.fizz_filter_enabled; case 8:  return g_ui.stereo_width;
    case 9:  return g_ui.out_route;          case 10: return g_ui.out_balance;         case 11: return g_ui.eq_enabled;
    case 12: case 13: case 14: case 15: case 16: return g_ui.eq_bands_db[what - 12];
    case 17: return g_ui.tuner.ref_a4_hz;    case 18: return g_ui.tuner.mute_output;   case 19: return g_ui.drum_bpm;
    case 20: return g_ui.drum_pattern;       case 21: return g_ui.drum_volume;         case 22: return g_ui.drum_drums_on;
    case 23: return g_ui.play_mode;          case 24: return g_ui.drum_playing;        case 25: return g_ui.settings_dirty;
    case 26: case 27: case 28: case 29: return g_ui.stomp_map[what - 26];
    case 30: return g_ui.view;               case 31: return g_ui.tuner.active;        case 32: return g_ui.preset_index;
    case 33: return g_ui.saved_index;        case 34: return g_ui.save_failed;
    case 35: return g_ui.home_view_mode;      /* 0 grid, 1 list */
    case 40: return g_ui.usb_mode;
    case 36: return g_ui.batt_pct;            case 37: return g_ui.battery_adc;         case 38: return g_ui.charging;          case 39: return g_ui.batt_warn;
    default: return -9999;
    }
}

extern volatile uint8_t g_pa_overload_force;
extern volatile uint8_t g_pa_drive_os2, g_pa_cab_parts_limit, g_pa_gov_level;
extern volatile uint32_t g_emu_slot_pk[8];
EMU_EXPORT int emu_get_slot_peak(int slot) { return (slot >= 0 && slot < 8) ? (int)g_emu_slot_pk[slot] : 0; }      /* tests: the largest |sample| the slot produced since the last clear (Q21) */
EMU_EXPORT void emu_clear_slot_peaks(void) { for (int i = 0; i < 8; i++) g_emu_slot_pk[i] = 0; }
/* tests: set the load governor's level as the audio interrupt does (audio_alnk.c): 0 full, 1 drive at 2x, 2 + the cabinet's partitions cut to 8 */
EMU_EXPORT void emu_set_governor(int level) { g_pa_gov_level = (uint8_t)level; g_pa_drive_os2 = (uint8_t)(level >= 1); g_pa_cab_parts_limit = (uint8_t)(level >= 2 ? 8 : 16); }
/* tests: the USB audio driver (usb_ble.c) without a host: open both directions, take the recording packets the unit would send, give it playback packets */
EMU_EXPORT void emu_usb_audio_start(void) { pa_usb_audio_set_rate(&g_usb_ble, 44100u); pa_usb_audio_capture_start(&g_usb_ble); pa_usb_audio_playback_start(&g_usb_ble); }
EMU_EXPORT int  emu_usb_capture_read(uint8_t *out, int cap) { return (int)pa_usb_audio_ep3_in_fill(&g_usb_ble, out, (uint32_t)cap); }
EMU_EXPORT void emu_usb_playback_write(const uint8_t *pkt, int len) { pa_usb_audio_ep3_out_rx(&g_usb_ble, pkt, (uint32_t)len); }
EMU_EXPORT int emu_get_usb_loopback(void) { return (int)g_usb_ble.loopback_enabled; }     /* tests: the USB driver's loopback switch */
#include "pa_bond.h"
/* tests: the Bluetooth bond log (pa_bond.h): stash keys as the radio does, run the main loop's write, read what is stored (-1 none, else the stored EDIV) */
EMU_EXPORT void emu_bond_stash(int seed) { uint8_t r[8], l[16]; for (int i = 0; i < 8; i++) r[i] = (uint8_t)(seed + i); for (int i = 0; i < 16; i++) l[i] = (uint8_t)(seed * 3 + i); pa_bond_stash(r, (uint16_t)seed, l); }
EMU_EXPORT int emu_bond_poll(unsigned ms) { return pa_bond_poll(ms); }
EMU_EXPORT int emu_bond_get(void) { pa_bond_t b; return pa_bond_load(&b) ? (int)b.ediv : -1; }
EMU_EXPORT int emu_preset_is_empty(int idx) { return pa_preset_is_empty((uint8_t)idx); }     /* tests: is this slot EMPTY */
EMU_EXPORT void emu_force_overload(int on) { g_pa_overload_force = (uint8_t)(on ? 1 : 0); }     /* tests: behave as overloaded (the popup) */
EMU_EXPORT void emu_get_menu_cursors(uint32_t *out) {
    out[0] = (uint32_t)g_ui.settings_cursor;
    out[1] = (uint32_t)g_ui.sub_cursor;
    out[2] = (uint32_t)g_ui.toast_kind;     /* the full-screen toast: 0 a preset was saved, 1 the factory reset was done */
}

EMU_EXPORT void emu_get_scope(float *in_256, float *out_256) {
    int rd = g_scope_wr;
    for (int i = 0; i < 256; i++) {
        int idx = (rd + i) & 255;
        if (in_256)  in_256[i]  = g_scope_in[idx];
        if (out_256) out_256[i] = g_scope_out[idx];
    }
}

EMU_EXPORT int emu_get_slot_type(int slot) {
    if (slot < 0 || slot >= PA_SLOT_COUNT) return 0;
    return (int)g_ui.slot_type[slot];
}

EMU_EXPORT int emu_get_slot_param(int slot, int param_id) {
    if (slot < 0 || slot >= PA_SLOT_COUNT || param_id < 0 || param_id >= 6) return 50;
    int32_t q = g_ui.slot_params[slot][param_id];
    int pct = (int)((((int64_t)q * 100) + (PA_Q21_ONE >> 1)) / PA_Q21_ONE);
    if (pct < 0) pct = 0;
    if (pct > 100) pct = 100;
    return pct;
}



/* tests: the radio calibration parser (include/pa_ble_trims.h) on a log image the host hands in; out = the 97 bytes of the four records (x106, x107, x108, x187); returns the mask of the valid records */
#include "../include/pa_ble_trims.h"
static const uint8_t *s_trim_log; static uint32_t s_trim_log_len;
static int emu_trim_rd(uint32_t off, uint8_t *b, uint32_t n) { if (off + n > s_trim_log_len) return -1; memcpy(b, s_trim_log + off, n); return 0; }
EMU_EXPORT int emu_ble_trims_parse(const uint8_t *log, uint32_t len, uint8_t *out) {
    pa_ble_trims_t t; memset(&t, 0, sizeof t);
    s_trim_log = log; s_trim_log_len = len;
    const uint32_t m = pa_ble_trims_parse(emu_trim_rd, &t);
    if (m == 0xFu && out) { memcpy(out, t.x106, 2); memcpy(out + 2, t.x107, 7); memcpy(out + 9, t.x108, 20); memcpy(out + 29, t.x187, 68); }
    return (int)m;
}
