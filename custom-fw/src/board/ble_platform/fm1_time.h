/* SPDX-License-Identifier: GPL-3.0-only */
/* Copyright (C) 2026 The Onyx Authors (@28spawn) */
/* The time source the imported radio driver uses: TIMER4, the 24 MHz free-running counter this firmware already
 * times itself with (pa_hw.h JL_TIMER4_BASE; main.c, audio_alnk.c). Optimist's platform runs the same counter at
 * the same rate, so its microsecond conversions (fm1_ticks() / FM1_TICKS_PER_US) hold unchanged. */
#pragma once
#include "fm1_cc.h"

#define FM1_TICKS_PER_US 24u
#define FM1_TIMER4_CNT   (*(volatile uint32_t *)0x00010804u)

FM1_INLINE uint32_t fm1_ticks(void) { return FM1_TIMER4_CNT; }

FM1_INLINE void fm1_delay_us(uint32_t us)
{
    uint32_t t0 = fm1_ticks(), span = us * FM1_TICKS_PER_US;
    while ((uint32_t)(fm1_ticks() - t0) < span)
        ;
}
