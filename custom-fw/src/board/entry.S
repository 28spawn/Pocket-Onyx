# SPDX-License-Identifier: GPL-3.0-only
# Copyright (C) 2026 The Onyx Authors (@28spawn)
/*
 * src/board/entry.S — JieLi AC791N (pi32v2 / q32DSP) Startup & Section Relocation (QW-3)
 *
 * CRT0 startup sequence for JieLi AC791N (pi32v2):
 *   - Masks interrupts, sets user stack `sp` and supervisor stack `ssp`
 *   - Copies .ram_text and .data from XIP Flash to SRAM
 *   - Zeroes .bss
 *   - Calls pa_firmware_init()
 *   - Enables global interrupts (icfg |= 0x100, csync, sti)
 *   - Branches to pa_main_loop() in C
 */
    .section .text.crt0, "ax"
    .global _start
    .type _start, @function
_start:
    cli
    sp  = _ustack_top
    ssp = _sstack_top

    /* 1. Zero .bss */
    r3 = _bss_start
    r2 = _bss_end
    r2 = r2 - r3
    r2 = r2 >> 2
    r1 = 0
1:
    if (r2 == 0) goto 2f
    [r3++=4] = r1
    r2 += -1
    goto 1b
2:

    /* 2. Copy .ram_text from Flash (_rt_load) to SRAM (_rt_start.._rt_end) */
    r4 = _rt_start
    r1 = _rt_load
    r2 = _rt_end
    r2 = r2 - r4
    r2 = r2 >> 2
3:
    if (r2 == 0) goto 4f
    r3 = [r1++=4]
    [r4++=4] = r3
    r2 += -1
    goto 3b
4:

    /* 2b. Copy .ramfn (core 0 code that runs from SRAM) from Flash (_rf_load) to SRAM (_rf_start.._rf_end) */
    r4 = _rf_start
    r1 = _rf_load
    r2 = _rf_end
    r2 = r2 - r4
    r2 = r2 >> 2
5:
    if (r2 == 0) goto 6f
    r3 = [r1++=4]
    [r4++=4] = r3
    r2 += -1
    goto 5b
6:

    /* 3. Copy .data from Flash (_data_load) to SRAM (_data_start.._data_end) */
    r4 = _data_start
    r1 = _data_load
    r2 = _data_end
    r2 = r2 - r4
    r2 = r2 >> 2
5:
    if (r2 == 0) goto 6f
    r3 = [r1++=4]
    [r4++=4] = r3
    r2 += -1
    goto 5b
6:

    /* 4. Call C firmware initialization */
    call pa_firmware_init

    /* 5. Enable global interrupts (icfg |= 0x100; csync; sti) */
    r0 = icfg
    r1 = 0x100
    r0 = r0 | r1
    icfg = r0
    csync
    sti

    /* 6. Enter cooperative C hardware main loop (never returns) */
    call pa_main_loop

7:
    idle
    goto 7b
    .size _start, . - _start

/*
 * Hardware ISR entry wrapper for ALNK0 (Audio Link DMA IRQ 11)
 * Saves caller registers, calls C handler, restores, executes csync, and rti.
 */
    .section .text.isr_alnk0, "ax"
    .global isr_alnk0
    .type isr_alnk0, @function
isr_alnk0:
    [--sp] = {psr, rets, reti}
    [--sp] = {r3-r0}
    call pa_alnk0_isr_c
    {r3-r0} = [sp++]
    {psr, rets, reti} = [sp++]
    csync
    rti
    .size isr_alnk0, . - isr_alnk0

/*
 * Hardware ISR entry wrapper for TIMER5 (IRQ 63): periodic USB isochronous serviceiment (usb_dev.c pa_timer5_isr_c).
 * Same frame as isr_alnk0.
 */
    .section .text.isr_timer5, "ax"
    .global isr_timer5
    .type isr_timer5, @function
isr_timer5:
    [--sp] = {psr, rets, reti}
    [--sp] = {r3-r0}
    call pa_timer5_isr_c
    {r3-r0} = [sp++]
    {psr, rets, reti} = [sp++]
    csync
    rti
    .size isr_timer5, . - isr_timer5

/*
 * CPU exception entry (vector 1 of the table at 0x01C7FE00).
 *
 * An exception (bad access, stack/PC/write-limit guard, bus error) is delivered through the
 * same vector table as the interrupts, as vector 1. The frame below follows standard pi32v2 exception frame layout; pa_fault_c(frame) in main.c reads it:
 *   f[0..15] r0..r15, f[16] reti (the PC), f[17] rete, f[18] retx, f[19] rets, f[20] psr,
 *   f[21] icfg, f[22] usp, f[23] ssp, f[24] sp
 * It records the fault and resets into the mask-ROM download mode; it never returns.
 */
    .section .text.pa_fault_entry, "ax"
    .global pa_fault_entry
    .type pa_fault_entry, @function
pa_fault_entry:
    cli
    trigger
    [--sp] = {sp, ssp, usp, icfg, psr, rets, retx, rete, reti}
    [--sp] = {r15-r0}
    r0 = sp
    call pa_fault_c
8:
    goto 8b
    .size pa_fault_entry, . - pa_fault_entry

/*
 * Default handlers for every other interrupt vector (bench diagnostics).
 * The bootloader may leave interrupt sources enabled; their vector slots in the app's
 * table at 0x01C7FE00 would otherwise point at stale addresses. Each stub passes its
 * index to pa_default_isr_c(), which records it and disables that source.
 * isr_dflt_table lists the 64 stub addresses for main.c to install.
 */
    .macro PA_DFLT_ISR n
    .section .text.isr_dflt_\n, "ax"
    .global isr_dflt_\n
isr_dflt_\n:
    [--sp] = {psr, rets, reti}
    [--sp] = {r3-r0}
    r0 = \n
    call pa_default_isr_c
    {r3-r0} = [sp++]
    {psr, rets, reti} = [sp++]
    csync
    rti
    .endm

    .irp n, 0,1,2,3,4,5,6,7,8,9,10,11,12,13,14,15,16,17,18,19,20,21,22,23,24,25,26,27,28,29,30,31,32,33,34,35,36,37,38,39,40,41,42,43,44,45,46,47,48,49,50,51,52,53,54,55,56,57,58,59,60,61,62,63
    PA_DFLT_ISR \n
    .endr

    .section .rodata.isr_dflt_table, "a"
    .p2align 2
    .global isr_dflt_table
isr_dflt_table:
    .irp n, 0,1,2,3,4,5,6,7,8,9,10,11,12,13,14,15,16,17,18,19,20,21,22,23,24,25,26,27,28,29,30,31,32,33,34,35,36,37,38,39,40,41,42,43,44,45,46,47,48,49,50,51,52,53,54,55,56,57,58,59,60,61,62,63
    .long isr_dflt_\n
    .endr


/* Core-clock probe (same loop as bench/bench_entry.S): 64 dependent integer adds per iteration, one
 * cycle each, so TIMER4 time over iters*64 cycles gives the clock the app really runs at. */
    .section .text.pa_alu_loop, "ax"
    .p2align 2
    .global pa_alu_loop
    .type pa_alu_loop, @function
pa_alu_loop:
    r1 = 0
    r2 = 1
1:
    .rept 64
    r1 = r1 + r2
    .endr
    if (--r0 != 0) goto 1b
    r0 = r1
    rts
    .size pa_alu_loop, . - pa_alu_loop

/* ---- CPU1 (the second core): entry stubs (see src/board/cpu1.c) ---------------------------------------------------
 * Core 1 starts in the mask ROM, which jumps to the entry point address at 0x01C7FFF8.
 * pa_c1_entry sets the secondary core stacks and branches into RAM code. */
    .section .text.pa_c1_entry, "ax"
    .p2align 2
    .global pa_c1_entry
    .type pa_c1_entry, @function
pa_c1_entry:
    r1 = pa_c1_trace
    r2 = 0xC1000001
    [r1+0] = r2
    usp = _c1_ustack_top
    sp = _c1_sstack_top
    r2 = 0xC1000003                     /* the stacks are set */
    [r1+0] = r2
    reti = pa_c1_land
    rti
    .size pa_c1_entry, . - pa_c1_entry

    .section .c1_text, "ax"
    .p2align 2
    .global pa_c1_entry_ram
    .type pa_c1_entry_ram, @function
pa_c1_entry_ram:
    cli
    r1 = pa_c1_trace
    r2 = 0xC1000001
    [r1+0] = r2
    r2 = 0
    icfg = r2
    usp = _c1_ustack_top
    sp = _c1_sstack_top
    ssp = _c1_sstack_top
    reti = pa_c1_land
    rti
    .size pa_c1_entry_ram, . - pa_c1_entry_ram

    .section .text.pa_c1_land, "ax"
    .p2align 2
    .global pa_c1_land
    .type pa_c1_land, @function
pa_c1_land:                             /* the first code core 1 runs after the stub, from FLASH now: a breadcrumb, then the C main */
    r1 = pa_c1_trace
    r2 = 0xC1000004
    [r1+0] = r2
    r3 = pa_c1_hb                       /* a heartbeat: proves core 1 runs plain flash code before any C */
    r4 = 0
    r5 = 2000
1:
    r4 += 1
    [r3+0] = r4
    r5 += -1
    if (r5 == 0) goto 2f
    goto 1b
2:
    r0 = pa_c1_main
    goto r0
    .size pa_c1_land, . - pa_c1_land


    .section .pool.c1stack, "aw", @nobits
    .p2align 3
    .global _c1_ustack
_c1_ustack:
    .space 8192
    .global _c1_ustack_top
_c1_ustack_top:
    .space 256
    .global _c1_sstack_top
_c1_sstack_top:
