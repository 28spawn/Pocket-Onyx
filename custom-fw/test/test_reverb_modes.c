/* test/test_reverb_modes.c - every reverb type at high decay / high mix, measured.
 *
 * Field reports (2026-10-22): Shimmer stops working after a few seconds; Room, Hall and
 * Cloud have artifacts / noise at high decay and high mix; Swell and Spring are fine.
 *
 * For each type, the real fx_reverb node is driven with plucked "notes" (a decaying
 * 196 Hz tone with harmonics) for 2 s, then silence. Measured:
 *   over1     tank samples beyond +-1.0 full scale - where the old int16 tank clipped
 *             (informational: the int32 tank has 1,024x headroom)
 *   tankmax   the largest |tank| value, in full-scale units: it must stay far below the
 *             32-bit limit (1,024), or the tank would saturate
 *   clicks    sample-to-sample jumps in the wet output far above the local level
 *             (a discontinuity, not a note onset: counted only after the input stops)
 *   floor     output level 4..5 s after the input stopped (hiss / stuck DC)
 *   dc        mean of the output in that window
 *   shimmer   output level in 0.5 s windows while playing for 8 s: it must not collapse
 *
 * Build: clang -O2 -Icustom-fw/include -DPA_HOST_SIM=1 custom-fw/test/test_reverb_modes.c \
 *        <host_dsp sources> -lm
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include "pa_config.h"
#include "pa_fx.h"

void *pa_alloc(uint32_t n) { return calloc(1, n); }
void  pa_free(void *p)     { free(p); }

fx_node_t *fx_reverb_new(void);
uint32_t   pa_rev_tank_samples(void);
int32_t   *pa_rev_tank_base(const fx_node_t *nd);

static int fails = 0;
#define CHECK(ok, ...) do { printf((ok) ? "  ok   " : "  FAIL "); printf(__VA_ARGS__); printf("\n"); if (!(ok)) fails++; } while (0)

static const char *k_names[6] = { "Room", "Hall", "Swell", "Spring", "Shimmer", "Cloud" };
#define FS   44100
#define B    PA_BLOCK_N
#define Q21  2097152.0

/* plucked notes: a new note every 0.5 s, decaying, with a few harmonics */
static double note(int t) {
    int k = t % (FS / 2);
    double env = exp(-(double)k / (0.12 * FS));
    double ph = 2.0 * 3.14159265358979 * 196.0 * (double)t / FS;
    return 0.5 * env * (sin(ph) + 0.4 * sin(2 * ph) + 0.25 * sin(3 * ph) + 0.15 * sin(5 * ph));
}

typedef struct { long clip; long clicks; double floor_db; double dc; double peak; double hf_db; double tank_max; } res_t;

static res_t run_type(int type, int32_t decay_q21, int32_t mix_q21, int play_s, int tail_s,
                      double *win_db, int nwin) {
    fx_node_t *nd = fx_reverb_new();
    nd->create(nd);
    nd->set_param(nd, 10, type);
    nd->set_param(nd, 0, decay_q21);
    nd->set_param(nd, 1, (int32_t)(0.2 * Q21));
    nd->set_param(nd, 2, (int32_t)(0.3 * Q21));
    nd->set_param(nd, 3, mix_q21);
    int32_t *tank = pa_rev_tank_base(nd);
    const uint32_t tank_n = pa_rev_tank_samples();

    res_t r = { 0, 0, -200.0, 0.0, 0.0, -200.0, 0.0 };
    double y1 = 0.0, y2 = 0.0, hsum = 0.0, esum = 0.0;
    const int play = play_s * FS, total = (play_s + tail_s) * FS;
    int32_t buf[B];
    double prev = 0.0, lvl = 1e-9;                   /* running |sample| level for click detection */
    double fsum = 0.0, fdc = 0.0; long fn = 0;
    double wsum = 0.0; long wn = 0; int w = 0;
    for (int t0 = 0; t0 < total; t0 += B) {
        for (int i = 0; i < B; i++) buf[i] = (t0 + i < play) ? (int32_t)(note(t0 + i) * Q21) : 0;
        nd->run(nd, buf, B);
        /* 'clipped' = a tank sample beyond +-1.0 full scale: the old int16 tank pinned exactly there */
        for (uint32_t k = 0; k < tank_n; k++) {
            if (tank[k] >= (1 << 21) || tank[k] <= -(1 << 21)) r.clip++;
            double a = fabs((double)tank[k]) / Q21;
            if (a > r.tank_max) r.tank_max = a;
        }
        for (int i = 0; i < B; i++) {
            int t = t0 + i;
            double y = buf[i] / Q21;
            if (fabs(y) > r.peak) r.peak = fabs(y);
            double d = fabs(y - prev);
            prev = y;
            if (t > play + FS / 10 && d > 0.002 && d > 8.0 * lvl) r.clicks++;
            lvl = 0.999 * lvl + 0.001 * fabs(y);
            if (t >= play + 4 * FS && t < play + 5 * FS) { fsum += y * y; fdc += y; fn++; }
            /* roughness of the tail 0.25..1.25 s after the input stops: energy of the
             * second difference relative to the signal energy. A damped reverb tail is
             * smooth; read-pointer jumps and wrap errors add broadband HF. */
            if (t >= play + FS / 4 && t < play + FS + FS / 4) {
                double d2 = y - 2.0 * y1 + y2;
                hsum += d2 * d2; esum += y * y;
            }
            y2 = y1; y1 = y;
            if (win_db && t < play) {
                wsum += y * y; wn++;
                if (wn == FS / 2 && w < nwin) { win_db[w++] = 10.0 * log10(wsum / wn + 1e-20); wsum = 0; wn = 0; }
            }
        }
    }
    if (fn) { r.floor_db = 10.0 * log10(fsum / fn + 1e-20); r.dc = fdc / fn; }
    if (esum > 0) r.hf_db = 10.0 * log10(hsum / esum + 1e-20);
    nd->destroy(nd);
    return r;
}

int main(int argc, char **argv) {
    setvbuf(stdout, NULL, _IONBF, 0);
    const int strict = !(argc > 1 && strcmp(argv[1], "--report") == 0);
    const int32_t knob_max = (int32_t)Q21, mix_hi = (int32_t)(0.9 * Q21);

    printf("type      decay  over1.0(samples)   clicks  floor 4-5s after   dc        peak  tail HF\n");
    for (int t = 0; t < 6; t++) {
        res_t hi  = run_type(t, knob_max, mix_hi, 2, 6, NULL, 0);
        res_t mid = run_type(t, (int32_t)(0.7 * Q21), mix_hi, 2, 6, NULL, 0);
        printf("%-8s  max   %18ld  %6ld  %8.1f dBFS     %+.2e  %.2f  %6.1f dB\n", k_names[t], hi.clip, hi.clicks, hi.floor_db, hi.dc, hi.peak, hi.hf_db);
        printf("%-8s  0.7   %18ld  %6ld  %8.1f dBFS     %+.2e  %.2f  %6.1f dB\n", k_names[t], mid.clip, mid.clicks, mid.floor_db, mid.dc, mid.peak, mid.hf_db);
        if (strict) {
            CHECK(hi.tank_max < 64.0, "%s at max decay: the tank stays far from saturation (max |tank| %.2f FS, limit 1024)",
                  k_names[t], hi.tank_max);
            CHECK(hi.clicks == 0, "%s at max decay: no discontinuities in the tail (%ld)", k_names[t], hi.clicks);
            CHECK(fabs(hi.dc) < 1e-3, "%s: no DC left in the tail (%.2e)", k_names[t], hi.dc);
            CHECK(hi.peak < 2.0, "%s: output stays bounded (peak %.2f)", k_names[t], hi.peak);
        }
    }

    /* Shimmer must keep working while you play: level per 0.5 s window over 8 s */
    double win[16];
    for (int i = 0; i < 16; i++) win[i] = -200.0;
    res_t sh = run_type(4, (int32_t)(0.7 * Q21), mix_hi, 8, 1, win, 16);
    printf("\nShimmer level per 0.5 s while playing (dBFS):");
    for (int i = 0; i < 16; i++) printf(" %.0f", win[i]);
    printf("\n");
    if (strict) {
        double first = win[2], last = win[15];
        CHECK(last > first - 6.0, "Shimmer keeps sounding: last window %.1f dB vs %.1f dB at 1 s", last, first);
        CHECK(fabs(sh.dc) < 1e-3, "Shimmer: no DC build-up (%.2e)", sh.dc);
        CHECK(sh.tank_max < 4.0, "Shimmer: the feedback loop stays bounded (max |tank| %.2f FS after 8 s)", sh.tank_max);
    }
    /* Shimmer must shift UP an octave, CLEANLY. Feed a steady 220 Hz tone and measure the
     * output (Goertzel) at 440 Hz (the octave) and at 330 Hz (not an octave), against Hall,
     * which has the same combs and no shifter:
     *   - the octave must stand well above Hall's (the shifter works), and
     *   - 330 Hz must stay where Hall's is (no broadband junk).
     * The old code failed the second: its saturated loop put +11 dB of junk at 330 Hz. */
    {
        double lv[2][2];                               /* [Hall, Shimmer][440, 330] */
        const int types[2] = { 1, 4 };
        const double fr[2] = { 440.0, 330.0 };
        for (int k = 0; k < 2; k++) {
            for (int f = 0; f < 2; f++) {
                fx_node_t *nd = fx_reverb_new();
                nd->create(nd);
                nd->set_param(nd, 10, types[k]);
                nd->set_param(nd, 0, (int32_t)(0.7 * Q21));
                nd->set_param(nd, 2, (int32_t)(0.3 * Q21));
                nd->set_param(nd, 3, (int32_t)(1.0 * Q21));      /* wet only */
                int32_t buf[B];
                double c = 2.0 * cos(2.0 * 3.14159265358979 * fr[f] / FS), s1 = 0, s2 = 0;
                for (int t0 = 0; t0 < 4 * FS; t0 += B) {
                    for (int i = 0; i < B; i++)
                        buf[i] = (int32_t)(0.4 * sin(2.0 * 3.14159265358979 * 220.0 * (t0 + i) / FS) * Q21);
                    nd->run(nd, buf, B);
                    if (t0 >= 2 * FS)
                        for (int i = 0; i < B; i++) { double s0 = buf[i] / Q21 + c * s1 - s2; s2 = s1; s1 = s0; }
                }
                lv[k][f] = 10.0 * log10(s1 * s1 + s2 * s2 - c * s1 * s2 + 1e-30);
                nd->destroy(nd);
            }
        }
        printf("\n220 Hz in -> 440 Hz: Hall %.1f dB, Shimmer %.1f dB | 330 Hz: Hall %.1f dB, Shimmer %.1f dB\n",
               lv[0][0], lv[1][0], lv[0][1], lv[1][1]);
        if (strict) {
            CHECK(lv[1][0] > lv[0][0] + 12.0, "Shimmer adds the octave above (+%.1f dB over Hall at 440 Hz)",
                  lv[1][0] - lv[0][0]);
            CHECK(fabs(lv[1][1] - lv[0][1]) < 6.0, "Shimmer adds no junk off the octave (330 Hz: %+.1f dB vs Hall)",
                  lv[1][1] - lv[0][1]);
        }
    }

    if (strict) printf("\n%s\n", fails ? "FAILED" : "REVERB MODES OK");
    return strict && fails ? 1 : 0;
}
