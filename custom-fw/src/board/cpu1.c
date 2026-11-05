/* custom-fw/src/board/cpu1.c - starting and testing the chip's second core (CPU1).
 *
 * The facts come from the open FM-1 firmware (charlesvestal/fm1-x0x, firmware/hal/fm1_cpu1.h, fm1_guard.h, fm1_sys.h;
 * the FM-1 is the same AC791N) and are re-stated here as our own code:
 *   - core 1 control register 0x01EEE004 (JL_CPU1_CTRL in pa_hw.h): bit 3 = enable, bit 1 = hold.
 *     Start: set bit 3, then clear bit 1. Halt: set bit 1, then clear bit 3.
 *   - clock gate: bit 3 of 0x00010008 is set while it starts.
 *   - the entry address goes into the word at 0x01C7FFF8 (top of RAM, behind the CPU0 write-limit window).
 *   - core 1 starts in the mask ROM, so the two PC-limit windows must be open (0x01EEE380..0x01EEE38C).
 *   - its interrupt bank (0x01EEF300 + 4*i) is zeroed first; its stack-limit bank is at 0x01EEF2D0.
 *   - on some units core 1 reads memory wrong when both cores are busy at the bootloader's supply levels
 *     (VDC14 = P33 reg 0x06 bits 2..0, SYSVDD = P33 reg 0x09 bits 3..0); nominal operating voltages are configured.
 *     So jobs carry a check word and every result is verified.
 *
 * This file is only a TEST at boot (see pa_cpu1_selftest, started by a button combination); nothing here runs
 * in normal operation. Results are in SRAM at 0x01C7CC00 (tools/jl_ram_read.py slots). */
#include "pa_config.h"

#if !defined(PA_HOST_BUILD) && !defined(PA_HOST_SIM) && !defined(PA_EMU_DLL) && !defined(__wasm__)
#include <stdint.h>
#include "pa_hw.h"
#include "pa_plat.h"

extern uint8_t  plat_p33_read(uint16_t reg);
extern void     plat_p33_write(uint16_t reg, uint8_t v);
extern uint32_t pa_alu_loop(uint32_t iters);
extern void     pa_c1_entry(void);
extern void     pa_c1_entry_ram(void);
extern uint8_t  _c1_ustack[], _c1_ustack_top[], _c1_sstack_top[];
extern uint32_t _rt_load[], _rt_start[], _rt_end[];

#define C1_CON        (*(volatile uint32_t *)0x01EEE004u)
#define C1_CLK        (*(volatile uint32_t *)0x00010008u)
#define C1_ENTRY_WORD (*(volatile uint32_t *)0x01C7FFF8u)
#define DBG_WR_EN     (*(volatile uint32_t *)0x01EEE240u)
#define C0_WR_LIM_EN  (*(volatile uint32_t *)0x01EEE348u)
#define T4()          (*(volatile uint32_t *)0x00010804u)       /* TIMER4 count, 24 MHz (the firmware's plat_ticks24) */
#define RES           ((volatile uint32_t *)(uintptr_t)0x01C7CC00u)
#define TEST_BUF      ((volatile uint32_t *)(uintptr_t)0x01C58000u)   /* pool region: free at boot, before the DSP pool is used */

volatile uint32_t pa_c1_trace;          /* breadcrumbs written by core 1 */
volatile uint32_t pa_c1_hb;             /* heartbeat counter written by core 1's landing stub */
static volatile uint8_t s_c1_running;
static volatile uint32_t s_job_last, s_job_max;   /* core 1's own timing of the jobs it runs */   /* 1 while core 1 has been started and not halted */

typedef struct {
    volatile uint32_t job, arg, fn, check;
    uint32_t pad0[12];
    volatile uint32_t alive, done, bad, bad_job, bad_done, bad_fn, result, ticks;
    uint32_t pad1[8];
} c1_mailbox_t;
static c1_mailbox_t s_mb __attribute__((aligned(64), section(".pool")));

#define C1_CHECK(fn, arg, job) ((uint32_t)(fn) ^ (uint32_t)(arg) ^ ((uint32_t)(job) * 0x9E3779B1u))

/* ---- core 1's main loop: lives in RAM (.c1_text), runs jobs handed over through the mailbox ---------------------- */
__attribute__((noreturn)) void pa_c1_main(void) {   /* in FLASH (XIP): core 1 could not be entered in the low RAM */
    uint32_t last = 0;
    pa_c1_trace = 0xC1000002u;
    s_mb.alive = 1u;
    for (;;) {
        const uint32_t j = s_mb.job;
        if (j == last) continue;
        const uint32_t fn = s_mb.fn, arg = s_mb.arg, chk = s_mb.check;
        if (j != last + 1u || chk != C1_CHECK(fn, arg, j)) {         /* a misread: refuse, count, move on */
            s_mb.bad_job = j; s_mb.bad_done = last; s_mb.bad_fn = fn; s_mb.bad = s_mb.bad + 1u;
            last = j; s_mb.done = j;
            continue;
        }
        const uint32_t jt0 = T4();
        ((void (*)(uint32_t))(uintptr_t)fn)(arg);
        __asm__ volatile("csync" ::: "memory");
        { const uint32_t jt = T4() - jt0; s_job_last = jt; if (jt > s_job_max) s_job_max = jt; }
        last = j; s_mb.done = j;
    }
}

/* ---- small helpers -------------------------------------------------------------------------------------------- */
static void delay_us(uint32_t us) { const uint32_t t0 = T4(); while ((uint32_t)(T4() - t0) < us * 24u) { } }

static void dbg_unlock(void) { if (!(DBG_WR_EN & 1u)) DBG_WR_EN = 0xE7u; }
static void dbg_lock(void)   { if (DBG_WR_EN & 1u)  DBG_WR_EN = 0xE7u; }

void pa_c1_halt_if_running(void) {
    if (!s_c1_running) return;
    C1_CON |= 0x2u;                 /* hold, then disable */
    C1_CON &= ~0x8u;
    s_c1_running = 0u;
}

static int c1_submit(void (*fn)(uint32_t), uint32_t arg) {
    const uint32_t j = s_mb.job + 1u;
    s_mb.fn = (uint32_t)(uintptr_t)fn; s_mb.arg = arg;
    s_mb.check = C1_CHECK((uint32_t)(uintptr_t)fn, arg, j);
    __asm__ volatile("csync" ::: "memory");
    s_mb.job = j;
    return (int)j;
}
static int c1_wait(uint32_t job, uint32_t ms) {                     /* 0 = done, -1 = timeout */
    const uint32_t t0 = T4();
    while (s_mb.done != job) { if ((uint32_t)(T4() - t0) > ms * 24000u) return -1; }
    return 0;
}
static int c1_wait_us(uint32_t job, uint32_t us) {                  /* 0 = done, -1 = timeout */
    const uint32_t t0 = T4();
    while (s_mb.done != job) { if ((uint32_t)(T4() - t0) > us * 24u) return -1; }
    return 0;
}

/* ---- the test jobs (they run from flash on whichever core is told to) --------------------------------------- */
static void job_alu(uint32_t iters)  { const uint32_t t0 = T4(); (void)pa_alu_loop(iters); s_mb.ticks = T4() - t0; }
static void job_loads(uint32_t n) {                                  /* n sequential word loads from the test buffer */
    uint32_t sum = 0; const uint32_t t0 = T4();
    for (uint32_t i = 0; i < n; i++) sum += TEST_BUF[i];
    s_mb.ticks = T4() - t0; s_mb.result = sum;
}
static void job_sum(uint32_t n) {                                    /* checksum of the first n words (an integrity check) */
    uint32_t sum = 0;
    for (uint32_t i = 0; i < n; i++) sum = sum * 31u + TEST_BUF[i];
    s_mb.result = sum;
}
static uint32_t local_loads(uint32_t n) { uint32_t sum = 0; const uint32_t t0 = T4(); for (uint32_t i = 0; i < n; i++) sum += TEST_BUF[i]; RES[40] = T4() - t0; return sum; }
static uint32_t local_sum(uint32_t n)   { uint32_t sum = 0; for (uint32_t i = 0; i < n; i++) sum = sum * 31u + TEST_BUF[i]; return sum; }

/* ---- the voltage steps (up only, one level per 100 us, VDC14 first: SYSVDD is regulated from it) ---------------- */
__attribute__((unused)) static void volt_raise(uint16_t reg, uint8_t mask, uint8_t target) {
    uint8_t v = (uint8_t)(plat_p33_read(reg) & mask);
    while (v < target) {
        v++;
        plat_p33_write(reg, (uint8_t)((plat_p33_read(reg) & ~mask) | v));
        delay_us(100u);
        v = (uint8_t)(plat_p33_read(reg) & mask);
    }
}


/* ---- production API (used by src/sys/smp_cpu1.c) ---------------------------------------------------------------- */
uint32_t pa_c1_submit(void (*fn)(uint32_t), uint32_t arg) { return (uint32_t)c1_submit(fn, arg); }
int      pa_c1_wait(uint32_t job, uint32_t ms) { return c1_wait(job, ms); }
int      pa_c1_wait_us(uint32_t job, uint32_t us) { return c1_wait_us(job, us); }
uint32_t pa_c1_bad_jobs(void) { return s_mb.bad; }
uint32_t pa_c1_job_ticks_last(void) { return s_job_last; }
uint32_t pa_c1_job_ticks_max(void) { return s_job_max; }
void     pa_c1_job_max_reset(void) { s_job_max = 0; }
/* How many bytes of core 1's user stack have ever been used (the marker words from the bottom up). */
uint32_t pa_c1_stack_used(void) {
    const uint32_t *p = (const uint32_t *)(const void *)_c1_ustack, *e = (const uint32_t *)(const void *)_c1_ustack_top;
    uint32_t free_words = 0;
    while (p < e && *p == 0x43314D4Bu) { p++; free_words++; }
    return (uint32_t)((e - (const uint32_t *)(const void *)_c1_ustack) - free_words) * 4u;
}
uint32_t pa_c1_stack_size(void) { return (uint32_t)(_c1_ustack_top - _c1_ustack); }

/* Start core 1 the way the stock firmware does (verified by pa_cpu1_selftest, variant 0): PC limits open, its interrupt bank
 * zeroed, the entry word pointing at the flash stub, then enable and release. Returns 1 when it answers within 100 ms. */
int pa_c1_start(void) {
    if (s_c1_running) return 1;
    s_mb.job = 0; s_mb.done = 0; s_mb.alive = 0; s_mb.bad = 0;
    s_job_last = 0; s_job_max = 0;
    pa_c1_trace = 0u; pa_c1_hb = 0u;
    for (uint32_t* q = (uint32_t *)(void *)_c1_ustack; q < (uint32_t *)(void *)_c1_ustack_top; q++) *q = 0x43314D4Bu;   /* depth marker */
    dbg_unlock();
    *(volatile uint32_t *)0x01EEE384u = 0u;  *(volatile uint32_t *)0x01EEE380u = 0xFFFFFFFFu;
    *(volatile uint32_t *)0x01EEE38Cu = 0u;  *(volatile uint32_t *)0x01EEE388u = 0xFFFFFFFFu;
    C0_WR_LIM_EN &= ~4u;
    for (uint32_t i = 0; i < 32u; i++) *(volatile uint32_t *)(uintptr_t)(0x01EEF300u + 4u * i) = 0u;
    C1_ENTRY_WORD = (uint32_t)(uintptr_t)pa_c1_entry;
    __asm__ volatile("csync" ::: "memory");
    s_c1_running = 1u;
    C1_CON |= 8u;
    C1_CON &= ~2u;
    { const uint32_t t0 = T4(); while (!s_mb.alive && (uint32_t)(T4() - t0) < 100u * 24000u) { } }
    if (!s_mb.alive) { pa_c1_halt_if_running(); return 0; }
    return 1;
}

/* ---- the whole test ---------------------------------------------------------------------------------------- */
void pa_cpu1_selftest(int want_variant) {
    RES[0] = 0x54533143u;                 /* "C1ST" */
    for (int i = 1; i < 64; i++) RES[i] = 0u;
    RES[1] = 1u;                          /* progress: entered */
    RES[16] = plat_p33_read(0x06) & 7u;   /* VDC14 level before (bootloader: 3 = 1.40 V) */
    RES[17] = plat_p33_read(0x09) & 15u;  /* SYSVDD level before (bootloader: 11 = 1.26 V) */
    RES[18] = C1_CON; RES[19] = C1_CLK;   /* control and clock registers as the bootloader left them */

    /* (the supply levels are already at nominal values on this unit, so nothing is raised) */
    RES[20] = plat_p33_read(0x06) & 7u; RES[21] = plat_p33_read(0x09) & 15u;
    RES[1] = 2u;                          /* progress: voltages done */

    /* fill the test buffer with a known pattern */
    for (uint32_t i = 0; i < 4096u; i++) TEST_BUF[i] = i * 2654435761u + 12345u;
    const uint32_t expect = local_sum(1024u);

    /* guards: open the PC limits (core 1 starts in the ROM), unlock the top of RAM for the entry word */
    dbg_unlock();
    *(volatile uint32_t *)0x01EEE384u = 0u;          *(volatile uint32_t *)0x01EEE380u = 0xFFFFFFFFu;
    *(volatile uint32_t *)0x01EEE38Cu = 0u;          *(volatile uint32_t *)0x01EEE388u = 0xFFFFFFFFu;
    C0_WR_LIM_EN &= ~4u;
    /* core 1's stack-limit bank, in the order the stock firmware uses: clear the enable bit (3), write the limits, set the
     * enable bit, then read everything back. (Our first version wrote the limits first and none of it stuck.) */
    {
        volatile uint32_t *B = (volatile uint32_t *)(uintptr_t)0x01EEF2D0u;
        B[0] &= ~8u;
        B[3] = (uint32_t)(uintptr_t)_c1_ustack;                   /* +12: low limit  */
        B[2] = (uint32_t)(uintptr_t)_c1_sstack_top;               /* +8:  high limit */
        B[5] = 0u;                                                /* +20 */
        B[4] = 0xFFFFFFFFu;                                       /* +16 */
        B[0] |= 8u;
        RES[52] = B[0]; RES[53] = B[2]; RES[54] = B[3]; RES[55] = B[4]; RES[56] = B[5];   /* readback: CON, +8, +12, +16, +20 */
    }
    for (uint32_t i = 0; i < 32u; i++) *(volatile uint32_t *)(uintptr_t)(0x01EEF300u + 4u * i) = 0u;
    for (uint32_t* p = (uint32_t *)(void *)_c1_ustack; p < (uint32_t *)(void *)_c1_ustack_top; p++) *p = 0x43314D4Bu;
    RES[1] = 3u;                          /* progress: guards set */

    {   /* is the RAM image (.c1_text) really at 0x01C00000? compare it with its flash copy */
        const uint32_t n = (uint32_t)(_rt_end - _rt_start);
        uint32_t bad = 0;
        for (uint32_t i = 0; i < n; i++) if (_rt_start[i] != _rt_load[i]) bad++;
        RES[57] = bad; RES[58] = n; RES[59] = _rt_start[0]; RES[60] = _rt_load[0];
    }
    s_mb.job = 0; s_mb.done = 0; s_mb.alive = 0; s_mb.bad = 0;
    pa_c1_trace = 0u;
    const uint32_t clk_saved = C1_CLK;
    RES[30] = *(volatile uint32_t *)0x01EEE384u; RES[31] = *(volatile uint32_t *)0x01EEE380u;   /* PC limit 0 readback */
    RES[32] = *(volatile uint32_t *)0x01EEF2D0u; RES[33] = *(volatile uint32_t *)0x01EEF2D8u;   /* core 1 stack-limit bank readback */
    RES[34] = *(volatile uint32_t *)0x01EEE244u; RES[35] = *(volatile uint32_t *)0x01EEF0D4u;   /* DBG_MSG / EMU_MSG before the start */
    s_c1_running = 1u;
    RES[1] = 4u;                          /* progress: about to release core 1 */

    /* Three start orderings, tried one after another until core 1 answers. All use the stock order for the final steps
     * (entry word, clear the alive flag, enable, release). They differ in what the stock firmware does not do or does:
     *   0: STOCK REPLICA: entry stub in FLASH, no clock-register write                      100 ms
     *   1: flash stub, plus the clock bit (the FM-1 firmware sets it while starting)        100 ms
     *   2: the stub in RAM, no clock write                                                  100 ms */
    int variant_ok = -1;
    for (int variant = want_variant; variant == want_variant && variant_ok < 0; variant++) {
        pa_c1_trace = 0u;
        s_mb.alive = 0u;
        C1_ENTRY_WORD = (variant == 2) ? (uint32_t)(uintptr_t)pa_c1_entry_ram : (uint32_t)(uintptr_t)pa_c1_entry;
        RES[28] = C1_ENTRY_WORD;                                      /* read back AFTER the write: did the entry word stick? */
        RES[29] = (variant == 2) ? (uint32_t)(uintptr_t)pa_c1_entry_ram : (uint32_t)(uintptr_t)pa_c1_entry;
        pa_c1_hb = 0u;
        __asm__ volatile("csync" ::: "memory");
        if (variant == 1) C1_CLK = clk_saved | 8u;
        C1_CON |= 8u;
        C1_CON &= ~2u;
        { const uint32_t t0 = T4(); while (!s_mb.alive && (uint32_t)(T4() - t0) < 100u * 24000u) { } }
        RES[61] = pa_c1_hb;                                           /* heartbeat: 2000 = core 1 ran the whole flash loop */
        RES[36 + variant] = s_mb.alive ? 1u : 0u;                     /* result of this ordering */
        RES[39 + variant] = pa_c1_trace;                              /* its breadcrumb */
        RES[42 + variant] = C1_CON;                                   /* control register right after */
        RES[45 + variant] = *(volatile uint32_t *)0x01EEE244u;        /* DBG_MSG after (core 1 errors show here) */
        RES[48 + variant] = *(volatile uint32_t *)0x01EEF0D4u;        /* EMU_MSG after */
        {   /* core 1's own exception registers (its bank is core 0's + 0x200) and what the debug registers say */
            volatile uint32_t *DG = (volatile uint32_t *)(uintptr_t)0x01C7CF00u;
            DG[0] = 0x44473143u;                                              /* "C1GD" */
            for (uint32_t i = 0; i < 8u; i++) DG[1 + i] = *(volatile uint32_t *)(uintptr_t)(0x01EEF2D0u + 4u * i);   /* core 1 EMU bank */
            DG[9]  = *(volatile uint32_t *)0x01EEF3C0u;                       /* core 1 ETM_CON */
            for (uint32_t i = 0; i < 6u; i++) DG[10 + i] = *(volatile uint32_t *)(uintptr_t)(0x01EEF3C4u + 4u * i);  /* core 1 last branch targets */
            for (uint32_t i = 0; i < 8u; i++) DG[16 + i] = *(volatile uint32_t *)(uintptr_t)(0x01EEE240u + 4u * i);  /* DBG window */
            DG[24] = *(volatile uint32_t *)0x01EEE380u; DG[25] = *(volatile uint32_t *)0x01EEE384u;
            DG[26] = *(volatile uint32_t *)0x01EEE388u; DG[27] = *(volatile uint32_t *)0x01EEE38Cu;
            DG[28] = C1_CON; DG[29] = pa_c1_trace; DG[30] = (uint32_t)variant;
        }
        if (s_mb.alive) variant_ok = variant;
        else { C1_CON |= 2u; C1_CON &= ~8u; C1_CLK = clk_saved; delay_us(2000u); }   /* halt again before the next ordering */
    }
    RES[2] = s_mb.alive; RES[3] = pa_c1_trace;
    C1_CLK = clk_saved;
    if (!s_mb.alive) { pa_c1_halt_if_running(); dbg_lock(); RES[1] = 0xDEAD0001u; return; }
    RES[51] = (uint32_t)variant_ok;
    RES[1] = 5u;                          /* progress: core 1 is alive */

    /* A: the clock of each core (20000 iterations of 64 dependent adds) */
    { int j = c1_submit(job_alu, 20000u); if (c1_wait((uint32_t)j, 50u) == 0) RES[4] = s_mb.ticks; else RES[4] = 0xFFFFFFFFu; }
    { const uint32_t t0 = T4(); (void)pa_alu_loop(20000u); RES[5] = T4() - t0; }
    RES[1] = 6u;
    /* B: 4096 sequential loads, each core alone */
    { int j = c1_submit(job_loads, 4096u); if (c1_wait((uint32_t)j, 50u) == 0) { RES[6] = s_mb.ticks; RES[22] = s_mb.result; } else RES[6] = 0xFFFFFFFFu; }
    { uint32_t s = local_loads(4096u); RES[7] = RES[40]; RES[23] = s; }
    RES[1] = 7u;
    /* C: both at once - core 1 starts, core 0 runs the same loads immediately */
    { int j = c1_submit(job_loads, 4096u); uint32_t s = local_loads(4096u); RES[8] = RES[40]; (void)s;
      if (c1_wait((uint32_t)j, 50u) == 0) RES[9] = s_mb.ticks; else RES[9] = 0xFFFFFFFFu; }
    RES[1] = 8u;
    /* D: integrity under load: 2000 checksum jobs on core 1 while core 0 keeps loading memory */
    { uint32_t wrong = 0, timeouts = 0;
      for (uint32_t it = 0; it < 2000u; it++) {
          int j = c1_submit(job_sum, 1024u);
          (void)local_loads(256u);                                        /* keep core 0 on the memory bus too */
          if (c1_wait((uint32_t)j, 20u) < 0) { timeouts++; break; }
          if (s_mb.result != expect) wrong++;
      }
      RES[10] = s_mb.job; RES[11] = s_mb.done; RES[12] = s_mb.bad; RES[13] = wrong; RES[14] = timeouts; }
    RES[15] = 0xC1D0u;                    /* finished */
    { uint32_t depth = 0; const uint32_t *p = (const uint32_t *)(const void *)_c1_ustack;
      while (p < (const uint32_t *)(const void *)_c1_ustack_top && *p == 0x43314D4Bu) { p++; depth++; }
      RES[24] = (uint32_t)(((const uint32_t *)(const void *)_c1_ustack_top - (const uint32_t *)(const void *)_c1_ustack) - depth) * 4u; }  /* stack bytes used */
    RES[25] = s_mb.bad_job; RES[26] = s_mb.bad_done; RES[27] = s_mb.bad_fn;
    pa_c1_halt_if_running();
    dbg_lock();
    RES[1] = 9u;                          /* progress: done, core 1 halted */
}
#endif
