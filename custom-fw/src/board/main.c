/* custom-fw/src/board/main.c — M-VAVE Pocket Amp Custom Firmware Main Integration Loop
 *
 * Unifies all Quick Wins (QW-1..8), Big Rocks (BR-1..5), and Secondary Priorities (SP-1..4):
 *   - QW-2/3: Watchdog, P33 PMIC boost, 24 MHz TIMER4 cycle counter, 10 kHz TIMER5 tick
 *   - BR-1:   Full-duplex ALNK0 24-bit I2S Guitar ADC (CH1) + Stereo DAC (CH0) @ 44,118 Hz
 *   - BR-2:   On-chip 0x01EE0400 Vector/FIR Coprocessor + 16-Unit Q21 NAM A2 GRU + Q18 IR Cab
 *   - BR-3:   Full 8-Slot Effect Chain (Dynamics, Drive, Amp, Cab, EQ, Mod, Delay, Reverb) + C7 Reclaim
 *   - BR-4:   Color TFT UI (ST7735S/ST7789) + 8-Slot Strip + Parameter Bars + Chromatic Tuner + Telemetry
 *   - BR-5:   USB Composite (USB-MIDI EP4 + UAC1 24-bit Audio EP3 + OTA) & BLE MIDI Bridge
 *   - SP-1/2: Dual-Core CPU1 Post-FX Offload + Zero-Flash Procedural 808/909 Drum Machine & Metronome
 */

#include "../../include/pa_evlog.h"
#include "pa_config.h"
#include "pa_plat.h"
#include "pa_hw.h"
#include "pa_audio.h"
#include "pa_hw_accel.h"
#include "pa_usb_dev.h"
#include "pa_model.h"
#include "pa_cab.h"
#include "pa_fx.h"
#include "pa_ui.h"
#include "pa_usb_ble.h"
#include "pa_bond.h"
#include "pa_ble_radio.h"
#include "pa_smp.h"
#include "pa_drums.h"
#include "pa_preset.h"
#include "pa_sysex.h"
#include "pa_cpu_meter.h"
#include "pa_wavenet.h"
#include "pa_settings.h"
#include "pa_user_nam.h"
#include "pa_ota.h"
#include "pa_diag.h"
#include "pa_spi_nor.h"

/* Global Subsystems */
pa_fx_chain_t   g_fx_chain;
pa_nam_model_t  g_nam_model  __attribute__((aligned(32)));
#ifndef PA_HOST_SIM
pa_wavenet_model_t g_wn_model __attribute__((section(".pool"), aligned(32)));
#else
pa_wavenet_model_t g_wn_model __attribute__((aligned(32)));
#endif
int             g_active_amp_engine = 0; /* 0: GRU, 1: WaveNet */
static fx_node_t      *g_cab_node = 0;
static pa_ui_ctx_t     g_ui;
static pa_usb_ble_ctx_t g_usb_ble;
#if !defined(PA_HOST_BUILD) && !defined(PA_HOST_SIM)
#include "pa_ble_midi_bridge.h"
static void ble_midi_to_router(const uint8_t pkt4[4]) { pa_usb_midi_rx_packet(&g_usb_ble, pkt4, 4u); }
#endif
static pa_drums_t      g_drums;
static pa_cpu_meter_t  g_cpu_meter;

/* Static Pool Allocator in .pool (@ 0x01C20000) for C7 Dynamic Effect SRAM Reclaim.
 * The budget is PA_DSP_POOL_BYTES (147,456 B, src/sys/pa_pool.c); the measured
 * high-water mark with every effect on is 136,336 B (custom-fw/test/test_pool.c). */
/* The declaration is NOT inside a PA_HOST_SIM guard: the host build of this
 * file needs it too (the calibration tools link main.c), and with the guard it
 * was simply missing there, so the host build did not compile at all. */
extern uint32_t pa_pool_used_bytes(void); /* allocator lives in src/sys/pa_pool.c */
#include "pa_build_id.h"
volatile uint32_t g_main_iters;           /* main-loop pass counter (test state, and the overlay when it is on) */

/* ---- Bench debugging: the RAM black box and the on-screen overlay ---------------------
 *
 * The black box sits at the start of the NOINIT region (0x01C7C000, nothing else uses
 * it), so it survives a reset that keeps the SRAM powered (watchdog, crash) but not a
 * power-on (random contents -> wrong magic -> starts fresh). The dongle reads it from the
 * mask-ROM download mode before the flash loader is uploaded:
 *     python custom-fw/tools/pa_transporter.py blackbox
 * Layout is fixed (pa_transporter.py decodes it): 16 u32 header words, then 32 events of
 * 3 u32 (time ms, code, argument). PA_DEBUG_OVERLAY=0 removes the overlay and the black box. */
#ifndef PA_DEBUG_OVERLAY
#define PA_DEBUG_OVERLAY 0    /* off: diagnose from RAM instead (tools/jl_ram_read.py slots); -DPA_DEBUG_OVERLAY=1 brings it back */
#endif
#if PA_DEBUG_OVERLAY && !defined(PA_HOST_BUILD) && !defined(PA_HOST_SIM)
#define PA_BB_ADDR      0x01C7C000u
#define PA_BB_MAGIC     0x50414242u          /* "PABB" */
#define PA_BB_VERSION   5u
#define PA_BB_EVENTS    32u
enum { BB_EV_BOOT = 1, BB_EV_STAGE = 2, BB_EV_BUTTONS = 3, BB_EV_OTA_REBOOT = 4, BB_EV_ALIVE = 5,
       BB_EV_OTA_SPURIOUS = 6, BB_EV_STRAY_IRQ = 7 };
typedef struct {
    uint32_t magic, version, boot_count, last_uptime_ms;
    uint32_t prev_uptime_ms, loop_count, stage, ev_head;
    uint32_t porta_in, porth_in, portc_in, btn_state;
    uint32_t warm, spur_count, spur_value, spur_ms;
    uint32_t ev[PA_BB_EVENTS][3];
    uint32_t snap[16];          /* first 64 bytes of g_usb_ble at the first spurious OTA flag */
    /* v3: interrupts. irq_* count hits on the default (stray) handlers; irqen_boot is the
     * interrupt priority/enable bank (0x01EEF100..0x01EEF11C, a nibble per source, bit 0 =
     * enabled) as the bootloader left it, read before the app registers anything. */
    uint32_t irq_count, irq_first, irq_last, irq_mask_lo, irq_mask_hi;
    uint32_t irqen_boot[8];
    /* v4: clocks. cal_ticks = TIMER4 ticks for 1,000,000 iterations of a volatile loop at boot;
     * t4_secs counts 24,000,000-tick TIMER4 periods (one per second if TIMER4 really is 24 MHz) */
    uint32_t cal_ticks, t4_secs, t4_mark;
    /* v5: boot-failure streak. prev_stage = stage the previous run last reached (8 = main loop).
     * fail_streak counts consecutive warm resets that did not reach the main loop. */
    uint32_t fail_streak, prev_stage, wdt_test;
} pa_blackbox_t;
#ifndef PA_FAIL_STREAK_MAX
#define PA_FAIL_STREAK_MAX 3u
#endif
#ifndef PA_WDT_TEST_HANG
#define PA_WDT_TEST_HANG 0          /* 1 = deliberate boot hang (watchdog test only) */
#endif
#ifndef PA_SELF_RESCUE
#define PA_SELF_RESCUE 1            /* 1 = polled 5 s rescue to download mode; 0 only for the watchdog test */
#endif
#define BB ((volatile pa_blackbox_t *)(uintptr_t)PA_BB_ADDR)

/* RAM guard: 16 KB of .bss filled with a pattern at boot. Off by default since 2026-10-30:
 * every build that had it on the bench unit failed to draw, so it is a suspect itself. */
#ifndef PA_RAM_GUARD
#define PA_RAM_GUARD 0
#endif
#if PA_RAM_GUARD
#define PA_GUARD_WORDS   4096u
#define PA_GUARD_PATTERN 0x5AA5C33Cu
static uint32_t g_ram_guard[PA_GUARD_WORDS];
static void guard_fill(void) { for (uint32_t i = 0; i < PA_GUARD_WORDS; i++) g_ram_guard[i] = PA_GUARD_PATTERN ^ i; }
static uint32_t guard_check(uint32_t *first_idx, uint32_t *first_val) {
    uint32_t bad = 0;
    for (uint32_t i = 0; i < PA_GUARD_WORDS; i++) {
        uint32_t v = ((volatile uint32_t *)g_ram_guard)[i];
        if (v != (PA_GUARD_PATTERN ^ i)) { if (!bad) { *first_idx = i; *first_val = v; } bad++; }
    }
    return bad;
}
#endif /* PA_RAM_GUARD */

/* ---- stray interrupts --------------------------------------------------------------------
 * entry.S provides isr_dflt_0..63 (table isr_dflt_table). irq_install_defaults() points every
 * vector at them before interrupts are enabled; the audio driver then replaces vector 11.
 * A stray source (something the bootloader left enabled) lands in pa_default_isr_c(): it is
 * recorded and its enable nibble cleared, in both banks, clearing enable bits across both priority banks. */
#define PA_IRQ_VECTORS     0x01C7FE00u
#define PA_IRQ_PRIO_BANK0  0x01EEF100u
#define PA_IRQ_PRIO_BANK1  0x01EEF300u
/* Vector 1 is the CPU exception, not an interrupt. With PA_FAULT_HANDLER=1 it points at
 * pa_fault_entry (entry.S), which records the fault in the crash record (PA_CRASH_ADDR) and
 * resets into the mask-ROM download mode. 0 leaves the slot as the bootloader left it. */
#ifndef PA_FAULT_HANDLER
#define PA_FAULT_HANDLER 1
#endif
extern const uint32_t isr_dflt_table[64];
static void bb_event(uint32_t code, uint32_t arg);
void pa_default_isr_c(uint32_t idx);

void pa_default_isr_c(uint32_t idx) {
    uint32_t reg = (idx >> 3) * 4u, mask = 0xFu << ((idx & 7u) * 4u);
    PA_WRITE32(PA_IRQ_PRIO_BANK0 + reg, PA_READ32(PA_IRQ_PRIO_BANK0 + reg) & ~mask);
    PA_WRITE32(PA_IRQ_PRIO_BANK1 + reg, PA_READ32(PA_IRQ_PRIO_BANK1 + reg) & ~mask);
    if (BB->irq_count == 0u) BB->irq_first = idx;
    BB->irq_count = BB->irq_count + 1u;
    BB->irq_last = idx;
    if (idx < 32u) BB->irq_mask_lo = BB->irq_mask_lo | (1u << idx);
    else           BB->irq_mask_hi = BB->irq_mask_hi | (1u << (idx - 32u));
    bb_event(BB_EV_STRAY_IRQ, idx);
}

static void irq_install_defaults(void) {
    for (uint32_t i = 0; i < 8u; i++) BB->irqen_boot[i] = PA_READ32(PA_IRQ_PRIO_BANK0 + i * 4u);
    BB->irq_count = 0u; BB->irq_first = 0u; BB->irq_last = 0u; BB->irq_mask_lo = 0u; BB->irq_mask_hi = 0u;
    for (uint32_t i = 0; i < 64u; i++) {
        if (PA_FAULT_HANDLER && i == 1u) continue;     /* vector 1 = the exception handler */
        PA_WRITE32(PA_IRQ_VECTORS + i * 4u, isr_dflt_table[i]);
    }
}

static void bb_event(uint32_t code, uint32_t arg) {
    uint32_t i = BB->ev_head % PA_BB_EVENTS;
    BB->ev[i][0] = plat_ms(); BB->ev[i][1] = code; BB->ev[i][2] = arg;
    BB->ev_head = BB->ev_head + 1u;
}

static void bb_boot(void) {
    if (BB->magic != PA_BB_MAGIC || BB->version != PA_BB_VERSION) {
        volatile uint32_t *w = (volatile uint32_t *)(uintptr_t)PA_BB_ADDR;
        for (uint32_t i = 0; i < sizeof(pa_blackbox_t) / 4u; i++) w[i] = 0u;
        BB->magic = PA_BB_MAGIC; BB->version = PA_BB_VERSION;
        BB->warm = 0u;                      /* power-on: RAM was random */
    } else {
        BB->warm = 1u;                      /* RAM kept: a reset, not a power-on */
        BB->prev_uptime_ms = BB->last_uptime_ms;
        BB->prev_stage = BB->stage;
        BB->fail_streak = (BB->stage == 8u) ? 0u : BB->fail_streak + 1u;
    }
    BB->boot_count = BB->boot_count + 1u;
    BB->last_uptime_ms = 0u; BB->loop_count = 0u; BB->stage = 0u;
    BB->spur_count = 0u; BB->spur_value = 0u; BB->spur_ms = 0u;
    bb_event(BB_EV_BOOT, BB->boot_count);
}

static void bb_stage(uint32_t s) {
    plat_wdt_feed();                 /* boot checkpoint: progress was made */
    BB->stage = s;
    bb_event(BB_EV_STAGE, s);
}

static void bb_sample(void) {
    uint32_t a = PA_READ32(JL_PORTA_IN), h = PA_READ32(JL_PORTH_IN), c = PA_READ32(JL_PORTC_IN);
    uint32_t b = plat_buttons();
    if (b != BB->btn_state) bb_event(BB_EV_BUTTONS, (b << 24) | (a & 0xFFFFu));
    BB->porta_in = a; BB->porth_in = h; BB->portc_in = c; BB->btn_state = b;
    BB->last_uptime_ms = plat_ms();
    BB->loop_count = BB->loop_count + 1u;
}

extern void ui_draw_str(uint16_t x, uint16_t y, const char *s, uint16_t fg, uint16_t bg);
extern void st7735s_fill_rect(uint16_t x, uint16_t y, uint16_t w, uint16_t h, uint16_t rgb565);

static char *ov_hex(char *p, uint32_t v, int digits) {
    for (int i = digits - 1; i >= 0; i--) *p++ = "0123456789ABCDEF"[(v >> (4 * i)) & 0xFu];
    return p;
}

static char *ov_dec(char *p, uint32_t v) {
    char t[10]; int n = 0;
    do { t[n++] = (char)('0' + v % 10u); v /= 10u; } while (v && n < 10);
    while (n) *p++ = t[--n];
    return p;
}

static char *ov_str(char *p, const char *s) { while (*s) *p++ = *s++; return p; }

/* Three lines at the bottom of the screen, redrawn every 100 ms over whatever the UI drew. */

#if PA_BENCH_CRUMBS
/* Step marker: draw "cp nn" at once (magenta, y=197) and keep a copy in SRAM. The LCD keeps its
 * image when the CPU stops, so after a freeze the last marker on the screen is the last step
 * that finished its draw. Main loop only: it uses the LCD, never call it from an interrupt. */
void pa_cp(uint32_t n) {
    char l[12], *p = l;
    PA_WRITE32(0x01C7C3F4u, n);
    p = ov_str(p, "cp "); p = ov_hex(p, n, 2); *p = 0;
    st7735s_fill_rect(0, 195, 52, 11, 0x0000u);
    ui_draw_str(2, 197, l, 0xF81Fu, 0x0000u);
}
#endif

/* Overlay drawing without flicker: the old code cleared a line to black and then drew over it, so every
 * redraw showed a black frame. Now a line is padded to the full width and drawn once with its
 * background colour, which overdraws the old text in place. */
#define ov_clr(...) ((void)0)
static void ov_draw(uint16_t x, uint16_t y, char *s, uint16_t fg, uint16_t bg) {
    uint32_t n = 0;
    while (s[n]) n++;
    while (n < 39u) s[n++] = ' ';
    s[n] = 0;
    ui_draw_str(x, y, s, fg, bg);
}

static void debug_overlay_draw(void) {
    char l[48], *p;
    const uint16_t fg = 0xFFE0u, bg = 0x0000u;      /* yellow on black */
    ov_clr(0, 206, 240, 34, bg);
    PA_CP(0x3A);
    p = l; p = ov_str(p, "B"); p = ov_dec(p, BB->boot_count); p = ov_str(p, BB->warm ? "w up " : "c up ");
    p = ov_dec(p, BB->last_uptime_ms / 1000u); p = ov_str(p, "s prev "); p = ov_dec(p, BB->prev_uptime_ms / 1000u);
    p = ov_str(p, "s st"); p = ov_dec(p, BB->stage); *p = 0;
    ov_draw(2, 208, l, fg, bg);
    PA_CP(0x3B);
    p = l; p = ov_str(p, "A "); p = ov_hex(p, BB->porta_in, 8); p = ov_str(p, " H "); p = ov_hex(p, BB->porth_in, 8); *p = 0;
    ov_draw(2, 219, l, fg, bg);
    p = l; p = ov_str(p, "C "); p = ov_hex(p, BB->portc_in, 2); p = ov_str(p, " btn "); p = ov_hex(p, BB->btn_state, 2);
    p = ov_str(p, " loop "); p = ov_dec(p, BB->loop_count); *p = 0;
    ov_draw(2, 230, l, fg, bg);
    PA_CP(0x33);
    {   /* audio DMA: bytes the RX DMA really writes, interrupts/s, processed halves/s, xruns */
        extern uint32_t pa_alnk_dma_extent_bytes(void);
        extern uint32_t pa_alnk_isr_entries(void);
        static uint32_t s_t0, s_isr0, s_blk0, s_isr_ps, s_blk_ps;
        uint32_t now = plat_ms(), isr = pa_alnk_isr_entries(), blk = g_pa_audio.irq_count;
        if ((uint32_t)(now - s_t0) >= 1000u) {
            s_isr_ps = isr - s_isr0; s_blk_ps = blk - s_blk0;
            s_t0 = now; s_isr0 = isr; s_blk0 = blk;
        }
        ov_clr(0, 184, 240, 11, bg);
        p = l; p = ov_str(p, "DMA "); p = ov_dec(p, pa_alnk_dma_extent_bytes()); p = ov_str(p, "B irq ");
        p = ov_dec(p, s_isr_ps); p = ov_str(p, "/s blk "); p = ov_dec(p, s_blk_ps); p = ov_str(p, "/s xr ");
        p = ov_dec(p, g_pa_audio.xrun_count); p = ov_str(p, pa_hw_accel_broken() ? " ACC SW " : " acc ");
        p = ov_dec(p, pa_hw_accel_errors()); *p = 0;  /* "ACC SW": vector accelerator fell back to software */
        ov_draw(2, 186, l, 0x07FFu, bg);        /* cyan */
        extern uint32_t pa_alnk_isr_last_us(void);
        extern uint32_t pa_alnk_isr_max_us(void);
        ov_clr(0, 162, 240, 11, bg);
        p = l; p = ov_str(p, "ISR "); p = ov_dec(p, pa_alnk_isr_last_us()); p = ov_str(p, "us max ");
        p = ov_dec(p, pa_alnk_isr_max_us()); p = ov_str(p, "us of 1451 byp ");
        p = ov_dec(p, g_pa_audio.bypass); *p = 0;
        ov_draw(2, 164, l, 0x07FFu, bg);        /* cyan: audio ISR cost vs the 1.45 ms block */
    }
    PA_CP(0x34);
    {   /* stray interrupts: count, first and last index (magenta if any), boot enable bank 0/1 */
        ov_clr(0, 151, 240, 11, bg);
        p = l; p = ov_str(p, "IRQ stray "); p = ov_dec(p, BB->irq_count);
        if (BB->irq_count) {
            p = ov_str(p, " first "); p = ov_dec(p, BB->irq_first);
            p = ov_str(p, " last "); p = ov_dec(p, BB->irq_last);
        }
        p = ov_str(p, " en "); p = ov_hex(p, BB->irqen_boot[0], 8); p = ov_str(p, " ");
        p = ov_hex(p, BB->irqen_boot[1], 8); *p = 0;
        ov_draw(2, 153, l, BB->irq_count ? 0xF81Fu : 0x07E0u, bg);
    }
    PA_CP(0x35);
    {   /* clocks: TIMER4 "seconds" (compare with a stopwatch) and the boot CPU-loop calibration */
        /* rates kept in high SRAM (0x01C7C440), recomputed once per TIMER4 second */
        typedef struct { uint32_t isr_prev, loop_prev, isr_ps, loop_ps; } rates_t;
        volatile rates_t *rt = (volatile rates_t *)(uintptr_t)0x01C7C268u;
        typedef struct { uint32_t isr_entries, isr_last_cyc, isr_max_cyc, overruns, auto_bypass, magic; } stats_t;
        volatile stats_t *st = (volatile stats_t *)(uintptr_t)0x01C7C250u;
        uint32_t now4 = PA_READ32(JL_TIMER4_BASE + 0x04u);
        while ((uint32_t)(now4 - BB->t4_mark) >= 24000000u) {
            BB->t4_mark = BB->t4_mark + 24000000u;
            BB->t4_secs = BB->t4_secs + 1u;
            rt->isr_ps = st->isr_entries - rt->isr_prev;  rt->isr_prev = st->isr_entries;
            rt->loop_ps = g_main_iters - rt->loop_prev; rt->loop_prev = g_main_iters;   /* real loop passes per second */
        }
        ov_clr(0, 140, 240, 11, bg);
        p = l; p = ov_str(p, "CLK t4 "); p = ov_dec(p, BB->t4_secs); p = ov_str(p, "s cal ");
        p = ov_dec(p, BB->cal_ticks); p = ov_str(p, " t4/1M loops"); *p = 0;
        ov_draw(2, 142, l, 0xFFFFu, bg);        /* white */
        ov_clr(0, 129, 240, 11, bg);
        p = l; p = ov_str(p, "irq/s "); p = ov_dec(p, rt->isr_ps); p = ov_str(p, " ui/s ");
        p = ov_dec(p, rt->loop_ps); p = ov_str(p, " over "); p = ov_dec(p, st->overruns);
        p = ov_str(p, st->auto_bypass ? " AUTO-BYP" : " dsp on");
        { extern uint32_t pa_hw_accel_job_max_us(void); p = ov_str(p, " job "); p = ov_dec(p, pa_hw_accel_job_max_us()); p = ov_str(p, "us"); }
        *p = 0;
        ov_draw(2, 131, l, st->auto_bypass ? 0xFD20u : 0xFFFFu, bg);   /* orange if bypassed */
    }
    PA_CP(0x36);
    {   /* watchdog: P33 WDT_CON read back once a second (0x5D = armed at 8 s; 0x00 = not armed) */
        static uint32_t s_wdt_t0;
        static uint8_t s_wdt_v = 0xEEu;
        uint32_t wnow = plat_ms();
        if ((uint32_t)(wnow - s_wdt_t0) >= 1000u) { s_wdt_t0 = wnow; s_wdt_v = plat_wdt_read(); }
        PA_CP(0x37);
        ov_clr(0, 173, 240, 11, bg);
        PA_CP(0x38);
        p = l; p = ov_str(p, "wdt "); p = ov_hex(p, s_wdt_v, 2); *p = 0;
        ov_draw(2, 175, l, 0x07E0u, bg);        /* green */
        PA_CP(0x39);
    }
#if PA_RAM_GUARD
    {   /* RAM guard: words overwritten, the first one's address and value */
        uint32_t fi = 0, fv = 0, bad = guard_check(&fi, &fv);
        ov_clr(0, 173, 240, 11, bg);
        p = l; p = ov_str(p, "GUARD "); p = ov_dec(p, bad);
        if (bad) {
            p = ov_str(p, " @"); p = ov_hex(p, (uint32_t)(uintptr_t)&g_ram_guard[fi], 8);
            p = ov_str(p, "="); p = ov_hex(p, fv, 8);
        } else {
            p = ov_str(p, " clean @"); p = ov_hex(p, (uint32_t)(uintptr_t)g_ram_guard, 8);
        }
        *p = 0;
        ov_draw(2, 175, l, bad ? 0xF81Fu : 0x07E0u, bg);   /* magenta if hit, green if clean */
    }
#endif
    if (BB->spur_count) {                           /* only when the OTA flag was corrupted */
        ov_clr(0, 195, 240, 11, bg);
        p = l; p = ov_str(p, "OTA spur "); p = ov_dec(p, BB->spur_count); p = ov_str(p, " v");
        p = ov_hex(p, BB->spur_value, 2); p = ov_str(p, " "); p = ov_hex(p, BB->snap[0], 8);
        p = ov_str(p, " "); p = ov_hex(p, BB->snap[1], 8); p = ov_str(p, " "); p = ov_hex(p, BB->snap[2], 8); *p = 0;
        ov_draw(2, 197, l, 0xF800u, bg);        /* red */
    }
}

/* The OTA flag was non-zero but not the magic: record it (and the first time, the bytes
 * around it) instead of rebooting into the downloader. */
static void bb_ota_spurious(const void *ctx, uint8_t value) {
    if (BB->spur_count == 0u) {
        const uint32_t *src = (const uint32_t *)ctx;
        for (uint32_t i = 0; i < 16u; i++) BB->snap[i] = src[i];
        BB->spur_ms = plat_ms();
    }
    BB->spur_count = BB->spur_count + 1u;
    BB->spur_value = value;
    bb_event(BB_EV_OTA_SPURIOUS, value);
}
#define BB_BOOT()      bb_boot()
#define BB_STAGE(s)    bb_stage(s)
#define BB_EVENT(c, a) bb_event((c), (a))
#else
#define BB_BOOT()      ((void)0)
#define BB_STAGE(s)    ((void)0)
#define BB_EVENT(c, a) ((void)0)
#endif

/* The AMP node and the audio block callback now live in the portable module
 * src/sys/pa_audio_core.c, so the host emulator and the WebAssembly build run
 * the DEVICE's code: the same tone stack, the same per-model voicing and output
 * trim, the same Q21 arithmetic and the same 512-entry activation LUTs. */
#include "pa_audio_core.h"

/* The mute handshake state machine lives in pa_audio_core.c (one owner for every
 * platform), so the device, the emulator and the WASM build crossfade identically. */


/* The soft-knee ceiling and the block callback moved to pa_audio_core.c, so
 * the host emulator and the WebAssembly build run the DEVICE's code. The
 * wrappers that used to live here are gone: the ALNK driver and the portable
 * core now call pa_audio_core_process_block() directly, so keeping them would
 * have been dead code in the device's .text. */

extern void st7735s_init(void);
extern void pa_gpio_scan_init(void);
extern void pa_gpio_scan_poll_1khz(void);
extern void pa_stock_assets_init(void);

#if !defined(PA_HOST_BUILD) && !defined(PA_HOST_SIM)
/* Timer4 is the 24 MHz cycle counter the firmware times itself with (JL_TIMER4_BASE lives
 * in include/pa_hw.h, so the watchdog can be checked against it at compile time). */
#define JL_TIMER4_CON (*(volatile uint32_t *)JL_TIMER4_BASE)
#define JL_TIMER4_CNT (*(volatile uint32_t *)(JL_TIMER4_BASE + 0x04u))
#define JL_TIMER4_PRD (*(volatile uint32_t *)(JL_TIMER4_BASE + 0x08u))

static void timer4_init_24mhz(void) {
    if ((JL_TIMER4_CON & 0xFu) == ((2u << 2) | 1u)) return;
    JL_TIMER4_CON = 0;
    JL_TIMER4_PRD = 0xFFFFFFFFu;
    JL_TIMER4_CNT = 0;
    JL_TIMER4_CON = (2u << 2) | 1u | (1u << 14); /* 24 MHz OSC, run, clear pend */
}

uint32_t plat_ticks24(void) {
    return JL_TIMER4_CNT;
}

/* The millisecond clock.
 *
 * Timer4's counter is 32 bits at 24 MHz, so it wraps every 2^32 / 24e6 = 178.96
 * seconds.  Returning the raw counter divided by 24000 therefore made plat_ms() jump
 * back to zero every three minutes - which broke the auto power-off, the toasts and the
 * long-press timing.  The running count is kept by pa_ms_clock_tick() (pa_plat.h, so the
 * host can test it): the clock now wraps at 2^32 ms, 49 days. */
static pa_ms_clock_t g_ms_clock;

uint32_t plat_ms(void) {
#if !defined(PA_HOST_BUILD) && !defined(PA_HOST_SIM)
    return pa_ms_clock_tick(&g_ms_clock, JL_TIMER4_CNT, 24000u);
#else
    return 0u;
#endif
}

uint32_t plat_cycles(void) {
    return JL_TIMER4_CNT * 15u; /* 360 MHz / 24 MHz = 15x ratio */
}

void plat_delay_us(uint32_t us) {
    uint32_t start = JL_TIMER4_CNT;
    uint32_t span = us * 24u;
    while ((uint32_t)(JL_TIMER4_CNT - start) < span) {}
}

void plat_delay_ms(uint32_t ms) {
    while (ms--) {
        plat_delay_us(1000u);
    }
}
#endif

#if !defined(PA_HOST_BUILD) && !defined(PA_HOST_SIM)
/* Boot self-rescue: if the firmware has not reached the main loop within a few
 * seconds of power-on, hand control to the mask-ROM downloader so the black box
 * (which survives a reset that keeps SRAM powered) can be retrieved. */
static uint32_t g_boot_start_ticks = 0u;
#define PA_BOOT_RESCUE_TIMEOUT_TICKS  (5u * PA_TIMER_HZ)
static void pa_boot_self_rescue_check(void) {
#if PA_SELF_RESCUE
    if ((uint32_t)(plat_ticks24() - g_boot_start_ticks) >= PA_BOOT_RESCUE_TIMEOUT_TICKS) {
        rc_recover_uboot();
    }
#endif
}
#endif

/* ---- IMP-10: persistent settings ------------------------------------------------------- */



static void boot_restore(void) {
    /* The device's boot sequence lives in the portable core. */
    static const pa_board_services_t bsp = {
        plat_wdt_feed, plat_battery_mv, plat_battery_is_brownout,
        plat_backlight_set, pa_pool_used_bytes, plat_ms,
        plat_battery_adc, plat_charging
    };
    pa_audio_core_boot(&g_fx_chain, &g_ui, &g_usb_ble, &g_drums, &g_cpu_meter, &bsp);
}
static const char g_recovery_marker[] __attribute__((used)) = "RCREC1:PA:KEYS+USB";

#if PA_FAULT_HANDLER && !defined(PA_HOST_BUILD) && !defined(PA_HOST_SIM)
/* ---- CPU exception handler ------------------------------------------------------------------
 * The exception is vector 1 of the table at 0x01C7FE00 (the same table as the interrupts).
 * pa_fault_entry (entry.S) saves the frame and calls pa_fault_c(frame); this records the fault
 * in the crash record and resets into the mask-ROM download mode (the SRAM survives), so the
 * dongle can read it:   python custom-fw/tools/pa_transporter.py crash
 * The record sits after the black box in the NOINIT region (0x01C7C000 + 0x300). The
 * registers: EMU_MSG 0x01EEF0D4 and DBG_MSG 0x01EEE244 say why (DBG_MSG: the PC-limit, bus and
 * write-limit guards, per core); ILAT_CLR 0x01EEF1A4; DBG_WR_EN 0x01EEE240 (write 0xE7 to
 * unlock); DBG_MSG_CLR 0x01EEE248; ETM_CON 0x01EEF1C0 / ETM_PC 0x01EEF1C4 hold the last
 * branch targets. Reference: pi32v2 exception architecture and the Felucca BSP (MPL-2.0). */
#define PA_CRASH_ADDR   0x01C7C300u
#define PA_CRASH_MAGIC  0x52434150u          /* "PACR" */
#define PA_EMU_MSG      0x01EEF0D4u
#define PA_DBG_MSG      0x01EEE244u
#define PA_DBG_MSG_CLR  0x01EEE248u
#define PA_DBG_WR_EN    0x01EEE240u
#define PA_ILAT_CLR     0x01EEF1A4u
#define PA_ETM_CON      0x01EEF1C0u
#define PA_ETM_PC       0x01EEF1C4u
typedef struct {
    uint32_t magic, count;
    uint32_t pc, rete, retx, rets, psr, icfg, usp, ssp, sp;   /* frame words 16..24 */
    uint32_t emu, dbg, uptime_ms, stage;
    uint32_t etm[4];
    uint32_t r[16];                                           /* r0..r15 at the fault */
} pa_crash_t;                                                 /* 35 words = 140 bytes */
#define PA_CRASH ((volatile pa_crash_t *)(uintptr_t)PA_CRASH_ADDR)
extern void pa_fault_entry(void);
void pa_fault_c(const uint32_t *f);

void pa_fault_c(const uint32_t *f) {
    static volatile uint32_t in_fault;
    uint32_t n = (PA_CRASH->magic == PA_CRASH_MAGIC) ? PA_CRASH->count : 0u;
    if (in_fault++) rc_recover_uboot();          /* a fault while reporting: just reset */
    PA_CRASH->magic = PA_CRASH_MAGIC;
    PA_CRASH->count = n + 1u;
    PA_CRASH->pc = f[16];   PA_CRASH->rete = f[17]; PA_CRASH->retx = f[18];
    PA_CRASH->rets = f[19]; PA_CRASH->psr = f[20];  PA_CRASH->icfg = f[21];
    PA_CRASH->usp = f[22];  PA_CRASH->ssp = f[23];  PA_CRASH->sp = f[24];
    PA_CRASH->emu = PA_READ32(PA_EMU_MSG);
    PA_CRASH->dbg = PA_READ32(PA_DBG_MSG);
    PA_CRASH->uptime_ms = (plat_ticks24() - g_boot_start_ticks) / 24000u;
#if PA_DEBUG_OVERLAY
    PA_CRASH->stage = BB->stage;
#else
    PA_CRASH->stage = 0xFFFFFFFFu;
#endif
    for (uint32_t i = 0; i < 4u; i++) PA_CRASH->etm[i] = PA_READ32(PA_ETM_PC + 4u * i);
    for (uint32_t i = 0; i < 16u; i++) PA_CRASH->r[i] = f[i];
    rc_recover_uboot();                          /* never returns */
}

static void pa_fault_init(void) {
    PA_WRITE32(PA_ILAT_CLR, 0xFFu);
    PA_WRITE32(PA_EMU_MSG, 0xFFFFFFFFu);         /* clear what the bootloader left */
    if (!(PA_READ32(PA_DBG_WR_EN) & 1u)) PA_WRITE32(PA_DBG_WR_EN, 0xE7u);
    PA_WRITE32(PA_DBG_MSG_CLR, 0xFFFFFFFFu);
    PA_WRITE32(PA_IRQ_VECTORS + 4u, (uint32_t)(uintptr_t)pa_fault_entry);
    PA_WRITE32(PA_IRQ_PRIO_BANK0, (PA_READ32(PA_IRQ_PRIO_BANK0) & ~0xF0u) | 0xF0u);  /* enable, prio 7 */
    PA_WRITE32(PA_ETM_CON, PA_READ32(PA_ETM_CON) | 1u);                              /* branch trace */
}
#endif

/* DIAGNOSTICS (read-only / bench; hold "<" at power-on to skip both).
 *
 * 1. CLOCK DUMP (SRAM 0x01C7E900, "CLKD"): the registers of the clock tree as they stand, to compare with what the stock firmware sets (its start-up asks for a 240 MHz
 *    PLL and reads a clock table; which clock does its CPU really run on?). Words only read, never written. A marker at 0x01C7E8FC says "dump in progress"; if a read ever
 *    faults, the next warm boot sees the marker and skips the dump instead of faulting again.
 * 2. CORE CONTENTION TEST (SRAM 0x01C7E800, "CONT"): does work on core 0 slow core 1's amp? Seven phases of 2 s, 14 s after boot, the main loop on core 0 busy for 12 of every
 *    16 ms with: idle / ALU only / SRAM reads / SRAM writes / flash (XIP) reads / peripheral register reads / idle again. Per phase: the amp slot's average time on core 1
 *    (TIMER4 ticks, 24 per us), its calls, the iterations core 0 managed, and the WaveNet block counter's change (non-zero = the engine ran, the phase is not comparable). */
extern uint32_t g_boot_buttons;
extern void pa_flash_probe_run(void);
#define DG_CONT ((volatile uint32_t *)(uintptr_t)0x01C7E800u)
#define DG_CLK  ((volatile uint32_t *)(uintptr_t)0x01C7E900u)
#define DG_MARK (*(volatile uint32_t *)(uintptr_t)0x01C7E8FCu)
extern uint8_t plat_p33_read(uint16_t reg);

static void diag_clock_dump(void) {
    if ((DG_MARK & 0xFFFF0000u) == 0xD1A60000u && (DG_MARK & 0xFFFFu) != 0xFFFFu) return;       /* the last dump did not finish: do not repeat it */
    volatile uint32_t *D = DG_CLK;
    static const uint32_t base[6] = { 0x000119A0u, 0x00016A00u, 0x00010000u, 0x00040200u, 0x00040400u, 0x00011C00u };
    static const uint8_t  nw[6]   = { 16u, 8u, 8u, 4u, 4u, 4u };
    uint32_t o = 8u;
    for (uint32_t r = 0; r < 6u; r++) {
        DG_MARK = 0xD1A60000u | r;                              /* in progress: range r */
        D[1 + r] = base[r];
        for (uint32_t i = 0; i < nw[r]; i++) D[o + i] = *(volatile uint32_t *)(uintptr_t)(base[r] + 4u * i);
        o += nw[r];
    }
    DG_MARK = 0xD1A60000u | 0xA5u;
    for (uint32_t i = 0; i < 16u; i++) D[o + i] = plat_p33_read((uint16_t)i);                  /* P33 registers 0..15 (VDC14 = 6, SYSVDD = 9) */
    D[o + 16] = plat_p33_read(0x80u); D[o + 17] = plat_p33_read(0xA0u);
    D[7] = o;                                                   /* where the P33 words start */
    D[0] = 0x444B4C43u;                                         /* "CLKD" */
    DG_MARK = 0xD1A6FFFFu;                                      /* done */
}

static volatile uint32_t s_dg_sink;
static uint32_t dg_spin(uint32_t mode, uint32_t until_ticks) {
    uint32_t it = 0, x = 12345u, acc = 0;
    volatile uint32_t *sram = (volatile uint32_t *)(uintptr_t)0x01C02000u;                    /* 16 KB of low SRAM, unused while the GRU runs the amp */
    const volatile uint32_t *xip = (const volatile uint32_t *)(uintptr_t)0x02010000u;         /* the app's own code in flash: read through the XIP window */
    volatile uint32_t *tmr = (volatile uint32_t *)(uintptr_t)0x00010804u;
    uint32_t pos = 0;
    while ((int32_t)(until_ticks - plat_ticks24()) > 0) {
        for (uint32_t k = 0; k < 64u; k++) {
            switch (mode) {
            case 1: x = x * 1664525u + 1013904223u; x ^= x >> 7; acc += x; break;                                  /* ALU only */
            case 2: acc += sram[(pos++) & 4095u]; break;                                                          /* SRAM reads */
            case 3: sram[(pos++) & 4095u] = acc + pos; break;                                                     /* SRAM writes */
            case 4: acc += xip[(pos += 8u) & 0x1FFFFu]; break;                                                    /* flash reads, 32 B apart over 512 KB */
            case 5: acc += *tmr; break;                                                                           /* peripheral reads */
            default: break;
            }
        }
        it++;
    }
    s_dg_sink = acc + x;
    return it;
}

/* called from the 60 Hz UI tick; now = ms, since = ms since boot */
__attribute__((unused)) static void diag_contention_tick(uint32_t now, uint32_t since) {
    static uint32_t ph = 0xFFu, t_ph, sum0, calls0, wn0, iters;
    volatile uint32_t *AMP = (volatile uint32_t *)(uintptr_t)(0x01C7C500u + 16u * 2u);        /* [1] = sum of the slot's ticks / 16, [2] = calls */
    volatile uint32_t *WN = (volatile uint32_t *)(uintptr_t)0x01C7EF00u;                      /* [3] = WaveNet blocks */
    if (g_boot_buttons & 0x08u) return;
    /* With the Bluetooth radio running this test hung the unit: its flash-read phase (22.0 s on, 512 KB of reads 32 B apart) stopped the CPU 0.3 s in, the same
     * second after boot in two builds (packs 114, 115). Flash misses and the radio's DMA seem to deadlock the bus. The test is a bench diagnostic: it does not run beside the radio. */
    if (pa_ble_radio_running()) return;
    if (ph == 0xFFu) {
        if (since < 14000u) return;
        ph = 0u; t_ph = now; sum0 = AMP[1]; calls0 = AMP[2]; wn0 = WN[3]; iters = 0u;
        for (uint32_t i = 0; i < 32u; i++) DG_CONT[i] = 0u;
        DG_CONT[0] = 0x544E4F43u;                               /* "CONT" */
    }
    if (ph >= 7u) return;
    if ((uint32_t)(now - t_ph) >= 2000u) {                      /* the phase is over: store it */
        const uint32_t dc = AMP[2] - calls0, ds = AMP[1] - sum0;
        volatile uint32_t *R = DG_CONT + 4u + 4u * ph;
        R[0] = dc ? (ds * 16u) / dc : 0u;                       /* average amp slot time, ticks */
        R[1] = dc; R[2] = iters; R[3] = WN[3] - wn0;
        ph++;
        DG_CONT[1] = ph;
        t_ph = now; sum0 = AMP[1]; calls0 = AMP[2]; wn0 = WN[3]; iters = 0u;
        if (ph >= 7u) { DG_CONT[2] = 0x444F4E45u; return; }     /* "DONE" */
    }
    /* phases 0 and 6 are idle; 1..5 keep core 0 busy for 12 ms of this 16 ms tick */
    if (ph >= 1u && ph <= 5u) iters += dg_spin(ph, plat_ticks24() + 12u * 24000u);
}

/* BOOT TRACE (SRAM 0x01C7C600, read with tools/jl_ram_read.py boot): [0..4] PORTA OUT, DIR, PU, PD, IN as the app
 * starts (is the backlight pin already driven?), [5] magic, [8..17] TIMER4 stamps (24 MHz) at each boot stage. */
#define BT ((volatile uint32_t *)(uintptr_t)0x01C7C600u)
uint32_t g_boot_buttons;                    /* the buttons as read at power-on (the 2.5 s PARA failsafe, the test combinations, safe mode) */
#define BT_STAMP(i) (BT[(i)] = plat_ticks24())


/* RAM PROBE (SRAM 0x01C7C700): at the earliest boot point, walk the whole RAM the linker script hands out
 * ([end of .bss, 0x01C74000): the rest of the 96 KB RAM region and the 336 KB effect pool), write two opposite
 * patterns to every 64th word, read them back and restore the old value. Nothing is allocated yet, so this
 * is safe. Words: 0 magic, 1 start, 2 end, 3 words tested, 4 failures, 5 first failing address,
 * 6 last failing address, 7 bytes of the region proven good (tested words x 64 bytes minus failures). */
extern uint8_t _bss_end[];
static void pa_ram_probe(void) {
    volatile uint32_t *R = (volatile uint32_t *)(uintptr_t)0x01C7C700u;
    uint32_t start = ((uint32_t)(uintptr_t)_bss_end + 63u) & ~63u;
    const uint32_t end = 0x01C74000u;
    uint32_t tested = 0, bad = 0, first = 0, last = 0;
    for (uint32_t a = start; a + 4u <= end; a += 64u) {
        volatile uint32_t *p = (volatile uint32_t *)(uintptr_t)a;
        uint32_t old = *p, ok = 1u;
        *p = 0xA5A5A5A5u ^ a; if (*p != (0xA5A5A5A5u ^ a)) ok = 0u;
        *p = 0x5A5A5A5Au ^ a; if (*p != (0x5A5A5A5Au ^ a)) ok = 0u;
        *p = old;
        tested++;
        if (!ok) { bad++; if (!first) first = a; last = a; }
    }
    R[1] = start; R[2] = end; R[3] = tested; R[4] = bad; R[5] = first; R[6] = last;
    R[7] = (tested - bad) * 64u;
    R[0] = 0x504D4152u;                                    /* "RAMP" */
}

/* MEMORY PROBE (SRAM 0x01C7C780, 16 words, TIMER4 ticks of 24 MHz; read with jl_ram_read.py slots). Runs once at boot
 * in the (already verified) pool region, before anything is allocated. Why: the float GRU should have been several
 * times faster than the Q21 one if arithmetic were the limit, and it was not, so the loads are suspected.
 *   0/1  4096 independent loads, cold / warm pass (pool SRAM)       2/3  8192 dependent loads (pointer chase), pass 1 / 2
 *   4    the same chase in a second pool bank (0x01C50000)           5/6  4096 sequential loads from the XIP flash, cold / warm
 *   7    float dot product of 4096 pairs, two accumulators (pool)    8    sum of the loads (keeps the compiler honest)
 *   15   magic "MEMP" */
static void pa_mem_probe(void) {
    volatile uint32_t *R = (volatile uint32_t *)(uintptr_t)0x01C7C780u;
    volatile uint32_t *a = (volatile uint32_t *)(uintptr_t)0x01C30000u;       /* pool region: free at this point */
    volatile uint32_t *b = (volatile uint32_t *)(uintptr_t)0x01C50000u;       /* a second pool bank, free at boot */
    const uint32_t N = 4096u, HOPS = 8192u;
    uint32_t t0, sum = 0, p;
    for (uint32_t i = 0; i < N; i++) a[i] = i;
    for (int pass = 0; pass < 2; pass++) {
        t0 = plat_ticks24();
        for (uint32_t i = 0; i < N; i++) sum += a[i];
        R[pass] = plat_ticks24() - t0;
    }
    for (uint32_t i = 0; i < 256u; i++) a[i * 16u] = ((i + 1u) & 255u) * 16u;      /* a ring of 256 nodes, 64 B apart */
    for (int pass = 0; pass < 2; pass++) {
        p = 0; t0 = plat_ticks24();
        for (uint32_t k = 0; k < HOPS; k++) p = a[p];
        R[2 + pass] = plat_ticks24() - t0; sum += p;
    }
    for (uint32_t i = 0; i < 128u; i++) b[i * 16u] = ((i + 1u) & 127u) * 16u;      /* 128 nodes: 8 KB */
    p = 0; t0 = plat_ticks24();
    for (uint32_t k = 0; k < HOPS; k++) p = b[p];
    R[4] = plat_ticks24() - t0; sum += p;
    {
        volatile const uint32_t *x = (volatile const uint32_t *)(uintptr_t)0x02000120u;
        for (int pass = 0; pass < 2; pass++) {
            t0 = plat_ticks24();
            for (uint32_t i = 0; i < N; i++) sum += x[i];
            R[5 + pass] = plat_ticks24() - t0;
        }
    }
    {
        volatile float *f = (volatile float *)(uintptr_t)0x01C30000u;
        for (uint32_t i = 0; i < N; i++) f[i] = 1.0f + (float)(i & 7u) * 0.125f;
        float s0 = 0.0f, s1 = 0.0f;
        t0 = plat_ticks24();
        for (uint32_t i = 0; i < N; i += 2u) { s0 += f[i] * f[i + 1u]; s1 += f[i + 1u] * f[i]; }
        R[7] = plat_ticks24() - t0;
        sum += (uint32_t)(s0 + s1);
    }
    {   /* 64-bit pair loads (the core has {rA, rB} = [rP+]): sequential, pool */
        volatile const uint64_t *q = (volatile const uint64_t *)(uintptr_t)0x01C30000u;
        uint64_t s64 = 0;
        t0 = plat_ticks24();
        for (uint32_t i = 0; i < N / 2u; i++) s64 += q[i];
        R[9] = plat_ticks24() - t0; sum += (uint32_t)s64;
    }
#if !PA_HOT_RAM && !PA_RAMFN_ON   /* with code in the low SRAM (0x01C01000: PA_RAMFN) it is not empty any more: do not write test patterns into it */
    {   /* the empty low SRAM (RAMTEXT, 0x01C01000 - 0x01C05000, and the gap at 0x01C06000): saved, tested, restored */
        volatile uint32_t *save = (volatile uint32_t *)(uintptr_t)0x01C40000u;            /* pool: free at this point */
        volatile uint32_t *lo = (volatile uint32_t *)(uintptr_t)0x01C01000u;   /* sram-window-ok: this whole block is compiled out when PA_RAMFN_ON */
        volatile uint32_t *gap = (volatile uint32_t *)(uintptr_t)0x01C06000u;
        for (uint32_t i = 0; i < 4096u; i++) save[i] = lo[i];
        for (uint32_t i = 0; i < 64u; i++) lo[i * 16u] = ((i + 1u) & 63u) * 16u;           /* ring of 64 nodes, 64 B apart */
        p = 0; t0 = plat_ticks24();
        for (uint32_t k = 0; k < HOPS; k++) p = lo[p];
        R[10] = plat_ticks24() - t0; sum += p;
        for (uint32_t i = 0; i < 4096u; i++) lo[i] = i;
        t0 = plat_ticks24();
        for (uint32_t i = 0; i < 4096u; i++) sum += lo[i];
        R[11] = plat_ticks24() - t0;
        {   volatile const uint64_t *q = (volatile const uint64_t *)(uintptr_t)0x01C01000u; /* sram-window-ok */ uint64_t s64 = 0;
            t0 = plat_ticks24();
            for (uint32_t i = 0; i < 2048u; i++) s64 += q[i];
            R[13] = plat_ticks24() - t0; sum += (uint32_t)s64; }
        for (uint32_t i = 0; i < 4096u; i++) lo[i] = save[i];
        for (uint32_t i = 0; i < 2048u; i++) save[i] = gap[i];
        for (uint32_t i = 0; i < 2048u; i++) gap[i] = i;
        t0 = plat_ticks24();
        for (uint32_t i = 0; i < 2048u; i++) sum += gap[i];
        R[12] = plat_ticks24() - t0;
        for (uint32_t i = 0; i < 2048u; i++) gap[i] = save[i];
    }
#endif
    R[8] = sum;
    R[15] = 0x504D454Du;                                     /* "MEMP" */
}

/* OP COST PROBE (SRAM 0x01C7CB00, 16 words; READ-ONLY apart from its own result block and locals). Run once at boot, before the audio starts: the time of N = 2048 of
 * each basic operation, in TIMER4 ticks (24 per us), so that a change to the DSP code is judged on what the operation costs on this core and not on a guess. Every
 * loop reads one volatile operand per pass (so nothing folds); word 0 is that loop with nothing else in it, to be subtracted.
 *   0 baseline   1 float add (dependent)   2 float multiply (dependent)   3 float divide (dependent)   4 int -> float   5 float -> int
 *   6 read-modify-write of a word in a static volatile struct   7 the same on the stack   8 fabsf as a library call   9 __builtin_fabsf   10 sqrtf (library)
 *   11 64-bit multiply-accumulate   12 integer divide   13 a call to an empty noinline function   14 N   15 magic "OPCS" */
#ifndef PA_OPCOST_PROBE
#define PA_OPCOST_PROBE 1
#endif
#if PA_OPCOST_PROBE
extern float fabsf(float);
extern float sqrtf(float);
static volatile struct { uint32_t f[8]; } s_opc_struct;
__attribute__((noinline)) static uint32_t opc_empty(uint32_t v) { return v + 1u; }
static void pa_opcost_probe(void) {
    volatile uint32_t *R = (volatile uint32_t *)(uintptr_t)0x01C7CB00u;
    enum { N = 2048 };
    volatile float vc = 1.0000001f, vd = 1.0000002f;
    volatile int32_t vi = 12345;
    volatile uint32_t sink = 0u;
    uint32_t t0;
    float x;
    uint32_t acc;
    int64_t acc64;

    t0 = plat_ticks24();
    for (int i = 0; i < N; i++) { const float c = vc; (void)c; }
    R[0] = plat_ticks24() - t0;

    x = 1.0f; t0 = plat_ticks24();
    for (int i = 0; i < N; i++) { const float c = vc; x = x + c; }
    R[1] = plat_ticks24() - t0; sink = (uint32_t)x;

    x = 1.0f; t0 = plat_ticks24();
    for (int i = 0; i < N; i++) { const float c = vc; x = x * c; }
    R[2] = plat_ticks24() - t0; sink = (uint32_t)x;

    x = 1.0f; t0 = plat_ticks24();
    for (int i = 0; i < N; i++) { const float c = vd; x = x / c; }
    R[3] = plat_ticks24() - t0; sink = (uint32_t)x;

    x = 0.0f; t0 = plat_ticks24();
    for (int i = 0; i < N; i++) { const float c = vc; x += (float)(vi + i) * c; }
    R[4] = plat_ticks24() - t0; sink = (uint32_t)x;

    acc = 0u; t0 = plat_ticks24();
    for (int i = 0; i < N; i++) { const float c = vc; acc += (uint32_t)(int32_t)(c * (float)(i + 1)); }
    R[5] = plat_ticks24() - t0; sink = acc;

    t0 = plat_ticks24();
    for (int i = 0; i < N; i++) { const float c = vc; (void)c; s_opc_struct.f[i & 7] += 1u; }
    R[6] = plat_ticks24() - t0;

    { volatile uint32_t loc[8] = { 0 };
      t0 = plat_ticks24();
      for (int i = 0; i < N; i++) { const float c = vc; (void)c; loc[i & 7] += 1u; }
      R[7] = plat_ticks24() - t0; sink = loc[0]; }

    x = 1.0f; t0 = plat_ticks24();
    for (int i = 0; i < N; i++) { const float c = vc; x = fabsf(x + c); }
    R[8] = plat_ticks24() - t0; sink = (uint32_t)x;

    x = 1.0f; t0 = plat_ticks24();
    for (int i = 0; i < N; i++) { const float c = vc; x = __builtin_fabsf(x + c); }
    R[9] = plat_ticks24() - t0; sink = (uint32_t)x;

    x = 1.0f; t0 = plat_ticks24();
    for (int i = 0; i < N; i++) { const float c = vc; x = sqrtf(x * c); }
    R[10] = plat_ticks24() - t0; sink = (uint32_t)x;

    acc64 = 0; t0 = plat_ticks24();
    for (int i = 0; i < N; i++) { const int32_t a = vi + i; acc64 += (int64_t)a * (int64_t)vi; }
    R[11] = plat_ticks24() - t0; sink = (uint32_t)acc64;

    acc = 1u; t0 = plat_ticks24();
    for (int i = 0; i < N; i++) { const uint32_t d = (uint32_t)vi + 1u; acc = (acc + 1000003u) / d + 7u; }
    R[12] = plat_ticks24() - t0; sink = acc;

    acc = 0u; t0 = plat_ticks24();
    for (int i = 0; i < N; i++) { acc = opc_empty(acc); }
    R[13] = plat_ticks24() - t0; sink = acc;

    (void)sink;
    R[14] = (uint32_t)N;
    R[15] = 0x5343504Fu;                                     /* "OPCS" */
}
#endif

/* RUNTIME MEMORY PROBE (SRAM 0x01C7C7C0): the same loads again while the audio DMA and LCD are live, in the free tail of the
 * RAM region (0x01C1E000..0x01C20000, after .bss; nothing uses it). Words: 0 dependent chase (128 nodes, 8192 hops),
 * 1 sequential 2048 loads, 2 magic, 4 g_active_amp_engine, 5 WaveNet model active, 6 GRU model active. */
extern int g_active_amp_engine;
extern pa_nam_model_t g_nam_model;
extern pa_wavenet_model_t g_wn_model;
static void pa_mem_probe_runtime(void) {
    /* READ-ONLY. The first version wrote a test pattern into 0x01C1E000.., which became live .bss once the data region grew
     * (that is what turned g_nam_model.active into 148). Now it only records the engine flags. */
    volatile uint32_t *R = (volatile uint32_t *)(uintptr_t)0x01C7C7C0u;
    R[4] = (uint32_t)g_active_amp_engine; R[5] = (uint32_t)g_wn_model.active; R[6] = (uint32_t)g_nam_model.active;
    R[2] = 0x52544D50u;                                      /* "RTMP" */
}

/* CPU1 PROBE, stage 1: READ-ONLY (SRAM 0x01C7CA00). Runs only when VOL- and VOL+ are both held at power-on, so a normal boot
 * never touches these addresses. Before every read the address is stored in R[1]; if the read faults or hangs, the fault
 * handler or the watchdog returns the unit to download mode with SRAM intact and R[1] names the culprit.
 * R[0] magic "C1PR", R[1] address being read (0 when finished), R[2] reads completed, R[3] 0xC1D0 when finished,
 * then (address, value) pairs from R[8].
 * The first group is the system block around JL_CPU1_CTRL (0x01EEE004), already used by the firmware (mapped); the second
 * group is the unproven candidate set from include/pa_smp.h. */
extern uint8_t plat_p33_read(uint16_t reg);
extern void pa_cpu1_selftest(int raise_voltage);
static void pa_cpu1_probe(void) {
    static const uint32_t addrs[] = {
        0x01EEE000u, 0x01EEE004u, 0x01EEE008u, 0x01EEE00Cu, 0x01EEE010u, 0x01EEE014u, 0x01EEE018u, 0x01EEE01Cu,
        0x01EEE020u, 0x01EEE024u, 0x01EEE028u, 0x01EEE02Cu, 0x01EEE030u, 0x01EEE034u, 0x01EEE038u, 0x01EEE03Cu,
        0x01EEE340u, 0x01EEE344u, 0x01EEE348u, 0x01EEE34Cu,
        0x00010008u, 0x00010804u,
        0x01C80000u, 0x01C84000u,          /* just above the 512 KB window: is there more RAM (the spec says 578 KB)? */
        0x001F0000u, 0x001F0004u, 0x001F0018u, 0x001F001Cu, 0x001F0914u, 0x001F0918u,
    };
    volatile uint32_t *R = (volatile uint32_t *)(uintptr_t)0x01C7CA00u;
    const uint32_t n = (uint32_t)(sizeof addrs / sizeof addrs[0]);
    R[0] = 0x52503143u; R[2] = 0u; R[3] = 0u;
    for (uint32_t i = 0; i < n; i++) {
        R[1] = addrs[i];                                      /* the marker: survives a fault or a hang */
        __asm__ volatile("csync" ::: "memory");
        const uint32_t v = *(volatile uint32_t *)(uintptr_t)addrs[i];
        R[8 + 2 * i] = addrs[i]; R[9 + 2 * i] = v;
        R[2] = i + 1u;
    }
    R[4] = plat_p33_read(0x06) & 7u; R[5] = plat_p33_read(0x09) & 15u;       /* VDC14 and SYSVDD levels (bootloader: 3 and 11) */
    {   /* a survey of the P33 registers, to tell a real reading from a helper that returns all-ones */
        volatile uint32_t *S = (volatile uint32_t *)(uintptr_t)0x01C7CE00u;
        for (uint32_t i = 0; i < 16u; i++) S[i] = plat_p33_read((uint16_t)i);
        S[16] = plat_p33_read(0x80u); S[17] = plat_p33_read(0xA0u); S[18] = 0x50333353u;      /* "S33P" */
    }
    R[1] = 0u; R[3] = 0xC1D0u;
}

void pa_firmware_init(void) {
#if !defined(PA_HOST_BUILD) && !defined(PA_HOST_SIM)
    BT[0] = PA_READ32(JL_PORTA_OUT); BT[1] = PA_READ32(JL_PORTA_DIR); BT[2] = PA_READ32(JL_PORTA_PU);
    BT[3] = PA_READ32(JL_PORTA_PD);  BT[4] = PA_READ32(JL_PORTA_IN);  BT[5] = 0x544F4F42u;
    for (uint32_t i = 8; i < 24u; i++) BT[i] = 0u;
    /* Backlight off at once (PA9 high = off, active-low), before anything slow: the panel shows its
     * power-up noise for as long as the light is on and the LCD has not been initialised yet. */
    PA_WRITE32(JL_PORTA_OUT, PA_READ32(JL_PORTA_OUT) | PA_LCD_BL_PORTA_BIT);
    PA_WRITE32(JL_PORTA_DIR, PA_READ32(JL_PORTA_DIR) & ~PA_LCD_BL_PORTA_BIT);
    timer4_init_24mhz();
    g_boot_start_ticks = plat_ticks24();
    {   /* STACK WATERMARK: paint the unused part of the user stack (the main loop AND the audio interrupt run on it) with a pattern; the
         * readout finds how deep it was ever used. Done this early, before the audio interrupt exists, so nothing live is overwritten. */
        extern uint32_t _ustack_lo;
        volatile uint32_t *lo = (volatile uint32_t *)&_ustack_lo;
        volatile uint32_t here;                                    /* a local: its address is about where the stack is now */
        volatile uint32_t *top = (volatile uint32_t *)((uintptr_t)&here - 512u);
        for (volatile uint32_t *q = lo; q < top; q++) *q = 0xA5A5A5A5u;
    }
    pa_ram_probe();
    pa_mem_probe();
#if PA_OPCOST_PROBE
    pa_opcost_probe();
#endif
    BT_STAMP(8);
    BB_BOOT();
#if PA_FAULT_HANDLER
    pa_fault_init();               /* the exception handler is in place before anything else can fault */
#endif
#define PA_RUNFLAG_ADDR  0x01C7D5F0u        /* set when the main loop is running; survives a warm reset, not a power-off */
#define PA_RUNFLAG_MAGIC 0x31204E55u
#if !PA_DEBUG_OVERLAY
    {   /* FREEZE RESCUE (these two lived in the overlay build only, so pack 14 on had neither, and a hang stayed frozen with nothing to
         * read). If the last run reached the main loop and the memory survived, it did not end cleanly (the watchdog reset a hang, or a
         * fault): go to the mask-ROM download mode ONCE, with the SRAM untouched, so `jl_ram_read.py slots` shows what the unit was doing.
         * The mark makes the boot after that readout carry on. A power-off scrambles the memory, so a normal power-on never trips it. */
        volatile uint32_t *run  = (volatile uint32_t *)(uintptr_t)PA_RUNFLAG_ADDR;
        volatile uint32_t *mark = (volatile uint32_t *)(uintptr_t)0x01C7C3F0u;
        if (*run == PA_RUNFLAG_MAGIC) {
            *run = 0u;
            /* WHY the last run ended in a reset: the chip's reset-source register (P33 0x12) as this boot finds it, kept in SRAM 0x01C7D5C8 for the readout
             * ("RST" in the top three bytes). A watchdog, a fault, a power glitch and our own download-mode reset each leave their own value. The main loop clears it. */
            *(volatile uint32_t *)(uintptr_t)0x01C7D5C8u = 0x52535400u | plat_p33_read(0x0012u);
            if (*mark == 0x52455343u) *mark = 0u;                  /* "RESC": the boot after the readout (or after a deliberate return) */
            else { *mark = 0x52455343u; rc_recover_uboot(); }
        }
        plat_wdt_arm(0x0Du);                                       /* arm the P33 watchdog (about 8 s); the main loop feeds it every tick */
    }
#endif
#if PA_DEBUG_OVERLAY
    if (BB->fail_streak >= PA_FAIL_STREAK_MAX) rc_recover_uboot();   /* too many failed boots: download mode */
#ifndef PA_RESCUE_ON_RESET
#define PA_RESCUE_ON_RESET 1       /* a reset after the main loop was reached goes to download mode once */
#endif
#if PA_RESCUE_ON_RESET
    {   /* The last run reached the main loop (stage 8) and was then reset: with the watchdog armed
         * that is a hang. Go to the mask-ROM download mode ONCE so the dongle can read the black
         * box (the SRAM survives). The mark makes the boot after that readout (runapp) carry on. */
        volatile uint32_t *mark = (volatile uint32_t *)(uintptr_t)0x01C7C3F0u;
        if (BB->warm && BB->prev_stage == 8u) {
            if (*mark == 0x52455343u) *mark = 0u;              /* "RESC": the boot after the readout */
            else { *mark = 0x52455343u; rc_recover_uboot(); }
        }
    }
#endif
#ifndef PA_WDT_ARM
#define PA_WDT_ARM 1               /* 1: really arm the P33 watchdog (about 8 s); a hang then resets */
#endif
#if PA_WDT_ARM
    plat_wdt_arm(0x0Du);           /* the earlier "arm" only fed it; this sets the enable bit */
#else
    plat_wdt_feed();               /* feed only: the watchdog is not armed (see PA_WDT_ARM) */
#endif
#ifndef PA_BENCH_IRQ_LOGGER
#define PA_BENCH_IRQ_LOGGER 0      /* Optional diagnostic IRQ logger: off by default */
#endif
#if PA_BENCH_IRQ_LOGGER
    irq_install_defaults();        /* before any sti: every vector gets a logging handler */
#else
    for (uint32_t i = 0; i < 8u; i++) BB->irqen_boot[i] = PA_READ32(PA_IRQ_PRIO_BANK0 + i * 4u);
#endif
    BT_STAMP(9);
    {   /* clock calibration: TIMER4 ticks for a fixed CPU loop (24 MHz assumed for TIMER4) */
        uint32_t t0 = JL_TIMER4_CNT;
        for (volatile uint32_t i = 0; i < 1000000u; i++) { }
        BB->cal_ticks = JL_TIMER4_CNT - t0;
        BB->t4_secs = 0u; BB->t4_mark = JL_TIMER4_CNT;
        volatile uint32_t *rates = (volatile uint32_t *)(uintptr_t)0x01C7C268u;
        for (uint32_t i = 0; i < 4u; i++) rates[i] = 0u;   /* irq/s and ui/s, see the overlay */
    }
    BT_STAMP(10);
    {   /* the app's real core clock (the ROM-mode clock is lower): 64 dependent adds x 20,000 iterations */
        extern uint32_t pa_alu_loop(uint32_t iters);
        volatile uint32_t *clk = (volatile uint32_t *)(uintptr_t)0x01C7C5A0u;
        uint32_t t0 = JL_TIMER4_CNT;
        (void)pa_alu_loop(20000u);
        uint32_t dt = JL_TIMER4_CNT - t0;
        clk[0] = dt;                                                          /* TIMER4 ticks (24 MHz) */
        clk[1] = dt ? (uint32_t)((uint64_t)20000u * 64u * 24u / dt) : 0u;     /* MHz */
        clk[2] = 0x4B4C4352u;
    }
#if PA_RAM_GUARD
    guard_fill();
#endif
#endif
    BT_STAMP(11);
    pa_gpio_scan_init();
#if !defined(PA_HOST_BUILD) && !defined(PA_HOST_SIM)
    pa_boot_self_rescue_check();
#endif
    BB_STAGE(1);
    /* Boot-time failsafe key check: hold PARA (bit 5) for 2.5s on power-up to enter mask-ROM */
    {
        uint32_t held_ms = 0;
        pa_gpio_scan_poll_1khz();
        while ((plat_buttons() & (1u << 5)) != 0u) {
            pa_gpio_scan_poll_1khz();
            plat_delay_ms(20u);
            held_ms += 20u;
            plat_wdt_feed();
            if (held_ms >= 2500u) {
                rc_recover_uboot();
            }
        }
    }
    BT_STAMP(12);
#ifndef PA_FLASH_TEST_IMAGE
#define PA_FLASH_TEST_IMAGE 0   /* 1 = a DIAGNOSTIC image, not the firmware: after the PARA failsafe above it runs the READ-ONLY flash controller probe (flash_ram.c) and goes straight to the ROM download mode with the memory intact, so the result can always be read (jl_ram_read.py flashprobe ADDR) whatever the probe did to the app. Replace it with a normal pack afterwards. */
#endif
#if PA_FLASH_TEST_IMAGE
    { extern void pa_flash_probe_boot(void); pa_flash_probe_boot(); rc_recover_uboot(); }
#endif
    {   /* the CPU1 read-only probe runs only with VOL- and VOL+ held at power-on (logical buttons 1 and 2) */
        extern uint32_t plat_buttons(void);
        for (int k = 0; k < 8; k++) { pa_gpio_scan_poll_1khz(); plat_delay_ms(2u); }    /* plat_buttons() is only refreshed by the poll */
        const uint32_t b = plat_buttons();
        BT[19] = b;                                                       /* boot trace: the buttons as read at power-on */
        g_boot_buttons = b;
        if ((b & 0x06u) == 0x06u) pa_cpu1_probe();                    /* stage 1: VOL- + VOL+ (read only) */
        if ((b & 0x07u) == 0x07u)                                      /* stage 2: HOME + VOL- + VOL+ ... which start variant: */
            pa_cpu1_selftest((b & 0x08u) ? 2 : (b & 0x10u) ? 1 : 0);   /*   + "<" = RAM stub, + ">" = flash stub with clock bit, none = stock replica */
    }
    st7735s_init();
    BT_STAMP(16);
#if !defined(PA_HOST_BUILD) && !defined(PA_HOST_SIM)
    pa_boot_self_rescue_check();
#endif
    BB_STAGE(2);
#endif
    pa_stock_assets_init();
#if !defined(PA_HOST_BUILD) && !defined(PA_HOST_SIM)
    pa_boot_self_rescue_check();
#endif
    BB_STAGE(3);
#if PA_WDT_TEST_HANG && !defined(PA_HOST_BUILD) && !defined(PA_HOST_SIM)
    for (;;) { }                    /* deliberate hang: no feeds, the watchdog must reset the chip */
#endif
    pa_hw_accel_init();
    cpu_meter_init(&g_cpu_meter, PA_CPU_HZ, PA_BLOCK_N, PA_SAMPLE_RATE);
#if !defined(PA_HOST_BUILD) && !defined(PA_HOST_SIM)
    cpu_meter_set_clock(&g_cpu_meter, plat_cycles);
#endif

    fx_node_t *amp_nd = pa_amp_node_create(&g_nam_model, &g_wn_model, &g_active_amp_engine);
    g_cab_node        = fx_cab_ir_new();
    pa_fx_chain_init(&g_fx_chain, amp_nd, g_cab_node);
    g_fx_chain.meter = &g_cpu_meter;

#if !defined(PA_HOST_BUILD) && !defined(PA_HOST_SIM)
    pa_boot_self_rescue_check();
#endif
    BB_STAGE(4);
    pa_ui_init(&g_ui);
    pa_drums_init(&g_drums);
    pa_usb_ble_init(&g_usb_ble);
#if !defined(PA_HOST_BUILD) && !defined(PA_HOST_SIM)
    pa_ble_bridge_set_rx_consumer(ble_midi_to_router);          /* BLE-MIDI from a central enters the same router as USB MIDI */
#endif
#if !defined(PA_HOST_BUILD) && !defined(PA_HOST_SIM)
    pa_boot_self_rescue_check();
#endif
    BB_STAGE(5);
    /* Attach SPI NOR flash reader to the diagnostic channel. */
    pa_diag_set_flash_reader(pa_spi_nor_read);
#if !defined(PA_HOST_BUILD) && !defined(PA_HOST_SIM)
#ifndef PA_FLASH_PROBE_RAM
#define PA_FLASH_PROBE_RAM 1   /* the read-only flash-controller probe (flash_ram.c): ON since pack 168 with ONE trigger, '<' alone held at power-on. Its first hardware run (pack 140, auto-run after every software reset) was followed by a dead USB and a pegged input meter, cause not shown, and it entered the wrong address (fixed in pack 167): so it never runs by itself now */
#endif
#if PA_FLASH_PROBE_RAM
    { extern void pa_flash_probe_boot(void);
      if ((g_boot_buttons & 0x1Fu) == 0x08u) pa_flash_probe_boot();                                  /* '<' alone: the read-only probe (a diagnostic; the driver is armed either way, by pa_spi_nor_init) */
    }   /* '<' alone at power-on, and nothing else: a hang here is restarted by the watchdog and the next boot skips the probe (the marker in flash_ram.c); interrupts are off and core 1 has not started */
#endif
    pa_ble_radio_boot(g_boot_buttons);   /* the radio starts only on the power-on gesture (pa_ble_radio.h); before core 1 starts, interrupts still off */
#endif
    pa_smp_init(&g_fx_chain);
    /* Bind the runtime the portable core's jack path (the device's audio I/O)
     * runs on, so the ISR, the emulator and the browser all drive the same
     * chain through the same code. */
    pa_audio_core_bind(&g_fx_chain, &g_ui, &g_drums, &g_usb_ble);
#if !defined(PA_HOST_BUILD) && !defined(PA_HOST_SIM)
    { extern int pa_ota_boot_cleanup(void); (void)pa_ota_boot_cleanup(); }   /* a staged update loader an update left behind (0xB8000..0xBCFFF) is erased, before the settings are read */
#endif
    /* Restore settings + the last preset BEFORE the audio ISR starts. The ISR
     * runs the chain, so applying a preset afterwards rewrote chain state
     * underneath the audio thread. */
    boot_restore();
#if !defined(PA_HOST_BUILD) && !defined(PA_HOST_SIM)
    pa_ble_radio_restore_bond();     /* the bond of a central that paired before the power-off goes back into the stack's key slot (pa_bond.h) */
    pa_boot_self_rescue_check();
#endif
    BB_STAGE(6);
    /* Apply restored output level and unity gain BEFORE starting ALNK to prevent
     * any initial 100% volume transient on the first audio block */
    pa_audio_core_init_jack(&g_ui);
    /* Bench bisect: PA_BENCH_NO_AUDIO=1 leaves the ALNK (I2S + its DMA) off, to tell whether
     * the RAM corruption seen on the bench unit comes from the audio DMA. */
#ifndef PA_BENCH_NO_AUDIO
#define PA_BENCH_NO_AUDIO 0          /* Diagnostic: 1 disables ALNK DMA during isolation testing */
#endif
#if !PA_BENCH_NO_AUDIO || defined(PA_HOST_BUILD) || defined(PA_HOST_SIM)
    pa_audio_alnk_init();
#endif
    /* Bench: PA_BENCH_AUDIO_BYPASS=1 makes the audio ISR copy input to output with no DSP,
     * to tell whether the DSP load in the ISR is what starves the main loop. */
#ifndef PA_BENCH_AUDIO_BYPASS
#define PA_BENCH_AUDIO_BYPASS 0
#endif
#if PA_BENCH_AUDIO_BYPASS && !defined(PA_HOST_BUILD) && !defined(PA_HOST_SIM)
    g_pa_audio.bypass = 1;
#endif
#if !defined(PA_HOST_BUILD) && !defined(PA_HOST_SIM)
    pa_boot_self_rescue_check();
#endif
    BB_STAGE(7);
#if !defined(PA_HOST_BUILD) && !defined(PA_HOST_SIM)
    pa_boot_self_rescue_check();
#endif
}

/* The Settings > Bluetooth row changed the mode (ui_app.c calls this): OFF stops the radio's advertising, BLE / BLE+A2DP start it again. */
void pa_board_bt_mode_changed(uint8_t mode) { pa_ble_radio_set_enabled(mode != 0u); }

/* ---- the test-state block ------------------------------------------------------------------------
 * A fixed block in SRAM that the main loop refreshes every UI tick. The PC test runner
 * (tools/hw_test.py) reads it over the diagnostic channel's RAM read (F0 7D 02 ...) to see what the
 * unit is doing, without knowing any struct layout; it is also readable from the ROM download mode
 * after a stall. All words are little-endian u32, at 0x01C7C390 (12 words, 0x01C7C390..0x01C7C3BF):
 *   [0]  magic "TSTA" 0x41545354 (written last)           [1]  main-loop passes since boot
 *   [2]  seconds since boot (TIMER4)                       [3]  view | preset<<8 | master volume<<16 | play mode<<24
 *   [4]  selected slot | cursor slot<<8 | editor cursor<<16 | edit mode<<24
 *   [5]  sub cursor | settings cursor<<8 | in peak %<<16 | out peak %<<24
 *   [6]  battery mV | batt %<<16 | save_failed<<24 | settings_dirty<<25 | usb_connected<<26
 *   [7]  drum bpm | drum playing<<16 | drum pattern<<24    [8]  button mask (logical bits, 1 = down)
 *   [9]  audio interrupt entries                           [10] audio bypass | accelerator broken<<8 | auto-bypass<<16
 *   [11] reserved */
#define PA_TEST_STATE ((volatile uint32_t *)0x01C7C390u)
static void pa_test_state_update(void) {
    extern uint32_t pa_alnk_isr_entries(void);
    volatile uint32_t *t = PA_TEST_STATE;
    t[1]  = g_main_iters;
    t[2]  = (plat_ticks24() - g_boot_start_ticks) / 24000000u;
    t[3]  = (uint32_t)g_ui.view | (uint32_t)g_ui.preset_index << 8 | (uint32_t)g_ui.master_volume << 16 | (uint32_t)g_ui.play_mode << 24;
    t[4]  = (uint32_t)g_ui.selected_slot | (uint32_t)g_ui.cursor_slot_idx << 8 | ((uint32_t)(uint8_t)g_ui.editor_cursor) << 16 | (uint32_t)g_ui.edit_mode << 24;
    t[5]  = (uint32_t)g_ui.sub_cursor | (uint32_t)g_ui.settings_cursor << 8 | (uint32_t)g_ui.in_peak_pct << 16 | (uint32_t)g_ui.out_peak_pct << 24;
    t[6]  = (uint32_t)g_ui.battery_mv | (uint32_t)g_ui.batt_pct << 16 | (uint32_t)(g_ui.save_failed ? 1u : 0u) << 24 |
            (uint32_t)(g_ui.settings_dirty ? 1u : 0u) << 25 | (uint32_t)(g_ui.usb_connected ? 1u : 0u) << 26;
    t[7]  = (uint32_t)g_ui.drum_bpm | (uint32_t)g_ui.drum_playing << 16 | (uint32_t)g_ui.drum_pattern << 24;
    t[8]  = plat_buttons();
    t[9]  = pa_alnk_isr_entries();
    t[10] = (uint32_t)(g_pa_audio.bypass ? 1u : 0u) | (uint32_t)(pa_hw_accel_broken() ? 1u : 0u) << 8;
    t[11] = 0u;
    t[0]  = 0x41545354u;
}

void pa_firmware_tick(uint32_t buttons, int32_t enc_delta, const uint16_t adc_pots[4]) {
    /* Dynamically wake or idle/clock-gate CPU1 depending on DSP effect load & NAM usage */
    PA_CP(0x50);
    pa_smp_update_demand(&g_fx_chain);
    PA_CP(0x51);

    /* The whole 60 Hz tick lives in the portable core (src/sys/pa_audio_core.c)
     * so the device, the host emulator and the WebAssembly build run the SAME
     * firmware logic. Only the platform services are injected here. */
    static const pa_board_services_t bsp = {
        plat_wdt_feed, plat_battery_mv, plat_battery_is_brownout,
        plat_backlight_set, pa_pool_used_bytes, plat_ms,
        plat_battery_adc, plat_charging
    };
    pa_audio_core_tick(&g_fx_chain, &g_ui, &g_drums, &g_usb_ble, &g_cpu_meter, &bsp,
                       buttons, enc_delta, adc_pots);
    PA_CP(0x52);
}
void pa_main_loop(void) {
#if !defined(PA_HOST_BUILD) && !defined(PA_HOST_SIM)
    BT_STAMP(17);
    uint32_t last_ui_tick = plat_ms();
    const uint32_t s_t0_probe = last_ui_tick;
#if PA_DEBUG_OVERLAY
    uint32_t last_dbg = last_ui_tick;
    BB_STAGE(8);                                   /* 8 = main loop running */
#endif
    { extern volatile uint8_t g_pa_irq_on; g_pa_irq_on = 1u; { extern void pa_flash_dev_boot_done(void); pa_flash_dev_boot_done(); } }   /* the interrupts are on from here: a flash operation must quiesce the system (pa_audio_core.c) */
    pa_evlog_init();
    pa_evlog(PA_EV_BOOT, g_boot_buttons, 0u);
    PA_WRITE32(0x01C7D5C8u, 0u); PA_WRITE32(0x01C7D5CCu, 0u);   /* the rescue boot's reset record (above) has been read by now: a boot that reaches the main loop starts clean */
    {   /* which build this is: SRAM 0x01C7D5B0 = "BLDI", the id (yyMMddHHmm), then the build time as 16 characters (jl_ram_read.py prints it) */
        volatile uint32_t *B = (volatile uint32_t *)(uintptr_t)0x01C7D5B0u;
        const char *t = PA_BUILD_TEXT;
        uint32_t w[4] = { 0, 0, 0, 0 };
        for (uint32_t i = 0; i < 16u && t[i]; i++) w[i >> 2] |= (uint32_t)(uint8_t)t[i] << (8u * (i & 3u));
        B[1] = PA_BUILD_ID; B[2] = w[0]; B[3] = w[1]; B[4] = w[2]; B[5] = w[3];
        B[0] = 0x49444C42u;                                    /* "BLDI", written last */
    }
    {   extern void st7735s_text_selftest(void); st7735s_text_selftest(); }
#ifndef PA_AUTO_UBOOT_SECS
#define PA_AUTO_UBOOT_SECS 0     /* OFF: it looked like a crash 170 s after boot. (HOME + PARA for 2 s still drops to download mode.) Was 170: (HOME + PARA click for 2 s returns earlier): this many real seconds after boot (TIMER4, 24 MHz) the app resets into the mask-ROM
                                  * download mode, SRAM kept, so the dongle can read the logs and flash the next build with no
                                  * power-on handshake. 0 = off (the release setting). Max 179 (TIMER4 wraps). */
#endif
    while (1) {
        g_main_iters++;                            /* real main-loop passes: the overlay's "ui/s" counts these */
        if (g_main_iters == 1u) *(volatile uint32_t *)(uintptr_t)PA_RUNFLAG_ADDR = PA_RUNFLAG_MAGIC;
        {   /* the audio interrupt must keep running: if it has not entered for 3 s the unit is frozen in a way the interrupt cannot report */
            static uint32_t s_chk_t, s_chk_n, s_chk_stale;
            const uint32_t tnow = plat_ticks24();
            if ((uint32_t)(tnow - s_chk_t) >= 12000000u) {         /* every 0.5 s */
                s_chk_t = tnow;
                const uint32_t n = pa_alnk_isr_entries();
                if (n != 0u && n == s_chk_n) {
                    if (++s_chk_stale >= 6u) {
                        pa_evlog(PA_EV_FREEZE, 2u, g_main_iters);
                        *(volatile uint32_t *)(uintptr_t)0x01C7C3F0u = 0x52455343u;
                        rc_recover_uboot();
                    }
                } else s_chk_stale = 0u;
                s_chk_n = n;
            }
        }
#if PA_AUTO_UBOOT_SECS
        if ((uint32_t)(plat_ticks24() - g_boot_start_ticks) >= (uint32_t)PA_AUTO_UBOOT_SECS * 24000000u) {
            *(volatile uint32_t *)(uintptr_t)0x01C7C3F0u = 0x52455343u;   /* the one-shot mark: the boot after the readout carries on */
            rc_recover_uboot();                                           /* never returns */
        }
#endif
        PA_CP(0x10);
        pa_gpio_scan_poll_1khz();
        pa_usb_dev_poll();                         /* the USB device: enumeration, MIDI in/out (polled) */
        PA_CP(0x11);

        /* The OTA handshake's "enter upgrade" frame asks for the JieLi downloader.
         * This is the app's half of the recovery route: the device reboots into the
         * USB DFU mode and the host flashes the stock .fwsc (or a new build). */
        if (g_usb_ble.ota_reboot_pending && g_usb_ble.ota_reboot_pending != PA_OTA_REBOOT_MAGIC) {
            /* Not a real "enter upgrade" frame: corrupted memory. Log it, never reboot on it. */
#if PA_DEBUG_OVERLAY
            bb_ota_spurious(&g_usb_ble, g_usb_ble.ota_reboot_pending);
#endif
            g_usb_ble.ota_reboot_pending = 0u;
        }
        if (g_usb_ble.ota_reboot_pending == PA_OTA_REBOOT_MAGIC) {
            g_usb_ble.ota_reboot_pending = 0u;
            BB_EVENT(BB_EV_OTA_REBOOT, 0u);
            /* Flush any pending settings to flash before rebooting into DFU */
            extern void pa_audio_core_flush_settings(pa_ui_ctx_t *ui);
            pa_audio_core_flush_settings(&g_ui);
            (void)plat_reset_to_downloader();
        }

        if (g_usb_ble.ota_session_pending) {            /* the 0x7F frame: the vendor's MIDI update (step 1). Returns only if nothing was committed (refused, failed, cancelled) */
            g_usb_ble.ota_session_pending = 0u;
            BB_EVENT(BB_EV_OTA_REBOOT, 2u);
            extern int pa_ota_session_run(void);
            (void)pa_ota_session_run();
            g_ui.dirty = 1u;
        }

        PA_CP(0x12);
        uint32_t now = plat_ms();
        if ((uint32_t)(now - last_ui_tick) >= 16u) { /* ~60 Hz UI update */
            {   /* SMOOTHNESS (SRAM 0x01C7E740, read by jl_ram_read.py): how late the 60 Hz tick runs. [6..10] ticks by gap: <=17, <=20, <=33, <=50, over 50 ms; [11] ticks; [12] longest gap */
                volatile uint32_t *U = (volatile uint32_t *)(uintptr_t)0x01C7E740u;
                static uint8_t s_ui_hist_init;
                if (!s_ui_hist_init) { s_ui_hist_init = 1u; for (uint32_t i = 0; i < 16u; i++) U[i] = 0u; U[0] = 0x53485855u; }
                const uint32_t gap = (uint32_t)(now - last_ui_tick);
                U[6u + (gap <= 17u ? 0u : gap <= 20u ? 1u : gap <= 33u ? 2u : gap <= 50u ? 3u : 4u)]++;
                U[11]++; if (gap > U[12]) U[12] = gap;
            }
            last_ui_tick = now;
            {   /* Light the panel only once the first complete frame is on it, then ramp up (about 150 ms).
                 * s_ls: 0 waiting for a settled UI, 1..5 ramping (duty 2,4,6,8,10 of 10), 6 done. */
                static uint8_t s_ls, s_clean;
                static uint32_t s_ls_t, s_t0;
                extern void plat_backlight_on(void);
                extern void plat_backlight_duty10(uint8_t d);
                if (!s_t0) s_t0 = now | 1u;
                if (s_ls == 0u) {
                    if (!g_ui.dirty) s_clean++; else s_clean = 0u;
                    if (s_clean >= 4u || (uint32_t)(now - s_t0) > 1500u) { s_ls = 1u; s_ls_t = now; plat_backlight_duty10(2u); }
                } else if (s_ls < 6u && (uint32_t)(now - s_ls_t) >= 30u) {
                    s_ls_t = now;
                    if (++s_ls >= 6u) { plat_backlight_on(); BT_STAMP(18); }          /* steady full brightness */
                    else plat_backlight_duty10((uint8_t)(2u * s_ls));
                }
            }
            {   /* once, about 3 s in: the runtime memory probe (the audio is in bypass by then) */
                static uint8_t s_rt_done, s_maxreset_done;
                if (!s_maxreset_done && (uint32_t)(now - s_t0_probe) > 8000u) {
                    /* The longest times in every readout used to include the first audio block (the amp's start-up work, the self-tests): a
                     * 2 ms "spike" that is really one block at boot. At 8 s the maxima so far are kept at SRAM 0x01C7EC00 ("BOOT", 8 slot
                     * maxima, core 1's longest job, core 0's longest wait, the interrupt's longest, then the chain-call maxima, all in TIMER4
                     * ticks) and every maximum starts again, so the readout shows the steady state. */
                    extern void pa_c1_job_max_reset(void);
                    extern uint32_t pa_c1_job_ticks_max(void);
                    volatile uint32_t *K = (volatile uint32_t *)(uintptr_t)0x01C7EC00u;
                    volatile uint32_t *SL = (volatile uint32_t *)(uintptr_t)0x01C7C500u;
                    volatile uint32_t *SM = (volatile uint32_t *)(uintptr_t)0x01C7D200u;
                    volatile uint32_t *TM = (volatile uint32_t *)(uintptr_t)0x01C7D4E0u;
                    volatile uint32_t *ST = (volatile uint32_t *)(uintptr_t)0x01C7C250u;
                    s_maxreset_done = 1u;
                    for (uint32_t i = 0; i < 8u; i++) { K[1 + i] = SL[4u * i]; SL[4u * i] = 0u; }
                    K[9] = pa_c1_job_ticks_max(); pa_c1_job_max_reset();
                    K[10] = SM[5]; SM[5] = 0u;
                    K[11] = ST[2]; ST[2] = 0u;                                   /* the interrupt's longest (isr_max_cyc) */
                    for (uint32_t i = 0; i < 10u; i++) { K[12 + i] = TM[2u * i + 1u]; TM[2u * i + 1u] = 0u; }
                    K[0] = 0x544F4F42u;                                          /* "BOOT" */
                }
                /* (the core contention test of pack 75 is retired: it wrote the low SRAM, where the WaveNet keeps its matrices, and made a capture loaded in its 14-28 s window fall back to the stock amp) */
                if (!s_rt_done && (uint32_t)(now - s_t0_probe) > 3000u) {
                    s_rt_done = 1u; pa_mem_probe_runtime();
                    if (!(g_boot_buttons & 0x08u)) {        /* READ-ONLY: the system PLL multiplier, as the FM-1 firmwares (Melodee, fm1-x0x) read it; never written. "<" at power-on skips it */
                        volatile uint32_t *P = (volatile uint32_t *)(uintptr_t)0x01C7DB00u;
                        const uint32_t raw = *(volatile uint32_t *)(uintptr_t)0x000119ACu;        /* PLL2_CON1: FBDS = multiplier - 2 */
                        const uint32_t mult = (raw & 0xFFFu) + 2u;
                        P[1] = raw; P[2] = mult; P[3] = mult * 4u / 3u;                          /* SYS clock = 2/3 of the VCO (24 MHz / 12 * mult) */
                        P[0] = 0x324C4C50u;                                                        /* "PLL2" */
                        diag_clock_dump();
                        pa_flash_probe_run();                    /* PA_FLASH_PROBE (default OFF): read-only survey for the flash controller, flash_probe.c */
                    }
                }
            }
            PA_CP(0x20);
            plat_battery_poll();
            plat_pot_poll();
            PA_CP(0x21);
            plat_wdt_feed();              /* main loop healthy: feed once per UI tick */
            PA_CP(0x22);

            uint32_t btns = plat_buttons_for_ui();
            int32_t  enc  = plat_enc_delta();
            PA_CP(0x23);
#ifndef PA_COMBO_UBOOT
#define PA_COMBO_UBOOT 1          /* BENCH: hold HOME + PARA click for 2 s = reset into the mask-ROM download mode, SRAM kept */
#endif
#if PA_COMBO_UBOOT
            {
                static uint32_t combo_t0;                           /* 0 = not held */
                if ((btns & 0x21u) == 0x21u) {                      /* logical bit 0 = HOME, bit 5 = PARA click */
                    const uint32_t tn = plat_ticks24();
                    if (combo_t0 == 0u) combo_t0 = tn | 1u;
                    else if ((uint32_t)(tn - combo_t0) >= 2u * 24000000u) {
                        *(volatile uint32_t *)(uintptr_t)0x01C7C3F0u = 0x52455343u;   /* one-shot mark, as the auto-return */
                        rc_recover_uboot();                                           /* never returns */
                    }
                } else {
                    combo_t0 = 0u;
                }
            }
#endif
            {   /* event log: what the unit was asked, and what it did about it */
                static uint32_t ev_btn, ev_view = 0xFFu, ev_preset = 0xFFu, ev_smp;
                static uint8_t  ev_model[PA_SLOT_COUNT];
                if (btns != ev_btn) { pa_evlog(PA_EV_BUTTONS, btns, ev_btn); ev_btn = btns; }
                if (enc)            pa_evlog(PA_EV_ENCODER, (uint32_t)enc, 0u);
                const uint32_t smp_reason = ((volatile uint32_t *)(uintptr_t)0x01C7D200u)[6];
                if (smp_reason != ev_smp) { pa_evlog(PA_EV_SMP, smp_reason, ev_smp); ev_smp = smp_reason; }
                (void)ev_model; (void)ev_view; (void)ev_preset;
            }
            const uint32_t ev_t0 = plat_ticks24();
            {   /* the Bluetooth icon shows the radio's real state; the OFF setting (not saved yet: it is applied from the first second of the run) is acted on once */
                static uint8_t s_bt_boot_applied;
                if (!s_bt_boot_applied && plat_ms() > 1000u) { s_bt_boot_applied = 1u; if (g_ui.bt_mode == 0u) pa_ble_radio_set_enabled(0); }
                g_ui.usb_connected = (uint8_t)pa_usb_dev_connected();
                const int lk = pa_ble_radio_link();
                if (lk >= 0) g_ui.bt_state = (uint8_t)lk;
            }
#if defined(PA_HAS_MASTER_POT) && PA_HAS_MASTER_POT
            uint16_t pots[4] = { plat_pot_q12(0), plat_pot_q12(1), plat_pot_q12(2), plat_pot_q12(3) };
            pa_firmware_tick(btns, enc, pots);
#else
            pa_firmware_tick(btns, enc, 0);      /* Blk-01: no pot on this hardware */
#endif
            {
                const uint32_t dt_us = (plat_ticks24() - ev_t0) / 24u;
                {   /* one UI tick normally takes ~100 ms here (see the header word 3 of the event log: the longest), so only log the really slow ones */
                    static uint32_t s_max_us;
                    if (dt_us > s_max_us) { s_max_us = dt_us; ((volatile uint32_t *)(uintptr_t)0x01C7D60Cu)[0] = dt_us; }
                    if (dt_us > 250000u) pa_evlog(PA_EV_SLOW_TICK, dt_us, *(volatile uint32_t *)(uintptr_t)0x01C7C3F4u);
                }
                static uint32_t v_view = 0xFFu, v_preset = 0xFFu;
                static uint8_t  v_model[PA_SLOT_COUNT];
                if ((uint32_t)g_ui.view != v_view)            { pa_evlog(PA_EV_VIEW, (uint32_t)g_ui.view, v_view); v_view = (uint32_t)g_ui.view; }
                if ((uint32_t)g_ui.preset_index != v_preset)  { pa_evlog(PA_EV_PRESET, (uint32_t)g_ui.preset_index, v_preset); v_preset = (uint32_t)g_ui.preset_index; }
                for (uint32_t k = 0; k < PA_SLOT_COUNT; k++)
                    if (g_ui.slot_type[k] != v_model[k]) { v_model[k] = g_ui.slot_type[k]; pa_evlog(PA_EV_MODEL, k, v_model[k]); }
            }
            PA_CP(0x25);
            pa_test_state_update();                /* the PC test runner reads this block over MIDI */
        }
#if PA_DEBUG_OVERLAY
        if ((uint32_t)(now - last_dbg) >= 100u) {   /* after the UI tick, so it stays on top */
            last_dbg = now;
            PA_CP(0x30);
            bb_sample();                              /* every 100 ms: keeps the button log fine-grained */
            PA_CP(0x31);
            if ((BB->loop_count % 5u) == 0u) debug_overlay_draw();   /* redraw twice a second: less flicker, less LCD time */
            PA_CP(0x32);
            if ((BB->loop_count % 50u) == 0u) bb_event(BB_EV_ALIVE, BB->last_uptime_ms / 1000u);
        }
#endif
        PA_CP(0x3E);
        pa_ble_radio_poll();       /* the Bluetooth radio's debug log (no-op unless it was started at power-on) */
        { const int br = pa_bond_poll(plat_ms()); if (br) pa_evlog(PA_EV_BLE, PA_EV_BLE_BOND_SAVED, br > 0 ? 1u : 0xFFu); }     /* a bond the radio stashed is written to the flash (never from the interrupt) */
        PA_CP(0x40);
        pa_ui_render_pass();        /* a frame in progress goes on every pass, not only every 16 ms tick (a band took longer than the tick budget, so a frame took 20 ticks) */
        plat_delay_us(500);
    }
#endif
}

