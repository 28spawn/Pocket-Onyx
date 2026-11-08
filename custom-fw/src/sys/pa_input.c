/* SPDX-License-Identifier: GPL-3.0-only */
/* Copyright (C) 2026 The Onyx Authors (@28spawn) */
/* src/sys/pa_input.c - the portable front-panel input layer (see pa_input.h). */
#include <string.h>
#include "../../include/pa_input.h"

void pa_input_init(pa_input_state_t *s) {
    if (!s) return;
    uint32_t keep = s->raw;
    uint32_t keep2 = s->cand;
    uint32_t keep3 = s->cand_ms;
    memset(s, 0, sizeof(*s));
    s->raw = keep;
    s->cand = keep2;
    s->cand_ms = keep3;
}

/* PARA and HOME have a click AND a hold, so their click waits for the
 * release (see the button loop below). */
#define PARA_BTN_INDEX 5
#define HOME_BTN_INDEX 0

void pa_input_set_raw(pa_input_state_t *s, uint32_t raw_mask) {
    if (!s) return;
    s->raw = raw_mask & 0x3Fu;
}

uint32_t pa_input_update(pa_input_state_t *s, uint32_t now_ms) {
    if (!s) return 0;
    if (!s->started) { s->started = 1; s->cand = s->raw; s->cand_ms = now_ms; s->stable = s->raw; }

    /* 1. Debounce: the raw mask must be stable for PA_IN_DEBOUNCE_MS. */
    if (s->raw != s->cand) {
        s->cand = s->raw;
        s->cand_ms = now_ms;
    } else if (now_ms - s->cand_ms >= PA_IN_DEBOUNCE_MS) {
        s->stable = s->cand;
    }

    uint32_t out = 0;

    /* 2. The six buttons.
     *
     * PARA and HOME have BOTH a click and a hold: click is decided on RELEASE
     * if released before the hold threshold, and hold is decided at the threshold/release.
     *
     * VOL-, VOL+, < and > are combo candidate buttons. When pressed down, their
     * single action is deferred by PA_IN_COMBO_WINDOW_MS (~25 ms) so that two-button
     * chords (e.g. VOL- & VOL+ for Tuner, or < & > for Bank) don't trigger an accidental
     * volume change or preset decrement if fingers land slightly apart.
     * If released before 25 ms, the single action fires immediately on release. */
    for (int b = 0; b < 6; b++) {
        uint32_t bit = 1u << b;
        int down = (s->stable & bit) != 0;
        int was  = (s->prev    & bit) != 0;
        const int is_combo_btn = (b >= 1 && b <= 4);
        if (down && !was) s->defer[b] = (uint8_t)(is_combo_btn && s->keys_hold_mode);     /* decided at the press, kept until the release */
        const int click_on_release = (b == PARA_BTN_INDEX) || (b == HOME_BTN_INDEX) || s->defer[b];

        if (down && !was) {
            s->press_ms[b]        = now_ms;
            s->next_repeat_ms[b]  = now_ms + PA_IN_REPEAT_DELAY_MS;
            s->hold_fired[b]      = 0;
            s->press_fired[b]     = 0;
            if (!click_on_release && !is_combo_btn) {
                out |= bit;
                s->press_fired[b] = 1;
            }
        } else if (down && was) {
            if (click_on_release && !s->hold_fired[b] && !s->combo_used[b]) {
                /* a hold fires the moment its time is reached, however long the button then stays down */
                const uint32_t held = now_ms - s->press_ms[b];
                if (b == HOME_BTN_INDEX) {
                    if (held >= PA_IN_HOME_HOLD_MS) { out |= PA_BTN_HOME_HOLD; s->hold_fired[b] = 1; }
                } else if (b == PARA_BTN_INDEX) {
                    if (held >= PA_IN_HOLD2_MS) { out |= PA_BTN_PARA_HOLD2; s->hold_fired[b] = 1; }
                } else if (held >= PA_IN_KEYHOLD_MS) {
                    static const uint32_t k_key_hold[4] = { PA_BTN_VOLM_HOLD, PA_BTN_VOLP_HOLD, PA_BTN_LEFT_HOLD, PA_BTN_RIGHT_HOLD };
                    out |= k_key_hold[b - 1];
                    s->hold_fired[b] = 1;
                }
            }
            if (!click_on_release && is_combo_btn && !s->press_fired[b] && !s->combo_used[b]) {
                if (now_ms - s->press_ms[b] >= PA_IN_COMBO_WINDOW_MS) {
                    out |= bit;
                    s->press_fired[b] = 1;
                }
            } else if (!click_on_release && s->press_fired[b] && !s->combo_used[b] &&
                       (int32_t)(now_ms - s->next_repeat_ms[b]) >= 0) {
                s->next_repeat_ms[b] = now_ms + PA_IN_REPEAT_MS;
                out |= bit;                       /* auto-repeat */
            }
        } else if (!down && was) {
            if (click_on_release) {
                uint32_t held = now_ms - s->press_ms[b];
                /* the hold that already fired while the button was down leaves nothing for the release */
                const uint32_t hold_ms = (b == HOME_BTN_INDEX) ? PA_IN_HOME_HOLD_MS : (b == PARA_BTN_INDEX) ? PA_IN_HOLD1_MS : PA_IN_KEYHOLD_MS;
                if (!s->hold_fired[b]) {
                    if (held < hold_ms) {
                        if (!s->combo_used[b]) out |= bit;      /* a quick press = a click */
                    } else if (b == PARA_BTN_INDEX) {
                        out |= PA_BTN_PARA_HOLD1;               /* PARA held 1-3 s: Save to */
                    }
                }
            } else if (is_combo_btn) {
                /* If released before combo window expired and wasn't part of a chord, emit single click */
                if (!s->press_fired[b] && !s->combo_used[b]) {
                    out |= bit;
                    s->press_fired[b] = 1;
                }
            }
            s->combo_used[b] = 0;
            s->defer[b] = 0;
        }
    }
    s->prev = s->stable;

    /* 3. The combos: fire once when the combination becomes active.
     * Evaluated after button loop so any single button press bits queued on this
     * tick for chord buttons are cleanly stripped. */
    {
        static const uint16_t k_combo_mask[4] = {
            (uint16_t)(PA_BTN_VOL_MINUS | PA_BTN_VOL_PLUS),  /* tuner  */
            (uint16_t)(PA_BTN_LEFT | PA_BTN_RIGHT),          /* bank   */
            (uint16_t)(PA_BTN_HOME | PA_BTN_VOL_MINUS),      /* drums  */
            (uint16_t)(PA_BTN_HOME | PA_BTN_VOL_PLUS),       /* chain  */
        };
        static const uint32_t k_combo_bit[4] = {
            PA_BTN_COMBO_TUNER, PA_BTN_COMBO_BANK,
            PA_BTN_COMBO_DRUMS, PA_BTN_COMBO_CHAIN
        };
        for (int c = 0; c < 4; c++) {
            int active = (s->stable & k_combo_mask[c]) == k_combo_mask[c];
            if (active && !s->combo_fired[c]) {
                s->combo_fired[c] = 1;
                out &= ~(uint32_t)k_combo_mask[c];
                out |= k_combo_bit[c];
                for (int b = 0; b < 6; b++) {
                    if (k_combo_mask[c] & (1u << b)) {
                        s->combo_used[b] = 1;
                        s->press_fired[b] = 1; /* suppress single action */
                    }
                }
            } else if (!active) s->combo_fired[c] = 0;
        }
    }

    return out;
}
