/* SPDX-License-Identifier: GPL-3.0-only */
/* Copyright (C) 2026 The Onyx Authors (@28spawn) */
/* custom-fw/include/hal/pa_timer_hal.h — AC791N TIMER4/TIMER5 & cycle counter HAL (QW-2). */
#ifndef PA_TIMER_HAL_H
#define PA_TIMER_HAL_H
#include <stdint.h>

#define PA_T4_CON        (*(volatile uint32_t *)0x10800u)
#define PA_T4_CNT        (*(volatile uint32_t *)0x10804u)
#define PA_T4_PRD        (*(volatile uint32_t *)0x10808u)
#define PA_T5_CON        (*(volatile uint32_t *)0x10900u)
#define PA_T5_CNT        (*(volatile uint32_t *)0x10904u)
#define PA_T5_PRD        (*(volatile uint32_t *)0x10908u)
#define PA_CPU_CYC_CNT   (*(volatile uint32_t *)0x1EEF0F0u)
#define PA_CPU_CYC_PRD   (*(volatile uint32_t *)0x1EEF0F4u)
#define PA_TICKS_PER_US  24u

static inline void pa_timer4_init(void) {
    if ((PA_T4_CON & 0xFu) != ((2u << 2) | 1u)) {
        PA_T4_CON = 0;
        PA_T4_PRD = 0xFFFFFFFFu;
        PA_T4_CNT = 0;
        PA_T4_CON = (2u << 2) | 1u | (1u << 14);
    }
    PA_CPU_CYC_PRD = 0xFFFFFFFFu;
}

static inline uint32_t pa_ticks24(void) { return PA_T4_CNT; }
static inline uint32_t pa_cycles(void)  { return PA_CPU_CYC_CNT; }

static inline void pa_delay_us(uint32_t us) {
    uint32_t t0 = PA_T4_CNT, span = us * PA_TICKS_PER_US;
    while ((uint32_t)(PA_T4_CNT - t0) < span) ;
}
static inline void pa_delay_ms(uint32_t ms) {
    while (ms--) pa_delay_us(1000u);
}
static inline void pa_timer5_ack(void) { PA_T5_CON |= 0x4000u; }

#endif /* PA_TIMER_HAL_H */
