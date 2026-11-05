/* test/test_fft_conv_mac.c - the firmware's own partitioned convolver (src/fx/fft_conv.c, the register-accumulating multiply-accumulate of pack 185) against a direct time-domain convolution, for the full
 * 1,024-tap cabinet, for the governor's 8-partition limit once the fade has finished, and through the fade (no NaN, no jump). The bench copy in bench/ is a different file: this one tests the firmware's. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include "../include/fft_conv.h"

static int fails;
#define CHECK(ok, ...) do { if (!(ok)) { fails++; printf("  FAIL: " __VA_ARGS__); printf("\n"); } } while (0)

static float rnd(void) { return (float)rand() / (float)RAND_MAX * 2.0f - 1.0f; }
static fc_state_t S;

static double run_vs_direct(const float *ir, int taps_used, int limit, int blocks, int skip)
{
    static float x[64 * 400], y[64];
    double worst = 0.0;
    for (int i = 0; i < blocks * 64; i++) x[i] = 0.3f * rnd();
    fc_set_limit(&S, limit);
    for (int b = 0; b < blocks; b++) {
        fc_block(&S, x + b * 64, y);
        if (b < skip) continue;
        for (int i = 0; i < 64; i++) {
            double ref = 0.0;
            const int n = b * 64 + i;
            for (int t = 0; t < taps_used && t <= n; t++) ref += (double)ir[t] * (double)x[n - t];
            const double e = fabs(ref - (double)y[i]);
            if (e > worst) worst = e;
        }
    }
    return worst;
}

int main(void)
{
    static float ir[1024];
    srand(7);
    fc_tables_init();
    for (int i = 0; i < 1024; i++) ir[i] = rnd() * expf(-(float)i / 300.0f) * 0.2f;

    CHECK(fc_init(&S, ir, 1024) == 0, "init");
    double e16 = run_vs_direct(ir, 1024, 16, 200, 0);
    printf("  16 partitions: max error vs direct convolution %.2e\n", e16);
    CHECK(e16 < 2e-5, "16 partitions differ from the direct convolution by %.2e", e16);

    /* the governor's 8-partition limit: after the 24-block fade the output is the 512-tap convolution */
    CHECK(fc_init(&S, ir, 1024) == 0, "init");
    run_vs_direct(ir, 1024, 16, 100, 100);                      /* run 100 blocks at 16 (the weights snap to 1) */
    double e8 = run_vs_direct(ir, 512, 8, 120, 60);            /* then the limit drops: skip the fade (blocks 0..59), compare the rest */
    printf("  8 partitions after the fade: max error vs the 512-tap convolution %.2e\n", e8);
    CHECK(e8 < 2e-5, "8 partitions differ from the 512-tap convolution by %.2e", e8);

    /* through the fade: finite and no jump larger than the signal itself */
    CHECK(fc_init(&S, ir, 1024) == 0, "init");
    static float x[64 * 80], y[64], prev = 0.0f;
    for (int i = 0; i < 64 * 80; i++) x[i] = 0.3f * sinf(0.05f * (float)i);
    double maxstep = 0.0; int finite = 1;
    for (int b = 0; b < 80; b++) {
        if (b == 30) fc_set_limit(&S, 8);
        if (b == 60) fc_set_limit(&S, 16);
        fc_block(&S, x + b * 64, y);
        for (int i = 0; i < 64; i++) {
            if (!isfinite(y[i])) finite = 0;
            const double st = fabs((double)y[i] - (double)prev);
            if (b > 4 && st > maxstep) maxstep = st;
            prev = y[i];
        }
    }
    printf("  through the fade down and up: largest sample step %.4f\n", maxstep);
    CHECK(finite, "a NaN or infinity in the fade");
    CHECK(maxstep < 0.35, "a jump of %.3f in the fade", maxstep);
    printf("%s\n", fails ? "FFT CONV MAC FAILED" : "FFT CONV MAC OK");
    return fails ? 1 : 0;
}
