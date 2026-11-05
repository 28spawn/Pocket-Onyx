/* Host test: the three float GRU steps against the original Q21 step, same weights, same input:
 *   nam_step_float   CPU only
 *   nam_step_hw      all 16 rows on the (emulated) accelerator
 *   nam_step_hybrid  accelerator rows 0..8 + CPU rows 9..15 (what the device runs)
 * The float steps keep one global GRU state, so each engine runs in its own pass (pa_nam_reset between passes).
 * Prints the RMS and peak difference relative to the output level. Exit 1 if a realistic-weight error is large. */
#define PA_HOST_SIM 1
#include "../src/nam/nam_engine.c"
#include <stdio.h>
#include <stdlib.h>
#include <math.h>

static uint32_t rng = 12345u;
static float frand(void) { rng = rng * 1664525u + 1013904223u; return ((float)(rng >> 8) / 8388608.0f) - 1.0f; }

#define N 44100
static int32_t xs[N], yq[N], yv[N];

static void report(const char *name, const int32_t *y, double rms_y, int *fail, float scale) {
    double se = 0, pk = 0;
    for (int n = 0; n < N; n++) {
        double d = (double)(y[n] - yq[n]) / 2097152.0;
        se += d * d; if (fabs(d) > pk) pk = fabs(d);
    }
    double rms_err = sqrt(se / N);
    double rel = rms_y > 0 ? 100.0 * rms_err / rms_y : 0.0;
    printf("  %-34s error rms %.6f (%.3f%% of output), peak %.5f\n", name, rms_err, rel, pk);
    if (scale <= 0.6f && rms_y > 1e-4 && rel > 1.0) *fail = 1;      /* chaotic hot networks are not held to this */
}

int main(int argc, char **argv) {
    static pa_nam_model_t a, b;
    const float scale = (argc > 1) ? (float)atof(argv[1]) : 0.18f;
    pa_nam_init_default(&a);
    int32_t *w = (int32_t *)&a;                     /* the weights occupy the start of the struct */
    for (size_t i = 0; i < offsetof(pa_nam_model_t, r_gate) / 4; i++) w[i] = (int32_t)(frand() * scale * 2097152.0f);
    for (int i = 0; i < NF_H; i++) {
        a.b_r[i] = (int32_t)(frand() * 0.3f * 2097152.0f); a.b_z[i] = (int32_t)(frand() * 0.3f * 2097152.0f);
        a.b_ni[i] = (int32_t)(frand() * 0.1f * 2097152.0f); a.b_nh[i] = (int32_t)(frand() * 0.1f * 2097152.0f);
    }
    a.b_out = 0; a.scale_q21 = 1 << PA_NAM_Q_SHIFT; a.active = 1;
    for (int n = 0; n < N; n++) {
        float x = 0.8f * sinf(2.0f * 3.14159265f * 196.0f * n / 44100.0f) * (0.5f + 0.5f * sinf(n * 0.0003f));
        xs[n] = (int32_t)(x * 2097152.0f);
    }
    int fail = 0;

    b = a; pa_nam_reset(&b);                                         /* the reference */
    double sy = 0;
    for (int n = 0; n < N; n++) { yq[n] = nam_step_q21(&b, xs[n]); sy += ((double)yq[n] / 2097152.0) * ((double)yq[n] / 2097152.0); }
    const double rms_y = sqrt(sy / N);
    printf("weight scale %.2f: output rms %.5f\n", scale, rms_y);
    printf("  accelerator self-test (host software path): %s\n", nam_hw_selftest() ? "passed" : "FAILED");

    b = a; nam_float_sync(&b); pa_nam_reset(&b);
    for (int n = 0; n < N; n++) yv[n] = nam_step_float(&b, xs[n]);
    report("CPU float", yv, rms_y, &fail, scale);

    b = a; pa_nam_reset(&b);
    for (int n = 0; n < N; n++) yv[n] = nam_step_hw(&b, xs[n]);
    report("all rows on the accelerator", yv, rms_y, &fail, scale);

    b = a; pa_nam_reset(&b);
    for (int n = 0; n < N; n++) yv[n] = nam_step_hybrid(&b, xs[n]);
    report("hybrid (accel 0..8 + CPU 9..15)", yv, rms_y, &fail, scale);

    return fail;
}
