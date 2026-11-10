/* SPDX-License-Identifier: GPL-3.0-only */
/* Copyright (C) 2026 The Onyx Authors (@28spawn) */
#include "../../include/pa_config.h"
/*
 * audio_alnk.c — Pocket Amp Full-Duplex ALNK0 I2S Driver & Ping-Pong ISR (Big Rock 1)
 *
 * Hardware register sequence:
 *   - Clock mux, PA4..PA8 GPIO, ALNK0 CON0..CON3, ADR0, ADR1, SHN
 *   - IRQ 11 registration at 0x01C7FE2C + 0x1EEF104
 *   - Ping-pong ISR, 24-bit sign extension, peak metering, CPU meter
 */
#include "../../include/pa_evlog.h"
#include <string.h>
#include "../../include/pa_audio.h"
#include "../../include/pa_audio_core.h"
#include "../../include/pa_hw.h"
#include "../../include/pa_plat.h"
#include "../../include/pa_fx.h"
#include "../../include/pa_cpu_meter.h"

/* Audio DMA ping-pong buffers (TX and RX) allocated with safety headroom.
 *
 * The bench unit showed the RX DMA writing far past PA_AUDIO_SHN_FRAMES * PA_AUDIO_CHANNELS
 * words (it corrupted g_drums, g_usb_ble and g_ui, ~4 KB after the start of a 1 KB buffer):
 * the ALNK's SHN unit is not what this driver assumed. Until it is measured, both buffers
 * get PA_ALNK_DMA_SLACK times the nominal size, the part past the nominal size is filled
 * with a canary, and pa_alnk_dma_extent_bytes() reports how far the DMA really writes. */
#define PA_ALNK_DMA_NOMINAL  (PA_AUDIO_SHN_FRAMES * PA_AUDIO_CHANNELS)
#define PA_ALNK_DMA_SLACK    8u
#define PA_ALNK_CANARY       0xC0DEC0DEu
static int32_t g_alnk_tx_dma[PA_ALNK_DMA_NOMINAL * PA_ALNK_DMA_SLACK]
    __attribute__((aligned(32)));
static int32_t g_alnk_rx_dma[PA_ALNK_DMA_NOMINAL * PA_ALNK_DMA_SLACK]
    __attribute__((aligned(32)));

/* Bytes from the start of the RX buffer up to the last word the DMA overwrote (the canary
 * fills everything past the nominal size at init). Zeros written by the DMA count too. */
uint32_t pa_alnk_dma_extent_bytes(void) {
    uint32_t n = PA_ALNK_DMA_NOMINAL * PA_ALNK_DMA_SLACK;
    while (n > PA_ALNK_DMA_NOMINAL && (uint32_t)g_alnk_rx_dma[n - 1u] == PA_ALNK_CANARY) n--;
    return n * 4u;
}

pa_audio_state_t g_pa_audio;




/* The device's audio I/O path now lives in the portable core
 * (src/sys/pa_audio_core.c), because the host emulator and the WebAssembly
 * build have to run exactly the same code as the hardware: the same 24-bit
 * input gain, the same master volume slew and the same 1-LSB TPDF dither.
 * This wrapper keeps the ALNK driver's own entry point, so the ISR is
 * unchanged and the device still processes its half-buffers exactly as
 * before. */
void pa_audio_process_half(const int32_t *rx_half, int32_t *tx_half) {
    pa_audio_core_process_half(rx_half, tx_half);
}

#if !defined(PA_HOST_BUILD) && !defined(PA_HOST_SIM)
/*
 * On-device ALNK0 Interrupt Service Routine (Vector 11)
 * Called from assembly wrapper `isr_alnk0` in entry.S which handles csync and rti.
 */
extern void isr_alnk0(void);

/* Bench stats live at a fixed address in high SRAM, after the black box (main.c, 0x01C7C000),
 * where .bss corruption cannot reach them. Layout shared with main.c's overlay. */
typedef struct {
    uint32_t isr_entries, isr_last_cyc, isr_max_cyc, overruns, auto_bypass, magic;
} pa_bench_stats_t;
#define PA_BENCH_STATS ((volatile pa_bench_stats_t *)(uintptr_t)0x01C7C250u)
/* The auto-bypass tripwire. It sat at 1.35 ms; the WaveNet amp with a cabinet, delay and reverb keeps this interrupt at 1.36-1.42 ms (core 0 waits for core 1 inside it),
 * which is inside the 1.451 ms block, so 8 such blocks in a row muted the NAM 60 ms after it was selected. A block that really overruns is over 1.451 ms; 1.44 leaves 11 us. */
#define PA_ISR_BUDGET_CYC (PA_US_SCALE(1440u) * 24u)        /* TIMER4 @ 24 MHz */
/* Step breadcrumbs: one store of a step number to SRAM 0x01C7C3F8 (see pa_plat.h PA_ISR_CP);
 * the last one left after a stall says where the audio interrupt was. */
#define PA_ISR_CP(n) PA_WRITE32(0x01C7C3F8u, (n))
uint32_t pa_alnk_isr_entries(void) { return PA_BENCH_STATS->isr_entries; }
uint32_t pa_alnk_isr_last_us(void) { return PA_BENCH_STATS->isr_last_cyc / 24u; }
uint32_t pa_alnk_isr_max_us(void)  { return PA_BENCH_STATS->isr_max_cyc / 24u; }

/* Called when a model or preset swap begins (the output is faded out and muted): zero the transmit DMA buffer. The swap work runs inside the interrupt for 3-6 ms, so the
 * next block is late and the DMA replays the buffer it holds - the tail of the fade-out, an audible click when the amp or the cabinet is switched. Zeros replay silently. */
void pa_alnk_silence_tx(void) {
    for (uint32_t i = 0; i < PA_ALNK_DMA_NOMINAL; i++) g_alnk_tx_dma[i] = 0;
}

/* ---- the output-stage enable (the pop at power-on) -------------------------------------------------------------------------------------------------------------------------------------------
 * The Pocket AMP's converter has no control bus: it starts when the I2S clocks start, and its output then jumps from nothing to its resting level. With the output stage on, that jump is the
 * "cable being plugged in" noise in the headphones. PA2 is the board's enable (or mute) of the output stage or the converter: the sequence the board is known to run with is PA2 low while the
 * link starts and high after it (third_party/pendel_loader/boards/pocketamp/pa_audio.h, verified on a unit 2026-11-01). This firmware never drove PA2, so the stage was in whatever state the
 * bootloader left it while the clocks came up.
 * Now PA2 is low from the first lines of the boot (main.c), stays low while the link starts and for PA_AUDIO_EN_SETTLE_BLOCKS blocks of digital silence after it, and only then goes high. The
 * silence lasts longer than that (PA_BOOT_QUIET_BLOCKS, pa_audio_core.c), so the stage is switched on over a silent, settled converter and the sound then fades in.
 * PA_AUDIO_EN_GATE=0 leaves PA2 alone (the old behaviour), for a unit where this turns out wrong. The pin belongs in the board header (include/boards/) once that split has landed. */
#ifndef PA_AUDIO_EN_GATE
#define PA_AUDIO_EN_GATE 1
#endif
#ifndef PA_AUDIO_EN_PORTA_BIT
#define PA_AUDIO_EN_PORTA_BIT (1u << 2)            /* PA2: low = output stage off, high = on */
#endif
#define PA_AUDIO_EN_SETTLE_BLOCKS 138u             /* 138 x 1.451 ms = 200 ms of running link before the stage is switched on */
static volatile uint32_t s_en_wait;                /* blocks left until PA2 goes high; 0 = not waiting */
static volatile uint8_t  s_en_on;                  /* 1 = the stage is meant to be on: the audio interrupt keeps PA2 high (see pa_alnk0_isr_body) */

void pa_audio_output_enable(int on) {
#if PA_AUDIO_EN_GATE
    PA_WRITE32(JL_PORTA_DIE, PA_READ32(JL_PORTA_DIE) | PA_AUDIO_EN_PORTA_BIT);
    if (on) PA_WRITE32(JL_PORTA_OUT, PA_READ32(JL_PORTA_OUT) | PA_AUDIO_EN_PORTA_BIT);
    else    PA_WRITE32(JL_PORTA_OUT, PA_READ32(JL_PORTA_OUT) & ~PA_AUDIO_EN_PORTA_BIT);
    PA_WRITE32(JL_PORTA_DIR, PA_READ32(JL_PORTA_DIR) & ~PA_AUDIO_EN_PORTA_BIT);
#else
    (void)on;
#endif
}

static void pa_alnk0_isr_body(void);
extern void pa_backlight_isr_tick(void);       /* gpio_scan.c */
extern void pa_gpio_scan_isr_tick(void);       /* gpio_scan.c */
extern volatile uint8_t g_pa_drive_os2;          /* fx_drive.c */
/* The audio interrupt's measured time, for the Performance page: the average and a peak that decays over a few seconds, in microseconds. */
volatile uint32_t g_pa_isr_avg_us, g_pa_isr_peak_us;
extern volatile uint8_t g_pa_cab_parts_limit;     /* fx_cab_ir.c */
extern volatile uint8_t g_pa_stereo_allow;        /* fx_graph.c: 0 = the stereo cabinet / EQ run mono */
extern volatile uint8_t g_pa_cab_stereo;          /* fx_graph.c: 1 = this block's cabinet ran in stereo */
extern volatile uint8_t g_pa_gov_level;           /* pa_audio_core.c: the load governor's level, 0..2 (2 = the cabinet's partitions are cut); the overload popup reads it */
volatile uint8_t g_pa_gov_swap;                    /* set by the audio thread when it applied a swap in this block: the governor does not count that block (a swap stalls ~0.8 ms on purpose) */

/* STEREO GOVERNOR. The second channel costs about 180 us of core 0 time a block (a second multiply-accumulate pass, a second inverse FFT, the
 * right channel's filters and the stereo EQ), and the interrupt has about 290 us of headroom under the heaviest preset. So the stereo is the
 * first thing to go and the last to come back, and it must go before the interrupt can overrun eight blocks in a row (the auto-bypass):
 *   off  when, with the stereo running, the average block time passes PA_STEREO_OFF_US, or three blocks in a row pass PA_STEREO_HOT_US;
 *   on   again after PA_STEREO_HOLD blocks (about 4 s) once the average block time WITHOUT the stereo is under PA_STEREO_ON_US.
 * The load governor below does not see the stereo (its input has PA_STEREO_COST_US taken off in a stereo block), so the stereo never
 * costs the drive its oversampling or the cabinet its partitions. All four numbers are set from the estimate, not yet measured on the unit. */
#define PA_STEREO_COST_US 180u
#define PA_STEREO_OFF_US  PA_US_SCALE(1300u)
#define PA_STEREO_HOT_US  PA_US_SCALE(1400u)
#define PA_STEREO_ON_US   PA_US_SCALE(1080u)
#define PA_STEREO_HOLD    2800u

volatile uint32_t g_pa_alnk_t0, g_pa_alnk_in;     /* the audio interrupt's entry tick and whether it is running (read by the USB timer interrupt's holdoff record) */
void pa_alnk0_isr_c(void) {
    uint32_t t0 = PA_READ32(JL_TIMER4_BASE + 0x04u);
    g_pa_alnk_t0 = t0; g_pa_alnk_in = 1u;
    pa_alnk0_isr_body();
    g_pa_alnk_in = 0u;
    PA_ISR_CP(0x05);
    uint32_t dt = PA_READ32(JL_TIMER4_BASE + 0x04u) - t0;
    volatile pa_bench_stats_t *st = PA_BENCH_STATS;
    st->isr_last_cyc = dt;
    if (dt > st->isr_max_cyc) st->isr_max_cyc = dt;
    /* Auto-bypass: after 8 blocks over budget, run the audio as a plain pass-through so the
     * main loop (UI, buttons) keeps running. Shown on the overlay; a reboot clears it. */
    /* Only CONSECUTIVE overruns count: a model or preset swap runs the cabinet FFT load inside this interrupt and
     * takes several ms once (the output is muted then), which used to add up over a session and mute the amp for good. */
    static uint32_t s_consec;
    if (dt > PA_ISR_BUDGET_CYC) {
        st->overruns = st->overruns + 1u;
        if (s_consec == 0u && dt > PA_US_SCALE(1430u) * 24u) pa_evlog(PA_EV_ISR_LONG, dt / 24u, PA_READ32(0x01C7C3F8u));
        if (++s_consec >= 8u && !g_pa_audio.bypass) { g_pa_audio.bypass = 1; st->auto_bypass = 1u; pa_evlog(PA_EV_BYPASS, dt / 24u, 0u); }
    } else {
        s_consec = 0u;
    }
    /* LOAD GOVERNOR: the two cores cannot run every effect at full quality at once (the amp alone takes 83 % of core 1). When the
     * audio interrupt's average time climbs toward the block length, run the drive's clipper at 2x instead of 4x (level 1) and use
     * 8 of the cabinet's 16 partitions (level 2); go back up one level after about a second of calm. */
    if (dt > 200u * 24u) {                                   /* a real block, not the early return of the other DMA channel */
        static int32_t s_ema, s_raw_ema; static uint32_t s_calm, s_hold, s_calm_need = 700u, s_since_down, s_steady; static uint8_t s_level, s_settle;
        static uint16_t s_st_hold, s_st_hot;
        const int32_t dt_g = g_pa_cab_stereo ? (int32_t)dt - (int32_t)(PA_STEREO_COST_US * 24u) : (int32_t)dt;   /* what the governor sees: the block without the stereo */
        /* A model / preset swap runs the cabinet FFT inside this interrupt and takes 3-4 ms once (the output is muted then). That is not load: the average
         * ignores such a block and the next 8, and it starts from a nominal 950 us instead of from the first block (which at boot is a swap). Before this
         * the boot swaps pushed the average over the trip point, the governor stepped to level 2, and it stayed there: the way back needed an average under
         * 1,020 us and the real load sits between that and the trip point (2026-11-02 readout, pack 99). */
        const int swapped = g_pa_gov_swap;                  /* the audio thread applied a model / preset swap in this block (pa_audio_core.c) */
        g_pa_gov_swap = 0u;
        const int ign = swapped || (dt > 2500u * 24u) || s_settle;
        if (swapped || dt > 2500u * 24u) s_settle = 8u; else if (s_settle) s_settle--;
        if (s_ema == 0) s_ema = (int32_t)(PA_US_SCALE(950u) * 24u);
        if (s_raw_ema == 0) s_raw_ema = (int32_t)(PA_US_SCALE(950u) * 24u);
        if (!ign) {
            s_ema += (dt_g - s_ema) >> 4;
            s_raw_ema += ((int32_t)dt - s_raw_ema) >> 4;
        }
        if (s_hold) s_hold--;
        if (g_pa_cab_stereo) {
            s_st_hot = (!ign && dt > PA_STEREO_HOT_US * 24u) ? (uint16_t)(s_st_hot + 1u) : 0u;
            if (g_pa_stereo_allow && (s_raw_ema > (int32_t)(PA_STEREO_OFF_US * 24u) || s_st_hot >= 3u)) {
                g_pa_stereo_allow = 0u; s_st_hold = PA_STEREO_HOLD; s_st_hot = 0u;
                pa_evlog(PA_EV_GOVERNOR, 0x10u, (uint32_t)s_raw_ema / 24u);
            }
        } else {
            s_st_hot = 0u;
            if (s_st_hold) s_st_hold--;
            else if (!g_pa_stereo_allow && s_raw_ema < (int32_t)(PA_STEREO_ON_US * 24u)) g_pa_stereo_allow = 1u;
        }
        g_pa_isr_avg_us = (uint32_t)s_raw_ema / 24u;
        {   /* the stereo's real cost, for the readout (SRAM 0x01C7D4C4: [0] average audio time in blocks WITH the stereo cabinet, [1] in blocks without,
             * both in us; [2] bit 0 = stereo allowed now, bits 8.. = times the governor switched it off) */
            static int32_t s_st_ema, s_mo_ema; static uint32_t s_st_trips; static uint8_t s_was_allow = 1u;
            if (g_pa_cab_stereo) s_st_ema = (s_st_ema == 0) ? (int32_t)dt : s_st_ema + (((int32_t)dt - s_st_ema) >> 4);
            else                 s_mo_ema = (s_mo_ema == 0) ? (int32_t)dt : s_mo_ema + (((int32_t)dt - s_mo_ema) >> 4);
            if (s_was_allow && !g_pa_stereo_allow) s_st_trips++;
            s_was_allow = g_pa_stereo_allow;
            volatile uint32_t *R = (volatile uint32_t *)(uintptr_t)0x01C7D4C4u;
            R[0] = (uint32_t)s_st_ema / 24u; R[1] = (uint32_t)s_mo_ema / 24u; R[2] = (uint32_t)g_pa_stereo_allow | (s_st_trips << 8);
        }
        {   static uint32_t s_peak_age; const uint32_t us = dt / 24u;
            if (us >= g_pa_isr_peak_us || ++s_peak_age > 2000u) { g_pa_isr_peak_us = us; s_peak_age = 0u; } }
        /* The way up is above 1,250 us. The way down is under 1,130 us for s_calm_need blocks in a row (~0.9 s at first). If a step down is followed by a
         * step up within ~5 s the level is flapping: the calm needed is doubled (up to ~16 s) so it cannot audibly switch back and forth, and it goes back to
         * ~0.9 s after ~30 s without a change. */
        if (s_since_down < 0xFFFFFFu) s_since_down++;
        if (s_steady < 0xFFFFFFu) s_steady++;
        if (s_steady > 40000u) s_calm_need = 700u;
        if (s_ema > (int32_t)(PA_US_SCALE(1250u) * 24u) && s_level < 2u && s_hold == 0u) {
            s_level++; s_calm = 0u; s_hold = 300u; s_steady = 0u;
            if (s_since_down < 7000u && s_calm_need < 16000u) s_calm_need *= 2u;
            pa_evlog(PA_EV_GOVERNOR, s_level, (uint32_t)s_ema / 24u);
        } else if (s_ema < (int32_t)(PA_US_SCALE(1130u) * 24u) && s_level > 0u) {
            if (++s_calm >= s_calm_need) {
                s_level--; s_calm = 0u; s_hold = 300u; s_since_down = 0u; s_steady = 0u;
                pa_evlog(PA_EV_GOVERNOR, s_level, (uint32_t)s_ema / 24u);
            }
        } else {
            s_calm = 0u;
        }
        {   /* FREEZE DETECTOR: the main loop counts its passes. If the count has not moved for about 4.5 s (3 looks, 1.5 s apart) the main loop
             * is hung while the audio still runs: write that to the event log and go to the download mode with the RAM intact. */
            extern volatile uint32_t g_main_iters;                 /* main.c */
            static uint32_t s_blk, s_last_iters, s_stale;
            if ((++s_blk & 1023u) == 0u) {
                const uint32_t it = g_main_iters;
                if (it != 0u && it == s_last_iters) {
                    if (++s_stale >= 3u) {
                        pa_evlog(PA_EV_FREEZE, 1u, it);
                        *(volatile uint32_t *)(uintptr_t)0x01C7C3F0u = 0x52455343u;   /* one-shot mark: the boot after the readout carries on */
                        rc_recover_uboot();
                    }
                } else s_stale = 0u;
                s_last_iters = it;
            }
        }
        g_pa_gov_level = s_level;
        g_pa_drive_os2 = (s_level >= 1u) ? 1u : 0u;
        g_pa_cab_parts_limit = (s_level >= 2u) ? 8u : 16u;
        ((volatile uint32_t *)(uintptr_t)0x01C7D4C0u)[0] = s_level | ((uint32_t)s_ema / 24u) << 8;   /* for the readout */
    }
    pa_backlight_isr_tick();
    pa_gpio_scan_isr_tick();                       /* buttons + encoder, while the main loop is busy drawing */
}

static void pa_alnk0_isr_body(void) {
    PA_BENCH_STATS->isr_entries = PA_BENCH_STATS->isr_entries + 1u;
    PA_ISR_CP(0x01);
    uint8_t con2_lo = PA_READ8(JL_ALNK0_CON2);
    uint8_t pnd = (uint8_t)(con2_lo >> 4);

    PA_WRITE8(JL_ALNK0_CON2, pnd);

    if (!(pnd & (1u << PA_ALNK_RX_CH))) {
        PA_ISR_CP(0x02);                     /* not the RX channel: nothing to process */
        return;
    }

    uint16_t con0 = PA_READ16(JL_ALNK0_CON0);
    uint32_t rx_half_idx = (con0 & PA_ALNK_CON0_CH1_BUF) ? 0u : 1u;
    uint32_t tx_half_idx = (con0 & PA_ALNK_CON0_CH0_BUF) ? 0u : 1u;

    const int32_t *rx_half = &g_alnk_rx_dma[rx_half_idx * (PA_AUDIO_BLOCK_FRAMES * PA_AUDIO_CHANNELS)];
    int32_t       *tx_half = &g_alnk_tx_dma[tx_half_idx * (PA_AUDIO_BLOCK_FRAMES * PA_AUDIO_CHANNELS)];

    PA_ISR_CP(0x03);
    pa_audio_process_half(rx_half, tx_half);
    PA_ISR_CP(0x04);
    if (s_en_wait && --s_en_wait == 0u) s_en_on = 1u;                  /* the link has run its settle time on silence: the output stage on (see PA_AUDIO_EN_GATE) */
#if PA_AUDIO_EN_GATE
    /* Checked every block, not set once: the main loop also read-modify-writes PORTA_OUT (the backlight), and a write of its that this interrupt landed inside would put PA2 back low for good. */
    if (s_en_on && !(PA_READ32(JL_PORTA_OUT) & PA_AUDIO_EN_PORTA_BIT)) pa_audio_output_enable(1);
#endif

    uint8_t post_pnd = (uint8_t)(PA_READ8(JL_ALNK0_CON2) >> 4);
    if (post_pnd) {
        PA_WRITE8(JL_ALNK0_CON2, post_pnd);
        if (post_pnd & (1u << PA_ALNK_RX_CH)) {
            g_pa_audio.xrun_count++;
        }
    }
}

#if PA_SAMPLE_RATE_HZ != 44100
/* ---- EXPERIMENTAL: the codec clock probe (48 kHz builds only, build.ps1 -Rate48k) --------------------------------------------------------------------------------------------------------------
 * The 44.1 kHz setting is known: the clock source field of CLK_CON2 (bits 8..11) at 0 and ALNK0 CON3 at 0x83, which gives 44,118 Hz (the 24 MHz crystal / 544). How this chip makes 48 kHz is not
 * known from any source in this tree, so this probe finds it on the unit: for every clock source (0..15) and every value of CON3 bits 2..4 (cleared at 44.1 kHz, a divider by their place),
 * it runs the link for at most 20 ms with the interrupt not yet attached, counts the receive channel's finished halves by polling, and works out the frame rate from TIMER4. The setting
 * nearest 48,000 Hz within 0.5 % is kept; if none is that close, the 44.1 kHz setting is restored and the DSP (written for 48 kHz) runs 8.8 % off: the header says so.
 * The output stage (PA2) is off for all of it (pa_audio_init holds it low), so a wrong clock is not heard. A clock source that does not run gives no halves and is skipped.
 * RISK: bits 8..11 of CLK_CON2 may route other clocks too; a setting that stops something the boot needs would hang here, before the audio starts. This is an experimental build.
 * Results at SRAM 0x01C7DB40 (48 words; read with tools/jl_ram_read.py at that address):
 *   [0] "CK48"  [1] settings tried  [2] settings that ran  [3] the chosen setting: source | div << 4 | ok << 8 (ok 1 = within 0.5 % of 48 kHz; 0 = the 44.1 kHz setting is back)
 *   [4] the chosen setting's measured rate (Hz)  [5] the probe's time (ms)  [6..47] the settings that ran: source | div << 4 | rate (Hz) << 8, in the order tried */
#define PA_CK48 ((volatile uint32_t *)(uintptr_t)0x01C7DB40u)
#define PA_CK48_N 42u

static uint32_t pa_alnk_try_rate(uint32_t src, uint32_t div) {
    PA_WRITE16(JL_ALNK0_CON0, 0);
    PA_WRITE8(JL_ALNK0_CON2, 0x0Fu);                                    /* clear any pending half */
    PA_WRITE32(0x00010014u, (PA_READ32(0x00010014u) & ~0x00000F00u) | (src << 8));
    PA_WRITE8(JL_ALNK0_CON3, (uint8_t)(0x83u | (div << 2)));
    PA_WRITE16(JL_ALNK0_CON0, PA_ALNK_CON0_MCLK_EN | PA_ALNK_CON0_SCLK_MASTER);
    PA_WRITE16(JL_ALNK0_CON0, PA_ALNK_CON0_MCLK_EN | PA_ALNK_CON0_SCLK_MASTER | PA_ALNK_CON0_EN);
    const uint32_t t0 = plat_ticks24();
    uint32_t n = 0, tf = 0, tl = 0;
    while ((uint32_t)(plat_ticks24() - t0) < 20u * 24000u && n < 12u) {
        const uint8_t pnd = (uint8_t)(PA_READ8(JL_ALNK0_CON2) >> 4);
        if (!pnd) continue;
        PA_WRITE8(JL_ALNK0_CON2, pnd);
        if (pnd & (1u << PA_ALNK_RX_CH)) { tl = plat_ticks24(); if (n == 0u) tf = tl; n++; }
    }
    PA_WRITE16(JL_ALNK0_CON0, 0);
    PA_WRITE8(JL_ALNK0_CON2, 0x0Fu);
    if (n < 3u || tl == tf) return 0u;
    return (uint32_t)(((uint64_t)(n - 1u) * PA_AUDIO_BLOCK_FRAMES * 24000000u + (tl - tf) / 2u) / (tl - tf));
}

/* Called with ADR, LEN and CON1 set and the interrupt not attached. Leaves the chosen clock source and CON3, the link stopped. */
static void pa_alnk_clock_probe(void) {
    volatile uint32_t *R = PA_CK48;
    const uint32_t t_start = plat_ticks24();
    for (uint32_t i = 0; i < 48u; i++) R[i] = 0u;
    uint32_t tried = 0, ran = 0, best = 0u, best_rate = 0u, best_err = 0xFFFFFFFFu;
    for (uint32_t src = 0; src < 16u; src++) {
        for (uint32_t div = 0; div < 8u; div++) {
            plat_wdt_feed();
            const uint32_t rate = pa_alnk_try_rate(src, div);
            tried++;
            if (!rate) continue;
            if (ran < PA_CK48_N) R[6u + ran] = src | (div << 4) | (rate << 8);
            ran++;
            const uint32_t err = rate > 48000u ? rate - 48000u : 48000u - rate;
            if (err < best_err) { best_err = err; best = src | (div << 4); best_rate = rate; }
        }
    }
    const uint32_t ok = (best_rate != 0u && best_err * 200u <= 48000u) ? 1u : 0u;   /* within 0.5 % */
    if (!ok) { best = 0u; best_rate = pa_alnk_try_rate(0u, 0u); }                    /* back to the 44.1 kHz setting */
    PA_WRITE32(0x00010014u, (PA_READ32(0x00010014u) & ~0x00000F00u) | ((best & 0xFu) << 8));
    PA_WRITE8(JL_ALNK0_CON3, (uint8_t)(0x83u | (((best >> 4) & 7u) << 2)));
    PA_WRITE16(JL_ALNK0_CON0, PA_ALNK_CON0_MCLK_EN | PA_ALNK_CON0_SCLK_MASTER);
    R[1] = tried; R[2] = ran; R[3] = best | (ok << 8); R[4] = best_rate; R[5] = (plat_ticks24() - t_start) / 24000u;
    R[0] = 0x38344B43u;                                                               /* "CK48" (the bytes of a little-endian dump) */
}
#endif

/* Register IRQ 11 in pi32v2 vector table */
static void pa_alnk0_register_irq(void) {
    const uint32_t irq_idx = 11u; /* IRQ_ALINK0_IDX */
    const uint32_t prio    = 4u;

    /* Point vector 11 to the assembly wrapper isr_alnk0 */
    PA_WRITE32(0x01C7FE00u + (irq_idx * 4u), (uint32_t)(uintptr_t)&isr_alnk0);

    uint32_t reg_addr = 0x01EEF100u + ((irq_idx >> 3) * 4u);
    uint32_t shift    = (irq_idx & 7u) * 4u;
    uint32_t nibble   = ((prio << 1) & 0xEu) | 0x1u;
    uint32_t val      = PA_READ32(reg_addr);
    val = (val & ~(0xFu << shift)) | (nibble << shift);
    PA_WRITE32(reg_addr, val);
}
#else
void pa_audio_output_enable(int on) { (void)on; }      /* the host has no output stage */
#endif /* !PA_HOST_BUILD && !PA_HOST_SIM */

void pa_audio_init(pa_audio_block_cb_t cb) {
    uint16_t mvol  = g_pa_audio.master_vol_q15;
    uint16_t mcur  = g_pa_audio.master_vol_cur_q15;     /* what pa_audio_core_init_jack set: 0 on the device (the power-on mute) */
    uint16_t igain = g_pa_audio.input_gain_q15 ? g_pa_audio.input_gain_q15 : 0x8000u;
#if !defined(PA_HOST_BUILD) && !defined(PA_HOST_SIM)
    {   volatile pa_bench_stats_t *st = PA_BENCH_STATS;
        st->isr_entries = 0u; st->isr_last_cyc = 0u; st->isr_max_cyc = 0u;
        st->overruns = 0u; st->auto_bypass = 0u; st->magic = 0x53544154u; }
    for (uint32_t i = 0; i < 32u; i++) ((volatile uint32_t *)(uintptr_t)0x01C7C500u)[i] = 0u;   /* per-slot timing, see fx_graph.c */
    for (uint32_t i = 0; i < 8u; i++) ((volatile uint32_t *)(uintptr_t)0x01C7C660u)[i] = 0u;    /* GRU step profile, see nam_engine.c */
#endif
    for (uint32_t i = 0; i < PA_ALNK_DMA_NOMINAL * PA_ALNK_DMA_SLACK; i++) {
        g_alnk_tx_dma[i] = 0;                             /* all of it silence: the canary is for what the DMA WRITES (the RX buffer); in the TX buffer it would be played if the DMA ever read past the nominal size */
        g_alnk_rx_dma[i] = (i < PA_ALNK_DMA_NOMINAL) ? 0 : (int32_t)PA_ALNK_CANARY;
    }
    memset(&g_pa_audio, 0, sizeof(g_pa_audio));

    g_pa_audio.input_gain_q15 = igain;
    g_pa_audio.master_vol_q15 = mvol;
    /* The CURRENT level is kept as it was handed over, not set to the target. Setting it to the target undid the power-on mute: the first block went out at the saved master level and faded
     * to 0 within the block, an audible burst of the converter's start-up input through the amp at every power-on. */
    g_pa_audio.master_vol_cur_q15 = mcur;
    /* the core runs the same jack path, so the custom callback has to
     * be registered there too */
    pa_audio_core_set_block_cb(cb);

#if !defined(PA_HOST_BUILD) && !defined(PA_HOST_SIM)
    pa_audio_output_enable(0);                 /* the output stage off while the clocks start (it has been since the first lines of the boot; this is for a restart of the link) */
    s_en_wait = 0u; s_en_on = 0u;
    plat_delay_ms(5u);
    PA_WRITE16(JL_ALNK0_CON0, 0);
    PA_WRITE16(JL_ALNK0_CON1, 0);
    PA_WRITE8(JL_ALNK0_CON2,  0);
    PA_WRITE8(JL_ALNK0_CON3,  0);

    uint32_t clk_con = PA_READ32(0x00051030u);
    clk_con = (clk_con & ~0x000000C0u) | 0x00000040u;
    PA_WRITE32(0x00051030u, clk_con);

    const uint32_t out_pins = (1u << 4) | (1u << 5) | (1u << 6) | (1u << 7);
    const uint32_t in_pin   = (1u << 8);

    PA_WRITE32(JL_PORTA_OUT, PA_READ32(JL_PORTA_OUT) | out_pins);
    PA_WRITE32(JL_PORTA_DIR, (PA_READ32(JL_PORTA_DIR) & ~out_pins) | in_pin);
    PA_WRITE32(JL_PORTA_PD,  PA_READ32(JL_PORTA_PD)  & ~in_pin);
    PA_WRITE32(JL_PORTA_PU,  PA_READ32(JL_PORTA_PU)  |  in_pin);
    PA_WRITE32(JL_PORTA_DIE, PA_READ32(JL_PORTA_DIE) |  in_pin);

    PA_WRITE32(0x00010014u, PA_READ32(0x00010014u) & ~0x00000F00u);

    PA_WRITE16(JL_ALNK0_CON0, PA_ALNK_CON0_MCLK_EN | PA_ALNK_CON0_SCLK_MASTER);
    /* The DMA buffer length goes in LEN (+0x20). Writing SHN and
     * leaving LEN at its reset value made the RX DMA run far past the 1 KB buffer and corrupt
     * .bss on the bench unit (2026-10-30). */
#ifndef PA_BENCH_ALNK_SHN
#define PA_BENCH_ALNK_SHN 0        /* 0 = LEN (default bounded DMA length register) */
#endif
#if PA_BENCH_ALNK_SHN
    PA_WRITE16(JL_ALNK0_SHN,  PA_AUDIO_SHN_FRAMES);
#else
    PA_WRITE16(JL_ALNK0_LEN,  PA_AUDIO_SHN_FRAMES);
#endif
    PA_WRITE8(JL_ALNK0_CON3,  0x83u);

    PA_WRITE32(JL_ALNK0_ADR1, (uint32_t)(uintptr_t)g_alnk_rx_dma);
    PA_WRITE32(JL_ALNK0_ADR0, (uint32_t)(uintptr_t)g_alnk_tx_dma);
    PA_WRITE16(JL_ALNK0_CON1, PA_ALNK_CON1_FULL_DUPLEX);
#if PA_SAMPLE_RATE_HZ != 44100
    pa_alnk_clock_probe();                     /* EXPERIMENTAL: finds the 48 kHz clock and leaves it set (see above) */
    PA_WRITE16(JL_ALNK0_CON1, PA_ALNK_CON1_FULL_DUPLEX);
#endif

    pa_alnk0_register_irq();
    s_en_wait = PA_AUDIO_EN_SETTLE_BLOCKS;     /* the audio interrupt switches the output stage on after this many blocks of silence */
    PA_WRITE16(JL_ALNK0_CON0, PA_READ16(JL_ALNK0_CON0) | PA_ALNK_CON0_EN);
#endif

    g_pa_audio.running = 1;
}

void pa_audio_alnk_init(void) {
    pa_audio_init(0);
}

/* Before a restart the firmware itself starts (the update's reboot): fade the output to silence (the master hold: a one-block slew), wait for the fade (3 blocks, bounded to 20 ms), switch
 * the output stage off. The link keeps running on silence; the reset stops it. Main loop only. A power-off is not such a restart: the switch cuts the supply and no code runs (see main.c). */
void pa_audio_quiet_for_reset(void) {
#if !defined(PA_HOST_BUILD) && !defined(PA_HOST_SIM)
    extern volatile uint8_t g_pa_master_hold;
    g_pa_master_hold = 1u;
    if (g_pa_audio.running) {
        const uint32_t c0 = g_pa_audio.irq_count, t0 = plat_ticks24();
        while ((uint32_t)(g_pa_audio.irq_count - c0) < 3u && (uint32_t)(plat_ticks24() - t0) < 20u * 24000u) { }
    }
    s_en_wait = 0u; s_en_on = 0u;
    pa_audio_output_enable(0);
#endif
}

void pa_audio_stop(void) {
#if !defined(PA_HOST_BUILD) && !defined(PA_HOST_SIM)
    s_en_wait = 0u; s_en_on = 0u;
    pa_audio_output_enable(0);                 /* the output stage off before the clocks stop */
    PA_WRITE16(JL_ALNK0_CON0, 0);
    PA_WRITE16(JL_ALNK0_CON1, 0);
#endif
    g_pa_audio.running = 0;
}
