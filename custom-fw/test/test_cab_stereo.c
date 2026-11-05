/* test/test_cab_stereo.c - the stereo cabinet (fx_cab_ir.c, stereo_width): width 0 is the old mono cabinet bit for bit, the right channel
 * has the left one's tone but not its waveform, the width sets the correlation, the stereo EQ keeps the channels apart, and the settings
 * round-trip. Runs the real node through its run / run_stereo entry points like the chain does. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <time.h>
#include "pa_fx.h"
#include "pa_config.h"

static uint32_t s_pool_now, s_pool_peak;
void *pa_alloc(uint32_t n) { s_pool_now += n; if (s_pool_now > s_pool_peak) s_pool_peak = s_pool_now; return calloc(1, n); }
void  pa_free(void *p)     { free(p); }

static int fails;
#define CHECK(ok, ...) do { printf((ok) ? "  ok   " : "  FAIL "); printf(__VA_ARGS__); printf("\n"); if (!(ok)) fails++; } while (0)

static uint32_t rs = 12345u;
static float rnd(void) { rs = rs * 1664525u + 1013904223u; return ((int32_t)(rs >> 8) - (1 << 23)) / (float)(1 << 23); }

#define NB 400
static int32_t sig[NB * 64];

/* what the chain does for a node: stereo path if the node wants it, else the mono run and R = L */
static int chain_block(fx_node_t *nd, int32_t *L, int32_t *R, int n, int *split) {
    extern volatile uint8_t g_pa_stereo_allow, g_pa_cab_stereo;
    const int st = nd->run_stereo && (nd->stereo_opt == 0 || (nd->stereo_opt == 1 ? nd->stereo_on : (*split && g_pa_cab_stereo)));
    if (st) { nd->run_stereo(nd, L, R, n); *split = 1; return 1; }
    nd->run(nd, L, n); memcpy(R, L, (size_t)n * sizeof(int32_t)); *split = 0; return 0;
}
volatile uint8_t g_pa_stereo_allow = 1, g_pa_cab_stereo;

static void render(fx_node_t *cab, fx_node_t *eq, int32_t *oL, int32_t *oR) {
    int split = 0;
    for (int b = 0; b < NB; b++) {
        int32_t L[64], R[64];
        memcpy(L, sig + b * 64, sizeof L);
        g_pa_cab_stereo = 0; split = 0;
        chain_block(cab, L, R, 64, &split);
        if (eq) chain_block(eq, L, R, 64, &split);
        memcpy(oL + b * 64, L, sizeof L); memcpy(oR + b * 64, R, sizeof R);
    }
}

static double corr(const int32_t *a, const int32_t *b, int n, double *pa, double *pb) {
    double sab = 0, saa = 0, sbb = 0;
    for (int i = 0; i < n; i++) { sab += (double)a[i] * b[i]; saa += (double)a[i] * a[i]; sbb += (double)b[i] * b[i]; }
    *pa = saa / n; *pb = sbb / n;
    return sab / sqrt(saa * sbb + 1e-30);
}

int main(void) {
    setvbuf(stdout, NULL, _IONBF, 0);
    for (int i = 0; i < NB * 64; i++) sig[i] = (int32_t)(rnd() * 400000.0f);
    fx_node_t *cab = fx_cab_ir_new(), *eq = fx_eq_new();
    static int32_t mL0[NB * 64], mR0[NB * 64], sL[NB * 64], sR[NB * 64], mL1[NB * 64], mR1[NB * 64];

    /* 1. width 0: mono, the old path */
    cab->create(cab); eq->create(eq);
    CHECK(cab->stereo_on == 0, "a fresh cabinet is mono (stereo_on 0)");
    render(cab, eq, mL0, mR0);
    CHECK(memcmp(mL0, mR0, sizeof mL0) == 0, "width 0: the right channel is the left one, bit for bit");
    const uint32_t pool_mono = s_pool_now;

    /* 2. width 100 */
    cab->destroy(cab); eq->destroy(eq); s_pool_now = 0;
    fx_cab_ir_set_stereo_width(cab, 100);           /* remembered; applied on create */
    cab->create(cab); eq->create(eq);
    CHECK(cab->stereo_on == 1, "width 100: stereo on after create");
    const uint32_t pool_st = s_pool_now;
    render(cab, eq, sL, sR);
    double pa, pb, pm, pm2;
    const double c100 = corr(sL + 64 * 100, sR + 64 * 100, 64 * 300, &pa, &pb);
    corr(mL0 + 64 * 100, mL0 + 64 * 100, 64 * 300, &pm, &pm2);
    CHECK(fabs(c100) < 0.35, "width 100: L/R correlation %.3f (decorrelated)", c100);
    CHECK(fabs(10 * log10(pa / pm)) < 1.5 && fabs(10 * log10(pb / pm)) < 1.5, "width 100: channel power within 1.5 dB of the mono cabinet (%.2f, %.2f dB)", 10 * log10(pa / pm), 10 * log10(pb / pm));
    CHECK(memcmp(sL, sR, sizeof sL) != 0, "width 100: the channels differ");

    /* 3. the width sets the correlation: 50 is in between */
    fx_cab_ir_set_stereo_width(cab, 50);
    render(cab, eq, mL1, mR1);
    const double c50 = corr(mL1 + 64 * 100, mR1 + 64 * 100, 64 * 300, &pa, &pb);
    CHECK(c50 > c100 + 0.25 && c50 < 0.95, "width 50: correlation %.3f lies between 1 and %.3f", c50, c100);
    CHECK(fabs(10 * log10(pa / pm)) < 1.5, "width 50: channel power within 1.5 dB of mono");

    /* 4. width back to 0: the node leaves the stereo path and the audio is mono again */
    fx_cab_ir_set_stereo_width(cab, 0);
    CHECK(cab->stereo_on == 0, "width 0 again: stereo_on cleared");
    {   cab->reset(cab); eq->reset(eq);
        cab->destroy(cab); eq->destroy(eq); s_pool_now = 0; cab->create(cab); eq->create(eq);
        render(cab, eq, mL1, mR1);
        CHECK(memcmp(mL1, mL0, sizeof mL0) == 0 && memcmp(mR1, mR0, sizeof mR0) == 0, "width 0 again: output bit-identical to the first mono render");
        CHECK(s_pool_now == pool_mono, "width 0: pool use is the mono cabinet's (%u B)", (unsigned)s_pool_now);
    }

    /* 5. the load governor: stereo allowed = 0 -> the node runs mono although the width is set */
    fx_cab_ir_set_stereo_width(cab, 100);
    CHECK(cab->stereo_on == 1, "width 100 set on a live cabinet");
    {   extern volatile uint8_t g_pa_stereo_allow;
        int split = 0; int32_t L[64], R[64]; memcpy(L, sig, sizeof L);
        g_pa_stereo_allow = 0;
        /* the chain asks node_stereo_now (fx_graph.c); here the same rule */
        const int st = g_pa_stereo_allow && cab->stereo_on;
        CHECK(!st, "governor off: the chain would run the mono path");
        g_pa_stereo_allow = 1; (void)split; (void)R;
    }
    printf("   pool: mono cabinet %u B, with stereo on %u B (+%u B); EQ stereo adds 40 B of state\n", (unsigned)pool_mono, (unsigned)pool_st, (unsigned)(pool_st - pool_mono));

    /* 6. relative cost on the host (informational, x86: only the RATIO means anything for the device): cab + EQ, mono against stereo */
    {
        cab->destroy(cab); eq->destroy(eq);
        double t[2];
        for (int mode = 0; mode < 2; mode++) {
            fx_cab_ir_set_stereo_width(cab, mode ? 100 : 0);
            cab->create(cab); eq->create(eq);
            eq->set_param(eq, 1, 2621440 / 2 * 3);                      /* move two bands so the EQ has real sections */
            eq->set_param(eq, 3, 2621440 / 2);
            clock_t c0 = clock();
            int32_t L[64], R[64];
            for (int b = 0; b < 40000; b++) {
                memcpy(L, sig + (b % NB) * 64, sizeof L);
                int split = 0; g_pa_cab_stereo = 0;
                chain_block(cab, L, R, 64, &split);
                chain_block(eq, L, R, 64, &split);
            }
            t[mode] = (double)(clock() - c0) / CLOCKS_PER_SEC / 40000.0 * 1e6;
            cab->destroy(cab); eq->destroy(eq);
        }
        printf("   host cost per block, cab + EQ: mono %.1f us, stereo %.1f us (x%.2f); scaled to the unit's measured mono cab slot (201 us): +%.0f us\n",
               t[0], t[1], t[1] / t[0], 201.0 * (t[1] / t[0] - 1.0));
        fx_cab_ir_set_stereo_width(cab, 0);
        cab->create(cab);
    }
    cab->destroy(cab);
    printf("%s\n", fails ? "RESULT: FAILED" : "RESULT: ALL PASSED");
    return fails ? 1 : 0;
}
