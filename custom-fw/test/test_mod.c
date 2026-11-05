/* custom-fw/test/test_mod.c - the MOD block (src/fx/fx_mod.c), measured.
 *
 *   [1] headroom: the chorus / flanger delay line is clean above 0 dBFS
 *   [2] level: CHORUS / FLANGER / PHASER / UNIVIBE at MIX 50 % stay within 2 dB of bypass
 *   [3] mono: the PHASER and UNIVIBE do not cancel when L and R are summed
 *   [4] phaser range: the first notch sweeps ~83 Hz .. ~830 Hz (stages at 200 Hz .. 2 kHz)
 *   [5] auto filter: it follows the playing (a loud note opens it an octave+ further than
 *       a quiet one) AND the LFO sweeps it on its own (a steady tone moves an octave+
 *       between the LFO's low and high point - after a saturated amp there is no envelope)
 *   [6] RATE: the Hz the screen shows (pa_mod_rate_mhz) is the LFO you hear
 *   [7] the knobs shown as "---" really do nothing on that type
 *   [8] max feedback stays bounded, no NaN
 *
 * clang -O2 -Icustom-fw/include -DPA_HOST_SIM=1 custom-fw/test/test_mod.c \
 *       custom-fw/src/fx/fx_mod.c -lm      (--report: print only)
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include "pa_config.h"
#include "pa_fx.h"

void *pa_alloc(uint32_t n) { return calloc(1, n); }
void  pa_free(void *p)     { free(p); }
fx_node_t *fx_mod_new(void);

#define FS  44100
#define Q21 2097152.0
#define PI  3.14159265358979
#define NMAX (FS * 6)

static int fails = 0, strict = 1;
#define CHECK(ok, ...) do { printf(!strict ? "  info " : (ok) ? "  ok   " : "  FAIL "); printf(__VA_ARGS__); printf("\n"); if (strict && !(ok)) fails++; } while (0)

static double X[NMAX], L[NMAX], R[NMAX];

static fx_node_t *mk(int type, double rate, double depth, double mix, double fb) {
    fx_node_t *nd = fx_mod_new();
    nd->create(nd);
    nd->set_param(nd, 10, type);
    nd->set_param(nd, 0, (int32_t)(rate * Q21));
    nd->set_param(nd, 1, (int32_t)(depth * Q21));
    nd->set_param(nd, 2, (int32_t)(mix * Q21));
    nd->set_param(nd, 3, (int32_t)(fb * Q21));
    return nd;
}
static void run(fx_node_t *nd, int n) {
    int32_t bl[PA_BLOCK_N], br[PA_BLOCK_N];
    for (int i = 0; i + PA_BLOCK_N <= n; i += PA_BLOCK_N) {
        for (int k = 0; k < PA_BLOCK_N; k++) bl[k] = br[k] = (int32_t)(X[i + k] * Q21);
        nd->run_stereo(nd, bl, br, PA_BLOCK_N);
        for (int k = 0; k < PA_BLOCK_N; k++) { L[i + k] = bl[k] / Q21; R[i + k] = br[k] / Q21; }
    }
}
static double rms(const double *v, int a, int b) { double s = 0; for (int i = a; i < b; i++) s += v[i] * v[i]; return sqrt(s / (b - a)); }
static double db(double v) { return 20 * log10(v + 1e-15); }
static double amp(const double *v, int a, int b, double f) {
    double re = 0, im = 0;
    for (int i = a; i < b; i++) { re += v[i] * cos(2 * PI * f * i / FS); im += v[i] * sin(2 * PI * f * i / FS); }
    return 2 * sqrt(re * re + im * im) / (b - a);
}
static void multisine(int n, double a) {
    const double f[5] = { 110, 247, 523, 1100, 2900 };
    for (int i = 0; i < n; i++) { X[i] = 0; for (int k = 0; k < 5; k++) X[i] += a * sin(2 * PI * f[k] * i / FS + k); }
}

/* the FIRST (lowest) notch of the L output (MIX 50 %) at the LFO position reached after
 * `pre` samples: the first local minimum more than 15 dB under the input. (Four stages
 * make two notches, at 0.41x and 2.41x the stage frequency; both are deep.) */
static double first_notch(int type, int pre) {
    double best = 1e9, bf = 0;
    for (double f = 40; f < 3000; f *= 1.02) {
        fx_node_t *nd = mk(type, 0.0, 1.0, 0.5, 0.0);
        int n = pre + 16384;
        for (int i = 0; i < n; i++) X[i] = (i < pre) ? 0.0 : 0.1 * sin(2 * PI * f * i / FS);
        run(nd, n);
        double a = amp(L, pre + 8192, n, f);
        nd->destroy(nd);
        if (a < best) { best = a; bf = f; }
        else if (db(best / 0.1) < -15.0) break;        /* past the first deep minimum */
    }
    return bf;
}

int main(int argc, char **argv) {
    for (int i = 1; i < argc; i++) if (!strcmp(argv[i], "--report")) strict = 0;
    const char *nm[7] = { "CHORUS", "PHASER", "FLANGER", "TREMOLO", "VIBRATO", "UNIVIBE", "AUTO FILTER" };
    int N = FS * 2 - (FS * 2) % PA_BLOCK_N;

    printf("[1] headroom: CHORUS, DEPTH 0, MIX 100 %% (the wet path alone), 220 Hz\n");
    {
        double worst = 0;
        const double lv[3] = { 0.5, 1.5, 3.0 };
        for (int k = 0; k < 3; k++) {
            fx_node_t *nd = mk(0, 0.0, 0.0, 1.0, 0.0);
            for (int i = 0; i < N; i++) X[i] = lv[k] * sin(2 * PI * 220.5 * i / FS);   /* 200 samples a period */
            run(nd, N);
            int a = N - 40000, b = N;
            double f1 = amp(L, a, b, 220.5), tot = rms(L, a, b) * sqrt(2);
            double thd = sqrt(fmax(tot * tot - f1 * f1, 0)) / f1 * 100;
            printf("     input %+5.1f dBFS: THD %.4f %%\n", db(lv[k]), thd);
            if (thd > worst) worst = thd;
            nd->destroy(nd);
        }
        CHECK(worst < 0.01, "no clipping in the delay line up to +9.5 dBFS (worst THD %.4f %%; it was 15 %% at +3.5)", worst);
    }

    printf("[2] level at MIX 50 %%, DEPTH 50 %%, RATE 30 %% (a 5-tone signal, 1.5 s)\n");
    {
        double worst = 0;
        const int types[4] = { 0, 1, 2, 5 };
        for (int k = 0; k < 4; k++) {
            fx_node_t *nd = mk(types[k], 0.3, 0.5, 0.5, 0.0);
            multisine(N, 0.05); run(nd, N);
            double in = rms(X, FS / 2, N), dl = db(rms(L, FS / 2, N) / in), dr = db(rms(R, FS / 2, N) / in);
            printf("     %-8s L %+5.1f dB  R %+5.1f dB\n", nm[types[k]], dl, dr);
            if (fabs(dl) > worst) worst = fabs(dl);
            if (fabs(dr) > worst) worst = fabs(dr);
            nd->destroy(nd);
        }
        CHECK(worst < 2.0, "engaging the effect keeps the level within 2 dB (worst %.1f dB; chorus was -3, phaser R -5.5)", worst);
    }

    printf("[3] mono sum: the wet part of (L+R)/2 re the input\n");
    {
        double least = 1e9;
        const int types[2] = { 1, 5 };
        for (int k = 0; k < 2; k++) {
            fx_node_t *nd = mk(types[k], 0.3, 1.0, 0.5, 0.0);
            multisine(N, 0.05); run(nd, N);
            /* residual of the mono sum after removing the dry part (dry gain cos 45 deg) */
            double s = 0;
            for (int i = FS / 2; i < N; i++) { double m = 0.5 * (L[i] + R[i]) - 0.70710678 * X[i]; s += m * m; }
            double wet = db(sqrt(s / (N - FS / 2)) / rms(X, FS / 2, N));
            printf("     %-8s wet in mono %+.1f dB\n", nm[types[k]], wet);
            if (wet < least) least = wet;
            nd->destroy(nd);
        }
        CHECK(least > -10.0, "the phaser / univibe wet survives the mono sum (least %+.1f dB; it was -inf: L and R cancelled)", least);
    }

    printf("[4] phaser sweep: first notch at the two ends of the LFO (DEPTH 100 %%, RATE 0 = 0.1 Hz)\n");
    {
        double lo = first_notch(1, 0);                 /* the LFO starts at its bottom */
        double hi = first_notch(1, FS * 5);            /* half a 0.1 Hz cycle later: the top */
        printf("     first notch %.0f Hz .. %.0f Hz\n", lo, hi);
        CHECK(lo > 60 && lo < 120 && hi > 600 && hi < 1100,
              "the notch sweeps the guitar's range, ~83 .. ~830 Hz (%.0f .. %.0f Hz; stages were at 1.1 .. 8 kHz)", lo, hi);
    }

    printf("[5] auto filter (DEPTH 100 %%, MIX 100 %%, RESO 50 %%, RATE 0 = 0.1 Hz)\n");
    {
        /* the filter's position: the power-weighted mean frequency of its gain at the
         * harmonics of a 110 Hz note (output harmonic / input harmonic) in a 0.2 s window */
        double fc[4];
        for (int k = 0; k < 4; k++) {
            fx_node_t *nd = mk(6, 0.0, 1.0, 1.0, 0.5);
            int pre = (k == 3) ? FS * 5 - (FS * 5) % PA_BLOCK_N : 0;     /* half an LFO cycle */
            int n = pre + FS / 2 - (FS / 2) % PA_BLOCK_N;
            double amp0 = (k == 0) ? 0.5 : (k == 1) ? 0.016 : 0.1;       /* -6 / -36 / -20 dBFS */
            for (int i = 0; i < n; i++) {
                X[i] = 0;
                for (int h = 1; h <= 24; h++) X[i] += amp0 / h * sin(2 * PI * 110 * h * i / FS);
            }
            run(nd, n);
            int a = pre + 8820, b = a + 8820;
            double num = 0, den = 0;
            for (int h = 1; h <= 24; h++) {
                double g = amp(L, a, b, 110.0 * h) / (amp0 / h);
                num += 110.0 * h * g * g; den += g * g;
            }
            fc[k] = num / den;
            nd->destroy(nd);
        }
        printf("     loud note %.0f Hz vs quiet note %.0f Hz (same LFO point)\n", fc[0], fc[1]);
        printf("     steady tone: LFO low %.0f Hz vs LFO high %.0f Hz\n", fc[2], fc[3]);
        CHECK(fc[0] / fc[1] > 2.0, "the envelope opens the filter: loud %.0f Hz vs quiet %.0f Hz (it barely moved before)",
              fc[0], fc[1]);
        CHECK(fc[3] / fc[2] > 2.0, "the LFO sweeps it on its own: %.0f -> %.0f Hz", fc[2], fc[3]);
    }

    printf("[6] RATE in Hz: shown vs measured (TREMOLO, DEPTH 100 %%)\n");
    {
        double worst = 0;
        const double knobs[3] = { 0.1, 0.5, 1.0 };
        for (int k = 0; k < 3; k++) {
            fx_node_t *nd = mk(3, knobs[k], 1.0, 0.0, 0.0);
            int n = FS * 6 - (FS * 6) % PA_BLOCK_N;
            for (int i = 0; i < n; i++) X[i] = 0.5;
            run(nd, n);
            /* the gain is 1 - lfo: count its upward crossings of the mid level */
            int first = -1, last = -1, cyc = 0;
            for (int i = 1; i < n; i++)
                if (L[i - 1] < 0.25 && L[i] >= 0.25) { if (first < 0) first = i; else { cyc++; last = i; } }
            double hz = cyc ? cyc * (double)FS / (last - first) : 0;
            double shown = pa_mod_rate_mhz((int32_t)(knobs[k] * Q21)) / 1000.0;
            printf("     knob %3.0f %%: shows %.2f Hz, measured %.3f Hz\n", knobs[k] * 100, shown, hz);
            double e = fabs(shown - hz) / hz; if (e > worst) worst = e;
            nd->destroy(nd);
        }
        CHECK(worst < 0.01, "the shown RATE is the LFO's (worst %.2f %% off)", worst * 100);
    }

    printf("[7] knobs shown as \"---\" do nothing\n");
    {
        /* type, knob id that must be inert */
        const int cases[5][2] = { { 3, 2 }, { 3, 3 }, { 4, 2 }, { 4, 3 }, { 0, 3 } };
        const char *kn[4] = { "RATE", "DEPTH", "MIX", "FDBK" };
        int bad = 0;
        static double ref[FS];
        for (int c = 0; c < 5; c++) {
            int t = cases[c][0], id = cases[c][1];
            fx_node_t *nd = mk(t, 0.4, 0.7, 0.2, 0.1); multisine(FS - FS % PA_BLOCK_N, 0.1); run(nd, FS - FS % PA_BLOCK_N);
            memcpy(ref, L, sizeof(ref)); nd->destroy(nd);
            nd = mk(t, 0.4, 0.7, 0.2, 0.1); nd->set_param(nd, id, (int32_t)(0.9 * Q21));
            run(nd, FS - FS % PA_BLOCK_N); nd->destroy(nd);
            int same = !memcmp(ref, L, sizeof(double) * (FS - FS % PA_BLOCK_N));
            if (!same) { bad++; printf("     %s: %s changes the sound\n", nm[t], kn[id]); }
        }
        CHECK(bad == 0, "TREMOLO / VIBRATO ignore MIX and FDBK, CHORUS ignores FDBK");
    }

    printf("[8] stability at maximum feedback (+6 dBFS input, 4 s)\n");
    {
        double pk = 0; int nan = 0;
        const int types[3] = { 1, 2, 6 };
        int n = FS * 4 - (FS * 4) % PA_BLOCK_N;
        for (int k = 0; k < 3; k++) {
            fx_node_t *nd = mk(types[k], 0.5, 1.0, 1.0, 1.0);
            multisine(n, 0.4); run(nd, n);
            for (int i = 0; i < n; i++) { if (L[i] != L[i] || R[i] != R[i]) nan = 1; if (fabs(L[i]) > pk) pk = fabs(L[i]); }
            nd->destroy(nd);
        }
        CHECK(!nan && pk < 16.0, "phaser / flanger / auto filter stay bounded (peak %.2f FS)", pk);
    }

    printf("[9] fractional delay interpolation: 10 kHz treble retention (Vibrato, MIX 100 %%, DEPTH 50 %%, 220 ms)\n");
    {
        fx_node_t *nd = mk(4, 0.3, 0.5, 1.0, 0.0);
        int n = FS - FS % PA_BLOCK_N;
        for (int i = 0; i < n; i++) X[i] = 0.5 * sin(2 * PI * 10000.0 * i / FS);
        run(nd, n);
        int a = n - 20000, b = n;
        double r10k = rms(L, a, b);
        double in_rms = rms(X, a, b);
        double atten_db = db(r10k / in_rms);
        printf("     10 kHz tone wet level: %+.2f dB (linear drops -2.4 dB, cubic Hermite preserves treble)\n", atten_db);
        nd->destroy(nd);
        CHECK(atten_db > -1.5, "cubic Hermite maintains <1.5 dB attenuation at 10 kHz (measured %+.2f dB)", atten_db);
    }

    printf("[10] mono compatibility: CHORUS and FLANGER summed to mono (L+R)/2\n");
    {
        double worst_drop = 0;
        int types[2] = { 0, 2 }; /* Chorus, Flanger */
        for (int k = 0; k < 2; k++) {
            fx_node_t *nd = mk(types[k], 0.3, 0.5, 0.5, 0.0);
            multisine(N, 0.05); run(nd, N);
            double in = rms(X, FS / 2, N);
            static double mono[NMAX];
            for (int i = FS / 2; i < N; i++) mono[i] = 0.5 * (L[i] + R[i]);
            double mono_db = db(rms(mono, FS / 2, N) / in);
            printf("     %-8s mono sum level re input: %+.2f dB\n", nm[types[k]], mono_db);
            if (fabs(mono_db) > worst_drop) worst_drop = fabs(mono_db);
            nd->destroy(nd);
        }
        CHECK(worst_drop < 3.0, "chorus and flanger remain full and loud in mono (worst deviation %.2f dB)", worst_drop);
    }

    printf("\n%s\n", fails ? "FAILED" : "MOD OK");
    return strict && fails ? 1 : 0;
}
