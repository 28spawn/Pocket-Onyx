/* custom-fw/src/sys/smp_cpu1.c - core 1 runs the front of the chain with the amp (see include/pa_smp.h). Device only. */
#include "pa_smp.h"

#if !defined(PA_HOST_BUILD) && !defined(PA_HOST_SIM) && !defined(PA_EMU_DLL) && !defined(__wasm__)
#include <string.h>
#include "pa_plat.h"

extern uint32_t g_boot_buttons;               /* main.c: the buttons as read at power-on */
extern uint32_t plat_ticks24(void);

#define SMPS ((volatile uint32_t *)(uintptr_t)0x01C7D200u)
/* 0 magic "SMPS", 1 online (1) or 0, 2 hand-overs, 3 core 1's last job (TIMER4 ticks), 4 its longest job, 5 the longest time core 0 waited
 * for core 1 at a hand-over (ticks), 6 why it is off (1 not started, 2 skipped by buttons, 3 start failed, 4 timeout, 5 bad job), 7 times core 0 had to wait at all */

/* SWAP PROBE (pack 158, INSTRUMENTATION ONLY: it changes no behaviour), SRAM 0x01C7D240, 40 words: why does core 1's first amp job after a preset or model swap take ~30 ms
 * (pack 157 bench, 2026-11-03: P01 -> P02 switched core 1 off for good)? One record per swap, the last 6 in a ring.
 *   0 magic "SWPD", 1 swaps seen, 2 next record index, 3 blocks in which core 1 was late (all told; a late block is silent, the job is waited for again)
 *   record (6 words, at 4 + 6 * index):
 *     r0  kind (1 preset, 2 model) | a << 8 | b << 16 | state << 24     a, b = the preset's amp and cab index, or the model's slot and type
 *         state bit 0 recorded, bit 1 the first job after the swap finished in time, bit 2 it was late at least once (waited for again; r3 and r4 are filled in when it ends), bit 3 the wait for core 1 BEFORE the swap timed out
 *     r1  ticks the swap waited for core 1 to go idle        r2  ticks the swap itself took on core 0 (the amp and cab load)
 *     r3  ticks core 0 waited for the first job after the swap (all the waiting, late blocks included)
 *     r4  core 1's own time for that job (when it finished in time)
 *     r5  unused (0)
 * Ticks are TIMER4 at 24 MHz. tools/swap_probe_pc.py prints them. */
#define SWD ((volatile uint32_t *)(uintptr_t)0x01C7D240u)
#define SWD_N 6u
static uint8_t  s_swd_first;                      /* the next job submitted is the first after a swap */
static uint8_t  s_swd_job_first;                  /* the pending job is the first after a swap */
static uint8_t  s_swd_widle_fail;                 /* the wait before the swap timed out */
static uint32_t s_swd_rec;                        /* record of the pending first job */
static uint32_t s_late_run;                       /* blocks in a row in which core 1 was late */
#define SMP_LATE_MAX 64u                          /* ~93 ms of blocks: longer than any slow job seen (30 ms), short of a hang */

typedef struct {
    pa_fx_chain_t *chain;
    volatile int32_t n, from, lo, has_r;
    int32_t in_L[PA_BLOCK_N], in_R[PA_BLOCK_N];     /* core 1's block: its input, then (in place) its result */
    int32_t dry_L[PA_BLOCK_N], dry_R[PA_BLOCK_N];   /* core 1's own crossfade scratch */
} smp_ctx_t;
static smp_ctx_t g __attribute__((section(".pool"), aligned(64)));   /* in the pool region: the 96 KB data region is full */

static uint8_t  s_online, s_pending;
static uint32_t s_job;

static void post_job(uint32_t unused) {
    (void)unused;
    pa_fx_chain_run_front(g.chain, g.in_L, g.has_r ? g.in_R : 0, (int)g.n, (int)g.lo, (int)g.from, g.dry_L, g.dry_R);
    __asm__ volatile("csync" ::: "memory");
}

/* The first few blocks after core 1 starts include the one-off start-up work (the accelerator self-tests, the model set-up): 5.4 ms measured on the
 * unit, and a 17 ms probe killed core 1 at boot once. Until core 1 has finished a few jobs in time, wait up to 12 ms instead of 6. */
static uint32_t s_ok_jobs;
#define SMP_WAIT_MS() (s_ok_jobs < 4u ? 12u : 6u)

void pa_smp_swap_note(uint32_t kind, uint32_t a, uint32_t b, uint32_t wait_ticks, uint32_t apply_ticks) {   /* overrides the weak one in pa_audio_core.c */
    const uint32_t idx = SWD[2] % SWD_N;
    volatile uint32_t *r = &SWD[4u + 6u * idx];
    r[0] = (kind & 0xFFu) | (a & 0xFFu) << 8 | (b & 0xFFu) << 16 | (1u | (s_swd_widle_fail ? 8u : 0u)) << 24;
    r[1] = wait_ticks; r[2] = apply_ticks; r[3] = 0u; r[4] = 0u; r[5] = 0u;
    s_swd_widle_fail = 0u;
    SWD[1] = SWD[1] + 1u;
    SWD[2] = (idx + 1u) % SWD_N;
    s_swd_first = 1u;
}

static void smp_fail(uint32_t why) {
    pa_fx_chain_set_post_handoff(0);              /* from the next block on, everything runs on core 0 again */
    pa_c1_halt_if_running();
    s_online = 0; s_pending = 0;
    SMPS[1] = 0u; SMPS[6] = why;
}

static void handoff(pa_fx_chain_t *chain, int32_t *L, int32_t *R, int n, int from, int lo) {
    if (s_pending) {
        const uint32_t t0 = plat_ticks24();
        /* A late block is not a dead core. Steady state: core 1 finishes inside the block (core 0 waits 5 us at most), so 1.2 ms is slack; the first 4 jobs after the start keep the
         * old 12 ms (the one-off self-test and set-up, 5.4 ms measured). A job that is late is waited for again at the next block, with this block silent. If it is still not done
         * after SMP_LATE_MAX blocks in a row (~93 ms: a real hang, not a slow job) the old rule applies: core 1 is switched off and the work runs on core 0. */
        if (pa_c1_wait_us(s_job, s_ok_jobs < 4u ? 12000u : 1200u) != 0) {
            const uint32_t tr = plat_ticks24();
            if (s_swd_job_first) {
                volatile uint32_t *r = &SWD[4u + 6u * s_swd_rec];
                r[3] = r[3] + (tr - t0); r[0] = r[0] | (4u << 24);                   /* state bit 2: the first job after the swap was late (kept pending, measured when it ends) */
            }
            SWD[3] = SWD[3] + 1u;                                                     /* late blocks, all told */
            if (++s_late_run >= SMP_LATE_MAX) {
                smp_fail(4u);
                pa_fx_chain_run_front(chain, L, R, n, lo, from, g.dry_L, g.dry_R);   /* this block's front runs here, with no latency */
                return;
            }
            for (int i = 0; i < n; i++) { L[i] = 0; if (R) R[i] = 0; }
            return;
        }
        s_late_run = 0u;
        s_pending = 0;
        if (s_ok_jobs < 1000u) s_ok_jobs++;
        const uint32_t w = plat_ticks24() - t0;
        if (s_swd_job_first) {
            volatile uint32_t *r = &SWD[4u + 6u * s_swd_rec];
            r[3] = r[3] + w; r[4] = pa_c1_job_ticks_last(); r[0] = r[0] | (2u << 24);   /* state bit 1: the first job after the swap has finished (r3 = all the waiting, r4 = core 1's own time) */
            s_swd_job_first = 0u;
        }
        if (w > 48u) SMPS[7] = SMPS[7] + 1u;                      /* core 0 really had to wait (more than 2 us) */
        if (w > SMPS[5]) SMPS[5] = w;
    }
    for (int i = 0; i < n; i++) {                                 /* swap: core 0 gets the finished block, core 1 the new one */
        const int32_t tl = g.in_L[i]; g.in_L[i] = L[i]; L[i] = tl;
        if (R) { const int32_t tr = g.in_R[i]; g.in_R[i] = R[i]; R[i] = tr; }
    }
    g.chain = chain; g.n = n; g.from = from; g.lo = lo; g.has_r = (R != 0);
    s_job = pa_c1_submit(post_job, 0u);
    s_pending = 1u;
    if (s_swd_first) { s_swd_job_first = 1u; s_swd_first = 0u; s_swd_rec = (SWD[2] + SWD_N - 1u) % SWD_N; }
    SMPS[2] = SMPS[2] + 1u;
    SMPS[3] = pa_c1_job_ticks_last(); SMPS[4] = pa_c1_job_ticks_max();
}

void pa_smp_wait_idle(void) {                                     /* overrides the weak one in pa_audio_core.c */
    if (!s_online) return;
    if (s_pending) {
        if (pa_c1_wait(s_job, SMP_WAIT_MS()) != 0) { s_swd_widle_fail = 1u; smp_fail(4u); return; }
        s_pending = 0;
    }
    memset(g.in_L, 0, sizeof g.in_L);                             /* the block in flight belongs to the old chain: drop it */
    memset(g.in_R, 0, sizeof g.in_R);
}

void pa_smp_init(pa_fx_chain_t *chain) {
    SMPS[0] = 0x53504D53u; SMPS[1] = 0u; SMPS[2] = 0u; SMPS[3] = 0u; SMPS[4] = 0u; SMPS[5] = 0u; SMPS[6] = 1u; SMPS[7] = 0u;
    s_online = 0; s_pending = 0; s_job = 0; s_ok_jobs = 0;
    for (uint32_t i = 0; i < 4u + 6u * SWD_N; i++) SWD[i] = 0u;
    SWD[0] = 0x44575053u;                                           /* "SWPD" */
    s_swd_first = 0u; s_swd_job_first = 0u; s_swd_widle_fail = 0u; s_swd_rec = 0u; s_late_run = 0u;
    g.chain = chain;
    memset(g.in_L, 0, sizeof g.in_L); memset(g.in_R, 0, sizeof g.in_R);
#if PA_SMP_AUDIO
    if ((g_boot_buttons & 0x10u) || (g_boot_buttons & 0x07u) == 0x07u) { SMPS[6] = 2u; return; }   /* ">" held = one core; the core-1 test combination */
    if (!pa_c1_start()) { SMPS[6] = 3u; return; }
    s_online = 1u; SMPS[1] = 1u; SMPS[6] = 0u;
    pa_fx_chain_set_post_handoff(handoff);
#endif
}

int pa_smp_is_online(void) { return (int)s_online; }

void pa_smp_update_demand(pa_fx_chain_t *chain) {
    (void)chain;
    if (s_online && pa_c1_bad_jobs() != 0u) smp_fail(5u);        /* core 1 refused a job: its memory reads are wrong */
    if (s_online) {                                                /* about once a second: how deep has core 1's stack been? */
        static uint32_t s_n;
        if ((++s_n & 63u) == 0u) {
            const uint32_t used = pa_c1_stack_used();
            SMPS[8] = used; SMPS[9] = pa_c1_stack_size();
            if (used + 256u >= pa_c1_stack_size()) smp_fail(6u);    /* nearly overflowed: switch core 1 off before it corrupts anything */
        }
    }
}

uint32_t pa_smp_cpu1_load_permille(void) {
    const uint32_t budget = 34800u;                               /* TIMER4 ticks in one 64-sample block */
    uint32_t p = (pa_c1_job_ticks_last() * 1000u) / budget;
    if (p > 1000u) p = 1000u;
    static int32_t s_ema;                                          /* the last job alone jumps by 5-10 % from one block to the next: a steady figure for the screen */
    s_ema = (s_ema == 0) ? (int32_t)p : s_ema + (((int32_t)p - s_ema) >> 4);
    return (uint32_t)s_ema;
}
#endif
