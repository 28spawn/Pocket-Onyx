# SPDX-License-Identifier: GPL-3.0-only
# Copyright (C) 2026 The Onyx Authors (@28spawn)
/*
 * custom-fw/bench/bench_entry.S - entry and timing loops for the RAM-resident WaveNet
 * benchmark. The JieLi ROM download mode (UBOOT1.00) jumps here after the host uploads
 * the image to RAM; when bench_main() returns, control goes back to the ROM, which then
 * answers the host's memory reads (that is how the results come back).
 *
 * The benchmark runs on its own stack (bench_stack_top, see bench.ld), so it does not
 * depend on how much stack the ROM left free. r4 keeps the ROM's sp across the call;
 * it is callee-saved, so bench_main() preserves it.
 */
    .section .text.entry, "ax"
    .global bench_entry
    .type bench_entry, @function
bench_entry:
    [--sp] = {rets, r4}
    r4 = sp
    sp = bench_stack_top
    call bench_main
    sp = r4
    {pc, r4} = [sp++]
    .size bench_entry, . - bench_entry

/*
 * Timing loops. Each takes r0 = iterations and runs 64 instructions of one kind per
 * iteration plus one loop branch. The host divides Timer4 time by the instruction count.
 *
 *   bench_alu_loop        64 dependent integer adds   -> the core clock (1 add = 1 cycle)
 *   bench_fadd_dep_loop   64 dependent float adds     -> float add latency
 *   bench_fmul_dep_loop   64 dependent float multiplies -> float multiply latency
 *   bench_fadd_ind_loop   64 float adds over 4 independent accumulators -> throughput
 */
    .text
    .global bench_alu_loop
    .type bench_alu_loop, @function
bench_alu_loop:
    r1 = 0
    r2 = 1
1:
    .rept 64
    r1 = r1 + r2
    .endr
    if (--r0 != 0) goto 1b
    r0 = r1
    rts
    .size bench_alu_loop, . - bench_alu_loop

    .global bench_fadd_dep_loop
    .type bench_fadd_dep_loop, @function
bench_fadd_dep_loop:
    r1 = 0
    r2 = 0
1:
    .rept 64
    r1 = r1 + r2 (f)
    .endr
    if (--r0 != 0) goto 1b
    r0 = r1
    rts
    .size bench_fadd_dep_loop, . - bench_fadd_dep_loop

    .global bench_fmul_dep_loop
    .type bench_fmul_dep_loop, @function
bench_fmul_dep_loop:
    r1 = 0
    r2 = 0
1:
    .rept 64
    r1 = r1 * r2 (f)
    .endr
    if (--r0 != 0) goto 1b
    r0 = r1
    rts
    .size bench_fmul_dep_loop, . - bench_fmul_dep_loop

    .global bench_fadd_ind_loop
    .type bench_fadd_ind_loop, @function
bench_fadd_ind_loop:
    [--sp] = {r5, r4}
    r1 = 0
    r2 = 0
    r3 = 0
    r4 = 0
    r5 = 0
1:
    .rept 16
    r1 = r1 + r5 (f)
    r2 = r2 + r5 (f)
    r3 = r3 + r5 (f)
    r4 = r4 + r5 (f)
    .endr
    if (--r0 != 0) goto 1b
    r0 = r1
    {r5, r4} = [sp++]
    rts
    .size bench_fadd_ind_loop, . - bench_fadd_ind_loop
