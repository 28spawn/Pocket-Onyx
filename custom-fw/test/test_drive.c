/* custom-fw/test/test_drive.c - the DRV block (src/fx/fx_drive.c), measured.
 *
 *   [1] LEVEL 0 % is silent; LEVEL 100 % is +12 dB over 50 %
 *   [2] loudness match: every model at every GRIT is within 1.5 dB of bypass at LEVEL 50 %
 *   [3] GRIT range: at 100 % every model really distorts a quiet note (THD > 20 %; the
 *       TS-808 used to reach 9.9 % at full GRIT on a 12 dB LOUDER note)
 *   [4] TS-808: the clean path is full range (100 Hz is not cut below the input level)
 *   [5] RAT: the diode ceiling is a ceiling (output does not grow with a hotter input)
 *   [6] aliasing at full GRIT on the highest guitar note (E6, 1319 Hz): nothing above
 *       -60 dB re the note (3.5 kHz is reported as info)
 *   [7] no DC, no NaN
 *
 *   --calibrate  prints k_match_db[][] for fx_drive.c (the loudness match table)
 *   --report     prints everything, fails nothing
 *
 * clang -O2 -Icustom-fw/include -DPA_HOST_SIM=1 custom-fw/test/test_drive.c \
 *       custom-fw/src/fx/fx_drive.c -lm
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include "pa_config.h"
#include "pa_fx.h"

void *pa_alloc(uint32_t n) { return calloc(1, n); }
void  pa_free(void *p)     { free(p); }
fx_node_t *fx_drive_new(void);

#define FS  44100
#define Q21 2097152.0
#define PI  3.14159265358979
#define N   (PA_BLOCK_N * 690)          /* ~1 s */

static int fails = 0, strict = 1;
#define CHECK(ok, ...) do { printf(!strict ? "  info " : (ok) ? "  ok   " : "  FAIL "); printf(__VA_ARGS__); printf("\n"); if (strict && !(ok)) fails++; } while (0)

static const char *k_name[6] = { "TS-808", "KLON", "OCD", "RAT", "BIG MUFF", "FUZZ FACE" };
static double X[N], Y[N];

static fx_node_t *mk(int type, double grit, double tone, double level) {
    fx_node_t *nd = fx_drive_new();
    nd->create(nd);
    nd->set_param(nd, 3, type);
    nd->set_param(nd, 0, (int32_t)(grit * Q21));
    nd->set_param(nd, 1, (int32_t)(tone * Q21));
    nd->set_param(nd, 2, (int32_t)(level * Q21));
    return nd;
}
static void run(fx_node_t *nd) {
    int32_t b[PA_BLOCK_N];
    for (int i = 0; i < N; i += PA_BLOCK_N) {
        for (int k = 0; k < PA_BLOCK_N; k++) b[k] = (int32_t)(X[i + k] * Q21);
        nd->run(nd, b, PA_BLOCK_N);
        for (int k = 0; k < PA_BLOCK_N; k++) Y[i + k] = b[k] / Q21;
    }
}
static double rms(const double *v) { double s = 0; for (int i = N / 2; i < N; i++) s += v[i] * v[i]; return sqrt(s / (N / 2)); }
static double db(double v) { return 20 * log10(v + 1e-15); }
static double tone_amp(const double *v, double f) {
    double re = 0, im = 0;
    for (int i = N / 2; i < N; i++) { re += v[i] * cos(2 * PI * f * i / FS); im += v[i] * sin(2 * PI * f * i / FS); }
    return 2 * sqrt(re * re + im * im) / (N / 2);
}
/* in-place radix-2 FFT (n a power of two) */
static void fft(double *re, double *im, int n) {
    for (int i = 1, j = 0; i < n; i++) {
        int bit = n >> 1;
        for (; j & bit; bit >>= 1) j ^= bit;
        j ^= bit;
        if (i < j) { double t = re[i]; re[i] = re[j]; re[j] = t; t = im[i]; im[i] = im[j]; im[j] = t; }
    }
    for (int len = 2; len <= n; len <<= 1) {
        double a = -2 * PI / len;
        for (int i = 0; i < n; i += len)
            for (int k = 0; k < len / 2; k++) {
                double wr = cos(a * k), wi = sin(a * k);
                double ur = re[i + k], ui = im[i + k];
                double vr = re[i + k + len / 2] * wr - im[i + k + len / 2] * wi;
                double vi = re[i + k + len / 2] * wi + im[i + k + len / 2] * wr;
                re[i + k] = ur + vr; im[i + k] = ui + vi;
                re[i + k + len / 2] = ur - vr; im[i + k + len / 2] = ui - vi;
            }
    }
}
#define NF 16384
static double FR[NF], FI[NF];

static void sine(double f, double a) { for (int i = 0; i < N; i++) X[i] = a * sin(2 * PI * f * i / FS); }
/* a guitar-like steady note: 110 Hz and 5 harmonics (1/n), peak at `peak` */
static void note(double peak) {
    double m = 0;
    for (int i = 0; i < N; i++) {
        X[i] = 0;
        for (int h = 1; h <= 6; h++) X[i] += sin(2 * PI * 110.25 * h * i / FS + h) / h;   /* period: 400 samples */
        if (fabs(X[i]) > m) m = fabs(X[i]);
    }
    for (int i = 0; i < N; i++) X[i] *= peak / m;
}
static double thd(double f) {
    double f1 = tone_amp(Y, f), tot = rms(Y) * sqrt(2);
    return sqrt(fmax(tot * tot - f1 * f1, 0)) / f1 * 100;
}

int main(int argc, char **argv) {
    int calibrate = 0;
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--report")) strict = 0;
        if (!strcmp(argv[i], "--calibrate")) calibrate = 1;
    }
    const double grits[5] = { 0, 0.25, 0.5, 0.75, 1.0 };
    const double cal_peak = pow(10, -12 / 20.0);

    /* the loudness of each model / GRIT re bypass, with the current table */
    double lv[6][5];
    note(cal_peak);
    double in_rms = rms(X);
    for (int t = 0; t < 6; t++)
        for (int g = 0; g < 5; g++) {
            fx_node_t *nd = mk(t, grits[g], 0.5, 0.5);
            run(nd);
            lv[t][g] = db(rms(Y) / in_rms);
            nd->destroy(nd);
        }
    if (calibrate) {
        /* the table to paste: the current entry minus what is still measured */
        printf("Measured re bypass with the CURRENT table (dB); subtract from k_match_db:\n");
        for (int t = 0; t < 6; t++) {
            printf("    { ");
            for (int g = 0; g < 5; g++) printf("%+.2ff%s", lv[t][g], g < 4 ? ", " : "");
            printf(" },   /* %s */\n", k_name[t]);
        }
        return 0;
    }

    printf("[1] LEVEL\n");
    {
        fx_node_t *nd = mk(0, 0.5, 0.5, 0.0); note(cal_peak); run(nd);
        double silent = db(rms(Y) / in_rms); nd->destroy(nd);
        nd = mk(0, 0.5, 0.5, 1.0); run(nd); double hi = db(rms(Y) / in_rms); nd->destroy(nd);
        nd = mk(0, 0.5, 0.5, 0.5); run(nd); double mid = db(rms(Y) / in_rms); nd->destroy(nd);
        CHECK(silent < -100, "LEVEL 0 %% is silent (%.0f dB)", silent);
        CHECK(fabs(hi - mid - 12.04) < 0.1, "LEVEL 100 %% is +12 dB over 50 %% (%+.2f dB)", hi - mid);
    }

    printf("[2] loudness at LEVEL 50 %% re bypass, -12 dBFS note, GRIT 0 / 25 / 50 / 75 / 100 %%\n");
    double worst = 0;
    for (int t = 0; t < 6; t++) {
        printf("     %-10s", k_name[t]);
        for (int g = 0; g < 5; g++) { printf(" %+5.1f", lv[t][g]); if (fabs(lv[t][g]) > worst) worst = fabs(lv[t][g]); }
        printf("  dB\n");
    }
    CHECK(worst < 1.5, "every model and GRIT is within 1.5 dB of bypass (worst %.2f dB)", worst);
    /* info: the match at a quieter and a hotter input (clipping makes it level dependent) */
    for (int pl = 0; pl < 2; pl++) {
        double pk = pow(10, (pl ? -3 : -24) / 20.0), w = 0;
        note(pk); double ir = rms(X);
        for (int t = 0; t < 6; t++) for (int g = 0; g < 5; g += 2) {
            fx_node_t *nd = mk(t, grits[g], 0.5, 0.5); run(nd);
            double d = db(rms(Y) / ir); if (fabs(d) > fabs(w)) w = d; nd->destroy(nd);
        }
        printf("  info at a %d dBFS note the largest deviation is %+.1f dB\n", pl ? -3 : -24, w);
    }

    printf("[3] GRIT range: THD of a quiet note (-24 dBFS 220 Hz sine)\n");
    {
        double minthd = 1e9; int mt = 0;
        for (int t = 0; t < 6; t++) {
            double r[2];
            for (int g = 0; g < 2; g++) {
                fx_node_t *nd = mk(t, g ? 1.0 : 0.0, 0.5, 0.5); sine(220, pow(10, -24 / 20.0)); run(nd);
                r[g] = thd(220); nd->destroy(nd);
            }
            printf("     %-10s GRIT 0 %%: %5.1f %%   GRIT 100 %%: %5.1f %%\n", k_name[t], r[0], r[1]);
            if (r[1] < minthd) { minthd = r[1]; mt = t; }
        }
        CHECK(minthd > 20, "every model distorts hard at GRIT 100 %% (least: %s %.1f %%)", k_name[mt], minthd);
    }

    printf("[4] TS-808 clean path\n");
    {
        fx_node_t *nd = mk(0, 0.0, 1.0, 0.5); sine(100, 0.001); run(nd);
        double r = db(tone_amp(Y, 100) / 0.001); nd->destroy(nd);
        nd = mk(0, 0.0, 1.0, 0.5); sine(700, 0.001); run(nd);
        double m = db(tone_amp(Y, 700) / 0.001); nd->destroy(nd);
        /* (the match gain applies to both; compare 100 Hz with the TS's own unity path) */
        printf("     small signal: 100 Hz %+.1f dB, 700 Hz %+.1f dB (the mid hump)\n", r, m);
        CHECK(m - r < 16.0 && r > -6.0, "100 Hz is not cut far below the input (%+.1f dB; it was -13.5 dB)", r);
    }

    printf("[5] RAT ceiling\n");
    {
        double pk[2];
        for (int k = 0; k < 2; k++) {
            fx_node_t *nd = mk(3, 1.0, 1.0, 0.5); sine(220, k ? 4.0 : 0.25); run(nd);
            pk[k] = 0; for (int i = N / 2; i < N; i++) if (fabs(Y[i]) > pk[k]) pk[k] = fabs(Y[i]);
            nd->destroy(nd);
        }
        CHECK(pk[1] < pk[0] * 1.1, "a +12 dBFS input is no louder than a -12 dBFS one at full GRIT (peak %.3f vs %.3f)", pk[1], pk[0]);
    }

    printf("[6] aliasing at GRIT 100 %%, TONE 100 %%, -12 dBFS\n");
    {
        /* each note sits exactly on an FFT bin of a 16384-point window, so every harmonic and
         * every alias of it lands on an exact bin: no leakage, no window needed */
        const int bins[2] = { 490, 1308 };         /* 1319 Hz (E6, 24th fret), 3521 Hz */
        for (int bi = 0; bi < 2; bi++) {
            const int bin = bins[bi];
            const double f0 = bin * (double)FS / NF;
            double w_all = -999; int wt = 0;
            printf("     note %.0f Hz:\n", f0);
            for (int t = 0; t < 6; t++) {
                fx_node_t *nd = mk(t, 1.0, 1.0, 0.5); sine(f0, 0.25); run(nd);
                for (int i = 0; i < NF; i++) { FR[i] = Y[N - NF + i]; FI[i] = 0; }
                fft(FR, FI, NF);
                double f1 = hypot(FR[bin], FI[bin]), w = 0; int wb = 0;
                for (int b = (int)(100.0 * NF / FS); b < (int)(20000.0 * NF / FS); b++) {
                    if (b % bin == 0) continue;                       /* a harmonic */
                    double a = hypot(FR[b], FI[b]); if (a > w) { w = a; wb = b; }
                }
                printf("       %-10s strongest alias %6.1f dB at %5.0f Hz\n", k_name[t], db(w / f1), wb * (double)FS / NF);
                if (db(w / f1) > w_all) { w_all = db(w / f1); wt = t; }
                nd->destroy(nd);
            }
            if (bi == 0)
                CHECK(w_all < -60, "E6: no alias above -60 dB re the note (worst %s %.1f dB; RAT was -44 dB)", k_name[wt], w_all);
            else
                printf("  info 3.5 kHz (above any guitar note): worst %s %.1f dB (it was -25.8 dB)\n", k_name[wt], w_all);
        }
    }

    printf("[7] DC and NaN\n");
    {
        double worst_dc = 0; int nan = 0;
        for (int t = 0; t < 6; t++) {
            fx_node_t *nd = mk(t, 1.0, 0.5, 1.0); note(0.5); run(nd);
            double m = 0; for (int i = N - 22000; i < N; i++) { m += Y[i]; if (Y[i] != Y[i]) nan = 1; }
            m /= 22000;                                    /* 55 whole periods of the note */ if (fabs(m) > worst_dc) worst_dc = fabs(m);
            nd->destroy(nd);
        }
        CHECK(worst_dc < 1e-3 && !nan, "no DC and no NaN at the output (worst DC %.2e)", worst_dc);
    }

    printf("\n%s\n", fails ? "FAILED" : "DRIVE OK");
    return strict && fails ? 1 : 0;
}
