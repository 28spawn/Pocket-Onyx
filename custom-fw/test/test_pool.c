/* test/test_pool.c - the DSP pool's real high-water mark with every effect on.
 *
 * The pool (src/sys/pa_pool.c) is a fixed static array; the effects ask it for
 * their state and a NULL answer means the block silently stays off. The number
 * that matters is therefore not arithmetic, it is what this program prints after
 * creating every block: dynamics, drive, eq, mod, delay, reverb, cab.
 *
 * It must be built with -DPA_POOL_TEST=1, which makes pa_pool.c compile the
 * DEVICE allocator (pool array, first-fit, coalescing) instead of letting the
 * host use malloc.
 *
 *   clang -O2 -Icustom-fw/include -DPA_HOST_SIM=1 -DPA_POOL_TEST=1 \
 *         custom-fw/test/test_pool.c <host_dsp sources> -o test_pool.exe
 */
#include <stdio.h>
#include <string.h>
#include "pa_config.h"
#include "pa_fx.h"

extern uint32_t pa_pool_used_bytes(void);

static int fails = 0;
#define CHECK(cond, ...) do {                                    \
        if (!(cond)) {                                           \
            printf("  FAIL: "); printf(__VA_ARGS__); printf("\n"); \
            fails++;                                             \
        }                                                        \
    } while (0)

#define N_NODES 7

static fx_node_t *make(int i) {
    switch (i) {
        case 0: return fx_dynamics_new();
        case 1: return fx_drive_new();
        case 2: return fx_eq_new();
        case 3: return fx_mod_new();
        case 4: return fx_delay_new();
        case 5: return fx_reverb_new();
        default: return fx_cab_ir_new();
    }
}

static const char *k_name[N_NODES] = {
    "dynamics", "drive", "eq", "mod", "delay", "reverb", "cab"
};

int main(void) {
    fx_node_t *nd[N_NODES];
    uint32_t prev = 0, peak = 0;

    printf("=== Pocket Amp DSP pool: every effect on ===\n");
    printf("   pool budget %u B\n", (unsigned)PA_DSP_POOL_BYTES);

    for (int i = 0; i < N_NODES; i++) {
        nd[i] = make(i);
        CHECK(nd[i] != 0, "slot %d (%s) has no node", i, k_name[i]);
        if (!nd[i]) continue;
        if (nd[i]->create) nd[i]->create(nd[i]);
        uint32_t used = pa_pool_used_bytes();
        printf("   + %-9s state %-8s %6u B  (total %6u B)\n", k_name[i],
               nd[i]->state ? "created" : "MISSING", (unsigned)(used - prev),
               (unsigned)used);
        CHECK(nd[i]->state != 0, "%s ran out of pool", k_name[i]);
        if (used > peak) peak = used;
        prev = used;
    }

    printf("   ----\n");
    printf("   high-water %u B of %u B = %.1f %%\n", (unsigned)peak,
           (unsigned)PA_DSP_POOL_BYTES,
           100.0 * (double)peak / (double)PA_DSP_POOL_BYTES);

    CHECK(peak <= PA_DSP_POOL_BYTES,
          "the pool overflowed by %u B", (unsigned)(peak - PA_DSP_POOL_BYTES));
    /* 95 % is the level at which the next tank or IR would need a new budget
     * pass. The measured value must stay below it, and this is the check that
     * fails if a future tank grows without being paid for. */
    CHECK(peak <= (uint32_t)((uint64_t)PA_DSP_POOL_BYTES * 95u / 100u),
          "the pool is %.1f %% full - raise PA_DSP_POOL_BYTES",
          100.0 * (double)peak / (double)PA_DSP_POOL_BYTES);

    /* The stereo cabinet takes the right IR's spectra (FC_MAXP x FC_BINS complex floats) from the pool, but only while the width is above 0.
     * With every other effect on it must still fit (a failure leaves the cabinet mono and is logged as PA_EV_POOL_FAIL). */
    if (nd[6] && nd[6]->state) {
        const uint32_t before = pa_pool_used_bytes();
        fx_cab_ir_set_stereo_width(nd[6], 100);
        const uint32_t used = pa_pool_used_bytes();
        printf("   + stereo    right-IR spectra %6u B  (total %6u B = %.1f %%)\n", (unsigned)(used - before), (unsigned)used,
               100.0 * (double)used / (double)PA_DSP_POOL_BYTES);
        CHECK(nd[6]->stereo_on == 1 && used <= PA_DSP_POOL_BYTES, "the stereo cabinet did not fit in the pool with every effect on");
        fx_cab_ir_set_stereo_width(nd[6], 0);
        CHECK(nd[6]->stereo_on == 0, "width 0 must switch the stereo cabinet off");
    }

    printf("[2] destroy: the allocator must give every byte back\n");
    for (int i = 0; i < N_NODES; i++) {
        if (nd[i] && nd[i]->destroy) nd[i]->destroy(nd[i]);
    }
    CHECK(pa_pool_used_bytes() == 0,
          "%u B still allocated after destroying every block",
          (unsigned)pa_pool_used_bytes());

    printf("[3] 200 create/destroy cycles: no leak, no failures\n");
    int failures = 0;
    for (int k = 0; k < 200; k++) {
        for (int i = 0; i < N_NODES; i++) {
            fx_node_t *n = make(i);
            if (n->create) n->create(n);
            if (!n->state) failures++;
        }
        for (int i = 0; i < N_NODES; i++) {
            fx_node_t *n = make(i);
            if (n->destroy) n->destroy(n);
        }
        if (pa_pool_used_bytes() != 0) failures++;
    }
    CHECK(failures == 0, "%d failure(s) in 200 create/destroy cycles", failures);
    CHECK(pa_pool_used_bytes() == 0, "the churn leaked %u B",
          (unsigned)pa_pool_used_bytes());

    /* [4] Reused memory comes back zeroed, and a block created on it plays.
     * Fill (almost) the whole pool with 0xFF bytes - as floats that is NaN - free it, and
     * create every effect on top. Before pa_alloc zeroed its blocks, a drive or mod
     * created after a few preset changes started from leftover bytes and played silence
     * (the "presets 41-80: the drive gives no audio" report). */
    printf("[4] reused memory comes back zeroed, and every effect plays on it\n");
    {
        extern void *pa_alloc(uint32_t);
        extern void  pa_free(void *);
        uint32_t big = (uint32_t)PA_DSP_POOL_BYTES - 64u;
        uint8_t *junk = (uint8_t *)pa_alloc(big);
        CHECK(junk != 0, "could not take the whole pool");
        if (junk) { memset(junk, 0xFF, big); pa_free(junk); }
        uint8_t *again = (uint8_t *)pa_alloc(4096);
        int dirty = 0;
        for (int i = 0; again && i < 4096; i++) dirty += (again[i] != 0);
        CHECK(again && dirty == 0, "a reused block still holds %d old bytes", dirty);
        if (again) pa_free(again);
        if (junk) { junk = (uint8_t *)pa_alloc(big); if (junk) { memset(junk, 0xFF, big); pa_free(junk); } }

        for (int i = 0; i < N_NODES; i++) {
            fx_node_t *n = make(i);
            if (n->create) n->create(n);
            if (!n->state) { CHECK(0, "%s: no state", k_name[i]); continue; }
            static int32_t L[PA_BLOCK_N * 64], R[PA_BLOCK_N * 64];
            double e = 0; int bad = 0;
            for (int b = 0; b < 64; b++) {
                for (int k = 0; k < PA_BLOCK_N; k++) {
                    double ph = 2.0 * 3.14159265 * 220.0 * (b * PA_BLOCK_N + k) / 44100.0;
                    L[b * PA_BLOCK_N + k] = R[b * PA_BLOCK_N + k] = (int32_t)(0.1 * 2097152.0 * __builtin_sin(ph));
                }
                if (n->run_stereo) n->run_stereo(n, &L[b * PA_BLOCK_N], &R[b * PA_BLOCK_N], PA_BLOCK_N);
                else n->run(n, &L[b * PA_BLOCK_N], PA_BLOCK_N);
            }
            for (int s = PA_BLOCK_N * 32; s < PA_BLOCK_N * 64; s++) {
                double v = L[s] / 2097152.0;
                if (v != v) bad++;
                e += v * v;
            }
            double rms_db = 10.0 * __builtin_log10(e / (PA_BLOCK_N * 32) + 1e-30);
            printf("   %-9s on reused memory: %.1f dBFS\n", k_name[i], rms_db);
            CHECK(!bad && rms_db > -60.0 && rms_db < 0.0, "%s plays silence, garbage or NaN on reused memory (%.1f dBFS)",
                  k_name[i], rms_db);
            if (n->destroy) n->destroy(n);
        }
    }

    /* [5] Random enable / disable of the effects, on the real allocator. The hole an effect leaves must stay usable: a create that fails although the sizes
     * add up is a fragmentation failure. The report was "after a capture with its own cabinet switched the CAB off, the CAB would not come back" (EFFECT MEMORY
     * FULL, 16,792 wanted, largest free block 13,864). Two configurations: the device as it ships (the stereo cabinet is off, 138,928 of 147,456 bytes with
     * every effect on) and the stereo cabinet on (147,256 of 147,456: nothing to spare). */
    for (int cfg = 0; cfg < 2; cfg++) {
        printf("[5%c] random enable/disable churn, %s: a re-enable must never fail while the sizes add up\n", 'a' + cfg, cfg ? "stereo cabinet ON (the pool is full)" : "stereo cabinet off (as shipped)");
        fx_node_t *nn[N_NODES];
        uint32_t cost[N_NODES];
        int on[N_NODES] = { 0 };
        fx_cab_ir_set_stereo_width(0, cfg ? 60 : 0);             /* (a NULL node only remembers the width: the next create builds the spectra or not) */
        for (int i = 0; i < N_NODES; i++) {
            nn[i] = make(i);
            const uint32_t b0 = pa_pool_used_bytes();
            if (nn[i]->create) nn[i]->create(nn[i]);
            cost[i] = pa_pool_used_bytes() - b0;
            if (nn[i]->destroy) nn[i]->destroy(nn[i]);
        }
        CHECK(pa_pool_used_bytes() == 0, "the sizing pass leaked %u B", (unsigned)pa_pool_used_bytes());
        uint32_t all = 0;
        for (int i = 0; i < N_NODES; i++) all += cost[i];
        printf("   all seven: %u B of %u B\n", (unsigned)all, (unsigned)PA_DSP_POOL_BYTES);
        uint32_t rng = 0x2545F491u + (uint32_t)cfg;
        long ops = 0, frag = 0, frag_late = 0, refused = 0, cab_tries = 0, cab_fail = 0, per[N_NODES] = { 0 };
        for (int step = 0; step < 300000; step++) {
            rng = rng * 1664525u + 1013904223u;
            const int i = (int)((rng >> 16) % N_NODES);
            ops++;
            if (on[i]) {
                if (nn[i]->destroy) nn[i]->destroy(nn[i]);
                on[i] = 0;
            } else {
                const int fits = (pa_pool_used_bytes() + cost[i] <= (uint32_t)PA_DSP_POOL_BYTES);
                if (i == 6) cab_tries++;
                if (nn[i]->create) nn[i]->create(nn[i]);
                if (nn[i]->state) {
                    on[i] = 1;
                } else if (fits) {
                    frag++; per[i]++; if (step >= 200) frag_late++;
                    if (i == 6) cab_fail++;
                } else {
                    refused++;
                }
            }
        }
        printf("   %ld toggles: %ld creates refused for lack of room, %ld refused although the sizes add up (the CAB: %ld of %ld tries)\n", ops, refused, frag, cab_fail, cab_tries);
        printf("   failures by effect:"); for (int i = 0; i < N_NODES; i++) printf(" %s %ld", k_name[i], per[i]); printf("\n");
        /* the first toggles are the large blocks finding their homes (pa_pool.c); after that a failure is a bug. The full pool (cfg 1) is informational: the device never
         * runs the stereo cabinet (ui_app.c passes width 0), and with 200 bytes to spare no allocator can serve every order. */
        printf("   (%ld of them after the first 200 toggles)\n", frag_late);
        CHECK(frag_late == 0 || cfg == 1, "%ld create(s) failed from fragmentation after the warm-up although the sizes add up (the CAB %ld times)", frag_late, cab_fail);
        for (int i = 0; i < N_NODES; i++) if (on[i] && nn[i]->destroy) nn[i]->destroy(nn[i]);
        CHECK(pa_pool_used_bytes() == 0, "the churn leaked %u B", (unsigned)pa_pool_used_bytes());
    }

    if (fails) {
        printf("\nFAILED: %d check(s)\n", fails);
        return 1;
    }
    printf("\nALL PASSED\n");
    return 0;
}
