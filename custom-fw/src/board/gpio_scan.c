/* SPDX-License-Identifier: GPL-3.0-only */
/* Copyright (C) 2026 The Onyx Authors (@28spawn) */
/*
 * src/board/gpio_scan.c — Pocket Amp Buttons, Rotary Encoder & SARADC Scanner (QW-6 / BR-4)
 *
 * Hardware pin mapping for AC791N:
 *   - Quadrature Rotary Encoder A/B on PC0 & PC1 (`JL_PORTC_IN & 0x03`):
 *       Quadrature Rotary Encoder A/B on PC0 & PC1 (`JL_PORTC_IN & 0x03` @ 0x50084)
 *   - 6 Active-Low Buttons packed into bits [5:0]:
 *       6 Active-Low Buttons packed into bits [5:0]:
 *         Bit 0: PH6 (`JL_PORTH_IN` @ 0x501C4 bit 6)
 *         Bit 1: PH8 (`JL_PORTH_IN` @ 0x501C4 bit 8)
 *         Bit 2: PH9 (`JL_PORTH_IN` @ 0x501C4 bit 9)
 *         Bit 3: PA0 (`JL_PORTA_IN` @ 0x50004 bit 0)
 *         Bit 4: PA1 (`JL_PORTA_IN` @ 0x50004 bit 1)
 *         Bit 5: PA3 (`JL_PORTA_IN` @ 0x50004 bit 3)
 *       Plus 4 two-button chord combos: (2,3), (4,5), (3,4), (1,5).
 *   - SARADC Channel 2:
 *       SARADC Channel 2 (`adc_get_value(2)`) with 5-sample median filter & hysteresis >= 3.
 */
#include <stdint.h>
#include "../../include/pa_config.h"
#include "../../include/pa_hw.h"
#include "../../include/pa_plat.h"

static uint32_t g_btn_state = 0;
static int32_t  g_enc_accum = 0;
static int8_t   g_enc_substep = 0;
static uint8_t  g_enc_prev_ab = 3u;
static uint16_t g_adc_pots[4] = { 2048, 2048, 2048, 2048 };
static volatile uint8_t  g_scan_busy;       /* the poll is called from the main loop AND the audio interrupt: never both at once */
static volatile uint32_t g_btn_pulse;       /* buttons the interrupt saw stably down since the last UI tick */
static uint32_t g_btn_hold;                 /* those that were released again before the tick: still reported one more tick */
static volatile uint8_t g_bl_duty10 = 0u;    /* IMP-06 backlight soft-PWM duty (0..10); 0 = off until the LCD init has blanked the panel */

/* Two-button chord pairs */
static const uint8_t k_combo_pairs[4][2] = {
    { 2, 3 }, /* PH9 + PA0 */
    { 4, 5 }, /* PA1 + PA3 */
    { 3, 4 }, /* PA0 + PA1 */
    { 1, 5 }  /* PH8 + PA3 */
};

void pa_gpio_scan_init(void) {
    g_btn_state   = 0;
    g_enc_accum   = 0;
    g_enc_substep = 0;
    g_enc_prev_ab = 3u;

#if !defined(PA_HOST_BUILD) && !defined(PA_HOST_SIM)
    /* Configure PC0, PC1 (Encoder A/B) as inputs with pull-up */
    PA_WRITE32(JL_PORTC_DIR, PA_READ32(JL_PORTC_DIR) | PA_ENC_PORTC_MASK);
    PA_WRITE32(JL_PORTC_PU,  PA_READ32(JL_PORTC_PU)  | PA_ENC_PORTC_MASK);
    PA_WRITE32(JL_PORTC_DIE, PA_READ32(JL_PORTC_DIE) | PA_ENC_PORTC_MASK);

    /* Configure PH6, PH8, PH9 as inputs with pull-up */
    const uint32_t ph_btns = PA_BTN0_PORTH_BIT | PA_BTN1_PORTH_BIT | PA_BTN2_PORTH_BIT;
    PA_WRITE32(JL_PORTH_DIR, PA_READ32(JL_PORTH_DIR) | ph_btns);
    PA_WRITE32(JL_PORTH_PU,  PA_READ32(JL_PORTH_PU)  | ph_btns);
    PA_WRITE32(JL_PORTH_DIE, PA_READ32(JL_PORTH_DIE) | ph_btns);

    /* Configure PA0, PA1, PA3 as inputs with pull-up */
    const uint32_t pa_btns = PA_BTN3_PORTA_BIT | PA_BTN4_PORTA_BIT | PA_BTN5_PORTA_BIT;
    PA_WRITE32(JL_PORTA_DIR, PA_READ32(JL_PORTA_DIR) | pa_btns);
    PA_WRITE32(JL_PORTA_PU,  PA_READ32(JL_PORTA_PU)  | pa_btns);
    PA_WRITE32(JL_PORTA_DIE, PA_READ32(JL_PORTA_DIE) | pa_btns);

    g_enc_prev_ab = (uint8_t)(PA_READ32(JL_PORTC_IN) & PA_ENC_PORTC_MASK);
#endif
}

/* Periodic 1 kHz hardware button and encoder scanner */
void pa_gpio_scan_poll_1khz(void) {
    (void)k_combo_pairs;
#if !defined(PA_HOST_BUILD) && !defined(PA_HOST_SIM)
    if (g_scan_busy) return;
    g_scan_busy = 1u;
    /* 1. Quadrature decode on PC0 & PC1 */
    uint8_t curr_ab = (uint8_t)(PA_READ32(JL_PORTC_IN) & PA_ENC_PORTC_MASK);
    uint8_t prev_ab = g_enc_prev_ab & PA_ENC_PORTC_MASK;
    if (curr_ab != prev_ab) {
        int32_t step = (int32_t)((prev_ab ^ (curr_ab >> 1)) & 1u)
                     - (int32_t)(((prev_ab >> 1) ^ (curr_ab & 1u)) & 1u);
        g_enc_substep = (int8_t)(g_enc_substep + step);
        g_enc_prev_ab = curr_ab;
        if (g_enc_substep > 3) {
            g_enc_accum++;
            g_enc_substep -= 4;
        } else if (g_enc_substep < -3) {
            g_enc_accum--;
            g_enc_substep += 4;
        }
    }

    /* 2. Read 6 physical buttons from Port H (PH6, PH8, PH9) & Port A (PA0, PA1, PA3) */
    uint32_t ph_in = PA_READ32(JL_PORTH_IN);
    uint32_t pa_in = PA_READ32(JL_PORTA_IN);

    uint32_t raw_high = 0;
    if (ph_in & PA_BTN0_PORTH_BIT) raw_high |= (1u << 0); /* PH6 */
    if (ph_in & PA_BTN1_PORTH_BIT) raw_high |= (1u << 1); /* PH8 */
    if (ph_in & PA_BTN2_PORTH_BIT) raw_high |= (1u << 2); /* PH9 */
    if (pa_in & PA_BTN3_PORTA_BIT) raw_high |= (1u << 3); /* PA0 */
    if (pa_in & PA_BTN4_PORTA_BIT) raw_high |= (1u << 4); /* PA1 */
    if (pa_in & PA_BTN5_PORTA_BIT) raw_high |= (1u << 5); /* PA3 */

    /* Buttons are active-low (0 = pressed) */
    const uint32_t down = (~raw_high) & 0x3Fu;
    /* Which physical button is on which pin, MEASURED on the bench unit (2026-10-31, one press at a
     * time, black-box button log): PH6 = PARA click, PH8 = HOME, PH9 = VOL-, PA0 = VOL+, PA1 = "<",
     * PA3 = ">".  The UI's logical bits (pa_input.h) are HOME 0, VOL- 1, VOL+ 2, "<" 3, ">" 4, PARA 5,
     * so the six pins are rotated by one place.  Before this, PARA acted as HOME and HOME as VOL-. */
    g_btn_state = ((down >> 1) & 0x1Fu) | ((down & 1u) << 5);
    g_scan_busy = 0u;

    /* The board layer only reports WHICH BUTTONS ARE DOWN. Turning that into
     * one-tick action pulses, debouncing the contacts and synthesising the
     * combo and HOLD bits is FIRMWARE logic and lives in src/sys/pa_input.c, so
     * the device, the host emulator and the WebAssembly build all turn a
     * button press into the same single action. The portable core calls
     * pa_input_set_raw() / pa_input_update() on it, so the driver keeps no
     * copy of its own (a second, never-updated copy was dead weight). */

    /* 3. The backlight PWM no longer runs here: this poll is once per main-loop pass, so a long frame
     *    redraw froze the light at whatever phase it was in. See pa_backlight_isr_tick() below. */
#else
    (void)g_enc_substep;
    (void)g_enc_prev_ab;
#endif
}

uint32_t plat_buttons(void) {
    return g_btn_state;
}

/* Called from the audio interrupt (about every 1.45 ms): keeps the buttons and the encoder sampled while the main loop is busy
 * redrawing the screen. A contact must be stable for 4 samples (~6 ms) to count as a press. */
void pa_gpio_scan_isr_tick(void) {
#if !defined(PA_HOST_BUILD) && !defined(PA_HOST_SIM)
    static uint32_t cand, cnt;
    pa_gpio_scan_poll_1khz();
    const uint32_t s = g_btn_state;
    if (s == cand) { if (cnt < 255u) cnt++; if (cnt == 4u) g_btn_pulse |= s; }
    else { cand = s; cnt = 0u; }
#endif
}

/* The buttons as the UI tick should see them: what is down now, plus any press that came and went since the last tick (a quick
 * click is shorter than one UI tick). A click that was only seen between ticks is reported for two ticks in a row, which is what
 * the input layer's debounce needs to accept it. */
uint32_t plat_buttons_for_ui(void) {
    const uint32_t p = g_btn_pulse;
    g_btn_pulse = 0u;
    const uint32_t fresh = p & ~g_btn_state;
    const uint32_t out = g_btn_state | fresh | g_btn_hold;
    g_btn_hold = fresh;
    return out;
}

int32_t plat_enc_delta(void) {
    int32_t d = g_enc_accum;
    g_enc_accum = 0;
    return -d;                      /* clockwise = increase (the quadrature decode above counts it the other way round on this board) */
}

static uint16_t g_battery_mv = 0u;    /* last filtered value; 0 = no valid sample yet */
static uint8_t  g_battery_valid = 0u;
static uint16_t g_battery_adc = 0u;   /* the averaged SARADC channel 9 count; 0 = none yet */
static uint8_t  g_charging = 0u;

static uint16_t g_adc_pots_filtered[4] = { 2048, 2048, 2048, 2048 };

static uint8_t g_pot_valid;                      /* the master knob has been read at least once */

/* 0xFFFF = no reading yet (channel 0, the master knob): the UI must not act on a made-up middle position. */
uint16_t plat_pot_q12(int ch) {
    if (ch < 0 || ch >= 4) return 2048u;
    if (ch == 0 && !g_pot_valid) return 0xFFFFu;
    int32_t raw = (int32_t)g_adc_pots[ch];
    int32_t cur = (int32_t)g_adc_pots_filtered[ch];
    int32_t diff = raw - cur;
    if (diff > 4 || diff < -4) {
        g_adc_pots_filtered[ch] = (uint16_t)(cur + (diff >> 2));
    }
    return g_adc_pots_filtered[ch];
}

/* ---- IMP-12: battery voltage ---------------------------------------------------------------
 * The reading comes from the SARADC (peripheral 0x00013100, data word +4, analog bridge bit in the
 * config word 0x00011900):

 *   - an ADC "id" has the channel in the low nibble; channel 15 is the PMU analog mux and the id's
 *     high half picks the PMU channel;
 *   - CON = 0xF06E | channel<<8, CON |= 0x10, then per sample CON |= 0x40, wait for CON bit 0x80,
 *     read the data word (10 bit);
 *   - config bit 0x4000 is cleared for every switch and set again only for id 15 (PMU channel 0).
 * PMU channel 0 is the bandgap: converted to supply voltage with
 * mV = 1023 * (800 + trim) / raw, trim = +-3 * bits[6:2] of the byte at 0x01C7FD6A (sign = bit 7),
 * and trims the supply up if that is below 3400. PMU channel 3 (id 0x3000F) is polled for supply
 * changes and reads plain channel 2 as a 0..100 % POT.
 * PA_VBAT_ADC_CH = 2 was wrong.
 *
 * Both readings share the ADC reference, so it cancels:  VBAT = DIV * Vbg * raw_vbat / raw_vbg.
 * PMU channel 3 = VBAT/4 is the JieLi convention but is NOT yet confirmed on the bench: the
 * diagnostic block at SRAM 0x01C7D000 (jl_ram_read.py battery / slots) holds the raw value of every
 * PMU channel and plain channel, so one read settles it. Until a sample lands in 2500..4800 mV
 * plat_battery_mv() returns 0 (the UI shows "unknown", pct 255) and the brownout guard stays open. */
/* PA_BATT_SARADC: 1 = run the SARADC battery reader below. OFF by default since 2026-10-31: on the first unit run (pack 11) the boot
 * was abandoned by the watchdog with no CPU exception and no battery diagnostics written, and this reader (a mask-ROM helper call
 * plus ADC register writes, run from the main loop) is the only new main-loop code. Markers 0x71..0x75 go to the main-loop
 * checkpoint word (0x01C7C3F4) so that, if it is turned back on and stalls, the stop point is known. */
#ifndef PA_BATT_SARADC
#define PA_BATT_SARADC 1       /* BENCH build (2026-11-01): the hang on pack 31 was a wrong address (see PA_BATT_ROM_HELPER), now fixed; stage markers 0x71..0x75 in 0x01C7C3F4 and "<" at power-on skips the reader. If a unit does not boot with this, set 0. */
#endif
/* PA_BATT_ROM_HELPER: 1 = select the PMU analog channel through the mask-ROM helper 0xC00F2E. OFF: on pack 31 the main loop stopped at
 * marker 0x72 (the call never returned), the watchdog reset the unit 6 s after boot and the rescue dropped it into the download mode. With it
 * off the reader only converts the plain channels and the current state of channel 15 for the bench, and reports no battery voltage. */
#ifndef PA_BATT_ROM_HELPER
#define PA_BATT_ROM_HELPER 1   /* 1 = select the PMU channel by a P33 read-modify-write of register 4. Register 4 is restored after every poll. */
#endif
#define PA_BCP(n) (*(volatile uint32_t *)0x01C7C3F4u = (uint32_t)(n))
#ifndef PA_VBAT_ADC_ID
#define PA_VBAT_ADC_ID        0x0003000Fu   /* PMU analog channel 3 (assumed VBAT / PA_VBAT_DIV) */
#endif
#ifndef PA_VBAT_DIV
#define PA_VBAT_DIV           4u
#endif
#define PA_SARADC_VBG_ID      0x0000000Fu   /* PMU channel 0: the 0.8 V bandgap */
#define PA_VBG_NOMINAL_MV     800
#define PA_SARADC_TRIM_ADDR   0x01C7FD6Au   /* bootloader RAM byte for bandgap trim */
#define PA_BATT_DIAG_ADDR     0x01C7D000u
#define PA_BATT_DIAG_MAGIC    0x54414256u   /* "VBAT" */

#if !defined(PA_HOST_BUILD) && !defined(PA_HOST_SIM)
#define JL_SARADC_CON         0x00013100u
#define JL_SARADC_DAT         0x00013104u
#define JL_SARADC_MUX         0x00011900u
extern uint8_t plat_p33_read(uint16_t reg);
extern void    plat_p33_write(uint16_t reg, uint8_t v);
/* P33 register RMW: reg = (reg & ~(mask << pos)) | ((val & mask) << pos), mask = (1 << len) - 1. */
static void p33_field(uint32_t reg, uint32_t pos, uint32_t len, uint32_t val) {
    uint32_t mask = ((1u << len) - 1u) << pos;
    plat_p33_write((uint16_t)reg, (uint8_t)((plat_p33_read((uint16_t)reg) & ~mask) | ((val << pos) & mask)));
}
#define PA_ROM_P33_FIELD      p33_field
static uint8_t s_reg4_orig, s_reg4_have;

static uint8_t  s_adc_dead;         /* conversions never complete: stop trying so the main loop is not stalled */
static uint8_t  s_adc_fail;
static uint32_t s_adc_timeouts;

/* Average of n conversions of the ADC id (the first 3 are discarded while the input settles); 0xFFFF = failed. */
static uint16_t saradc_avg(uint32_t id, uint32_t n) {
    if (s_adc_dead) return 0xFFFFu;
    PA_BCP(0x71);
    PA_WRITE32(JL_SARADC_MUX, PA_READ32(JL_SARADC_MUX) & ~0x4000u);
#if PA_BATT_ROM_HELPER
    if ((id & 0xFFFFu) == 15u) {
        if (!s_reg4_have) { s_reg4_orig = plat_p33_read(4u); s_reg4_have = 1u; }
        PA_BCP(0x72);                                   /* about to write P33 register 4 */
        PA_ROM_P33_FIELD(4u, 1u, 3u, id >> 16);
        PA_ROM_P33_FIELD(4u, 5u, 1u, 1u);
        PA_ROM_P33_FIELD(4u, 0u, 1u, 1u);
        PA_BCP(0x73);                                   /* the helper returned */
    }
#endif
    if (id == 15u) PA_WRITE32(JL_SARADC_MUX, PA_READ32(JL_SARADC_MUX) | 0x4000u);
    PA_WRITE32(JL_SARADC_CON, 0xF06Eu | ((id & 0xFu) << 8));
    PA_WRITE32(JL_SARADC_CON, PA_READ32(JL_SARADC_CON) | 0x10u);
    uint32_t sum = 0;
    PA_BCP(0x74);                                       /* about to convert */
    for (uint32_t i = 0; i < n + 3u; i++) {
        PA_WRITE32(JL_SARADC_CON, PA_READ32(JL_SARADC_CON) | 0x40u);
        uint32_t guard = 200000u;
        while (!(PA_READ32(JL_SARADC_CON) & 0x80u)) {
            if (--guard == 0u) {
                s_adc_timeouts++;
                if (++s_adc_fail >= 3u) s_adc_dead = 1u;
                return 0xFFFFu;
            }
        }
        uint32_t d = PA_READ32(JL_SARADC_DAT) & 0x3FFu;
        if (i >= 3u) sum += d;
    }
    s_adc_fail = 0u;
    PA_BCP(0x75);
    return (uint16_t)((sum + n / 2u) / n);
}
#endif

/* Raw 10-bit reading of plain ADC channel ch (0..14), averaged; 0 = no reading. */
__attribute__((weak)) uint16_t pa_hal_saradc_read(uint8_t ch) {
#if !defined(PA_HOST_BUILD) && !defined(PA_HOST_SIM) && PA_BATT_SARADC
    if (ch > 14u) return 0u;
    uint16_t v = saradc_avg(ch, 4u);
    return v == 0xFFFFu ? 0u : v;
#else
    (void)ch; return 0u;
#endif
}

/* The master volume knob: a plain SARADC channel (no PMU helper, so none of the battery reader's mask-ROM call). Read about 20 times a
 * second after the first 4 s, 10-bit, shown as 12-bit through plat_pot_q12(0). Holding "<" at power-on skips it (in case a unit does not
 * like the ADC being touched). The raw value and the failure counters are at SRAM 0x01C7D4D0 ("POTS", raw, adc dead, timeouts). */
#ifndef PA_POT_SARADC
#define PA_POT_SARADC 1
#endif
#ifndef PA_POT_CH
#define PA_POT_CH 2
#endif
extern uint32_t g_boot_buttons;
void plat_pot_poll(void) {
#if !defined(PA_HOST_BUILD) && !defined(PA_HOST_SIM) && PA_POT_SARADC
    static uint32_t last_ms;
    extern uint32_t plat_ms(void);
    const uint32_t now = plat_ms();
    if (now < 1500u || (uint32_t)(now - last_ms) < 50u) return;      /* was 4000: the output is held silent until the knob has been read (pa_audio_core.c g_pa_master_hold), so the wait is audible */
    if (g_boot_buttons & 0x08u) return;
    last_ms = now;
    const uint16_t v = saradc_avg(PA_POT_CH, 4u);
    volatile uint32_t *D = (volatile uint32_t *)(uintptr_t)0x01C7D4D0u;
    D[0] = 0x504F5453u; D[1] = v; D[2] = s_adc_dead; D[3] = s_adc_timeouts;
    if (v != 0xFFFFu) {
        g_adc_pots[0] = (uint16_t)(v * 4u + 2u);
        if (!g_pot_valid) { g_adc_pots_filtered[0] = g_adc_pots[0]; g_pot_valid = 1u; }
    }
#endif
}

/* Battery reading: the raw 10-bit SARADC channel 9, averaged over 150 samples, then the capacity formula of
 * pa_battery_pct_from_adc. The first reading is a quick one (the stock's boot path), so the icon is right on the first frame; then one sample every 25 ms, and every 150th
 * sample the average replaces the reading. The charging state is PMU status register 0x16A10, bit 16 (active low: 0 = USB power). The old reader (the bandgap, PMU channel 3
 * assumed to be VBAT / 4) read 4.5 V on a charged cell and is gone, with its P33 register writes. */
#define PA_VBAT_CH            9u
#define PA_BATT_AVG_N         150u
#define PA_BATT_SAMPLE_MS     25u
#define PA_BATT_MV_X10        66u        /* about 6.6 mV per count (full cell: 4.2 V ≈ 635 counts) */
#define PA_PMU_STATUS_ADDR    0x00016A10u
#define PA_BATT_CRITICAL_ADC  528u       /* critical low battery threshold (approx 3.49 V) */
extern uint32_t g_boot_buttons;
void plat_battery_poll(void) {
#if !defined(PA_HOST_BUILD) && !defined(PA_HOST_SIM) && PA_BATT_SARADC
    static uint32_t sum, cnt, last_ms, polls, bad;
    extern uint32_t plat_ms(void);
    volatile uint32_t *D = (volatile uint32_t *)(uintptr_t)PA_BATT_DIAG_ADDR;
    const uint32_t now = plat_ms();
    if (now < 1500u || (g_boot_buttons & 0x08u)) return;       /* the audio and the memory test are finished before the main loop starts; "<" at power-on skips the reader */
    const uint32_t st = PA_READ32(PA_PMU_STATUS_ADDR);
    g_charging = (uint8_t)(((st >> 16) & 1u) ? 0u : 1u);
    D[40] = st; D[41] = g_charging;
    if (!g_battery_adc) {                                      /* the fast path: one short average, taken as the first reading */
        const uint16_t v = saradc_avg(PA_VBAT_CH, 8u);
        if (v != 0xFFFFu && v != 0u) { g_battery_adc = v; g_battery_mv = (uint16_t)((uint32_t)v * PA_BATT_MV_X10 / 10u); g_battery_valid = 1u; D[38] = v; }
        else bad++;
        last_ms = now;
        return;
    }
    if ((uint32_t)(now - last_ms) < PA_BATT_SAMPLE_MS) return;
    last_ms = now;
    polls++;
    const uint16_t v = saradc_avg(PA_VBAT_CH, 1u);
    if (v == 0xFFFFu) { bad++; return; }
    sum += v; cnt++;
    D[32] = v; D[1] = polls; D[34] = s_adc_timeouts; D[35] = s_adc_dead; D[36] = now; D[37] = bad;
    if (cnt >= PA_BATT_AVG_N) {
        const uint16_t avg = (uint16_t)((sum + PA_BATT_AVG_N / 2u) / PA_BATT_AVG_N);
        sum = 0u; cnt = 0u;
        if (avg) { g_battery_adc = avg; g_battery_mv = (uint16_t)((uint32_t)avg * PA_BATT_MV_X10 / 10u); D[38] = avg; D[33] = g_battery_mv; }
    }
    D[39] = 3u;                                                 /* 3 = ADC channel 9, 150-sample average) */
    D[0] = PA_BATT_DIAG_MAGIC;
#endif
}

/* ---- IMP-06: backlight brightness -------------------------------------------------------------
 * PA9 drives the backlight (active-low) as a plain GPIO, so "brightness" had no effect. Stop-gap:
 * 10-step software PWM from the 1 kHz scan (100 Hz). Preferred: route a TIMER PWM output to PA9
 * (no flicker, no CPU) once the AC791N timer-PWM mapping is confirmed. */
/* IMP-06 backlight PWM on PA9 (active-low), called once per audio interrupt (about every 1.45 ms), so the
 * timing does not depend on how long the main loop is busy. Error diffusion over the 0..10 duty: duty 8
 * gives 1,1,1,1,0 repeating (about 138 Hz), duty 10 is steady on and duty 0 steady off. */
void pa_backlight_isr_tick(void) {
#if PA_BACKLIGHT_FIXED
    return;                                                       /* held on by plat_backlight_on / plat_backlight_set: nothing to do in the audio interrupt */
#endif
#if !defined(PA_HOST_BUILD) && !defined(PA_HOST_SIM)
    static uint8_t s_err;
    s_err = (uint8_t)(s_err + g_bl_duty10);
    if (s_err >= 10u) { s_err = (uint8_t)(s_err - 10u); PA_WRITE32(JL_PORTA_OUT, PA_READ32(JL_PORTA_OUT) & ~PA_LCD_BL_PORTA_BIT); }
    else              { PA_WRITE32(JL_PORTA_OUT, PA_READ32(JL_PORTA_OUT) | PA_LCD_BL_PORTA_BIT); }
#endif
}

/* Set the PWM duty directly (0..10 of 10), for the boot fade-in. */
void plat_backlight_duty10(uint8_t d) { g_bl_duty10 = d > 10u ? 10u : d; }

/* Called by the LCD init once the panel is black: until then the PWM holds the light off, so the
 * power-up noise in the controller's memory is never lit. */
void plat_backlight_on(void) {
    if (!g_bl_duty10) g_bl_duty10 = 10u;
#if !defined(PA_HOST_BUILD) && !defined(PA_HOST_SIM)
    PA_WRITE32(JL_PORTA_OUT, PA_READ32(JL_PORTA_OUT) & ~PA_LCD_BL_PORTA_BIT);   /* on at once, even if the audio interrupt is not running */
#endif
}

__attribute__((weak)) void plat_backlight_set(uint8_t pct) {
#if PA_BACKLIGHT_FIXED
    (void)pct; g_bl_duty10 = 10u;                                 /* steady on: no PWM, so nothing in the audio interrupt's timing can show as flicker */
#if !defined(PA_HOST_BUILD) && !defined(PA_HOST_SIM)
    PA_WRITE32(JL_PORTA_OUT, PA_READ32(JL_PORTA_OUT) & ~PA_LCD_BL_PORTA_BIT);   /* active low: on, and it stays on (the audio interrupt no longer drives the pin) */
#endif
    return;
#endif
    if (pct > 100u) pct = 100u;
    uint8_t d = (uint8_t)((pct + 5u) / 10u);
    g_bl_duty10 = d ? d : 1u;                                     /* never fully dark */
}

int plat_battery_mv(void) {
    return (int)g_battery_mv;
}

int plat_battery_adc(void) { return (int)g_battery_adc; }
int plat_charging(void)    { return (int)g_charging; }

/* Battery percentage quarters (0..4). */
int plat_battery_bars(void) {
    if (!g_battery_adc) return 0;
    extern uint8_t pa_battery_pct_from_adc(uint16_t adc);
    const uint8_t p = pa_battery_pct_from_adc(g_battery_adc);
    return (int)((p + 12u) / 25u);
}

__attribute__((weak)) int plat_battery_is_brownout(void) {
    /* Below critical count (528, approx 3.49 V) flash erase and write spikes risk a brownout reset mid-operation: the flash is locked, as in the stock. */
    return (g_battery_adc && g_battery_adc < PA_BATT_CRITICAL_ADC) ? 1 : 0;
}

/* The watchdog is NOT fed.
 *
 * The clear key used to be written to 0x00010800 - which is TIMER4's control register
 * (JL_TIMER4_BASE in include/pa_hw.h).  That reconfigured the timer the whole firmware
 * times itself with, and the low bits of the clear key (0x5A) clear the timer's enable
 * bit, so the main loop's 500 us delay never ended.
 *
 * The watchdog's real register block is not known yet (the JieLi AC79 SDK's app-level
 * headers are not in this tree), so nothing is written.
 * That cannot be
 * worse than the old behaviour: if the bootloader left the watchdog running the device
 * resets either way, and if it did not, the device now keeps running.  A compile-time
 * check in pa_hw.h keeps the watchdog and Timer4 from ever sharing a register block. */
/* ---- P33 Companion MCU interface ---- */
#if !defined(PA_HOST_BUILD) && !defined(PA_HOST_SIM)
static uint8_t p33_xfer(uint8_t b) {
    uint32_t n = 100000u;
    PA_WRITE32(JL_P33_DAT, b);
    PA_WRITE32(JL_P33_CON, PA_READ32(JL_P33_CON) | (1u << 4));
    (void)PA_READ32(JL_P33_CON);
    while (PA_READ32(JL_P33_CON) & (1u << 1)) {
        if (--n == 0) break;
    }
    return (uint8_t)PA_READ32(JL_P33_DAT);
}

static void p33_cs(uint16_t a) {
    if (a & 0x8000u) {
        PA_WRITE32(JL_P33_CON, PA_READ32(JL_P33_CON) | (1u << 0) | (1u << 8));
    } else {
        PA_WRITE32(JL_P33_CON, PA_READ32(JL_P33_CON) & ~(1u << 8));
        PA_WRITE32(JL_P33_CON, PA_READ32(JL_P33_CON) | (1u << 0));
    }
}

static void p33_op(uint8_t op, uint16_t a, uint8_t d) {
    p33_cs(a);
    p33_xfer((uint8_t)((op << 5) | ((a >> 8) & 3u)));
    p33_xfer((uint8_t)a);
    p33_xfer(d);
    PA_WRITE32(JL_P33_CON, PA_READ32(JL_P33_CON) & ~((1u << 0) | (1u << 8)));
}

static void p33_or(uint16_t a, uint8_t d) {
    p33_op(1u, a, d);
}
#endif

__attribute__((weak)) void plat_wdt_feed(void) {
#if !defined(PA_HOST_BUILD) && !defined(PA_HOST_SIM)
    p33_or(JL_P3_WDT_CON, 0x40u);
#endif
}

/* Arm the P33 watchdog. plat_wdt_feed() alone never armed it: the enable bit (0x10) was never
 * set, so the 2026-10-31 hang test saw no reset. The sequence, from the Felucca BSP's
 * fm1_wdt_arm (and a feed is "or 0x40"): clear the register, clear WDT_EXPT_EN in VLD_KEEP
 * (a plain reset, no exception), write the timeout with the clear bit, then set enable.
 * t: 0xA = 1 s .. 0xF = 32 s (0xD = about 8 s). */
/* Disarm the P33 watchdog (write 0 to WDT_CON: the enable bit goes). Only the Bluetooth radio bench build uses it, BEFORE the radio start-up: with the radio up the unit was
 * reset about 8.2 s after power-on, the watchdog's 8 s since it was armed (2026-11-02, pack 110). With no watchdog a hang is only caught by the audio interrupt's freeze
 * detector; a hang in the radio start-up itself would need a power cycle. */
void plat_wdt_disarm(void) {
#if !defined(PA_HOST_BUILD) && !defined(PA_HOST_SIM)
    p33_op(0u, JL_P3_WDT_CON, 0u);
#endif
}

void plat_wdt_arm(uint8_t t) {
#if !defined(PA_HOST_BUILD) && !defined(PA_HOST_SIM)
    p33_op(0u, JL_P3_WDT_CON, 0u);
    p33_op(2u, JL_P3_VLD_KEEP, (uint8_t)~0x40u);
    p33_op(0u, JL_P3_WDT_CON, (uint8_t)((t & 0x0Fu) | 0x40u));
    p33_op(1u, JL_P3_WDT_CON, 0x10u);
#else
    (void)t;
#endif
}

/* Generic P33 register access (the companion MCU's analog/power registers, e.g. the core supply levels). */
uint8_t plat_p33_read(uint16_t reg) {
#if !defined(PA_HOST_BUILD) && !defined(PA_HOST_SIM)
    uint8_t v;
    p33_cs(reg);
    p33_xfer((uint8_t)(0x80u | ((reg >> 8) & 3u)));
    p33_xfer((uint8_t)reg);
    v = p33_xfer(0x5Eu);
    PA_WRITE32(JL_P33_CON, PA_READ32(JL_P33_CON) & ~((1u << 0) | (1u << 8)));
    return v;
#else
    (void)reg; return 0u;
#endif
}
void plat_p33_write(uint16_t reg, uint8_t v) {
#if !defined(PA_HOST_BUILD) && !defined(PA_HOST_SIM)
    p33_op(0u, reg, v);
#else
    (void)reg; (void)v;
#endif
}

/* Read the watchdog register back (P33 0x80). 0x5D after plat_wdt_arm(0xD) means the enable bit
 * (0x10) and the timeout stuck; 0 means the write did not take. */
uint8_t plat_wdt_read(void) {
#if !defined(PA_HOST_BUILD) && !defined(PA_HOST_SIM)
    uint8_t v;
    p33_cs(JL_P3_WDT_CON);
    p33_xfer((uint8_t)(0x80u | ((JL_P3_WDT_CON >> 8) & 3u)));
    p33_xfer((uint8_t)JL_P3_WDT_CON);
    v = p33_xfer(0x5Eu);
    PA_WRITE32(JL_P33_CON, PA_READ32(JL_P33_CON) & ~((1u << 0) | (1u << 8)));
    return v;
#else
    return 0u;
#endif
}

/* ---- Autonomous Mask-ROM UBOOT Handover (The Escape Hatch) ----
 *
 * Writes 'usb_update_mode' to 0x01C7FD80, clears CPU0 write limit (0x01EEE348),
 * holds CPU1 (0x01EEE004 |= 2), and executes chip reset via P33 PMU (P3_PR_PWR |= 0x10).
 * Mask-ROM enumerates as WL82 UBOOT1.00 for jl-uboot-tool recovery. */
extern void pa_c1_halt_if_running(void);       /* cpu1.c: the second core must not run while the loader writes */
__attribute__((noinline)) void rc_recover_uboot(void) {
#if !defined(PA_HOST_BUILD) && !defined(PA_HOST_SIM)
    /* WHO asked for the download mode: the address this function was called from, kept in SRAM 0x01C7D5CC for the readout (jl_ram_read.py prints it; the build's map names the
     * function). Every path to the download mode ends here: the OTA frame, the HOME+PARA combination, the freeze detectors, the fault handler, the boot rescue. */
    if ((*(volatile uint32_t *)(uintptr_t)0x01C7D5C8u >> 8) != 0x525354u)     /* not in the boot rescue (main.c writes "RST" there first): that call must not hide who asked in the run before */
        *(volatile uint32_t *)(uintptr_t)0x01C7D5CCu = (uint32_t)(uintptr_t)__builtin_return_address(0);
#endif
#if !defined(PA_HOST_BUILD) && !defined(PA_HOST_SIM)
    pa_c1_halt_if_running();
#endif
#if !defined(PA_HOST_BUILD) && !defined(PA_HOST_SIM)
    static const char k[16] = "usb_update_mode";
    volatile uint8_t *box = (volatile uint8_t *)(uintptr_t)JL_UBOOT_MAILBOX;
    uint32_t i;

    __asm__ volatile("cli" ::: "memory");

    /* Clear write limit so mailbox area can be written */
    PA_WRITE32(JL_CPU0_WR_LIMIT_EN, 0u);

    /* Hold CPU1 in reset */
    PA_WRITE32(JL_CPU1_CTRL, (PA_READ32(JL_CPU1_CTRL) | 0x2u) & ~0x8u);

    /* Write update mailbox */
    for (i = 0; i < 16u; i++) {
        box[i] = (uint8_t)k[i];
    }

    /* Trigger PMU chip reset */
    p33_or(JL_P3_PR_PWR, 0x10u);

    for (;;) {
        /* Hang until reset takes */
    }
#endif
}

__attribute__((weak)) int plat_reset_to_downloader(void) {
    rc_recover_uboot();
    return 0;
}
