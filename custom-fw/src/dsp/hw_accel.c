/* SPDX-License-Identifier: GPL-3.0-only */
/* Copyright (C) 2026 The Onyx Authors (@28spawn) */
#include "../../include/pa_config.h"
/*
 * hw_accel.c — JieLi AC791N Hardware Vector/Matrix/FIR Accelerator Driver & Host Emulator
 *
 * Register protocol driver for hardware accelerator peripheral 0x01EE0400.
 */
#include "../../include/pa_hw_accel.h"
#include "../../include/pa_hw.h"
#include "../../include/pa_plat.h"

#if !defined(PA_HOST_BUILD) && !defined(PA_HOST_SIM)
/* On-device: use the stock SRAM command block at 0x01C1046C and busy flag at 0x01C0BFEC */
static volatile uint8_t g_hw_busy = 0;
static pa_hw_vec_cmd_t g_hw_cmd_block __attribute__((aligned(4)));
/* Set when the hardware stopped reporting READY: from then on everything runs in
 * software, so a broken accelerator slows the CPU down instead of hanging the device. */
static volatile uint8_t g_hw_broken = 0;
#if !defined(PA_HOST_BUILD) && !defined(PA_HOST_SIM)
#define HW_T4CNT (*(volatile uint32_t *)0x00010804u)   /* TIMER4 counter, 24 MHz */
/* Bounded by time now, not by spins: a spin is only tens of microseconds, shorter than a real
 * job may take. Job durations are recorded so the overlay shows the true time. */
#define PA_HW_ACCEL_WAIT_US 1000u
static volatile uint32_t g_hw_t0, g_hw_job_last, g_hw_job_max;
uint32_t pa_hw_accel_job_last_us(void) { return g_hw_job_last / 24u; }
uint32_t pa_hw_accel_job_max_us(void)  { return g_hw_job_max / 24u; }
#endif
static volatile uint32_t g_hw_errors = 0;
#endif

/* Bench switch: 1 = never touch the accelerator at 0x01EE0400, run every job in software (the
 * overlay then shows "ACC SW"). The 2026-10-31 bench run stalled inside the cabinet slot, which is
 * the first user of the accelerator; if reading 0x01EE0400 blocks the bus, no time limit can help.
 * 0 = use the hardware (the normal setting once the accelerator is proven). */
#ifndef PA_HW_ACCEL_FORCE_SW
#define PA_HW_ACCEL_FORCE_SW 0      /* back to the hardware: the FIR stride was the bug (see pa_hw_accel_fir_part64) */
#endif

/* BENCH: accelerator state for the RAM readout (tools/jl_ram_read.py slots), SRAM 0x01C7C5B0:
 * last job, longest job (TIMER4 ticks, 24 MHz), timeouts, broken flag, jobs completed. */
#if !defined(PA_HOST_BUILD) && !defined(PA_HOST_SIM)
#define HWS ((volatile uint32_t *)(uintptr_t)0x01C7C5B0u)
#endif

void pa_hw_accel_init(void) {
#if !defined(PA_HOST_BUILD) && !defined(PA_HOST_SIM)
    g_hw_busy = 0;
    g_hw_broken = PA_HW_ACCEL_FORCE_SW ? 1u : 0u;
    g_hw_errors = 0;
    for (uint32_t i = 0; i < 5u; i++) HWS[i] = 0u;
#endif
}

uint8_t pa_hw_accel_broken(void) {
#if !defined(PA_HOST_BUILD) && !defined(PA_HOST_SIM)
    return g_hw_broken;
#else
    return 0u;
#endif
}

uint32_t pa_hw_accel_errors(void) {
#if !defined(PA_HOST_BUILD) && !defined(PA_HOST_SIM)
    return g_hw_errors;
#else
    return 0u;
#endif
}

PA_HOT void pa_hw_accel_wait(void) {
#if !defined(PA_HOST_BUILD) && !defined(PA_HOST_SIM)
    if (g_hw_busy) {
        volatile uint32_t *reg = (volatile uint32_t *)PA_HW_VEC_BASE; /* 0x01EE0400 */
        const uint32_t t0 = g_hw_t0;
        PA_ISR_CP(0x68);                      /* waiting for READY: the first read of reg[0] follows */
        while ((reg[0] & PA_HW_VEC_STATUS_READY) == 0u) {
            /* Spin until hardware sets bit 7 (READY/PND) - but only for a BOUNDED number
             * of spins.  This runs inside the audio interrupt, so an unbounded wait is a
             * device lockup with no diagnostics, and the register protocol may simply be
             * wrong on this part.  On the timeout the accelerator is marked broken and
             * the caller does the work in software instead. */
            if ((uint32_t)(HW_T4CNT - t0) >= PA_HW_ACCEL_WAIT_US * 24u) {
                g_hw_errors++;
                HWS[2] = g_hw_errors; HWS[3] = 1u;
                g_hw_broken = 1u;
                g_hw_busy = 0u;
                reg[0] |= PA_HW_VEC_STATUS_ACK; /* Acknowledge and stop hardware engine */
                reg[0] = 0u;
                return;
            }
        }
        PA_ISR_CP(0x69);                      /* READY seen */
        { uint32_t job = HW_T4CNT - t0; g_hw_job_last = job; if (job > g_hw_job_max) g_hw_job_max = job;
          HWS[0] = job; HWS[1] = g_hw_job_max; HWS[4]++; }
        reg[0] |= PA_HW_VEC_STATUS_ACK; /* Set bit 6 (CPND) to acknowledge */
        PA_ISR_CP(0x6A);                      /* ACK written */
        g_hw_busy = 0;
    }
#endif
}

/* The software implementation of the same register protocol (the host build has always
 * used this).  It is also the device's fallback once the hardware has timed out. */
static void accel_software(const pa_hw_vec_cmd_t *cmd) {
    const uint32_t shift = (cmd->ctrl >> 24) & 0xFFu;

    if (cmd->config == PA_HW_CFG_MATVEC && (cmd->stride & 0xFFFFu) != 0u && (cmd->stride & 0xF000u) == 0u) {
        /* CONV MODE: src_a is the MATRIX, its row r starts (stride & 0xFFFF) bytes * r after src_a, each row is
         * cols words; src_b is the VECTOR (cols words); dst[r * dst_step] = saturate((sum_j A[r*step + j] * B[j] + round) >> shift) with dst_step =
         * ((stride >> 20) & 0xF) bytes (4: consecutive words, 12: 12-byte records). rows = ctrl bits 23:12, cols = ctrl bits 11:0, shift = ctrl bits 31:24.
         * This is the stock firmware's A2-Lite layer job. */
        const int32_t *A = (const int32_t *)cmd->src_a;
        const int32_t *B = (const int32_t *)cmd->src_b;
        int32_t       *D = (int32_t *)cmd->dst;
        const int      cols = (int)(cmd->ctrl & 0xFFFu), rows = (int)((cmd->ctrl >> 12) & 0xFFFu);
        const int      step = (int)((cmd->stride & 0xFFFFu) >> 2), dstep = (int)(((cmd->stride >> 20) & 0xFu) >> 2);
        for (int r = 0; r < rows; r++) {
            int64_t acc = 0;
            const int32_t *row = &A[r * step];
            for (int c = 0; c < cols; c++) acc += (int64_t)row[c] * (int64_t)B[c];
            acc = (acc + (shift ? (1LL << (shift - 1)) : 0LL)) >> shift;
            if (acc > 2147483647LL) acc = 2147483647LL;
            if (acc < -2147483648LL) acc = -2147483648LL;
            D[r * dstep] = (int32_t)acc;
        }
    } else if (cmd->config == PA_HW_CFG_MATVEC) {
        /* Matrix-Vector Multiply: dst[r] = sum_{c=0..cols-1}(mat[r*cols + c] * vec[c]) >> shift
         * Parameters: src_a = vec, src_b = mat, rows = 9, cols = cmd->ctrl & 0xFF (17 or 16) */
        const int32_t *vec  = (const int32_t *)cmd->src_a;
        const int32_t *mat  = (const int32_t *)cmd->src_b;
        int32_t       *dst  = (int32_t *)cmd->dst;
        const int      cols = (int)(cmd->ctrl & 0xFFu);
        int            rows = (int)((cmd->ctrl >> 12) & 0xFu);     /* the engine writes as many results as the control word says (9 and 10 seen on the unit) */
        if (rows == 0) rows = 9;

        for (int r = 0; r < rows; r++) {
            int64_t acc = 0;
            const int32_t *row_ptr = &mat[r * cols];
            for (int c = 0; c < cols; c++) {
                acc += (int64_t)row_ptr[c] * (int64_t)vec[c];
            }
            dst[r] = (int32_t)((acc + (shift ? (1LL << (shift - 1)) : 0LL)) >> shift);   /* the hardware rounds to nearest (verified on the unit, 2026-10-31) */
        }
    } else if (cmd->config == PA_HW_CFG_FIR_OVERWRITE ||
               cmd->config == PA_HW_CFG_FIR_ACCUMULATE) {
        /* Block-FIR Convolution:
         * src_a = coeff[0..N-1], src_b = hist[0..2*N-1], dst = acc_buf[0..N-1] */
        const int32_t *coeff = (const int32_t *)cmd->src_a;
        const int32_t *hist  = (const int32_t *)cmd->src_b;
        int32_t       *dst   = (int32_t *)cmd->dst;
        const int      n     = (int)((cmd->ctrl & 0x0FFFu) - 1u);
        const int      accum = (cmd->config == PA_HW_CFG_FIR_ACCUMULATE);

        for (int i = 0; i < n; i++) {
            int64_t sum = 0;
            for (int k = 0; k < n; k++) {
                sum += (int64_t)coeff[k] * (int64_t)hist[i + n - 1 - k];
            }
            int32_t val = (int32_t)(sum >> shift);
            if (accum) {
                dst[i] += val;
            } else {
                dst[i] = val;
            }
        }
    }
}

/* LEAN protocol for the WaveNet block engine (140 jobs a block): only the register protocol, as the stock's driver does - no timer reads (peripheral reads
 * are slow), no breadcrumbs, no statistics; the wait is bounded by a spin count, not by the timer. The standard submit/wait cost about 1 us a job. */
#define PA_HW_LEAN_SPINS 60000u         /* about 3 ms of polling: a 731 us job takes under 9,000 */
void pa_hw_accel_wait_lean(void) {
#if !defined(PA_HOST_BUILD) && !defined(PA_HOST_SIM)
    if (!g_hw_busy) return;
    volatile uint32_t *reg = (volatile uint32_t *)PA_HW_VEC_BASE;
    uint32_t n = PA_HW_LEAN_SPINS;
    while ((reg[0] & PA_HW_VEC_STATUS_READY) == 0u) {
        if (--n == 0u) {
            g_hw_errors++; HWS[2] = g_hw_errors; HWS[3] = 2u;
            g_hw_broken = 1u; g_hw_busy = 0u;
            reg[0] |= PA_HW_VEC_STATUS_ACK; reg[0] = 0u;
            return;
        }
    }
    reg[0] |= PA_HW_VEC_STATUS_ACK;
    g_hw_busy = 0u;
#endif
}

void pa_hw_accel_submit_lean(const pa_hw_vec_cmd_t *cmd) {
#if !defined(PA_HOST_BUILD) && !defined(PA_HOST_SIM)
    if (g_hw_broken) { accel_software(cmd); return; }
    pa_hw_accel_wait_lean();
    if (g_hw_broken) { accel_software(cmd); return; }
    volatile uint32_t *reg = (volatile uint32_t *)PA_HW_VEC_BASE;
    g_hw_cmd_block = *cmd;
    reg[2] = 0u;
    reg[1] = (uint32_t)(uintptr_t)&g_hw_cmd_block;
    reg[0] = 0x00000149u;
    g_hw_busy = 1u;
#else
    accel_software(cmd);
#endif
}

/* The same job with the six words as arguments: written straight into the command block once the job before has finished, with no struct built on the stack
 * and copied (140 jobs a block). */
void pa_hw_accel_job_lean(uint32_t config, const void *a, const void *b, void *dst, uint32_t ctrl, uint32_t stride) {
#if !defined(PA_HOST_BUILD) && !defined(PA_HOST_SIM)
    if (!g_hw_broken) {
        pa_hw_accel_wait_lean();
        if (!g_hw_broken) {
            volatile uint32_t *reg = (volatile uint32_t *)PA_HW_VEC_BASE;
            g_hw_cmd_block.config = config; g_hw_cmd_block.src_a = a; g_hw_cmd_block.src_b = b;
            g_hw_cmd_block.dst = dst; g_hw_cmd_block.ctrl = ctrl; g_hw_cmd_block.stride = stride;
            reg[2] = 0u;
            reg[1] = (uint32_t)(uintptr_t)&g_hw_cmd_block;
            reg[0] = 0x00000149u;
            g_hw_busy = 1u;
            return;
        }
    }
#endif
    pa_hw_vec_cmd_t c;
    c.config = config; c.src_a = a; c.src_b = b; c.dst = dst; c.ctrl = ctrl; c.stride = stride;
    pa_hw_accel_submit_lean(&c);
}

PA_HOT void pa_hw_accel_submit(const pa_hw_vec_cmd_t *cmd) {
#if !defined(PA_HOST_BUILD) && !defined(PA_HOST_SIM)
    if (g_hw_broken) {
        /* The hardware is not trusted any more: do the work in software. */
        accel_software(cmd);
        return;
    }
    volatile uint32_t *reg = (volatile uint32_t *)PA_HW_VEC_BASE; /* 0x01EE0400 */

    PA_ISR_CP(0x61);                          /* breadcrumbs: the last one left after a stall is the step that hung */
    /* Ensure previous job has completed before overwriting command block */
    pa_hw_accel_wait();
    PA_ISR_CP(0x62);

    g_hw_cmd_block = *cmd;
    PA_ISR_CP(0x63);

    reg[2] = 0u;                              /* [0x01EE0408] = 0 */
    PA_ISR_CP(0x64);
    reg[1] = (uint32_t)(uintptr_t)&g_hw_cmd_block; /* [0x01EE0404] = cmd block addr */
    PA_ISR_CP(0x65);
    g_hw_t0 = HW_T4CNT;
    reg[0] = 0x00000149u;                     /* [0x01EE0400] = 329 (START) */
    PA_ISR_CP(0x66);
    g_hw_busy = 1u;
#else
    accel_software(cmd);
#endif
}

void pa_hw_accel_fir_part64(const int32_t *coeffs_q18,
                            const int32_t *hist_x_q18,
                            int32_t *dst_q18,
                            int nframes,
                            int accumulate) {
    pa_hw_vec_cmd_t cmd;
    cmd.config = accumulate ? PA_HW_CFG_FIR_ACCUMULATE : PA_HW_CFG_FIR_OVERWRITE;
    cmd.src_a  = coeffs_q18;
    cmd.src_b  = hist_x_q18;
    cmd.dst    = dst_q18;
    cmd.ctrl   = PA_HW_CTRL_FIR_Q18(nframes);
    cmd.stride = 0u;    /* the stock FIR job writes 0 here (app.bin+0x9A722/0x9A7A8); 0x00010001 asked for a strided 2-D pass */
    PA_ISR_CP(0x60);

    if (pa_hw_accel_broken()) {
        accel_software(&cmd);
        return;
    }
    pa_hw_accel_submit(&cmd);
    PA_ISR_CP(0x67);
    pa_hw_accel_wait();
    PA_ISR_CP(0x6C);
    if (pa_hw_accel_broken()) {
        PA_ISR_CP(0x6D);                  /* the wait timed out: the software fallback runs */
        /* The hardware job was lost to the timeout: do the work in software, so the
         * result is still correct - it is just slower. */
        accel_software(&cmd);
    }
}
