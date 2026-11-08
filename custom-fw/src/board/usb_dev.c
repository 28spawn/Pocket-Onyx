/* SPDX-License-Identifier: GPL-3.0-only */
/* Copyright (C) 2026 The Onyx Authors (@28spawn) */
/* custom-fw/src/board/usb_dev.c - the Pocket Amp's USB device driver (full speed, class-compliant MIDI).
 *
 * Written from register-level facts proven on this chip family by the open FM-1 firmware (the FM-1 is
 * the same AC791N): the controller block at 0x11800, endpoint DMA address/count registers up to
 * 0x1183C, pad control at 0x51000 and the USB clock select in 0x10010. The SIE is a standard USB
 * device core reached through an indirect port: CON1 = (reg << 8) | value to write, or
 * (reg << 8) | 0x4000 to read, then wait for CON1 bit 15. Everything is polled, there is no USB
 * interrupt. The code below is our own; only the facts (addresses, bit meanings, the order of the
 * attach steps and the CSR0 values) come from that reference.
 *
 * Device: one configuration with two functions (include/pa_usb_desc.h): USB Audio 1.0 (interfaces 0..2: control, playback, recording; the recording endpoint EP3 IN sends
 * the amp output since pack 161 (silence in packs 157-160); the playback endpoint is declared and its packets are ignored) and USB MIDI (interfaces 3..4: control and MIDI streaming), bulk EP1 OUT/IN of 64 bytes.
 *
 * Debug words (SRAM, readable from the ROM download mode with the dongle's memr, or by the PC tool):
 *   0x01C7C3C0 flags: bit0 up, bit1 configured, bit2 suspended      0x01C7C3C4 bus resets
 *   0x01C7C3C8 SETUP packets                                         0x01C7C3CC OUT packets received
 *   0x01C7C3D0 IN packets sent                                       0x01C7C3D4 SIE timeouts (consecutive)
 *   0x01C7C3D8 / 3DC the last SETUP packet (8 bytes)
 *   0x01C7C420..43C recording endpoint counters (packets, frames, underruns, waits, alternate settings, TXCSR1)
 */
#include "pa_usb_dev.h"
#include "pa_usb_desc.h"

#if !defined(PA_HOST_BUILD) && !defined(PA_HOST_SIM)

#include "pa_plat.h"

#define U_CON0        (*(volatile uint32_t *)0x00011800u)
#define U_CON1        (*(volatile uint32_t *)0x00011804u)
#define U_EP_CNT(n)   (*(volatile uint32_t *)(0x00011808u + 4u * (n)))          /* EP0..3 */
#define U_EP0_ADR     (*(volatile uint32_t *)0x00011818u)
#define U_EP_TADR(n)  (*(volatile uint32_t *)(0x0001181Cu + 8u * ((n) - 1u)))   /* EP1..3 */
#define U_EP_RADR(n)  (*(volatile uint32_t *)(0x00011820u + 8u * ((n) - 1u)))   /* EP1..3 */
#define U_EP4_RADR    (*(volatile uint32_t *)0x0001183Cu)
#define U_IO_CON0     (*(volatile uint32_t *)0x00051000u)
#define U_CLK_CON1    (*(volatile uint32_t *)0x00010010u)

/* SIE register numbers (the standard USB device core's byte map) */
enum {
    S_FADDR = 0, S_POWER = 1, S_INTRTX1 = 2, S_INTRRX1 = 4, S_INTRUSB = 6,
    S_INTRTX1E = 7, S_INTRTX2E = 8, S_INTRRX1E = 9, S_INTRRX2E = 10, S_INTRUSBE = 11, S_INDEX = 14,
    S_TXMAXP = 16, S_CSR0 = 17, S_TXCSR1 = 17, S_TXCSR2 = 18, S_RXMAXP = 19, S_RXCSR1 = 20, S_RXCSR2 = 21,
    S_COUNT0 = 22, S_RXCOUNT1 = 22, S_RXCOUNT2 = 23, S_FRAME1 = 12, S_FRAME2 = 13
};

#define DBG ((volatile uint32_t *)0x01C7C3C0u)
#define PA_USB_HEARTBEAT 1   /* BENCH: release builds set 0 */

static uint8_t ep0buf[64] __attribute__((aligned(4)));
static uint8_t ep1tx[64]  __attribute__((aligned(4)));
static uint8_t ep1rx[68]  __attribute__((aligned(4)));
static uint8_t ep3tx[PA_USB_AUDIO_MAXP] __attribute__((aligned(4)));
static uint8_t ep3rx[PA_USB_AUDIO_MAXP + 4u] __attribute__((aligned(4)));   /* the playback packet (host -> unit): the controller writes it here */        /* the recording packet (silence for now) */

static pa_usb_ble_ctx_t *g_ctx;

static struct {
    uint8_t  frame_seen; uint16_t frame_last; uint32_t frame_ms;
    uint8_t  up, config, suspended, rx_pend, has_pend_addr, pend_addr, e0_tx, e0_zlp, tx_busy, e0_rx, e0_ep;
    const uint8_t *e0_src;
    uint16_t e0_left;
    volatile uint8_t busy;                              /* pa_usb_dev_poll() is running: the timer interrupt must not touch the SIE */
    volatile uint8_t svc_mode;                          /* who serves the recording endpoint: 0 the main loop's poll (default), 1 also a 2 kHz timer interrupt (pack 159 bench) */
    uint8_t  alt_play, alt_rec, iso_queued;            /* alternate setting of audio streaming interface 1 (playback) and 2 (recording); a recording packet was queued */
    uint32_t iso_pkts, iso_under, iso_wait, iso_frames, iso_last_frame, last_poll_t;
    uint32_t timeouts, resets, setups, rx_pkts, tx_pkts, last_retry_ms, last_hb_ms, hb_n, variant;
} u;

/* ---- the descriptors ----------------------------------------------------------------------------- */
/* The device and configuration descriptors are in include/pa_usb_desc.h (shared with the host test that checks them): an audio function (interfaces 0..2) and the MIDI function (3..4). */
static const uint8_t STR0[4] = { 4, 3, 0x09, 0x04 };
static const uint8_t STR1[] = { 10, 3, 'O',0,'n',0,'y',0,'x',0 };
static const uint8_t STR2[] = { 26, 3, 'C',0,'u',0,'s',0,'t',0,'o',0,'m',0,' ',0,'M',0,'I',0,'D',0,'I',0,' ',0 };

/* ---- the SIE indirect port ----------------------------------------------------------------------- */
/* sie_wr / sie_rd stay real calls: inlined at -O2 -mcpu=r3 the SIE accesses ran a few cycles apart and EP1 IN stopped after a packet or two on the same chip family
 * (felucca-bsp issue 14, fixed there with noinline). Pack 150A: this is the only change against pack 149A. */
__attribute__((noinline)) static void sie_wr(uint32_t r, uint32_t v) {
    uint32_t n = 20000u;
    if (!(U_CON0 & 4u)) return;                          /* the SIE answers only while USB_NRST (bit 2) is set */
    U_CON1 = ((r << 8) & 0xFFFF00u) | (v & 0xFFu);
    __asm__ volatile("csync" ::: "memory");
    while (!(U_CON1 & 0x8000u) && --n) { }
    if (n) u.timeouts = 0;
    else if (++u.timeouts > 50u) u.up = 0;               /* a dead SIE (no cable, no clock): stop polling it */
}

__attribute__((noinline)) static uint32_t sie_rd(uint32_t r) {
    uint32_t n = 20000u;
    if (!(U_CON0 & 4u)) return 0;
    U_CON1 = ((r << 8) & 0xFFBF00u) | 0x4000u;
    __asm__ volatile("csync" ::: "memory");
    while (!(U_CON1 & 0x8000u)) {
        if (!--n) { if (++u.timeouts > 50u) u.up = 0; return 0; }
    }
    u.timeouts = 0;
    return U_CON1 & 0xFFu;
}

volatile uint32_t g_irq_off_max_ticks = 0;
volatile uint32_t g_irq_off_count = 0;
volatile uint32_t s_irq_off_t0 = 0;
static volatile uint32_t s_sie_lock_t0 = 0;
volatile uint32_t g_sie_lock_max_ticks = 0;
volatile uint32_t g_ep3_rx_pkts = 0;
volatile uint32_t g_ep3_rx_last_bytes = 0;
volatile uint32_t g_ep3_rx_last_tick = 0;
volatile uint32_t g_usb_last_sof = 0;
extern void *pa_usb_audio_get_starve_log(void);
extern uint32_t pa_usb_audio_get_starve_count(void);
static inline void usb_sie_lock(void) {
    s_sie_lock_t0 = plat_ticks24();
    if (u.svc_mode) {
        *(volatile uint32_t *)(uintptr_t)0x01EEF11Cu &= ~0x10000000u;
        __asm__ volatile("csync" ::: "memory");
    }
    u.busy = 1u;
}

static inline void usb_sie_unlock(void) {
    u.busy = 0u;
    if (u.svc_mode) {
        *(volatile uint32_t *)(uintptr_t)0x01EEF11Cu |= 0x10000000u;
        __asm__ volatile("csync" ::: "memory");
    }
    uint32_t dt = plat_ticks24() - s_sie_lock_t0;
    if (dt > g_sie_lock_max_ticks) g_sie_lock_max_ticks = dt;
}

/* ---- bring-up ------------------------------------------------------------------------------------ */
static void usb_start(void) {
    u.timeouts = 0;
    U_CON0 = 0; U_CON1 = 0; U_IO_CON0 = 0x0Cu;           /* reset whatever the ROM left; pads detached */
    plat_delay_ms(25u);
    U_CLK_CON1 &= ~3u;                                   /* USB clock = PLL48M */
    U_EP0_ADR = (uint32_t)(uintptr_t)ep0buf;
    for (uint32_t n = 1; n <= 3u; n++) U_EP_RADR(n) = (uint32_t)(uintptr_t)ep0buf;
    U_EP4_RADR = (uint32_t)(uintptr_t)ep0buf;
    U_IO_CON0 |= (1u << 10) | (1u << 9);
    U_IO_CON0 &= ~(1u << 11);
    U_IO_CON0 |= 1u << 12;
    U_CON0 &= ~0x4035u;
    U_CON1 = 0;
    U_CON0 |= 1u;                                        /* PHY on */
    U_IO_CON0 &= ~0xF0u;
    U_IO_CON0 |= 0x40u;                                  /* D+ pull-up: attach */
    U_CON0 |= 0x3Cu;
    sie_wr(S_POWER, 0x60);
    sie_wr(S_INTRUSBE, 0x07);                            /* suspend, resume, reset */
    sie_wr(S_INTRTX1E, 0x01);                            /* EP0 */
    sie_wr(S_INTRTX2E, 0);
    sie_wr(S_INTRRX1E, 0);
    sie_wr(S_INTRRX2E, 0);
    U_CON0 &= ~(0x1Fu << 19);                            /* enable endpoints 0..4 */
    u.up = u.timeouts < 3u;
}

static void ep1_config(void) {
    U_EP_TADR(1) = (uint32_t)(uintptr_t)ep1tx;
    U_EP_RADR(1) = (uint32_t)(uintptr_t)ep1rx;
    sie_wr(S_INDEX, 1);
    sie_wr(S_TXMAXP, 0xFF);
    sie_wr(S_TXCSR1, 0x48);                              /* clear the data toggle, flush the FIFO */
    sie_wr(S_TXCSR2, 0);
    sie_wr(S_RXMAXP, 0xFF);
    sie_wr(S_RXCSR1, 0x90);
    sie_wr(S_RXCSR2, 0);
    sie_wr(S_INTRRX1E, 0x02);                            /* EP1 OUT */
    U_CON0 &= ~(1u << (19 + 1));
}

/* ---- the recording endpoint (EP3 IN, isochronous) ------------------------------------------------ */
/* Pack 157 (USB-AUDIO-PLAN.md step B): the alternate settings are accepted and the endpoint answers every IN token with a packet of silence of the right length
 * (44 or 45 frames of 2 x 3 bytes). The set-up follows the other FM-1 firmware that records over USB on this controller (hortator/usb_app.c uac_ep4_reset, uac_service):
 * index the endpoint, clear the toggle and flush, then TXCSR2 bit 6 = isochronous; one packet at a time, the next one only after TxPktRdy has cleared.
 *
 * Pack 159 (USB-AUDIO-PLAN.md step B, the 1 ms service experiment): pack 157 served the endpoint only from the main loop and delivered ~743 packets a second (the check read 73 %).
 * The poll's gaps and the host frames that went by without a packet are now counted, and the endpoint can ALSO be served by a 2 kHz TIMER5 interrupt (svc_mode 1, the default since pack 161; MIDI CC 91 = off,
 * CC 90 = on, for diagnosis). The SIE has one shared index/data port, so the interrupt only touches it while the main loop is not inside
 * pa_usb_dev_poll() (u.busy); the main loop's poll still serves the endpoint in both modes.
 *
 * Counters, SRAM 0x01C73F00 (the 288 free bytes between the effect pool and the stack guard), 64 words:
 *   0 magic "ISOT"   1 mode   2 packets queued   3 frames sent   4 underruns (a token found nothing)   5 polls that found the last packet still waiting   6 alt play | alt rec << 8   7 TXCSR1 at the last poll
 *   8..11 host frames between two queued packets: 1, 2, 3, 4 or more     12 host frames that passed with no packet at all (sum of gap - 1)
 *   16..23 main-loop poll gaps: < 0.5, < 1, < 1.5, < 2, < 3, < 5, < 10, >= 10 ms     24 longest poll gap (24 MHz ticks)
 *   32 timer interrupts taken    33 of them that served the endpoint    34 skipped because the main loop was in the poll    35 longest interrupt (ticks)    36 sum of interrupt ticks
 *   40 packets queued by the interrupt    41 by the main loop
 *   44 playback (host -> unit) packets taken    45 playback packets of a bad size    46..50 playback ring: packets seen by the mixer, starved, re-centred, lowest fill, highest fill
 *   51 recording packets sent as faded silence (ring starved / priming)    52 recording ring starved or re-centred    53 / 54 recording ring lowest / highest fill
 *   55 service tick (Hz)    56 / 57 host frame number, frame counter seen to run    58..63 time between two timer interrupts: < 350, < 500, < 750, < 1000, < 1500, >= 1500 us */
extern uint32_t g_pa_cap_recentres, g_pa_cap_fill_min, g_pa_cap_fill_max, g_pa_cap_starved;
extern volatile uint32_t g_pa_alnk_t0, g_pa_alnk_in;
extern uint32_t g_pa_pb_packets, g_pa_pb_starved, g_pa_pb_recentres, g_pa_pb_fill_min, g_pa_pb_fill_max;
#define ISOT ((volatile uint32_t *)0x01C73F00u)

static void iso_reset(void) {
    U_EP_TADR(3) = (uint32_t)(uintptr_t)ep3tx;
    sie_wr(S_INDEX, 3);
    sie_wr(S_TXMAXP, 0xFF);
    sie_wr(S_TXCSR1, 0x48);                              /* clear the data toggle, flush the FIFO */
    sie_wr(S_TXCSR2, 0);
    sie_wr(S_TXCSR1, 0);
    sie_wr(S_TXCSR2, 0x40);                              /* isochronous */
    u.iso_queued = 0;
    u.iso_last_frame = 0xFFFFFFFFu;
}

/* The playback endpoint (EP3 OUT, isochronous, 1 packet a ms): set up the way the other endpoints are (index it, clear the toggle and flush, then RXCSR2 bit 6 = isochronous) and read by
 * polling RxPktRdy from the same interrupt that serves the recording endpoint; the controller has already written the packet to ep3rx by then (as for EP1). Packets that arrive
 * while the interrupt cannot run are lost: that is why the service tick is 4 kHz. */
static void rx_reset(void) {
    U_EP_RADR(3) = (uint32_t)(uintptr_t)ep3rx;
    sie_wr(S_INDEX, 3);
    sie_wr(S_RXMAXP, 0xFF);
    sie_wr(S_RXCSR1, 0x90);                              /* clear the data toggle, flush the FIFO */
    sie_wr(S_RXCSR2, 0);
    sie_wr(S_RXCSR1, 0);
    sie_wr(S_RXCSR2, 0x40);                              /* isochronous */
}

static void rx_service(void) {
    uint32_t csr, n;
    if (!u.alt_play) return;
    sie_wr(S_INDEX, 3);
    csr = sie_rd(S_RXCSR1);
    if (!(csr & 1u)) return;                             /* no packet */
    n = sie_rd(S_RXCOUNT1) | (sie_rd(S_RXCOUNT2) << 8);
    __asm__ volatile("ssync" ::: "memory");
    if (n >= 6u && n <= PA_USB_AUDIO_MAXP && g_ctx) {
        pa_usb_audio_ep3_out_rx(g_ctx, ep3rx, n);
        ISOT[44] = ISOT[44] + 1u;
        g_ep3_rx_pkts++;
        g_ep3_rx_last_bytes = n;
        g_ep3_rx_last_tick = plat_ticks24();
    }
    else ISOT[45] = ISOT[45] + 1u;
    csr |= (uint32_t)sie_rd(S_RXCSR2) << 8;
    csr = (csr & ~0x164u) | 0x10u;                       /* release the packet, as ep1_rx does */
    sie_wr(S_RXCSR1, csr & 0xFFu);
    sie_wr(S_RXCSR2, csr >> 8);
    ISOT[25] = (uint32_t)(uintptr_t)pa_usb_audio_get_starve_log();
    ISOT[26] = pa_usb_audio_get_starve_count();
    ISOT[27] = g_sie_lock_max_ticks / 24u;
    ISOT[28] = g_irq_off_max_ticks / 24u;
    ISOT[29] = g_irq_off_count;
    ISOT[30] = g_ep3_rx_pkts;
    ISOT[31] = g_ep3_rx_last_bytes;
    ISOT[46] = g_pa_pb_packets; ISOT[47] = g_pa_pb_starved; ISOT[48] = g_pa_pb_recentres;
    ISOT[49] = g_pa_pb_fill_min; ISOT[50] = g_pa_pb_fill_max;
}

static void iso_alt(uint32_t itf, uint32_t alt) {
    if (itf == 2u) {
        iso_reset(); u.alt_rec = (uint8_t)alt;
        if (alt && g_ctx) pa_usb_audio_capture_start(g_ctx);               /* start from a clean ring, silent until it holds the target fill */
    } else {
        u.alt_play = (uint8_t)alt;
        if (alt) { rx_reset(); if (g_ctx) pa_usb_audio_playback_start(g_ctx); }
        else if (g_ctx) pa_usb_audio_playback_stop(g_ctx);
    }
    ISOT[6] = (uint32_t)u.alt_play | (uint32_t)u.alt_rec << 8;
}

/* returns 1 when it queued a packet; from_isr only labels the counters */
static uint32_t iso_service(uint32_t from_isr) {
    uint32_t csr, frames, bytes, fr;
    if (!u.alt_rec) return 0u;
    sie_wr(S_INDEX, 3);
    csr = sie_rd(S_TXCSR1);
    ISOT[7] = csr;
    if (csr & 0x01u) { u.iso_wait++; ISOT[5] = u.iso_wait; return 0u; }    /* the previous packet has not been taken yet */
    if (u.iso_queued && (csr & 0x04u)) { u.iso_under++; ISOT[4] = u.iso_under; }   /* UnderRun: a token came and found nothing */
    u.iso_queued = 1;
    /* The packet is the amp's output (pack 161): the capture ring the audio path fills every block (pa_audio_core.c, after the Groove Station mix and the sum limiter) is read
     * through the fill-steered resampler of usb_ble.c (IMP-08, tested on the host by test_usb_stereo.c): 44 or 45 frames of 2 x 24 bit, the codec's 44,118 Hz against the host's
     * 44,100 Hz absorbed by the read step. Before pack 161 this wrote silence. */
    fr = sie_rd(S_FRAME1) | (sie_rd(S_FRAME2) & 7u) << 8;     /* the host frame this packet is queued in (also the instrumentation) */
    if (u.iso_last_frame != 0xFFFFFFFFu && g_ctx) {         /* host frames that went by with nothing taken: the audio of those ms is gone from the host's timeline, so skip it here too (else the ring's fill ratchets up by a packet per miss) */
        const uint32_t gap = (fr - u.iso_last_frame) & 0x7FFu;
        if (gap > 1u) { pa_usb_audio_ep3_missed(g_ctx, gap - 1u); ISOT[13] = g_pa_cap_dropped; }
    }
    bytes = g_ctx ? pa_usb_audio_ep3_in_fill(g_ctx, ep3tx, PA_USB_AUDIO_MAXP) : 0u;
    frames = bytes / 6u;
    __asm__ volatile("csync" ::: "memory");
    U_EP_TADR(3) = (uint32_t)(uintptr_t)ep3tx;
    U_EP_CNT(3) = bytes;
    sie_wr(S_TXCSR1, 0x01);                              /* TxPktRdy (the UnderRun flag is cleared by this write) */
    if (u.iso_last_frame != 0xFFFFFFFFu) {
        const uint32_t gap = (fr - u.iso_last_frame) & 0x7FFu;
        if (gap >= 1u) { const uint32_t k = gap > 4u ? 4u : gap; ISOT[7u + k] = ISOT[7u + k] + 1u; }
        if (gap > 1u) ISOT[12] = ISOT[12] + (gap - 1u);
    }
    u.iso_last_frame = fr;
    u.iso_pkts++; u.iso_frames += frames;
    ISOT[2] = u.iso_pkts; ISOT[3] = u.iso_frames;
    if (from_isr) ISOT[40] = ISOT[40] + 1u; else ISOT[41] = ISOT[41] + 1u;
    ISOT[51] = g_pa_cap_starved; ISOT[52] = g_pa_cap_recentres; ISOT[53] = g_pa_cap_fill_min; ISOT[54] = g_pa_cap_fill_max;
    return 1u;
}

/* The 2 kHz timer interrupt (TIMER5, IRQ 63; registers and the 6 MHz source as on the other FM-1 firmware, which runs its whole USB poll from it). Mode 0 returns at once. */
#define T5_CON (*(volatile uint32_t *)0x00010900u)
#define T5_CNT (*(volatile uint32_t *)0x00010904u)
#define T5_PRD (*(volatile uint32_t *)0x00010908u)
void pa_timer5_isr_c(void) {
    const uint32_t t0 = plat_ticks24();
    T5_CON |= 0x4000u;                                   /* acknowledge */
    {   static uint32_t s_prev_t0;                       /* instrumentation: the time between two timer interrupts (nominal 250 us at 4 kHz): how late the interrupt can be */
        if (s_prev_t0) {
            const uint32_t us = (t0 - s_prev_t0) / 24u;
            const uint32_t b = us < 350u ? 0u : us < 500u ? 1u : us < 750u ? 2u : us < 1000u ? 3u : us < 1500u ? 4u : 5u;
            ISOT[58u + b] = ISOT[58u + b] + 1u;
            if (us >= 1200u) {                              /* what held the interrupt off: the last four gaps over 1.2 ms (word 14 counts them, 15 is the longest, 16..23 the ring) */
                const uint32_t n = ISOT[14]; ISOT[14] = n + 1u;
                if (us > ISOT[15]) ISOT[15] = us;
                ISOT[16u + (n & 3u) * 2u] = (us > 0xFFFFu ? 0xFFFFu : us) | (*(volatile uint32_t *)0x01C7C3F8u & 0xFFu) << 16;     /* gap in us | the audio interrupt's last breadcrumb << 16 (0x05 = it had finished) */
                {   /* where the gap sits against the audio interrupt: low half = us from the audio interrupt's entry to the END of the gap, bits 16..30 = to the START of the gap
                     * (it wraps below zero when the gap began before that block's interrupt); bit 31 = the audio interrupt was still running at the end */
                    const uint32_t ea = (t0 - g_pa_alnk_t0) / 24u, sa = (s_prev_t0 - g_pa_alnk_t0) / 24u;
                    ISOT[17u + (n & 3u) * 2u] = (ea & 0xFFFFu) | (sa & 0x7FFFu) << 16 | (g_pa_alnk_in ? 0x80000000u : 0u);
                }
            }
        }
        s_prev_t0 = t0; }
    if (!u.svc_mode || !u.up || !u.config) return;
    ISOT[32] = ISOT[32] + 1u;
    if (u.busy) { ISOT[34] = ISOT[34] + 1u; return; }
    (void)iso_service(1u);
    rx_service();
    {   const uint32_t dt = plat_ticks24() - t0;
        ISOT[33] = ISOT[33] + 1u; ISOT[36] = ISOT[36] + dt;
        if (dt > ISOT[35]) ISOT[35] = dt; }
}
extern void isr_timer5(void);                            /* entry.S */
static volatile uint32_t s_t5_prd = 1500u;               /* the timer period in 6 MHz ticks */
static void timer5_set_prio(uint32_t prio) {
    const uint32_t irq = 63u;
    volatile uint32_t *icfg = (volatile uint32_t *)(uintptr_t)(0x01EEF100u + (irq >> 3) * 4u);
    const uint32_t sh = (irq & 7u) * 4u;
    *icfg = (*icfg & ~(0xFu << sh)) | ((((prio << 1) & 0xEu) | 1u) << sh);
    ISOT[1] = (ISOT[1] & 0xFFu) | (prio << 8);
}
/* BENCH (MIDI CC 93 = priority 7, CC 94 = 5): is the timer interrupt held off by the audio interrupt? The histogram in words 58..63 (the time between two timer interrupts) says. */
void pa_usb_dev_set_timer_prio(uint32_t prio) { timer5_set_prio(prio > 7u ? 7u : prio); }
static void timer5_register(void) {
    const uint32_t irq = 63u, prio = 5u;                 /* above the audio interrupt (4): it must be able to run while a block renders */
    T5_CON = 0x4000u; T5_CNT = 0u; T5_PRD = s_t5_prd;    /* 6 MHz / 1500 = 4 kHz (the default since pack 164), / 3000 = 2 kHz */
    *(volatile uint32_t *)(uintptr_t)(0x01C7FE00u + irq * 4u) = (uint32_t)(uintptr_t)&isr_timer5;
    {   volatile uint32_t *icfg = (volatile uint32_t *)(uintptr_t)(0x01EEF100u + (irq >> 3) * 4u);
        const uint32_t sh = (irq & 7u) * 4u;
        *icfg = (*icfg & ~(0xFu << sh)) | ((((prio << 1) & 0xEu) | 1u) << sh); }
    T5_CON = 0x4019u;                                    /* run, 6 MHz, interrupt on */
}

/* 0: the main loop's poll only; 1: the interrupt also, 2 kHz; 2: the interrupt also, 4 kHz (the default; MIDI CC 91 / 90 / 92) */
void pa_usb_dev_set_service_mode(uint32_t mode) {
    if (mode == 1u) s_t5_prd = 3000u; else if (mode == 2u) s_t5_prd = 1500u;
    T5_PRD = s_t5_prd;
    u.svc_mode = mode ? 1u : 0u; ISOT[1] = (ISOT[1] & ~0xFFu) | mode; ISOT[55] = 6000000u / s_t5_prd;
}

/* ---- endpoint 0 ---------------------------------------------------------------------------------- */
static void e0_chunk(void) {
    uint32_t n = u.e0_left > 64u ? 64u : u.e0_left, i;
    int last;
    for (i = 0; i < n; i++) ep0buf[i] = u.e0_src[i];
    __asm__ volatile("csync" ::: "memory");              /* the buffer is written before the DMA starts */
    U_EP0_ADR = (uint32_t)(uintptr_t)ep0buf;
    U_EP_CNT(0) = n;
    u.e0_src += n;
    u.e0_left = (uint16_t)(u.e0_left - n);
    last = (u.e0_left == 0) && !(n == 64u && u.e0_zlp);
    if (u.e0_left == 0 && n == 64u) u.e0_zlp = 0;
    sie_wr(S_INDEX, 0);
    sie_wr(S_CSR0, last ? 0x0A : 0x02);                  /* TxPktRdy (+ DataEnd on the last packet) */
    if (last) u.e0_tx = 0;
}

static void e0_send(const uint8_t *p, uint16_t len, uint16_t wlen) {
    if (len > wlen) len = wlen;
    u.e0_src = p;
    u.e0_left = len;
    u.e0_zlp = (len < wlen) && (len % 64u == 0);
    u.e0_tx = 1;
    sie_wr(S_INDEX, 0);
    sie_wr(S_CSR0, 0x40);                                /* ServicedRxPktRdy: the SETUP is accepted */
    e0_chunk();
}

static int get_desc(uint32_t wvalue, const uint8_t **d, uint16_t *l) {
    switch (wvalue >> 8) {
    case 1: *d = PA_DEV_DESC; *l = sizeof PA_DEV_DESC; return 1;
    case 2: *d = PA_CFG_DESC; *l = sizeof PA_CFG_DESC; return 1;
    case 3:
        switch (wvalue & 0xFFu) {
        case 0: *d = STR0; *l = sizeof STR0; return 1;
        case 1: *d = STR1; *l = sizeof STR1; return 1;
        case 2: *d = STR2; *l = sizeof STR2; return 1;
        default: return 0;
        }
    default: return 0;
    }
}

static void ep0_service(void) {
    static const uint8_t zero2[2];
    static uint8_t s[8];
    uint32_t csr, i;
    uint16_t wvalue, wlength;

    if (u.has_pend_addr) { sie_wr(S_FADDR, u.pend_addr); u.has_pend_addr = 0; }
    sie_wr(S_INDEX, 0);
    csr = sie_rd(S_CSR0);
    if (csr & 0x04u) { sie_wr(S_CSR0, 0); u.e0_tx = 0; u.e0_rx = 0; return; }          /* SentStall */
    if (csr & 0x10u) { sie_wr(S_CSR0, 0x80); u.e0_tx = 0; u.e0_rx = 0; }               /* SetupEnd: the host gave up */
    if (u.e0_tx) { if (!(csr & 0x02u)) e0_chunk(); return; }              /* still sending: next chunk when TxPktRdy clears */
    if (!(csr & 0x01u)) return;                                           /* no SETUP waiting */
    __asm__ volatile("ssync" ::: "memory");
    if (u.e0_rx) {                                                        /* SET_CUR sampling frequency data stage (3 bytes) */
        uint32_t cnt = sie_rd(S_COUNT0);
        if (cnt >= 3u && g_ctx) {
            uint32_t hz = (uint32_t)ep0buf[0] | ((uint32_t)ep0buf[1] << 8) | ((uint32_t)ep0buf[2] << 16);
            pa_usb_audio_set_rate_ep(g_ctx, u.e0_ep, hz);
        }
        u.e0_rx = 0;
        goto ack;
    }
    for (i = 0; i < 8u; i++) s[i] = ep0buf[i];
    u.setups++;
    DBG[2] = u.setups;
    DBG[6] = (uint32_t)s[0] | (uint32_t)s[1] << 8 | (uint32_t)s[2] << 16 | (uint32_t)s[3] << 24;
    DBG[7] = (uint32_t)s[4] | (uint32_t)s[5] << 8 | (uint32_t)s[6] << 16 | (uint32_t)s[7] << 24;
    wvalue  = (uint16_t)(s[2] | s[3] << 8);
    wlength = (uint16_t)(s[6] | s[7] << 8);

    switch ((uint32_t)s[0] << 8 | s[1]) {
    case 0x0005:                                                          /* SET_ADDRESS (applied after the status stage) */
        u.pend_addr = s[2] & 0x7Fu; u.has_pend_addr = 1;
        goto ack;
    case 0x8006: {                                                        /* GET_DESCRIPTOR */
        const uint8_t *d; uint16_t l;
        if (get_desc(wvalue, &d, &l)) {
            if (!wlength) goto ack;
            e0_send(d, l, wlength);
            return;
        }
        goto stall;
    }
    case 0x0009:                                                          /* SET_CONFIGURATION: 0 or 1 */
        if (s[2] > 1u) goto stall;
        u.config = s[2];
        if (u.config == 1u) { ep1_config(); iso_reset(); } else sie_wr(S_INTRRX1E, 0);
        u.alt_play = 0; u.alt_rec = 0;
        if (g_ctx) pa_usb_audio_playback_stop(g_ctx);
        goto ack;
    case 0x8008: e0_send(&u.config, 1, wlength); return;                  /* GET_CONFIGURATION */
    case 0x8000: case 0x8100: case 0x8200: e0_send(zero2, 2, wlength); return;   /* GET_STATUS */
    case 0x010B:                                                          /* SET_INTERFACE: wIndex = the interface, wValue = the alternate setting */
        /* Alternate setting 0 of every interface is accepted, and alternate setting 1 of the audio streaming interfaces (1 playback, 2 recording) once configured. The
         * playback endpoint is not served yet (its packets are ignored); the recording endpoint sends silence (iso_service). */
        if ((s[4] == 1u || s[4] == 2u) && wvalue <= 1u && u.config) { iso_alt(s[4], wvalue); goto ack; }
        if (wvalue == 0 && s[4] <= 4u) goto ack;
        goto stall;
    case 0x810A: {                                                        /* GET_INTERFACE */
        static uint8_t alt;
        alt = s[4] == 1u ? u.alt_play : s[4] == 2u ? u.alt_rec : 0u;
        e0_send(&alt, 1, wlength);
        return;
    }
    case 0x2201:                                                         /* SET_CUR, endpoint: sampling frequency */
        if ((s[4] != 0x03u && s[4] != 0x83u) || s[3] != 1u) goto stall;   /* EP3 OUT or EP3 IN, SAMPLING_FREQ_CONTROL */
        if (wlength) {
            u.e0_rx = 1; u.e0_ep = s[4];
            sie_wr(S_INDEX, 0);
            sie_wr(S_CSR0, 0x40);                                         /* ServicedRxPktRdy: data stage follows */
            return;
        }
        goto ack;
    case 0xA281:                                                          /* GET_CUR endpoint: sampling frequency */
    case 0xA282:                                                          /* GET_MIN */
    case 0xA283: {                                                        /* GET_MAX */
        static const uint8_t RATE44[3] = { 0x44, 0xAC, 0x00 };            /* 44100 Hz */
        static const uint8_t RATE48[3] = { 0x80, 0xBB, 0x00 };            /* 48000 Hz */
        if ((s[4] != 0x03u && s[4] != 0x83u) || s[3] != 1u) goto stall;
        if (s[1] == 0x83u || (s[1] == 0x81u && pa_usb_audio_get_rate(s[4]) == 48000u)) {
            e0_send(RATE48, 3, wlength);
            return;
        }
        e0_send(RATE44, 3, wlength);
        return;
    }
    case 0x0201: {                                                        /* CLEAR_FEATURE(ENDPOINT_HALT) */
        uint32_t ep = s[4] & 0x0Fu;
        if (wvalue == 0 && ep == 3u) goto ack;                            /* the isochronous endpoints: no halt, no toggle */
        if (wvalue != 0 || ep > 1u) goto stall;
        if (ep) {
            sie_wr(S_INDEX, ep);
            if (s[4] & 0x80u) sie_wr(S_TXCSR1, 0x40); else sie_wr(S_RXCSR1, 0x80);
        }
        goto ack;
    }
    default: goto stall;
    }
ack:
    sie_wr(S_INDEX, 0);
    sie_wr(S_CSR0, 0x48);                                                 /* ServicedRxPktRdy + DataEnd: no data stage */
    return;
stall:
    sie_wr(S_INDEX, 0);
    sie_wr(S_CSR0, 0x60);                                                 /* ServicedRxPktRdy + SendStall */
}

/* ---- the MIDI endpoints -------------------------------------------------------------------------- */
#include "pa_usb_chunk.h"
#define TXQ 128u
static uint32_t txq[TXQ];
static volatile uint32_t txq_w, txq_r;

int pa_usb_dev_tx(const uint8_t *pkts, uint32_t len) {
    if (!pkts) return -1;
    for (uint32_t i = 0; i + 4u <= len; i += 4u) {
        uint32_t next = (txq_w + 1u) % TXQ;
        if (next == txq_r) return -1;                    /* full: drop the rest (a reply to nobody) */
        txq[txq_w] = (uint32_t)pkts[i] | (uint32_t)pkts[i + 1u] << 8 | (uint32_t)pkts[i + 2u] << 16 | (uint32_t)pkts[i + 3u] << 24;
        txq_w = next;
    }
    return 0;
}

static void ep1_rx(void) {
    uint32_t csr, n;
    sie_wr(S_INDEX, 1);
    csr = sie_rd(S_RXCSR1) | (sie_rd(S_RXCSR2) << 8);
    if (!(csr & 1u)) { u.rx_pend = 0; return; }                           /* no packet */
    n = sie_rd(S_RXCOUNT1) | (sie_rd(S_RXCOUNT2) << 8);
    if (n > 64u) n = 64u;
    __asm__ volatile("ssync" ::: "memory");
    if (g_ctx) pa_usb_midi_rx_packet(g_ctx, ep1rx, n);
    u.rx_pend = 0;
    u.rx_pkts++;
    DBG[3] = u.rx_pkts;
    csr = (csr & ~0x164u) | 0x10u;                                        /* release the packet */
    sie_wr(S_RXCSR1, csr & 0xFFu);
    sie_wr(S_RXCSR2, csr >> 8);
}

/* TX diagnostics (SRAM 0x01C7C400, 8 words): 0 TXCSR1 before the last send, 1 TXCSR1 right after TxPktRdy,
 * 2 sends that completed (TxPktRdy seen clear afterwards), 3 polls that found the last send still pending,
 * 4 TXCSR2, 5 TXMAXP readback, 6 EP_CNT readback, 7 TXCSR1 at the last "still pending" poll. */
#define TXD ((volatile uint32_t *)0x01C7C400u)

static void ep1_tx(void) {
    uint32_t csr, n = 0;
    if (u.tx_busy) {                                                      /* a send is outstanding: has the host taken it? */
        sie_wr(S_INDEX, 1);
        csr = sie_rd(S_TXCSR1);
        if (csr & 0x01u) { TXD[3]++; TXD[7] = csr; }
        else { u.tx_busy = 0; TXD[2]++; }
    }
    if (txq_w == txq_r) return;
    sie_wr(S_INDEX, 1);
    csr = sie_rd(S_TXCSR1);
    if (csr & 0x01u) return;                                              /* the previous packet is still pending */
    TXD[0] = csr;
    if (csr & 0x80u) sie_wr(S_TXCSR1, csr & ~0x80u);
    while (txq_r != txq_w && n < 64u) {
        uint32_t p = txq[txq_r];
        ep1tx[n] = (uint8_t)p; ep1tx[n + 1u] = (uint8_t)(p >> 8); ep1tx[n + 2u] = (uint8_t)(p >> 16); ep1tx[n + 3u] = (uint8_t)(p >> 24);
        n += 4u;
        txq_r = (txq_r + 1u) % TXQ;
    }
    if (!n) return;
#ifndef PA_USB_PAD
#define PA_USB_PAD 0   /* Optional USB MIDI short-packet padding: off by default */
#endif
#if PA_USB_PAD
    if (pa_usb_needs_pad(n, (txq_w + TXQ - txq_r) % TXQ)) {                /* a full 64-byte packet with nothing behind it: the host would keep the transfer open for ever (pa_usb_chunk.h) */
        txq[txq_w] = 0u;                                                  /* one padding event packet (cable 0, code index 0: ignored by the class) closes it with a short packet */
        txq_w = (txq_w + 1u) % TXQ;
    }
#endif
    __asm__ volatile("csync" ::: "memory");
#if PA_USB_HEARTBEAT
    sie_wr(S_TXMAXP, u.variant == 1u ? 64u : 0xFFu);                      /* BENCH variants, see the heartbeat */
    sie_wr(S_TXCSR2, u.variant == 2u ? 0x20u : 0u);
#endif
    U_EP_TADR(1) = (uint32_t)(uintptr_t)ep1tx;
    U_EP_CNT(1) = n;
    sie_wr(S_TXCSR1, sie_rd(S_TXCSR1) | 0x01u);                           /* TxPktRdy */
#if PA_USB_HEARTBEAT
    if (u.variant == 3u) U_EP_CNT(1) = n;
#endif
    u.tx_pkts++;
    u.tx_busy = 1;
    DBG[4] = u.tx_pkts;
    TXD[1] = sie_rd(S_TXCSR1);
    TXD[4] = sie_rd(S_TXCSR2);
    TXD[5] = sie_rd(S_TXMAXP);
    TXD[6] = U_EP_CNT(1);
}

/* ---- the poll ------------------------------------------------------------------------------------ */
void pa_usb_dev_init(pa_usb_ble_ctx_t *ctx) {
    g_ctx = ctx;
    for (uint32_t i = 0; i < 8u; i++) { DBG[i] = 0; TXD[i] = 0; }
    for (uint32_t i = 0; i < 64u; i++) ISOT[i] = 0u;
    ISOT[0] = 0x544F5349u;                                /* "ISOT" */
    usb_start();
    DBG[0] = u.up;
    u.svc_mode = 1u; ISOT[1] = 2u; ISOT[55] = 6000000u / s_t5_prd;                        /* pack 161: the 2 kHz interrupt serves the recording endpoint by default (pack 160 bench: 99.4 % of the wall clock against 71 % from the main loop); MIDI CC 91 = main loop only */
    timer5_register();
}

uint32_t pa_usb_dev_configured(void) { return u.config; }

/* A host is there: the configuration is set, the bus is not suspended and (once the frame counter has been seen to run) the host's start-of-frame, 1 a millisecond, has not stopped for 600 ms (the main loop, which reads the counter, can be away for 120 ms: 100 ms made the diamond flicker).
 * A pulled cable leaves the configuration set; the suspend interrupt and the frame counter are what tell. */
uint32_t pa_usb_dev_connected(void) {
    if (!u.up || !u.config || u.suspended) return 0u;
    if (u.frame_seen && (uint32_t)(plat_ms() - u.frame_ms) > 600u) return 0u;
    return 1u;
}

void pa_usb_dev_poll(void) {
    uint32_t iu, it, ir;
    {   const uint32_t tn = plat_ticks24();                /* instrumentation: the gap between two polls */
        if (u.last_poll_t) {
            const uint32_t g = tn - u.last_poll_t;
            const uint32_t us = g / 24u;
            const uint32_t b = us < 500u ? 0u : us < 1000u ? 1u : us < 1500u ? 2u : us < 2000u ? 3u : us < 3000u ? 4u : us < 5000u ? 5u : us < 10000u ? 6u : 7u;
            ISOT[16u + b] = ISOT[16u + b] + 1u;
            if (g > ISOT[24]) ISOT[24] = g;
        }
        u.last_poll_t = tn; }
    if (!u.up) {                                          /* no cable at power-on: try again once a second */
        const uint32_t now = plat_ms();
        if ((uint32_t)(now - u.last_retry_ms) >= 1000u) { u.last_retry_ms = now; usb_start(); DBG[0] = u.up; }
        return;
    }
    usb_sie_lock();
    iu = sie_rd(S_INTRUSB);
    it = sie_rd(S_INTRTX1);
    ir = sie_rd(S_INTRRX1);
    {   const uint16_t f = (uint16_t)(sie_rd(S_FRAME1) | (sie_rd(S_FRAME2) << 8));      /* the host's frame number */
        if (f != u.frame_last) { u.frame_last = f; u.frame_ms = plat_ms(); u.frame_seen = 1u; }
        g_usb_last_sof = f;
        ISOT[56] = f; ISOT[57] = u.frame_seen; }
    if (iu & 0x01u) u.suspended = 1;                      /* suspend */
    if (iu & 0x02u) u.suspended = 0;                      /* resume */
    if (iu & 0x04u) {                                     /* bus reset */
        u.resets++;
        DBG[1] = u.resets;
        sie_wr(S_FADDR, 0);
        u.config = 0; u.suspended = 0; u.frame_ms = plat_ms(); u.e0_tx = 0; u.has_pend_addr = 0; u.rx_pend = 0;
        txq_r = txq_w; u.tx_busy = 0;                     /* nothing stale for the next host */
        u.alt_play = 0; u.alt_rec = 0; u.iso_queued = 0;
        if (g_ctx) pa_usb_audio_playback_stop(g_ctx);
        U_EP0_ADR = (uint32_t)(uintptr_t)ep0buf;
        sie_wr(S_INTRUSBE, 0x07);
        sie_wr(S_INTRTX1E, 0x01);
        sie_wr(S_INTRRX1E, 0);
    }
    if (it & 0x01u) ep0_service();
    if (ir & 0x02u) u.rx_pend = 1;
    if (u.rx_pend) ep1_rx();
    if (u.config) ep1_tx();
    usb_sie_unlock();
#if PA_USB_HEARTBEAT
    if (u.config && txq_w == txq_r && !u.tx_busy) {       /* BENCH: a plain CC every 500 ms, so the host sees traffic without a request */
        const uint32_t now = plat_ms();
        if ((uint32_t)(now - u.last_hb_ms) >= 500u) {
            uint8_t hb[4] = { 0x0B, 0xB0, 0x01, 0x7F };
            const uint32_t beat = u.hb_n++ & 3u;          /* the CC number (1..4) still cycles, so the heartbeat looks the same on the wire */
#ifndef PA_USB_VARIANTS
#define PA_USB_VARIANTS 0   /* Diagnostic TX variants: off (standard endpoint configuration used) */
#endif
            u.variant = PA_USB_VARIANTS ? beat : 0u;      /* which TX setup this beat tries (bench only) */
            hb[2] = (uint8_t)(1u + beat);
            u.last_hb_ms = now;
            pa_usb_dev_tx(hb, 4u);
        }
    }
#endif
    if (!u.svc_mode && u.config) { (void)iso_service(0u); rx_service(); }
    DBG[0] = (uint32_t)u.up | (uint32_t)(u.config ? 2u : 0u) | (uint32_t)(u.suspended ? 4u : 0u);
    DBG[5] = u.timeouts;
}

#else
typedef int pa_usb_dev_host_build_has_no_driver;          /* an empty translation unit is not valid C */
#endif
