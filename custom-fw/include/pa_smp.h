/* SPDX-License-Identifier: GPL-3.0-only */
/* Copyright (C) 2026 The Onyx Authors (@28spawn) */
/* custom-fw/include/pa_smp.h - the second core (CPU1) as an audio helper.
 *
 * The AC791N has two pi32v2 cores. Verified on the unit (2026-10-31, src/board/cpu1.c): core 1 starts with the control
 * register 0x01EEE004 (bit 3 enable, bit 1 hold) after an entry stub in FLASH is written to the word at 0x01C7FFF8, runs at
 * the same 329 MHz as core 0, and shares the memory bus without slowing it in the tests we ran. The registers that this
 * header used to name (0x001F0000...) were guesses and are gone.
 *
 * Pipeline (roles chosen from the measurements): core 1 runs the FRONT of the chain on each block - the input conditioning and every
 * slot up to and including the AMP, about 1.25 ms of the 1.451 ms block and no interrupt overhead - while core 0, in the audio
 * interrupt, runs the slots after the amp (CAB, EQ, MOD, DLY, REV in the user's order) and the tail (global EQ, volume, limiter) on
 * the PREVIOUS block's amp output, about 1.0 ms. One block (1.45 ms) more latency. If core 1 does not start or ever misses a
 * deadline, the hook is removed and everything runs on core 0 as before. Status: SRAM 0x01C7D200 (tools/jl_ram_read.py slots).
 *
 * Hold ">" while switching on to keep the unit on one core (safe mode). */
#ifndef PA_SMP_H
#define PA_SMP_H

#include <stdint.h>
#include "pa_config.h"
#include "pa_fx.h"

#ifndef PA_SMP_AUDIO
#define PA_SMP_AUDIO 1          /* 0 = never start core 1 */
#endif

void     pa_smp_init(pa_fx_chain_t *chain);          /* try to start core 1 and install the hook */
int      pa_smp_is_online(void);
void     pa_smp_update_demand(pa_fx_chain_t *chain); /* the main loop's health check (name kept) */
void     pa_smp_wait_idle(void);                     /* a preset swap calls this: core 1 idle, its block dropped */
uint32_t pa_smp_cpu1_load_permille(void);

/* src/board/cpu1.c: the mailbox */
int      pa_c1_start(void);                                       /* 1 = core 1 answered */
uint32_t pa_c1_submit(void (*fn)(uint32_t), uint32_t arg);
int      pa_c1_wait(uint32_t job, uint32_t ms);                   /* 0 = done, -1 = timeout */
int      pa_c1_wait_us(uint32_t job, uint32_t us);                /* 0 = done, -1 = timeout */
void     pa_c1_halt_if_running(void);
uint32_t pa_c1_bad_jobs(void);
uint32_t pa_c1_job_ticks_last(void);
uint32_t pa_c1_job_ticks_max(void);
uint32_t pa_c1_stack_used(void);
uint32_t pa_c1_stack_size(void);

#endif /* PA_SMP_H */
