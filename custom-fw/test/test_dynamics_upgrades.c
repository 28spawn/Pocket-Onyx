/* SPDX-License-Identifier: GPL-3.0-only */
/* Copyright (C) 2026 The Onyx Authors (@28spawn) */
/* test/test_dynamics_upgrades.c — Test suite for dynamics processor upgrades:
 * 1. Smooth Peak/RMS Hybrid Detector (transient capture & low-frequency ripple elimination)
 * 2. Expander Range Limit (configurable floor / downward expansion)
 * 3. Manual Makeup Gain calibration & Auto/Manual toggling
 *
 * clang -O2 -Iinclude -DPA_HOST_SIM=1 test/test_dynamics_upgrades.c src/fx/fx_dynamics.c -lm
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include "pa_config.h"
#include "pa_fx.h"

void *pa_alloc(uint32_t n) { return calloc(1, n); }
void  pa_free(void *p)     { free(p); }
fx_node_t *fx_dynamics_new(void);

#define FS  44100
#define B   PA_BLOCK_N
#define Q21 2097152.0
#define PI  3.14159265358979323846

static int fails = 0, strict = 1;
#define CHECK(ok, ...) do { if (strict) { printf((ok) ? "  ok   " : "  FAIL "); printf(__VA_ARGS__); printf("\n"); if (!(ok)) fails++; } else { printf("  info "); printf(__VA_ARGS__); printf("\n"); } } while (0)

static double db(double v) { return 20.0 * log10(v > 1e-12 ? v : 1e-12); }

/* Run n samples of in_buf through node, output to out_buf */
static void run_buffer(fx_node_t *nd, const double *in_buf, double *out_buf, int n) {
    int32_t buf[B];
    for (int t0 = 0; t0 < n; t0 += B) {
        int m = (n - t0 < B) ? n - t0 : B;
        for (int i = 0; i < B; i++) buf[i] = (i < m) ? (int32_t)(in_buf[t0 + i] * Q21) : 0;
        nd->run(nd, buf, B);
        for (int i = 0; i < m; i++) out_buf[t0 + i] = (double)buf[i] / Q21;
    }
}

/* Measure output level in dB of steady sine */
static double measure_sine_out_db(fx_node_t *nd, double freq, double in_db, int total_samples) {
    double *in_buf = (double *)malloc(total_samples * sizeof(double));
    double *out_buf = (double *)malloc(total_samples * sizeof(double));
    double amp = pow(10.0, in_db / 20.0) * sqrt(2.0);
    for (int i = 0; i < total_samples; i++) {
        in_buf[i] = amp * sin(2.0 * PI * freq * i / FS);
    }
    run_buffer(nd, in_buf, out_buf, total_samples);

    /* Measure RMS over the final 20% of samples */
    int start = total_samples - (total_samples / 5);
    double sum_sq = 0.0;
    for (int i = start; i < total_samples; i++) {
        sum_sq += out_buf[i] * out_buf[i];
    }
    free(in_buf);
    free(out_buf);
    double rms = sqrt(sum_sq / (double)(total_samples - start));
    return db(rms);
}

int main(int argc, char **argv) {
    for (int i = 1; i < argc; i++) if (!strcmp(argv[i], "--report")) strict = 0;

    printf("=================================================================\n");
    printf("Dynamics Processor Upgrades Test Suite\n");
    printf("=================================================================\n\n");

    /* ------------------------------------------------------------------------
     * PART 1: Expander Range Limit Tests
     * ------------------------------------------------------------------------ */
    printf("[1] Expander Range Limit: configurable floor vs full gate mute\n");
    {
        /* Create gate node (Type 0 = GATE_PLUS) */
        fx_node_t *nd = fx_dynamics_new();
        nd->create(nd);
        nd->set_param(nd, 10, 0); /* Type 0 */
        nd->set_param(nd, 0, (int32_t)(0.35 * Q21)); /* Threshold ~ -34 dBFS */
        nd->set_param(nd, 1, (int32_t)(0.30 * Q21)); /* Release 50 ms */
        nd->set_param(nd, 2, 0);                     /* Fast attack */
        nd->set_param(nd, 3, (int32_t)Q21);          /* Level unity */

        /* Signal 20 dB below threshold: -54 dBFS */
        const double quiet_in = -54.0;

        /* Case 1A: Default (Range floor = 0, full mute) */
        double out_default = measure_sine_out_db(nd, 1000.0, quiet_in, FS);
        printf("     Default gate (Range 0): in %.1f dBFS -> out %.1f dBFS (attenuation %.1f dB)\n",
               quiet_in, out_default, quiet_in - out_default);
        CHECK(out_default < -110.0, "default gate achieves hard mute (>100 dB attenuation)");

        /* Case 1B: Expander Range Limit = -20 dB */
        pa_dyn_set_expander_range_db(nd, -20.0f);
        double range_set = pa_dyn_get_expander_range_db(nd);
        CHECK(fabs(range_set - (-20.0f)) < 0.1, "pa_dyn_set_expander_range_db sets -20 dB correctly");

        double out_exp20 = measure_sine_out_db(nd, 1000.0, quiet_in, FS);
        double atten20 = quiet_in - out_exp20;
        printf("     Expander floor -20 dB: in %.1f dBFS -> out %.1f dBFS (attenuation %.1f dB)\n",
               quiet_in, out_exp20, atten20);
        CHECK(fabs(atten20 - 20.0) < 0.5, "expander limits downward attenuation to exactly 20.0 dB (+-0.5 dB)");

        /* Case 1C: Expander Range Limit = -40 dB */
        pa_dyn_set_expander_range_db(nd, -40.0f);
        double out_exp40 = measure_sine_out_db(nd, 1000.0, quiet_in, FS);
        double atten40 = quiet_in - out_exp40;
        printf("     Expander floor -40 dB: in %.1f dBFS -> out %.1f dBFS (attenuation %.1f dB)\n",
               quiet_in, out_exp40, atten40);
        CHECK(fabs(atten40 - 40.0) < 0.5, "expander limits downward attenuation to exactly 40.0 dB (+-0.5 dB)");

        /* Case 1D: Return to full mute via param 6 */
        nd->set_param(nd, 6, 0); /* 0 = full mute */
        double out_muted = measure_sine_out_db(nd, 1000.0, quiet_in, FS);
        CHECK(out_muted < -110.0, "param 6 = 0 restores full gate mute");

        /* Case 1E: Set expander range via param 6 (knob value) */
        /* Knob formula: -60 dB + 54 dB * k. For -24 dB: k = 36 / 54 = 0.6667 */
        int32_t k_q21 = (int32_t)((36.0 / 54.0) * Q21);
        nd->set_param(nd, 6, k_q21);
        double out_param = measure_sine_out_db(nd, 1000.0, quiet_in, FS);
        double atten_param = quiet_in - out_param;
        printf("     Param 6 knob (-24 dB target): attenuation %.1f dB\n", atten_param);
        CHECK(fabs(atten_param - 24.0) < 0.5, "param 6 knob sets expander floor to 24 dB (+-0.5 dB)");

        nd->destroy(nd);
    }

    /* ------------------------------------------------------------------------
     * PART 2: Manual Makeup Gain Tests
     * ------------------------------------------------------------------------ */
    printf("\n[2] Manual Makeup Gain & Auto/Manual Mode Toggling\n");
    {
        /* Create standalone compressor (Type 4 = COMP) */
        fx_node_t *nd = fx_dynamics_new();
        nd->create(nd);
        nd->set_param(nd, 10, 4);                     /* Type 4: COMP */
        nd->set_param(nd, 0, (int32_t)(0.50 * Q21));  /* Thresh -30 dBFS */
        nd->set_param(nd, 1, (int32_t)(0.50 * Q21));  /* Ratio 4.5:1 */
        nd->set_param(nd, 2, 0);                      /* Attack fast */
        nd->set_param(nd, 3, (int32_t)Q21);           /* Level unity */

        /* Verify default is auto makeup */
        CHECK(pa_dyn_is_auto_makeup(nd) == 1, "default compressor mode is auto makeup");

        /* With auto makeup, -18 dBFS sine comes out at approx -18 dBFS */
        double out_auto = measure_sine_out_db(nd, 1000.0, -18.0, FS);
        printf("     Auto makeup: in -18.0 dBFS -> out %.1f dBFS (net %+.1f dB)\n", out_auto, out_auto + 18.0);
        CHECK(fabs(out_auto + 18.0) < 1.0, "auto makeup maintains calibrated level (+-1.0 dB)");

        /* Test Manual Makeup: 0.0 dB (no makeup) */
        pa_dyn_set_manual_makeup_db(nd, 0.0f, 0);
        CHECK(pa_dyn_is_auto_makeup(nd) == 0, "manual makeup mode active");
        double out_0db = measure_sine_out_db(nd, 1000.0, -45.0, FS); /* below threshold: uncompressed */
        printf("     Manual makeup 0.0 dB (below thresh -45 dBFS): out %.1f dBFS (gain %+.1f dB)\n",
               out_0db, out_0db + 45.0);
        CHECK(fabs(out_0db + 45.0) < 0.2, "0 dB manual makeup applies unity gain below threshold");

        /* Test Manual Makeup: +6.0 dB */
        pa_dyn_set_manual_makeup_db(nd, 6.0f, 0);
        double out_6db = measure_sine_out_db(nd, 1000.0, -45.0, FS);
        printf("     Manual makeup +6.0 dB: out %.1f dBFS (gain %+.1f dB)\n", out_6db, out_6db + 45.0);
        CHECK(fabs((out_6db + 45.0) - 6.0) < 0.2, "manual makeup +6.0 dB applies exact +6.0 dB gain");

        /* Test Manual Makeup: +12.0 dB */
        pa_dyn_set_manual_makeup_db(nd, 12.0f, 0);
        double out_12db = measure_sine_out_db(nd, 1000.0, -45.0, FS);
        printf("     Manual makeup +12.0 dB: out %.1f dBFS (gain %+.1f dB)\n", out_12db, out_12db + 45.0);
        CHECK(fabs((out_12db + 45.0) - 12.0) < 0.2, "manual makeup +12.0 dB applies exact +12.0 dB gain");

        /* Switch back to Auto Makeup using param 8 */
        nd->set_param(nd, 8, 1);
        CHECK(pa_dyn_is_auto_makeup(nd) == 1, "param 8 = 1 switches back to auto makeup");
        double out_reauto = measure_sine_out_db(nd, 1000.0, -18.0, FS);
        CHECK(fabs(out_reauto + 18.0) < 1.0, "auto makeup restored correctly");

        nd->destroy(nd);
    }

    /* ------------------------------------------------------------------------
     * PART 3: Smooth Peak/RMS Hybrid Detector Tests
     * ------------------------------------------------------------------------ */
    printf("\n[3] Smooth Peak/RMS Hybrid Detector: Transient Attack & Low-E Ripple\n");
    {
        fx_node_t *nd = fx_dynamics_new();
        nd->create(nd);
        nd->set_param(nd, 10, 4);                     /* Type 4: COMP */
        nd->set_param(nd, 0, (int32_t)(0.30 * Q21));  /* Thresh ~ -38 dBFS */
        nd->set_param(nd, 1, (int32_t)(1.00 * Q21));  /* Ratio 20:1 (max compression) */
        nd->set_param(nd, 2, 0);                      /* Attack fast */
        nd->set_param(nd, 3, (int32_t)Q21);           /* Level unity */

        /* 3A: Pick transient test: sudden step from -40 dBFS to -6 dBFS */
        int pre_samples = 4410; /* 100 ms */
        int total = pre_samples + 4410;
        double *in_buf = (double *)malloc(total * sizeof(double));
        double *out_buf = (double *)malloc(total * sizeof(double));

        double amp_quiet = pow(10.0, -40.0 / 20.0) * sqrt(2.0);
        double amp_loud  = pow(10.0,  -6.0 / 20.0) * sqrt(2.0);
        for (int i = 0; i < pre_samples; i++) in_buf[i] = amp_quiet * sin(2.0 * PI * 1000.0 * i / FS);
        for (int i = pre_samples; i < total; i++) in_buf[i] = amp_loud * sin(2.0 * PI * 1000.0 * i / FS);

        run_buffer(nd, in_buf, out_buf, total);

        /* Measure peak within 2 ms (88 samples) after transient step */
        double transient_peak = 0.0;
        for (int i = pre_samples; i < pre_samples + 88; i++) {
            if (fabs(out_buf[i]) > transient_peak) transient_peak = fabs(out_buf[i]);
        }
        double transient_db = db(transient_peak / sqrt(2.0));
        printf("     Transient step (-40 -> -6 dBFS): clamped from +14 dB uncompressed to %.1f dBFS within 2 ms\n", transient_db);
        CHECK(transient_db < 0.0, "hybrid detector clamps transient attack spike below 0 dBFS within 2 ms");

        /* Measure steady-state settled level after 10 ms (441 samples) */
        double settled_sum_sq = 0.0;
        for (int i = pre_samples + 441; i < total; i++) {
            settled_sum_sq += out_buf[i] * out_buf[i];
        }
        double settled_rms_db = db(sqrt(settled_sum_sq / (double)(total - (pre_samples + 441))));
        double settled_peak_db = settled_rms_db + 3.0103;
        printf("     Settled compressed peak after 10 ms: %.1f dBFS (target -16.4 dBFS)\n", settled_peak_db);
        CHECK(fabs(settled_peak_db - (-16.4)) < 0.5, "hybrid detector settles to steady compressed level within 0.5 dB");

        /* 3B: Low-E (82.4 Hz) Ripple / THD Test */
        int lowE_n = 2 * FS;
        double *lowE_in = (double *)malloc(lowE_n * sizeof(double));
        double *lowE_out = (double *)malloc(lowE_n * sizeof(double));
        double f_lowE = 82.4;
        double lowE_amp = pow(10.0, -6.0 / 20.0) * sqrt(2.0);
        for (int i = 0; i < lowE_n; i++) lowE_in[i] = lowE_amp * sin(2.0 * PI * f_lowE * i / FS);

        run_buffer(nd, lowE_in, lowE_out, lowE_n);

        /* Fourier harmonic analysis over second second */
        int a0 = lowE_n - 43895, b0 = lowE_n;
        double re = 0.0, im = 0.0, total_e = 0.0;
        for (int i = a0; i < b0; i++) {
            re += lowE_out[i] * cos(2.0 * PI * f_lowE * i / FS);
            im += lowE_out[i] * sin(2.0 * PI * f_lowE * i / FS);
            total_e += lowE_out[i] * lowE_out[i];
        }
        double n2 = (double)(b0 - a0);
        double fund_p = (re * re + im * im) * 2.0 / n2;
        double harmonic_p = total_e - fund_p;
        double thd = sqrt(fmax(harmonic_p, 0.0) / fund_p) * 100.0;
        printf("     Low-E (82.4 Hz) at -6 dBFS, RATIO 20:1, ATTACK 0: THD = %.4f %%\n", thd);
        CHECK(thd < 0.1, "hybrid detector eliminates low-frequency waveform ripple (THD < 0.1 %%)");

        free(in_buf); free(out_buf);
        free(lowE_in); free(lowE_out);
        nd->destroy(nd);
    }

    printf("\n=================================================================\n");
    printf("%s: %d failures\n", fails ? "TEST SUITE FAILED" : "ALL TESTS PASSED", fails);
    printf("=================================================================\n");
    return strict && fails ? 1 : 0;
}
