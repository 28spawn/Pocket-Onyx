/* SPDX-License-Identifier: GPL-3.0-only */
/* Copyright (C) 2026 The Onyx Authors (@28spawn) */
/* include/pa_overload.h - when does a run of codec overruns mean the system is overloaded (the OVERLOAD popup)?
 *
 * A preset or model swap runs inside the audio interrupt for 1.8-5.4 ms (1.451 ms is a block), with the output muted, and each swap costs exactly ONE codec RX overrun (bench 2026-11-04,
 * tools/perf_monitor_pc.py: +115 xruns for 115 swaps, never a bypass, never a governor level 2). The popup's rule "4 overruns in one second" fired on a quick scroll through presets (4 swaps a
 * second), a normal use, and told the player to switch effects off. The overruns of the swaps are expected: each swap is allowed two, and only the overruns beyond that count. */
#ifndef PA_OVERLOAD_H
#define PA_OVERLOAD_H
#include <stdint.h>

#define PA_XRUN_BURST        4u    /* overruns beyond the swaps' own, in one second, that make a burst */
#define PA_XRUN_PER_SWAP     2u    /* what a swap may cost (measured: 1) */

static inline int pa_xrun_burst(uint32_t xruns_in_second, uint32_t swaps_in_second)
{
    const uint32_t allowed = swaps_in_second * PA_XRUN_PER_SWAP;
    return xruns_in_second > allowed && (xruns_in_second - allowed) >= PA_XRUN_BURST;
}
#endif
