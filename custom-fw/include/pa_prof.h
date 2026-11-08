/* SPDX-License-Identifier: GPL-3.0-only */
/* Copyright (C) 2026 The Onyx Authors (@28spawn) */
/* include/pa_prof.h - per-part timing sums in SRAM for the effects (device only; the host, emulator and WebAssembly builds compile it away).
 *
 * Where does an effect's time go, on the unit, in the real audio path? Each effect owns a block of SRAM words: [0] magic, [1] blocks counted, then one sum of 24 MHz ticks per part (and
 * whatever the effect adds). The sums clear every 2048 blocks. Read with tools/perf_parts_pc.py. Cost: a few timer reads and about ten stores per block of the effect. */
#ifndef PA_PROF_H
#define PA_PROF_H
#include <stdint.h>

#if !defined(PA_HOST_BUILD) && !defined(PA_HOST_SIM) && !defined(PA_EMU_DLL) && !defined(__wasm__) && !defined(__EMSCRIPTEN__)
extern uint32_t plat_ticks24(void);
#define PA_PROF_ON 1
#define PA_PROF_T() plat_ticks24()
#else
#define PA_PROF_ON 0
#define PA_PROF_T() 0u
#endif

#define PA_PROF_DRIVE  0x01C7E480u   /* "DRVP": [2] gain path + clean path  [3] up  [4] clipper  [5] down  [6] tone + DC + level  [7] blocks at 4x oversampling */
#define PA_PROF_REVERB 0x01C7E4C0u   /* "REVP": [2 + type] ticks and [10 + type] blocks, per reverb type 0..7 */

static inline void pa_prof_begin(uintptr_t base, uint32_t magic, unsigned words)
{
#if PA_PROF_ON
    volatile uint32_t *B = (volatile uint32_t *)base;
    if (B[1] >= 2048u) { B[1] = 0u; for (unsigned i = 2; i < 2u + words; i++) B[i] = 0u; }
    B[1] = B[1] + 1u;
    B[0] = magic;
#else
    (void)base; (void)magic; (void)words;
#endif
}

static inline void pa_prof_add(uintptr_t base, unsigned idx, uint32_t ticks)
{
#if PA_PROF_ON
    volatile uint32_t *B = (volatile uint32_t *)base;
    B[2 + idx] = B[2 + idx] + ticks;
#else
    (void)base; (void)idx; (void)ticks;
#endif
}
#endif
