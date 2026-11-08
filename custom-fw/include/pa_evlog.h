/* SPDX-License-Identifier: GPL-3.0-only */
/* Copyright (C) 2026 The Onyx Authors (@28spawn) */
/*
 * include/pa_evlog.h - the event log: what the unit did and how it answered, in a ring in SRAM.
 *
 * 64 events of 16 bytes at SRAM 0x01C7D600 (header 16 bytes first): { raw 24 MHz tick, code, a, b }. The main loop, the audio
 * interrupt and the model/preset swap write to it; nothing on the unit reads it. Read it after the fact with
 * `tools/jl_ram_read.py slots` (the "event log" block), from the download mode (HOME + PARA for 2 s) or over the USB readout.
 * Host and WebAssembly builds compile the calls away. Two writers (main loop, interrupt) can in rare cases overwrite the same
 * slot; the entry is written before the count moves, so a half-written entry is at worst the newest one.
 */
#ifndef PA_EVLOG_H
#define PA_EVLOG_H

#include <stdint.h>

enum {
    PA_EV_BUTTONS   = 1,    /* a = button mask now (logical bits: 0 HOME 1 VOL- 2 VOL+ 3 < 4 > 5 PARA), b = mask before */
    PA_EV_ENCODER   = 2,    /* a = PARA encoder steps */
    PA_EV_VIEW      = 3,    /* a = view now, b = view before */
    PA_EV_PRESET    = 4,    /* a = preset now, b = preset before */
    PA_EV_MODEL     = 5,    /* the screen's slot model changed: a = slot, b = model index */
    PA_EV_SLOW_TICK = 6,    /* one UI tick took long: a = microseconds, b = last main-loop marker (PA_CP) */
    PA_EV_SWAP_REQ  = 7,    /* a model/preset swap was handed to the audio thread: a = slot (0xFF preset), b = model/preset */
    PA_EV_SWAP_DONE = 8,    /* the audio thread applied it: a = microseconds it took, b = slot (0xFF preset) */
    PA_EV_ISR_LONG  = 9,    /* an audio block ran over budget (first of a run): a = microseconds, b = last audio-interrupt marker */
    PA_EV_BYPASS    = 10,   /* the audio went to plain pass-through (8 over-budget blocks in a row) */
    PA_EV_SMP       = 11,   /* the second core's status changed: a = reason code now, b = reason before */
    PA_EV_BOOT      = 12,   /* the log started: a = boot buttons */
    PA_EV_RUNAWAY   = 13,   /* the signal after the amp was absurdly loud, the effects after it were cleared: a = peak in 1/100 full scale, b = first slot reset */
    PA_EV_AMP_RESET = 14,   /* the amp's state went non-finite or out of range and was reset: a = which unit first, b = 0 */
    PA_EV_FREEZE    = 17,   /* the unit froze and rescued itself to the download mode: a = 1 the main loop stopped (b = its pass count), 2 the audio interrupt stopped */
    PA_EV_AMP_SLOW  = 16,   /* the amp took over 1600 us for one block: a = microseconds, b = input peak % << 16 | output peak % of that block */
    PA_EV_LOAD_FAIL = 19,   /* a NAM capture would not load and the stock amp 0 was loaded instead: a = reason (1 slot empty, 2 flash read, 3 WaveNet rejected, 4 bad size or CRC), b = capture number */
    PA_EV_POOL_FAIL = 18,   /* the effect memory could not give a block: a = bytes asked for, b = the largest free block */
    PA_EV_BLE       = 20,   /* the Bluetooth radio (pa_ble_radio.h): a = what happened (PA_EV_BLE_*), b = detail */
    PA_EV_MASTER    = 21,   /* the output came up from the power-on silence: a = ms since boot, b = the MASTER level in percent (0x100 + the saved level when the knob never answered) */
    PA_EV_GOVERNOR  = 15    /* the load governor changed the quality level: a = level now (0 full, 1 drive at 2x, 2 + shorter cabinet), b = average audio time in us */
};

/* PA_EV_BLE codes (a), detail (b):
 *  1 boot decision: b = state (0 idle, 1 requested, 2 no cal)   2 start-up began   3 radio+baseband up: b = microseconds   4 advertising enabled: b = start-up ms
 *  5 first advertising event counted: b = ms since enable   6 a central connected   7 the central left   8 error: b = PA_BLE_E_*   9 BLE interrupt longer than ever: b = us
 * 10 the central turned the MIDI notifications on   11 first BLE-MIDI message received
 * 12 the stored bond went back into the stack's key slot at start-up: b = EDIV   13 a bond was written to the flash: b = 1 ok / 0xFF failed   14 the bond was forgotten */
enum { PA_EV_BLE_DECISION = 1, PA_EV_BLE_BEGIN, PA_EV_BLE_UP, PA_EV_BLE_ENABLED, PA_EV_BLE_ADV_SEEN, PA_EV_BLE_CONNECT, PA_EV_BLE_DISCONNECT, PA_EV_BLE_ERROR, PA_EV_BLE_ISR_MAX, PA_EV_BLE_MIDI_ON, PA_EV_BLE_MIDI_IN, PA_EV_BLE_BOND_RESTORED, PA_EV_BLE_BOND_SAVED, PA_EV_BLE_BOND_FORGOTTEN };

#if !defined(PA_HOST_BUILD) && !defined(PA_HOST_SIM)
#define PA_EVLOG_BASE   ((volatile uint32_t *)(uintptr_t)0x01C7D600u)
#define PA_EVLOG_CAP    64u
#define PA_DRUMDBG ((volatile uint32_t *)(uintptr_t)0x01C7D540u)
extern uint32_t plat_ticks24(void);
static inline void pa_evlog_init(void) {
    volatile uint32_t *h = PA_EVLOG_BASE;
    for (uint32_t i = 0; i < 4u + PA_EVLOG_CAP * 4u; i++) h[i] = 0u;
    h[2] = PA_EVLOG_CAP;
    for (uint32_t i = 0; i < 4u; i++) ((volatile uint32_t *)(uintptr_t)0x01C7D580u)[i] = 0u;   /* the flash read check counters (pa_xip.h) */
    for (uint32_t i = 0; i < 16u; i++) ((volatile uint32_t *)(uintptr_t)0x01C7D400u)[i] = 0u;
    for (uint32_t i = 0; i < 16u; i++) PA_DRUMDBG[i] = 0u;
    for (uint32_t i = 0; i < 24u; i++) ((volatile uint32_t *)(uintptr_t)0x01C7D4E0u)[i] = 0u;   /* the per-slot output peaks (fx_graph.c): they keep their values across a reset otherwise */
    h[0] = 0x474C5645u;                                    /* "EVLG" */
}
static inline void pa_evlog(uint32_t code, uint32_t a, uint32_t b) {
    volatile uint32_t *h = PA_EVLOG_BASE;
    if (h[0] != 0x474C5645u) return;
    const uint32_t n = h[1];
    volatile uint32_t *e = h + 4u + (n % PA_EVLOG_CAP) * 4u;
    e[0] = plat_ticks24(); e[1] = code; e[2] = a; e[3] = b;
    h[1] = n + 1u;                                         /* the count moves last */
}
/* Where the audio interrupt's time goes, besides the effects: SRAM 0x01C7D4E0, two words (last, longest; TIMER4 ticks, 24 per us) per
 * part: [0] the tail (volume, speaker mode, limiter), [1] the whole chain call with drums, mix limiter, USB and the output filters. */
static inline void pa_timing(unsigned idx, uint32_t ticks) {
    volatile uint32_t *w = (volatile uint32_t *)(uintptr_t)0x01C7D4E0u + 2u * idx;
    w[0] = ticks;
    if (ticks > w[1]) w[1] = ticks;
}
/* The drum engine's own timing, SRAM 0x01C7D540, 16 words: [0] "DRMT"; then (last, longest) in TIMER4 ticks (24 per us) for [1,2] the voices
 * (clock, triggers, decoders, the sum of them), [3,4] the drum bus (low-pass, limiter, level), [5,6] the whole drum engine call,
 * [7,8] the guitar + drums sum limiter; [9] the voices sounding at the end of the last call, [10] the most at once, [11..13] the most
 * ADPCM / mu-law / synthesised voices at once, [14] calls with a voice sounding, [15] calls. */
#define PA_DRUMDBG_ON 1
#define PA_DRUMDBG_T(v) const uint32_t v = plat_ticks24()
static inline void pa_drumdbg_time(unsigned part, uint32_t ticks) {
    volatile uint32_t *w = PA_DRUMDBG + 1u + 2u * part;
    w[0] = ticks;
    if (ticks > w[1]) w[1] = ticks;
}
#define PA_DRUMDBG_TIME(part, t) pa_drumdbg_time((part), (t))
static inline void pa_drumdbg_voices(uint32_t active, uint32_t adpcm, uint32_t ulaw, uint32_t synth) {
    volatile uint32_t *w = PA_DRUMDBG;
    w[0] = 0x544D5244u;
    w[9] = active;
    if (active > w[10]) w[10] = active;
    if (adpcm > w[11]) w[11] = adpcm;
    if (ulaw > w[12]) w[12] = ulaw;
    if (synth > w[13]) w[13] = synth;
    if (active) w[14]++;
    w[15]++;
}
#else
#define PA_DRUMDBG_ON 0
#define PA_DRUMDBG_T(v) const uint32_t v = 0u; (void)v
#define PA_DRUMDBG_TIME(part, t) ((void)0)
#define pa_timing(i, t) ((void)0)
static inline void pa_evlog_init(void) {}
#define pa_evlog(c, a, b) ((void)0)
#endif

#endif /* PA_EVLOG_H */
