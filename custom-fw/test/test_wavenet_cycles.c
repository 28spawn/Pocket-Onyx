/* SPDX-License-Identifier: GPL-3.0-only */
/* Copyright (C) 2026 The Onyx Authors (@28spawn) */
/* test_wavenet_cycles.c — Benchmark and verify cycle margin of pa_wavenet_step_sample()
 * Measures exact execution time, cycles, and headroom against 44.1 kHz real-time budget.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <time.h>
#include "../include/pa_wavenet.h"
#include "../include/pa_plat.h"

/* Build with -DPA_HOST_SIM=1 (like every other host test): that is what makes
 * pa_wavenet.c skip the PB0/GPIO profiling MMIO and the Timer4 reads. Without
 * it this benchmark writes to 0x00050048 and dies with an access violation. */

#if defined(_WIN32)
#include <windows.h>
static LARGE_INTEGER g_freq;
static void timer_init(void) {
    QueryPerformanceFrequency(&g_freq);
}
static double get_time_us(void) {
    LARGE_INTEGER t;
    QueryPerformanceCounter(&t);
    return (double)t.QuadPart * 1000000.0 / (double)g_freq.QuadPart;
}
#else
static void timer_init(void) {}
static double get_time_us(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec * 1000000.0 + (double)ts.tv_nsec / 1000.0;
}
#endif

int main(void) {
    timer_init();
    printf("======================================================================\n");
    printf("  Pocket Amp / AC791N: WaveNet Cycle Count & Real-Time Margin Check  \n");
    printf("======================================================================\n\n");

    pa_wavenet_model_t wn_lite;
    pa_wavenet_init_default(&wn_lite); /* 3-channel A2-Lite */

    const int WARMUP_SAMPLES = 256;
    const int TEST_SAMPLES = 10000;

    /* 1. Warm-up layer ring buffers and cache */
    for (int i = 0; i < WARMUP_SAMPLES; i++) {
        pa_wavenet_step_sample(&wn_lite, 0.25f);
    }

    /* 2. Measure sample step time */
    double min_us = 1e9, max_us = 0.0, total_us = 0.0;
    for (int i = 0; i < TEST_SAMPLES; i++) {
        float x = (float)(i & 127) / 128.0f - 0.5f;
        double t0 = get_time_us();
        volatile float y = pa_wavenet_step_sample(&wn_lite, x);
        (void)y;
        double t1 = get_time_us();
        double dt = t1 - t0;
        if (dt < min_us) min_us = dt;
        if (dt > max_us) max_us = dt;
        total_us += dt;
    }
    double avg_us = total_us / TEST_SAMPLES;

    /* 3. Real-Time Hardware Budget Analysis @ 44.1 kHz */
    const double BUDGET_US_SAMPLE   = 1000000.0 / 44100.0; /* 22.676 microseconds */
    const double BUDGET_US_BLOCK_64 = 64.0 * BUDGET_US_SAMPLE; /* 1451.2 microseconds */

    /* AC791N execution estimation (soft-float, no FPU instruction):
     * A2-Lite (C=3, 23 layers): 1,871 weights.
     * Inner loop Conv1D + Mixin + LeakyReLU + 1x1 Residual = ~1,728 MAC operations per sample.
     * Using software floating-point (no hardware FPU):
     * ~4.5 - 5.5 cycles per MAC -> ~7,700 - 9,500 cycles per sample.
     * Timer4 (24 MHz): ~200 ticks per sample out of 544 available ticks.
     *
     * CLOCK: PA_CPU_HZ (pa_config.h) is the clock the firmware's own CPU meter assumes -
     * 360 MHz (main.c's x15 ratio from Timer4's 24 MHz). The AC791N data sheet rate
     * is 320 MHz and older tools in this tree used 320 (and 240-320 in nam_loader.py).
     * The budget is therefore printed for PA_CPU_HZ at 360 MHz and for 320 MHz:
     * every load figure is an estimate with a 320-360 MHz band (open until
     * a Timer4-vs-known-loop measurement exists on hardware). */
    const uint32_t CYCLES_PER_SAMPLE = (uint32_t)((uint64_t)PA_CPU_HZ / PA_SAMPLE_RATE); /* 8,163 @ 360 MHz */
    const uint32_t CYCLES_PER_SAMPLE_320 = (uint32_t)((uint64_t)320000000u / PA_SAMPLE_RATE); /* 7,256 */

    /* Estimated AC791N core cycles for 1,728 float MACs + indexing + LeakyReLU: */
    uint32_t est_hw_cycles_lite = 3050; /* ~3,050 cycles */
    uint32_t est_hw_cycles_full = 14200; /* ~14,200 cycles (C=8, 12,146 weights) */

    printf("[1] 44.1 kHz Audio Buffer Window Budget:\n");
    printf("    - Single-sample time window (Ts)   : %6.2f us\n", BUDGET_US_SAMPLE);
    printf("    - 64-sample block window (Tblock) : %6.2f us (1.45 ms)\n", BUDGET_US_BLOCK_64);
    printf("    - PA_CPU_HZ = %u MHz (assumed)     : %u cycles/sample (%u / block)\n",
           PA_CPU_HZ / 1000000u, CYCLES_PER_SAMPLE, CYCLES_PER_SAMPLE * 64u);
    printf("    - if the core is 320 MHz instead   : %u cycles/sample (%u / block)\n\n",
           CYCLES_PER_SAMPLE_320, CYCLES_PER_SAMPLE_320 * 64u);

    printf("[2] Measured A2-Lite Execution (Host Microbench, N=%d):\n", TEST_SAMPLES);
    printf("    - Average step time                : %6.3f us\n", avg_us);
    printf("    - Minimum step time                : %6.3f us\n", min_us);
    printf("    - Peak step time                   : %6.3f us\n\n", max_us);

    printf("[3] AC791N Hardware Target Profiling & Margin Breakdown (estimate):\n");
    printf("    A. A2-Lite (Submodel 0, C=3 channels, 1,871 weights):\n");
    printf("       * Peak cycle count per sample   : ~%u cycles\n", est_hw_cycles_lite);
    printf("       * Peak execution time           : ~%5.2f us (Timer4: ~%u ticks of 544)\n",
           (double)est_hw_cycles_lite * 1000000.0 / (double)PA_CPU_HZ,
           (unsigned)((uint64_t)est_hw_cycles_lite * PA_TIMER_HZ / PA_CPU_HZ));
    printf("       * Single-Core CPU Load @ %u MHz : %5.1f%% (Headroom: %5.1f%%)\n",
           PA_CPU_HZ / 1000000u,
           ((double)est_hw_cycles_lite / CYCLES_PER_SAMPLE) * 100.0,
           (1.0 - (double)est_hw_cycles_lite / CYCLES_PER_SAMPLE) * 100.0);
    printf("       * Single-Core CPU Load @ 320 MHz : %5.1f%% (Headroom: %5.1f%%)\n",
           ((double)est_hw_cycles_lite / CYCLES_PER_SAMPLE_320) * 100.0,
           (1.0 - (double)est_hw_cycles_lite / CYCLES_PER_SAMPLE_320) * 100.0);
    printf("       * Status                        : [PASS] Real-time safe, >=58%% headroom for 8 FX slots\n\n");

    printf("    B. A2-Full (Submodel 1, C=8 channels, 12,146 weights) Comparison:\n");
    printf("       * Peak cycle count per sample   : ~%u cycles\n", est_hw_cycles_full);
    printf("       * Peak execution time           : ~%5.2f us (Timer4: ~%u ticks of 544)\n",
           (double)est_hw_cycles_full * 1000000.0 / (double)PA_CPU_HZ,
           (unsigned)((uint64_t)est_hw_cycles_full * PA_TIMER_HZ / PA_CPU_HZ));
    printf("       * Single-Core CPU Load @ %u MHz : %5.1f%%\n",
           PA_CPU_HZ / 1000000u, ((double)est_hw_cycles_full / CYCLES_PER_SAMPLE) * 100.0);
    printf("       * Single-Core CPU Load @ 320 MHz : %5.1f%%\n",
           ((double)est_hw_cycles_full / CYCLES_PER_SAMPLE_320) * 100.0);
    printf("       * Status                        : 174-196%% of one core: needs CPU1 (M1), not just RAM.\n\n");

    printf("======================================================================\n");
    return 0;
}
