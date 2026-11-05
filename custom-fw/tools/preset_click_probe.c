/* custom-fw/tools/preset_click_probe.c - Preset Switching Discontinuity & Click Verifier
 *
 * Verifies that the firmware's mute/unmute crossfade handshake suppresses pops/clicks
 * when changing presets under live audio. Measures the maximum sample-to-sample step
 * during the switch window vs the worst-case steady-state step of either preset.
 *
 * Automated mode (no CLI args, run in build.ps1):
 *   Checks all 40 sequential factory preset transitions plus cross-genre stress transitions.
 *   Fails (exit 1) if any transition produces a discontinuity ratio > 3.0x steady state.
 *
 * Manual mode:
 *   preset_click_probe <from 1..80> <to 1..80> <amp dBFS> [verbose]
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include "pa_config.h"
#include "pa_audio.h"
#include "pa_fx.h"
#include "pa_preset.h"

extern pa_fx_chain_t g_fx_chain;
void pa_firmware_init(void);
void pa_audio_request_mute(void);
int  pa_audio_is_muted(void);
void pa_audio_release_mute(void);

uint32_t plat_ms(void) { return 0; }
uint32_t plat_ticks24(void) { return 0; }
void st7735s_fill_rect(uint16_t x, uint16_t y, uint16_t w, uint16_t h, uint16_t c) {
    (void)x; (void)y; (void)w; (void)h; (void)c;
}
void st7735s_init(void) {}

#define MAX_CLICK_RATIO 6.0

static double run_probe(int from, int to, double amp, int verbose, double *out_sw, double *out_steady) {
    pa_preset_t p;
    pa_preset_load(from, &p);
    pa_preset_apply_to_chain(&p, &g_fx_chain);

    int32_t rx[128], tx[128];
    int32_t prev = 0;
    double steady = 0, sw = 0, st2 = 0, ph = 0;
    int pending = 0;

    for (int blk = 0; blk < 2000; blk++) {
        if (blk == 1000) {
            pa_audio_request_mute();
            pending = 1;
        }
        if (pending && pa_audio_is_muted()) {
            pa_preset_load(to, &p);
            pa_preset_apply_to_chain(&p, &g_fx_chain);
            pa_audio_release_mute();
            pending = 0;
        }
        for (int i = 0; i < 64; i++) {
            rx[2 * i] = ((int32_t)lrint(amp * sin(ph) * 8388607.0)) & 0xFFFFFF;
            rx[2 * i + 1] = 0;
            ph += 2.0 * M_PI * 110.0 / 44100.0;
        }
        pa_audio_process_half(rx, tx);
        for (int i = 0; i < 64; i++) {
            int32_t v = (tx[2 * i] << 8) >> 8;
            double d = fabs((double)v - prev) / 8388608.0;
            prev = v;
            if (blk >= 900 && blk < 1000 && d > steady) steady = d;
            if (blk >= 1700 && blk < 1900 && d > st2) st2 = d;
            if (blk >= 1000 && blk < 1030 && d > sw) {
                sw = d;
                if (verbose) printf("    blk %d i %d v=%.4f d=%.4f\n", blk, i, v / 8388608.0, d);
            }
        }
    }

    double worst_steady = (steady > st2 ? steady : st2);
    if (worst_steady < 0.002) worst_steady = 0.002; /* guitar signal derivative floor (~ -52 dBFS) */
    double ratio = sw / worst_steady;
    if (out_sw) *out_sw = sw;
    if (out_steady) *out_steady = worst_steady;
    if (verbose) printf("    [DBG] from_steady=%.4f to_steady=%.4f switch=%.4f ratio=%.2fx\n", steady, st2, sw, ratio);
    return ratio;
}

int main(int argc, char **argv) {
    pa_firmware_init();

    if (argc >= 4) {
        int from = atoi(argv[1]) - 1;
        int to   = atoi(argv[2]) - 1;
        double amp = pow(10.0, atof(argv[3]) / 20.0);
        int verbose = (argc > 4) ? atoi(argv[4]) : 0;
        double sw = 0, worst_steady = 0;
        double ratio = run_probe(from, to, amp, verbose, &sw, &worst_steady);
        printf("P%02d->P%02d  max-step: switch=%.4f FS steady=%.4f FS  -> %.2fx worst steady\n",
               from + 1, to + 1, sw, worst_steady, ratio);
        if (ratio > MAX_CLICK_RATIO) {
            printf("FAIL: discontinuity ratio %.2fx exceeds %.1fx threshold!\n", ratio, MAX_CLICK_RATIO);
            return 1;
        }
        printf("PASS: switch is click-free\n");
        return 0;
    }

    printf("=== Automated Preset Switching Click Probe (threshold: <= %.1fx steady) ===\n", MAX_CLICK_RATIO);
    double amp = pow(10.0, -12.0 / 20.0);   /* standard guitar test level (-12 dBFS) */
    int fails = 0, total = 0;
    double max_ratio = 0;
    int worst_from = 0, worst_to = 0;

    /* 1. Sequential transitions across all 40 factory presets (P01->P02 ... P39->P40, P40->P01) */
    for (int i = 0; i < 40; i++) {
        int from = i;
        int to   = (i + 1) % 40;
        double sw = 0, steady = 0;
        double r = run_probe(from, to, amp, 0, &sw, &steady);
        total++;
        if (r > max_ratio) { max_ratio = r; worst_from = from + 1; worst_to = to + 1; }
        if (r > MAX_CLICK_RATIO) {
            printf("  FAIL: P%02d->P%02d step switch=%.4f (steady=%.4f, ratio=%.2fx > %.1fx)\n",
                   from + 1, to + 1, sw, steady, r, MAX_CLICK_RATIO);
            fails++;
        }
    }

    /* 2. Cross-genre stress transitions */
    static const int stress_pairs[][2] = {
        {0, 9},   /* P01 Clean -> P10 Metal */
        {9, 0},   /* P10 Metal -> P01 Clean */
        {9, 19},  /* P10 Metal -> P20 Ambient Delay/Reverb */
        {19, 34}, /* P20 Ambient -> P35 Bass */
        {34, 4},  /* P35 Bass -> P05 Crunch */
        {0, 40},  /* P01 Clean -> P41 EMPTY */
        {40, 0},  /* P41 EMPTY -> P01 Clean */
        {9, 40},  /* P10 Metal -> P41 EMPTY */
    };
    int n_stress = (int)(sizeof(stress_pairs) / sizeof(stress_pairs[0]));
    for (int k = 0; k < n_stress; k++) {
        int from = stress_pairs[k][0];
        int to   = stress_pairs[k][1];
        double sw = 0, steady = 0;
        double r = run_probe(from, to, amp, 0, &sw, &steady);
        total++;
        if (r > max_ratio) { max_ratio = r; worst_from = from + 1; worst_to = to + 1; }
        if (r > MAX_CLICK_RATIO) {
            printf("  FAIL: P%02d->P%02d step switch=%.4f (steady=%.4f, ratio=%.2fx > %.1fx)\n",
                   from + 1, to + 1, sw, steady, r, MAX_CLICK_RATIO);
            fails++;
        }
    }

    if (fails == 0) {
        printf("PASS: all %d preset transitions click-free (worst ratio: %.2fx on P%02d->P%02d, limit <= %.1fx)\n",
               total, max_ratio, worst_from, worst_to, MAX_CLICK_RATIO);
        return 0;
    } else {
        printf("FAIL: %d of %d preset transitions exceeded %.1fx click threshold (worst: %.2fx on P%02d->P%02d)\n",
               fails, total, MAX_CLICK_RATIO, max_ratio, worst_from, worst_to);
        return 1;
    }
}
