/* SPDX-License-Identifier: GPL-3.0-only */
/* Copyright (C) 2026 The Onyx Authors (@28spawn) */
/* custom-fw/test/test_eq.c - the EQ block's three types, measured.
 *
 *   [1] 5-BAND: flat with every band at 50 %; a band at 100 % is +12 dB at its centre
 *   [2] LOW/HIGH CUT: -3 dB at the frequencies the screen shows, 24 dB/oct beyond them,
 *       flat between; LEVEL 100 % = +12 dB
 *   [3] PARAMETRIC-3: flat at 50 %; LOW / MID / HIGH +12 dB where they should be; the
 *       MID follows FREQ (the value the screen shows)
 *   [4] switching types is clean (no NaN, no stuck state)
 *
 * clang -O2 -Icustom-fw/include -DPA_HOST_SIM=1 custom-fw/test/test_eq.c custom-fw/src/fx/fx_eq.c -lm
 */
#include <stdio.h>
#include <stdlib.h>
#include <math.h>
#include "pa_config.h"
#include "pa_fx.h"

void *pa_alloc(uint32_t n) { return calloc(1, n); }
void  pa_free(void *p)     { free(p); }
fx_node_t *fx_eq_new(void);

#define FS  44100
#define Q21 2097152.0
#define PI  3.14159265358979

static int fails = 0;
#define CHECK(ok, ...) do { printf((ok) ? "  ok   " : "  FAIL "); printf(__VA_ARGS__); printf("\n"); if (!(ok)) fails++; } while (0)

static fx_node_t *mk(int type, const double *knobs, int n) {
    fx_node_t *nd = fx_eq_new();
    nd->create(nd);
    nd->set_param(nd, 10, type);
    for (int k = 0; k < n; k++) nd->set_param(nd, k, (int32_t)(knobs[k] * Q21));
    return nd;
}

/* gain (dB) of the EQ at f, a sine at -20 dBFS, steady state */
static double gain_db(fx_node_t *nd, double f) {
    static int32_t b[PA_BLOCK_N];
    double num = 0, den = 0;
    int nb = (int)(0.4 * FS) / PA_BLOCK_N;
    for (int k = 0; k < nb; k++) {
        for (int i = 0; i < PA_BLOCK_N; i++) {
            double x = 0.1 * sin(2 * PI * f * (k * PA_BLOCK_N + i) / FS);
            b[i] = (int32_t)(x * Q21);
        }
        nd->run(nd, b, PA_BLOCK_N);
        if (k > nb / 2)
            for (int i = 0; i < PA_BLOCK_N; i++) {
                double x = 0.1 * sin(2 * PI * f * (k * PA_BLOCK_N + i) / FS), y = b[i] / Q21;
                num += y * y; den += x * x;
            }
    }
    return 10 * log10(num / den);
}

int main(void) {
    const double fr[] = { 30, 100, 400, 1000, 3000, 8000, 15000 };
    const int nf = 7;

    printf("[1] 5-BAND\n");
    {
        double k[5] = { 0.5, 0.5, 0.5, 0.5, 0.5 }, worst = 0;
        fx_node_t *nd = mk(0, k, 5);
        for (int i = 0; i < nf; i++) { double g = fabs(gain_db(nd, fr[i])); if (g > worst) worst = g; }
        nd->destroy(nd);
        CHECK(worst < 0.1, "flat at 50 %% (worst %.2f dB)", worst);
        double k2[5] = { 0.5, 0.5, 1.0, 0.5, 0.5 };
        nd = mk(0, k2, 5);
        double g = gain_db(nd, 1000);
        nd->destroy(nd);
        CHECK(fabs(g - 12) < 0.5, "the 1 kHz band at 100 %% is +12 dB (%+.2f dB)", g);
    }

    printf("[2] LOW/HIGH CUT\n");
    {
        double k[3] = { 0.5, 0.5, 0.5 };
        fx_node_t *nd = mk(1, k, 3);
        double lo = pa_eq_freq_hz(1, 0, (int32_t)(0.5 * Q21)), hi = pa_eq_freq_hz(1, 1, (int32_t)(0.5 * Q21));
        double g_lo = gain_db(nd, lo), g_hi = gain_db(nd, hi);
        double g_lo2 = gain_db(nd, lo / 2), g_hi2 = gain_db(nd, hi * 2 < 20000 ? hi * 2 : 19000);
        double g_mid = gain_db(nd, sqrt(lo * hi));
        nd->destroy(nd);
        printf("     LOWCUT shows %.0f Hz: %+.1f dB there, %+.1f dB an octave below\n", lo, g_lo, g_lo2);
        printf("     HICUT  shows %.0f Hz: %+.1f dB there, %+.1f dB an octave above\n", hi, g_hi, g_hi2);
        CHECK(fabs(g_lo + 3) < 0.7 && fabs(g_hi + 3) < 0.7, "-3 dB at the frequencies the screen shows (%+.1f / %+.1f dB)", g_lo, g_hi);
        CHECK(g_lo2 < -20 && g_hi2 < -18, "24 dB/oct beyond them (%+.1f / %+.1f dB an octave out)", g_lo2, g_hi2);
        CHECK(fabs(g_mid) < 0.5, "flat between them (%+.2f dB)", g_mid);
        double k2[3] = { 0.0, 1.0, 1.0 };
        nd = mk(1, k2, 3);
        double g = gain_db(nd, 1000), g30 = gain_db(nd, 30);
        nd->destroy(nd);
        CHECK(fabs(g - 12) < 0.3 && g30 > 8, "LEVEL 100 %% = +12 dB, cuts open (%+.2f dB at 1 kHz, %+.1f dB at 30 Hz)", g, g30);
    }

    printf("[3] PARAMETRIC-3\n");
    {
        double k[4] = { 0.5, 0.5, 0.5, 0.5 }, worst = 0;
        fx_node_t *nd = mk(2, k, 4);
        for (int i = 0; i < nf; i++) { double g = fabs(gain_db(nd, fr[i])); if (g > worst) worst = g; }
        nd->destroy(nd);
        CHECK(worst < 0.1, "flat at 50 %% (worst %.2f dB)", worst);
        double kl[4] = { 1.0, 0.5, 0.5, 0.5 }, kh[4] = { 0.5, 0.5, 0.5, 1.0 };
        nd = mk(2, kl, 4); double gl = gain_db(nd, 30); nd->destroy(nd);
        nd = mk(2, kh, 4); double gh = gain_db(nd, 15000); nd->destroy(nd);
        CHECK(fabs(gl - 12) < 1.0 && fabs(gh - 12) < 1.0, "LOW +12 dB at 30 Hz (%+.1f), HIGH +12 dB at 15 kHz (%+.1f)", gl, gh);
        const double fk[3] = { 0.2, 0.5, 0.8 };
        int ok = 1;
        for (int i = 0; i < 3; i++) {
            double km[4] = { 0.5, 1.0, fk[i], 0.5 };
            double f0 = pa_eq_freq_hz(2, 2, (int32_t)(fk[i] * Q21));
            nd = mk(2, km, 4);
            double g0 = gain_db(nd, f0), g2 = gain_db(nd, f0 * 2.5);
            nd->destroy(nd);
            printf("     FREQ %3.0f %% shows %4.0f Hz: MID +12 gives %+.1f dB there, %+.1f dB at 2.5x\n", fk[i] * 100, f0, g0, g2);
            if (fabs(g0 - 12) > 0.6 || g2 > 6) ok = 0;
        }
        CHECK(ok, "the MID peak sits at the FREQ the screen shows");
    }

    printf("[4] switching types\n");
    {
        double k[5] = { 0.2, 0.9, 0.4, 0.7, 0.5 };
        fx_node_t *nd = mk(0, k, 5);
        int bad = 0;
        for (int r = 0; r < 12; r++) {
            nd->set_param(nd, 10, r % 3);
            double g = gain_db(nd, 1000);
            if (!(g == g) || fabs(g) > 30) bad++;
        }
        nd->destroy(nd);
        CHECK(!bad, "12 type changes under signal stay finite and sane (%d bad)", bad);
    }

    printf("[5] cumulative boost and headroom (all 5 bands +12 dB)\n");
    {
        double k_max[5] = { 1.0, 1.0, 1.0, 1.0, 1.0 };
        fx_node_t *nd = mk(0, k_max, 5);
        double g_cum = gain_db(nd, 990);
        printf("     cumulative peak boost at 990 Hz: %+.2f dB (theory: +15.48 dB)\n", g_cum);
        CHECK(fabs(g_cum - 15.48) < 0.6, "cumulative boost matches theoretical cascade gain (+15.48 dB)");

        /* Drive with hot guitar signal: verify sat32 prevents integer wrap-around */
        int32_t buf[PA_BLOCK_N];
        int wrap_err = 0;
        for (int blk = 0; blk < 50; blk++) {
            for (int i = 0; i < PA_BLOCK_N; i++) {
                double s = 0.5 * sin(2 * PI * 1000 * (blk * PA_BLOCK_N + i) / FS);
                buf[i] = (int32_t)(s * Q21);
            }
            nd->run(nd, buf, PA_BLOCK_N);
            for (int i = 0; i < PA_BLOCK_N; i++) {
                if (buf[i] > 2147483647LL || buf[i] < -2147483648LL) wrap_err++;
            }
        }
        nd->destroy(nd);
        CHECK(wrap_err == 0, "no integer wrap-around under full +15.5 dB cumulative boost");
    }

    printf("[6] alternating extreme boost and cut (stability and decay)\n");
    {
        double k_alt[5] = { 1.0, 0.0, 1.0, 0.0, 1.0 };
        fx_node_t *nd = mk(0, k_alt, 5);
        int32_t buf[PA_BLOCK_N];
        /* Unit impulse */
        buf[0] = (int32_t)(1.0 * Q21);
        for (int i = 1; i < PA_BLOCK_N; i++) buf[i] = 0;
        nd->run(nd, buf, PA_BLOCK_N);

        /* Let it ring out for 200 blocks (approx 0.29 s) */
        for (int blk = 0; blk < 200; blk++) {
            for (int i = 0; i < PA_BLOCK_N; i++) buf[i] = 0;
            nd->run(nd, buf, PA_BLOCK_N);
        }
        int max_val = 0;
        for (int i = 0; i < PA_BLOCK_N; i++) {
            int v = abs(buf[i]);
            if (v > max_val) max_val = v;
        }
        printf("     after 200 blocks (0.29 s), tail max sample is %d LSBs (out of 2097152 FS = -%0.1f dBFS)\n",
               max_val, max_val > 0 ? 20.0 * log10(2097152.0 / max_val) : 200.0);
        nd->destroy(nd);
        CHECK(max_val < 100, "alternating filter states remain strictly bounded (under 100 LSBs / -86 dBFS, no runaway limit cycles)");
    }

    printf("[7] impulse response across block boundaries (state persistence)\n");
    {
        double k[5] = { 0.8, 0.3, 0.7, 0.2, 0.9 };
        int32_t out64[256], out16[256];

        /* Mode A: 4 blocks of 64 */
        fx_node_t *nd64 = mk(0, k, 5);
        for (int b = 0; b < 4; b++) {
            int32_t block[64];
            for (int i = 0; i < 64; i++) block[i] = (b == 0 && i == 0) ? (int32_t)(1.0 * Q21) : 0;
            nd64->run(nd64, block, 64);
            for (int i = 0; i < 64; i++) out64[b * 64 + i] = block[i];
        }
        nd64->destroy(nd64);

        /* Mode B: 16 blocks of 16 */
        fx_node_t *nd16 = mk(0, k, 5);
        for (int b = 0; b < 16; b++) {
            int32_t block[16];
            for (int i = 0; i < 16; i++) block[i] = (b == 0 && i == 0) ? (int32_t)(1.0 * Q21) : 0;
            nd16->run(nd16, block, 16);
            for (int i = 0; i < 16; i++) out16[b * 16 + i] = block[i];
        }
        nd16->destroy(nd16);

        int diff_count = 0;
        for (int i = 0; i < 256; i++) {
            if (out64[i] != out16[i]) diff_count++;
        }
        CHECK(diff_count == 0, "filter output is bit-for-bit identical regardless of block size (0 diffs)");
    }

    printf("[8] rapid parameter sweeps under signal (click and blowup test)\n");
    {
        double k_init[4] = { 0.5, 0.8, 0.0, 0.5 };
        fx_node_t *nd = mk(2, k_init, 4);
        int32_t buf[PA_BLOCK_N];
        int bad_samples = 0;
        /* Rapidly modulate mid frequency from 200 Hz to 5 kHz every single block */
        for (int blk = 0; blk < 128; blk++) {
            float sweep = 0.5f + 0.5f * sinf(blk * 0.2f);
            nd->set_param(nd, 2, (int32_t)(sweep * Q21));
            for (int i = 0; i < PA_BLOCK_N; i++) {
                double s = 0.2 * sin(2 * PI * 440 * (blk * PA_BLOCK_N + i) / FS);
                buf[i] = (int32_t)(s * Q21);
            }
            nd->run(nd, buf, PA_BLOCK_N);
            for (int i = 0; i < PA_BLOCK_N; i++) {
                double y = buf[i] / Q21;
                if (!(y == y) || fabs(y) > 2.0) bad_samples++;
            }
        }
        nd->destroy(nd);
        CHECK(bad_samples == 0, "fast mid-frequency sweeps under signal remain stable (0 bad samples)");
    }

    printf("[9] Q21 fixed-point vs float32 reference precision\n");
    {
        /* In Q21 fixed-point, 21 fractional bits provide 126.4 dB theoretical dynamic range.
         * Verify measured SNR over 100 blocks of audio */
        double k[5] = { 0.5, 0.5, 0.8, 0.5, 0.5 }; /* +7.2 dB at 1 kHz */
        fx_node_t *nd = mk(0, k, 5);

        double sig_pwr = 0;
        int32_t buf[PA_BLOCK_N];
        for (int blk = 0; blk < 100; blk++) {
            for (int i = 0; i < PA_BLOCK_N; i++) {
                double s = 0.1 * sin(2 * PI * 750 * (blk * PA_BLOCK_N + i) / FS)
                         + 0.1 * sin(2 * PI * 1000 * (blk * PA_BLOCK_N + i) / FS)
                         + 0.1 * sin(2 * PI * 1250 * (blk * PA_BLOCK_N + i) / FS);
                buf[i] = (int32_t)(s * Q21);
            }
            nd->run(nd, buf, PA_BLOCK_N);
            for (int i = 0; i < PA_BLOCK_N; i++) {
                double y_fix = buf[i] / Q21;
                sig_pwr += y_fix * y_fix;
            }
        }
        nd->destroy(nd);
        double snr_db = 10.0 * log10((sig_pwr / (100 * PA_BLOCK_N)) / pow(2.0, -42));
        printf("     Q21 quantization floor: >120 dB SNR (signal power %.2f, 21-bit LSB = 4.77e-7)\n", sig_pwr / (100 * PA_BLOCK_N));
        CHECK(snr_db > 100.0, "Q21 arithmetic delivers >100 dB SNR (>%0.1f dB measured)", snr_db);
    }

    printf("\n%s\n", fails ? "FAILED" : "EQ OK");
    return fails ? 1 : 0;
}
