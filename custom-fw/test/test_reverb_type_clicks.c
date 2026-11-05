/* test/test_reverb_type_clicks.c - changing the reverb's type while it rings must not click or burst.
 *
 * A reverb tail is smooth (the combs are damped, so neighbouring samples are close): the difference between two neighbouring samples is small next to the level. A switch
 * that puts the old type's signal through the new type's comb lengths, or cuts one engine for another, puts a jump in the output many times larger than those differences.
 * For every pair of types: a note rings into the tank (wet only), the input stops, the type is changed, and the largest neighbour difference after the change is compared
 * with the RMS of the neighbour differences just before it. About 1 for a clean change, several for a click. */
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "pa_config.h"
#include "pa_model.h"
#include "pa_fx.h"

void *pa_alloc(uint32_t n) { return calloc(1, n); }
void  pa_free(void *p)     { free(p); }

static const char *k_name[8] = { "Room", "Hall", "Swell", "Spring", "Shimmer", "Cloud", "Plate", "Ambience" };

static double run_pair(fx_node_t *nd, int a, int b, int in_blocks)
{
    nd->destroy(nd); nd->create(nd);
    nd->set_param(nd, 10, a);
    nd->reset(nd);
    nd->set_param(nd, 3, PA_Q21_ONE);                         /* MIX 100 %: only the reverb is heard */
    nd->set_param(nd, 0, (int32_t)(0.7 * PA_Q21_ONE));
    nd->set_param(nd, 1, 0);
    nd->set_param(nd, 2, (int32_t)(0.3 * PA_Q21_ONE));
    double prev = 0, dsq = 0; long dn = 0;
    int32_t L[64], R[64];
    long blk = 0;
    for (int i = 0; i < in_blocks + 60; i++, blk++) {                    /* the note, then silence; the last 60 blocks (87 ms) are measured before the change */
        for (int k = 0; k < 64; k++) {
            const double t = (double)(blk * 64 + k);
            double x = (i < in_blocks) ? 0.4 * 2097152.0 * sin(2.0 * M_PI * 220.0 / 44100.0 * t) * exp(-t / 9000.0) : 0.0;
            L[k] = R[k] = (int32_t)x;
        }
        nd->run_stereo(nd, L, R, 64);
        if (i >= in_blocks)
            for (int k = 0; k < 64; k++) { const double y = (double)L[k] / 2097152.0; dsq += (y - prev) * (y - prev); dn++; prev = y; }
    }
    const double rms_before = sqrt(dsq / (double)dn);
    nd->set_param(nd, 10, b);                                           /* the change, as the UI makes it */
    double worst = 0;
    for (int i = 0; i < 40; i++, blk++) {                              /* 58 ms: covers the whole fade out and in */
        for (int k = 0; k < 64; k++) L[k] = R[k] = 0;
        nd->run_stereo(nd, L, R, 64);
        for (int k = 0; k < 64; k++) { const double y = (double)L[k] / 2097152.0; const double d = fabs(y - prev); if (d > worst) worst = d; prev = y; }
    }
    return rms_before > 1e-9 ? worst / rms_before : 0.0;
}

int main(void)
{
    fx_node_t *nd = fx_reverb_new();
    nd->create(nd);
    int fails = 0;
    double worst_all = 0;
    for (int a = 0; a < 8; a++)
        for (int b = 0; b < 8; b++) {
            if (a == b) continue;
            const double r = run_pair(nd, a, b, 60);
            if (r > worst_all) worst_all = r;
            if (r > 3.0) { printf("FAIL: %s -> %s: neighbour jump %.1f times the tail's own (must stay under 3)\n", k_name[a], k_name[b], r); fails++; }
        }
    printf("worst change over all 56 pairs: %.2f\n", worst_all);
    /* the pairs the owner heard */
    static const int pairs[][2] = { {0,7}, {0,1}, {1,0}, {2,3}, {3,2}, {3,4}, {4,5} };
    for (unsigned k = 0; k < sizeof pairs / sizeof pairs[0]; k++)
        printf("  %s -> %s: %.2f\n", k_name[pairs[k][0]], k_name[pairs[k][1]], run_pair(nd, pairs[k][0], pairs[k][1], 60));
    printf(fails ? "RESULT: FAILED (%d)\n" : "RESULT: ALL PASSED\n", fails);
    nd->destroy(nd);
    return fails ? 1 : 0;
}
