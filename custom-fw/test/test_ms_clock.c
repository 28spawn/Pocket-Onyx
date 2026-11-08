/* SPDX-License-Identifier: GPL-3.0-only */
/* Copyright (C) 2026 The Onyx Authors (@28spawn) */
/* custom-fw/test/test_ms_clock.c - the millisecond clock survives the tick counter's wrap
 *
 * The pi32's 24 MHz counter is 32 bits, so it wraps every 178.96 s.  The old plat_ms()
 * returned the raw counter divided by 24000, so the clock jumped back to zero every three
 * minutes - which broke the auto power-off, the toasts and the long-press timing.
 */
#include <stdarg.h>
#include <stdio.h>
#include <stdint.h>
#include "pa_plat.h"

static int fails = 0;

static void check(int ok, const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    printf(ok ? "  ok   " : "  FAIL ");
    vprintf(fmt, ap);
    printf("\n");
    va_end(ap);
    if (!ok) fails++;
}

#define TICKS_PER_MS 24000u

int main(void) {
    printf("=== the millisecond clock ===\n");

    pa_ms_clock_t c = { 0, 0, 0 };
    uint32_t now = 0;

    /* 1. a straight second: 24000 ticks a millisecond */
    uint32_t ms = pa_ms_clock_tick(&c, 1000u * TICKS_PER_MS, TICKS_PER_MS);
    check(ms == 1000u, "1000 ms of ticks reads as 1000 ms");

    /* 2. the counter wraps (2^32) - the clock must NOT go back */
    now = 0xFFFFFF00u;                         /* just before the wrap */
    uint32_t before = pa_ms_clock_tick(&c, now, TICKS_PER_MS);
    now = 0x00000100u;                         /* 512 ticks later, after the wrap */
    uint32_t after = pa_ms_clock_tick(&c, now, TICKS_PER_MS);
    check(after >= before, "the clock does not go back when the counter wraps");
    check(after - before <= 1u, "the wrap costs no more than a millisecond");

    /* 3. the old behaviour, for the record: the raw counter / 24000 */
    uint32_t raw = now / TICKS_PER_MS;
    check(raw < 100u, "the raw counter divided by 24000 really would have gone back "
                      "(it reads %u now)", raw);

    /* 4. a long run across many wraps: 10 minutes of ticks, sampled every 100 ms */
    pa_ms_clock_t d = { 0, 0, 0 };
    uint32_t ticks = 0;
    uint32_t last_ms = 0;
    int monotonic = 1;
    for (int i = 0; i < 6000; i++) {           /* 6000 * 100 ms = 600 s = 10 minutes */
        ticks += 100u * TICKS_PER_MS;           /* 100 ms per step */
        uint32_t m = pa_ms_clock_tick(&d, ticks, TICKS_PER_MS);
        if (m < last_ms) monotonic = 0;
        last_ms = m;
    }
    check(monotonic, "the clock is monotonic over 10 minutes (three counter wraps)");
    check(last_ms >= 599000u && last_ms <= 601000u,
          "10 minutes of ticks reads as ~600000 ms (got %u)", last_ms);

    /* 5. the remainder is carried, so no time is lost: 1000 calls of 1 ms + 1234 ticks
     *    (a fraction of a millisecond each time) must add up to the total. */
    pa_ms_clock_t e = { 0, 0, 0 };
    uint32_t total = 0;
    const uint32_t step = TICKS_PER_MS + 1234u;
    for (int i = 0; i < 1000; i++) {
        total += step;
        pa_ms_clock_tick(&e, total, TICKS_PER_MS);
    }
    uint32_t want_ms = total / TICKS_PER_MS;
    check(e.ms + (e.remainder / TICKS_PER_MS) == want_ms,
          "the fractional milliseconds are carried over, not lost (%u ms of %u)",
          e.ms, want_ms);
    check(e.remainder == total % TICKS_PER_MS,
          "the remainder is the exact leftover (%u of %u)", e.remainder, total % TICKS_PER_MS);

    printf("\n%s\n", fails == 0 ? "MS CLOCK OK" : "MS CLOCK FAILED");
    return fails == 0 ? 0 : 1;
}
