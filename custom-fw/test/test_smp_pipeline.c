/*
 * test/test_smp_pipeline.c - the two-core pipeline's logic, with a synchronous stand-in for core 1.
 *
 * With the hook installed, pa_fx_chain_run_stereo gives each block to the hook, which runs the front of the chain (input
 * conditioning and the slots up to the amp) and returns the PREVIOUS block's result; the caller then runs the slots after the amp
 * and the tail. Run on the same input, the pipelined output for block k must be bit-for-bit the single-core output for block k-1,
 * and block 0 must be silent. That proves the split, the swap and the carried state (stereo, dry scratch) do not change the
 * audio, only delay it by one block.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include "pa_config.h"
#include "pa_cab.h"
#include "pa_fx.h"

void *pa_alloc(uint32_t n) { return calloc(1, n); }
void  pa_free(void *p)     { free(p); }

#define K 48
static int32_t out_single[K][2][PA_BLOCK_N], out_pipe[K][2][PA_BLOCK_N], in_blk[K][PA_BLOCK_N];

/* the stand-in for core 1: its own block buffers and its own dry scratch, run synchronously */
static int32_t c1_L[PA_BLOCK_N], c1_R[PA_BLOCK_N], c1_dryL[PA_BLOCK_N], c1_dryR[PA_BLOCK_N];
static void hook(pa_fx_chain_t *chain, int32_t *L, int32_t *R, int n, int to, int lo) {
    for (int i = 0; i < n; i++) {                       /* swap: the caller gets the finished block, "core 1" gets the new one */
        int32_t t = c1_L[i]; c1_L[i] = L[i]; L[i] = t;
        if (R) { t = c1_R[i]; c1_R[i] = R[i]; R[i] = t; }
    }
    /* "core 1" runs the front on its buffer. In the device the swap above happens after core 1 finished the previous block, so the
     * buffer holds the previous block's front output; here the front of the NEW block runs after the swap, as on the device. */
    pa_fx_chain_run_front(chain, c1_L, R ? c1_R : 0, n, lo, to, c1_dryL, c1_dryR);
}

static void feed(int32_t *L, int32_t *R, int k) {
    for (int i = 0; i < PA_BLOCK_N; i++) { L[i] = in_blk[k][i]; R[i] = in_blk[k][i]; }
}

static int run_case(const char *label, uint32_t budget_us, int drive_on) {
    int fails = 0;
    pa_fx_chain_set_core1_budget_us(budget_us);
    printf("--- %s ---\n", label);
    srand(7);
    for (int k = 0; k < K; k++)
        for (int i = 0; i < PA_BLOCK_N; i++)
            in_blk[k][i] = (int32_t)(sin(2.0 * 3.14159265 * 330.0 * (k * PA_BLOCK_N + i) / 44100.0) * (PA_Q21_ONE / 4))
                         + (rand() % 20000) - 10000;

    {   /* single core */
        pa_fx_chain_t chain;
        pa_fx_chain_init(&chain, 0, fx_cab_ir_new());
        if (drive_on) pa_fx_chain_set_slot_enabled_now(&chain, PA_SLOT_DRIVE, 1);
        for (int k = 0; k < K; k++) {
            feed(out_single[k][0], out_single[k][1], k);
            pa_fx_chain_run_stereo(&chain, out_single[k][0], out_single[k][1], PA_BLOCK_N);
        }
    }
    {   /* pipelined (a fresh chain, a fresh cabinet: the convolver state is a singleton) */
        pa_fx_chain_t chain;
        pa_fx_chain_init(&chain, 0, fx_cab_ir_new());
        if (drive_on) pa_fx_chain_set_slot_enabled_now(&chain, PA_SLOT_DRIVE, 1);
        memset(c1_L, 0, sizeof c1_L); memset(c1_R, 0, sizeof c1_R);
        pa_fx_chain_set_post_handoff(hook);
        for (int k = 0; k < K; k++) {
            feed(out_pipe[k][0], out_pipe[k][1], k);
            pa_fx_chain_run_stereo(&chain, out_pipe[k][0], out_pipe[k][1], PA_BLOCK_N);
        }
        pa_fx_chain_set_post_handoff(0);
    }

    int silent0 = 1, mismatches = 0, nonzero = 0;
    for (int i = 0; i < PA_BLOCK_N; i++) if (out_pipe[0][0][i] || out_pipe[0][1][i]) silent0 = 0;
    for (int k = 1; k < K; k++)
        for (int ch = 0; ch < 2; ch++)
            for (int i = 0; i < PA_BLOCK_N; i++) {
                if (out_pipe[k][ch][i] != out_single[k - 1][ch][i]) mismatches++;
                if (out_single[k - 1][ch][i]) nonzero++;
            }
    printf("  block 0 of the pipeline is silent: %s\n", silent0 ? "yes" : "NO");
    printf("  pipelined block k vs single-core block k-1: %d mismatching samples of %d (%d non-zero)\n",
           mismatches, (K - 1) * 2 * PA_BLOCK_N, nonzero);
    if (!silent0) fails++;
    if (mismatches) fails++;
    if (nonzero < 100) { printf("  FAIL: the test signal produced (almost) no output\n"); fails++; }
    return fails;
}

int main(void) {
    int fails = 0;
    fails += run_case("amp and everything in front on core 1 (budget 1300 us)", 1300u, 0);
    fails += run_case("drive in front of the amp, run on core 0 (budget 1300 us)", 1300u, 1);
    fails += run_case("the amp alone on core 1 (budget 1 us), drive on", 1u, 1);
    printf(fails ? "RESULT: FAILED\n" : "RESULT: ALL PASSED\n");
    return fails ? 1 : 0;
}
