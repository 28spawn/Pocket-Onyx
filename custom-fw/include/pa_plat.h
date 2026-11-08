/* SPDX-License-Identifier: GPL-3.0-only */
/* Copyright (C) 2026 The Onyx Authors (@28spawn) */
/* pa_plat.h — Pocket Amp hardware abstraction seam (roadmap QW-1).
 * Pattern adapted from charlesvestal/fm1-x0x (firmware/src/app/plat.h).
 * Portable code in src/dsp, src/nam, src/cab, src/fx, src/sys NEVER touches MMIO directly. */
#ifndef PA_PLAT_H
#define PA_PLAT_H

#include <stdint.h>
#include "pa_config.h"

/* PA_CPU_HZ lives in pa_config.h (included above) - it used to be defined here
 * as well, so the two copies could drift apart. */
#define PA_TIMER_HZ            24000000u
#define PA_POOL_BUDGET_BYTES  0x00054000u  /* 336 KiB POOL @ 0x01C20000 */
#define PA_NAM_LUT_ENTRIES    512

/* 1. Time & Performance */
uint32_t plat_ms(void);
uint32_t plat_ticks24(void);
uint32_t plat_cycles(void);
void     plat_delay_us(uint32_t us);
void     plat_delay_ms(uint32_t ms);

/* A millisecond clock that survives the tick counter's wrap.
 *
 * The pi32's 24 MHz counter is 32 bits, so it wraps every 2^32 / 24e6 = 178.96 s.
 * Returning the raw counter divided by the ticks per millisecond therefore made the
 * millisecond clock jump back to zero every three minutes, which broke the auto
 * power-off, the toasts and the long-press timing.  This keeps a running count instead:
 * the elapsed ticks since the last call are added (an unsigned subtraction handles the
 * counter's own wrap) and the remainder over a whole millisecond is carried over, so the
 * clock only wraps at 2^32 ms (49 days).
 *
 * It is here, and not in the board layer, so the host can test it. */
typedef struct {
    uint32_t last_ticks;    /* the counter's value at the last call */
    uint32_t remainder;     /* ticks left over from the last call (0..ticks_per_ms-1) */
    uint32_t ms;            /* the running millisecond count */
} pa_ms_clock_t;

static inline uint32_t pa_ms_clock_tick(pa_ms_clock_t *c, uint32_t now_ticks,
                                        uint32_t ticks_per_ms) {
    uint32_t elapsed = now_ticks - c->last_ticks;      /* unsigned: the wrap is handled */
    c->last_ticks = now_ticks;
    uint64_t ticks = (uint64_t)elapsed + (uint64_t)c->remainder;
    uint32_t ms = (uint32_t)(ticks / ticks_per_ms);
    c->remainder = (uint32_t)(ticks % ticks_per_ms);
    c->ms += ms;
    return c->ms;
}

/* 2. Pool / Heap Allocator */
void    *pa_alloc(uint32_t bytes);
void     pa_free(void *ptr);
uint32_t pa_pool_alloc_fail_count(void);        /* requests pa_alloc refused (pool full or too fragmented), since boot */
uint32_t pa_pool_alloc_fail_last_bytes(void);   /* the size of the last refused request */
uint32_t pa_heap_used(void);
uint32_t pa_heap_peak(void);

/* 3. HW DSP Accelerator (0x1EE0400) & Activation LUTs */
void plat_accel_matvec(const int32_t *weights, const int32_t *vec,
                       int32_t *out, int rows, int cols, int accumulate);
void plat_accel_fir(const int32_t *ir_coeffs, int32_t *hist_ring, uint32_t *hist_pos,
                    const int32_t *in_pcm, int32_t *out_pcm, int n_frames, int n_taps);
const int32_t *plat_nam_lut_slope(int pair);
const int32_t *plat_nam_lut_offset(int pair);

/* 4. Audio I/O (ALNK0 0x12E00) */
typedef void (*pa_audio_cb_t)(const int32_t *in_pcm, int32_t *out_pcm, int n_frames);
void plat_audio_init(pa_audio_cb_t cb);
void plat_audio_silence(void);
void plat_audio_gate(int gate_on);

/* 5. Controls, SARADC, Battery & Watchdog */
uint32_t plat_buttons(void);
int32_t  plat_enc_delta(void);
uint16_t plat_pot_q12(int ch);
int      plat_battery_bars(void);
int      plat_battery_mv(void);
int      plat_battery_adc(void);               /* the stock method: the 150-sample average of SARADC channel 9, 0..1023 (0 = no reading yet) */
int      plat_charging(void);                  /* 1 while USB power is present (PMU status 0x16A10 bit 16 = 0) */
int      plat_battery_is_brownout(void);
void     plat_battery_poll(void);              /* IMP-12: call from the UI tick (~60 Hz) */
void     plat_backlight_set(uint8_t pct);      /* IMP-06: 0..100 % */
uint32_t plat_buttons_for_ui(void);     /* live buttons + presses that came and went between UI ticks (see gpio_scan.c) */
void     plat_pot_poll(void);           /* the master knob (SARADC), from the main loop */
void     pa_gpio_scan_isr_tick(void);   /* from the audio interrupt: keeps the buttons and encoder sampled during a redraw */
void     plat_wdt_feed(void);
void     plat_wdt_arm(uint8_t timeout);   /* 0xA = 1 s .. 0xF = 32 s; see gpio_scan.c */
uint8_t  plat_wdt_read(void);             /* P33 WDT_CON (0x80) read back: shows whether the enable bit stuck */

/* Bench step markers. PA_CP(n) draws "cp nn" on the screen at once, from the main loop only
 * (never from an interrupt). A freeze leaves the last marker on the display, which keeps its
 * image when the CPU stops: that is where the loop stopped. Off on the host builds. */
#ifndef PA_DEBUG_OVERLAY
#define PA_DEBUG_OVERLAY 0      /* the on-screen overlay + black box are off: read the unit from RAM instead (tools/jl_ram_read.py slots) */
#endif
#ifndef PA_BENCH_CRUMBS
#define PA_BENCH_CRUMBS 0       /* 1 = the step markers and audio-interrupt breadcrumbs (slow: many LCD writes per loop) */
#endif
#if PA_BENCH_CRUMBS && PA_DEBUG_OVERLAY && !defined(PA_HOST_BUILD) && !defined(PA_HOST_SIM)
void pa_cp(uint32_t n);
#define PA_CP(n) pa_cp(n)
#elif !defined(PA_HOST_BUILD) && !defined(PA_HOST_SIM)
/* No on-screen crumbs, but still one cheap store of the step number to SRAM 0x01C7C3F4: a stalled main loop (watchdog reset into
 * download mode, SRAM intact) leaves its last checkpoint there, read by tools/jl_ram_read.py slots ("main-loop checkpoint"). */
#define PA_CP(n) ((void)(*(volatile uint32_t *)0x01C7C3F4u = (uint32_t)(n)))
#else
#define PA_CP(n) ((void)0)
#endif
/* PA_ISR_CP(n): one store of a step number to SRAM 0x01C7C3F8, safe inside an interrupt. After a
 * stall and the watchdog reset into download mode, `pa_transporter.py crash` shows the last one:
 * the step the audio interrupt was in. */
#ifndef PA_BENCH_ISR_CRUMBS
#define PA_BENCH_ISR_CRUMBS 1   /* cheap (one store): on, so a stall in the audio path is still located */
#endif
#if PA_BENCH_ISR_CRUMBS && !defined(PA_HOST_BUILD) && !defined(PA_HOST_SIM)
#define PA_ISR_CP(n) (*(volatile uint32_t *)0x01C7C3F8u = (uint32_t)(n))
#else
#define PA_ISR_CP(n) ((void)0)
#endif

/* 6. ST7789 240x240 Display (SPI1 0x11D00) */
#define PA_LCD_W 240u
#define PA_LCD_H 240u
void plat_lcd_init(void);
void plat_lcd_fill(uint32_t x, uint32_t y, uint32_t w, uint32_t h, uint16_t rgb565);
void plat_lcd_blit(uint32_t x, uint32_t y, uint32_t w, uint32_t h, const uint16_t *px);
void plat_lcd_sync(void);

/* 7. Flash Persistence (A/B Sector Store & Cab Partition) */
int plat_store_load(uint32_t obj, void *dst, uint32_t max_bytes);
int plat_store_save(uint32_t obj, const void *src, uint32_t len_bytes);
int plat_cab_read(uint32_t slot, void *dst, uint32_t max_bytes);

/* 8. USB-MIDI SysEx (00 59 framing) */
int  plat_sysex_recv(const uint8_t **payload_7bit, uint32_t *len);
void plat_sysex_done(void);
int  plat_sysex_send(const uint8_t *msg_f0_f7, uint32_t len);

/* 9. The JieLi downloader (the recovery route)
 *
 * Reboot the chip into the JieLi USB DFU mode, where it re-enumerates as 4d4a:4155 and
 * the host can flash an image (the stock "Pocket AMP.fwsc" to go back to the original
 * firmware, or a new build).  This is the app's half of the OTA handshake: the updater
 * sends F0 22 24 35 7F F7, the device reboots, and the *downloader* - not this app -
 * pulls the image from the host.
 *
 * Returns 0 if the request was accepted (the device may reboot before it returns).
 */
int  plat_reset_to_downloader(void);
void rc_recover_uboot(void);

#endif /* PA_PLAT_H */
