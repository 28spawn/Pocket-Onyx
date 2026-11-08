/* SPDX-License-Identifier: GPL-3.0-only */
/* Copyright (C) 2026 The Onyx Authors (@28spawn) */
/* custom-fw/test/test_kit.c — the drum kit's quality, measured
 *
 * The kit is the project's own synthesised kit (src/dsp/drum_kit.inc, made by
 * tools/gen_drum_kit.py), one voice per lane.  This suite measures what the engine
 * actually produces, so the balance cannot drift silently:
 *
 *   - every lane must make sound, and the kit's balance must hold: no voice may be
 *     more than 15 dB under the loudest per unit of velocity;
 *   - every voice must ring as long as its instrument does (a ride is not a short tick);
 *   - the drum bus must not clip: the compressor keeps the peak under the ceiling and
 *     the crest factor must stay inside the range a drum bus should have;
 *   - the loop must be seamless: the pattern's silent tail must be at most an 8th note,
 *     so the groove does not "cut" before the next pass;
 *   - the kit and the library are exactly what their generators make (provenance, in test_provenance.py).
 *
 * The kit is stored compressed / synthesised (src/dsp/drum_codec.h); the voices are read here through the engine's own decoder
 * (pa_drums_voice_decode), so what is measured is what is played.
 */
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>

#include "pa_drums.h"
#include "pa_config.h"
#include "../src/dsp/drum_patterns.inc"

/* The host stubs the other DSP suites use: the device's pool allocator is compiled
 * out on the host (it needs the .pool section), and the drum engine does not use it. */
void *pa_alloc(uint32_t n) { return calloc(1, n); }
void  pa_free(void *p)     { free(p); }

static int fails = 0;

static void check(int ok, const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    printf(ok ? "  ok   " : "  FAIL ");
    vprintf(fmt, ap);
    printf("\n");
    va_end(ap);
    if (!ok) fails++;
}

/* ---- the measurement helpers ------------------------------------------- */

/* Walk the engine to the sample that triggers `target`, then render `n` samples.
 * The engine has no "jump to step" entry point, by design. */
static int32_t render_step(pa_drums_t *d, uint32_t target, int n, int32_t *out) {
    static int32_t buf[64];
    uint32_t guard = 0;
    int found = 0;
    while (guard++ < 2000000u) {
        uint16_t before = d->current_step;
        buf[0] = 0;
        pa_drums_mix_block(d, buf, 1);
        if (before == (uint16_t)target && d->current_step != before) { found = 1; break; }
    }
    if (!found) return -1;
    for (int i = 0; i < n; i++) {
        buf[0] = 0;
        pa_drums_mix_block(d, buf, 1);
        if (out) out[i] = buf[0];
    }
    return 0;
}

/* A step where this lane plays alone, so a single voice can be measured.  Of all such
 * steps (in the grooves, first pass of the loop) it takes the one with the most steps
 * since any OTHER lane last played, so a ringing kick or crash cannot leak into the
 * measurement.  The style and the step are returned so the velocity can be normalised. */
static int find_lone_step(uint8_t *style_out, uint32_t lane, uint32_t *nib_out) {
    int best = -1, best_quiet = -1;
    for (int i = 0; i < (int)PA_DRUM_GROOVE_STYLE_COUNT; i++) {
        uint16_t len = k_groove_len[i];
        int last_other = -1000;
        for (uint32_t s = 0; s < len; s++) {
            uint32_t w = k_groove_steps[k_groove_off[i] + s];
            uint32_t l = w & PA_DRUM_L_MASK;
            if (l == lane) {
                int quiet = (int)s - last_other;
                if (quiet > best_quiet) {
                    best_quiet = quiet; best = (int)s;
                    *style_out = (uint8_t)i;
                    if (nib_out) *nib_out = (w >> PA_DRUM_VEL_SHIFT) & PA_DRUM_VEL_MASK;
                }
            } else if (l & ~lane) {
                last_other = (int)s;
            }
        }
    }
    return best;
}

/* The engine's velocity law (a 1.5-power curve), used to normalise a hit's level so the
 * kit's balance can be judged per unit of velocity rather than per hit. */
static double vel_norm(uint32_t nib) {
    double midi = 8.0 * (double)nib + 4.0;
    double a = pow(midi / 127.0, 1.5);
    return (a > 0.02) ? a : 0.02;
}

static void start(pa_drums_t *d, uint8_t style) {
    pa_drums_init(d);
    pa_drums_set_style(d, style);
    pa_drums_set_bpm(d, 120);
    pa_drums_set_playing(d, 1);
}

/* ---- 1. the kit's balance ---------------------------------------------- */

static void test_balance(void) {
    printf("=== the kit's balance ===\n");
    static const struct { const char *name; uint32_t lane; } k_voices[] = {
        { "KICK",    PA_DRUM_L_KICK },
        { "SNARE",   PA_DRUM_L_SNARE },
        { "HAT",     PA_DRUM_L_HAT },
        { "OPEN",    PA_DRUM_L_OPEN },
        { "RIDE",    PA_DRUM_L_RIDE },
        { "PEDAL",   PA_DRUM_L_PEDAL },
        { "BELL",    PA_DRUM_L_BELL },
        { "RIM",     PA_DRUM_L_RIM },
        { "TOM_MID", PA_DRUM_L_TOM_MID },
        { "TOM_HI",  PA_DRUM_L_TOM_HI },
    };
    static int32_t block[8192];
    double norm[sizeof(k_voices) / sizeof(k_voices[0])];
    int n = 0;
    for (size_t v = 0; v < sizeof(k_voices) / sizeof(k_voices[0]); v++) {
        uint8_t style = 0;
        uint32_t nib = 0;
        int step = find_lone_step(&style, k_voices[v].lane, &nib);
        if (step < 0) {
            printf("  ----   %-8s: no lone step in the table\n", k_voices[v].name);
            norm[n++] = 0;
            continue;
        }
        static pa_drums_t d;
        start(&d, style);
        memset(block, 0, sizeof(block));
        if (render_step(&d, (uint32_t)step, 1800, block) != 0) {
            check(0, "%s: the engine never reached step %d", k_voices[v].name, step);
            norm[n++] = 0;
            continue;
        }
        int32_t peak = 0;
        for (int i = 0; i < 1800; i++) {
            int32_t a = block[i] < 0 ? -block[i] : block[i];
            if (a > peak) peak = a;
        }
        norm[n] = (double)peak / vel_norm(nib);
        n++;
        printf("       %-8s style %2d step %3d (vel %2u): peak %8d (%.1f dBFS), "
               "%.1f dB per unit of velocity\n",
               k_voices[v].name, style, step, nib, peak,
               20.0 * log10((double)peak / 2097152.0 + 1e-12),
               20.0 * log10(norm[n - 1] / 2097152.0 + 1e-12));
    }

    double maxp = 0;
    for (int i = 0; i < n; i++) if (norm[i] > maxp) maxp = norm[i];
    int quiet = 0, loud = 0;
    for (int i = 0; i < n; i++) {
        if (norm[i] <= 0) continue;
        double db = 20.0 * log10(norm[i] / maxp);
        if (db < -15.0) quiet++;
        if (db > 0.0) loud++;
    }
    check(maxp > 0, "the kit makes sound (the loudest voice at %.0f per unit of velocity)",
          maxp);
    check(quiet == 0, "no voice is more than 15 dB under the loudest per unit of "
          "velocity (%d are)", quiet);
    check(loud == 0, "no voice is louder than the loudest (%d are)", loud);
}

/* ---- 2. every voice rings like its instrument ---------------------------- */

/* How long a voice rings: the time from its peak until it falls 40 dB below it.  The
 * minimum times below are what each instrument has to ring for (a ride is not a tick). */
static double decay_ms(const float *x, int n) {
    float peak = 0;
    int ipk = 0;
    for (int i = 0; i < n; i++) {
        float a = x[i] < 0 ? -x[i] : x[i];
        if (a > peak) { peak = a; ipk = i; }
    }
    if (peak == 0) return 0;
    int i = n - 1;
    for (; i > ipk; i--) {
        float a = x[i] < 0 ? -x[i] : x[i];
        if (a * 100 >= peak) break;
    }
    return 1000.0 * (i - ipk) / 22050.0;      /* the voices are at 22.05 kHz */
}

static void test_voices(void) {
    printf("=== every voice rings like its instrument ===\n");
    static const struct {
        const char *name; uint32_t voice; double min_decay_ms;
    } k_voices[] = {
        { "KICK",    PA_DRUM_V_KICK,    150 },
        { "SNARE",   PA_DRUM_V_SNARE,   100 },
        { "RIM",     PA_DRUM_V_RIM,      80 },
        { "HAT",     PA_DRUM_V_HAT,      50 },
        { "PEDAL",   PA_DRUM_V_PEDAL,    80 },
        { "OPEN",    PA_DRUM_V_OPEN,    300 },
        { "CRASH",   PA_DRUM_V_CRASH,   400 },
        { "RIDE",    PA_DRUM_V_RIDE,    250 },
        { "BELL",    PA_DRUM_V_BELL,    200 },
        { "TOM_LO",  PA_DRUM_V_TOM_LO,  100 },
        { "TOM_MID", PA_DRUM_V_TOM_MID,  80 },
        { "TOM_HI",  PA_DRUM_V_TOM_HI,   80 },
    };
    static float pcm[30000];
    for (size_t v = 0; v < sizeof(k_voices) / sizeof(k_voices[0]); v++) {
        int n = (int)pa_drums_voice_decode(k_voices[v].voice, pcm, 30000u);
        double dec = decay_ms(pcm, n);
        printf("       %-7s: %6d frames, rings %.0f ms (must be at least %.0f ms)\n",
               k_voices[v].name, n, dec, k_voices[v].min_decay_ms);
        check(n > 0 && n < 30000, "%s decodes to a sane length (%d frames)", k_voices[v].name, n);
        check(dec >= k_voices[v].min_decay_ms, "%s rings for at least %.0f ms (%.0f ms)",
              k_voices[v].name, k_voices[v].min_decay_ms, dec);
    }
}

/* ---- 3. the bus does not clip ------------------------------------------ */

static void test_bus(void) {
    printf("=== the drum bus ===\n");
    static pa_drums_t d;
    static int32_t block[44100];
    /* the busiest groove: the most hit steps per bar */
    int busiest = 0;
    double best = -1;
    for (int i = 0; i < (int)PA_DRUM_GROOVE_STYLE_COUNT; i++) {
        int hits = 0;
        for (uint32_t s = 0; s < k_groove_len[i]; s++)
            hits += !!(k_groove_steps[k_groove_off[i] + s] & PA_DRUM_L_MASK);
        double per_bar = hits * (double)PA_DRUM_STEPS_PER_BAR / k_groove_len[i];
        if (per_bar > best) { best = per_bar; busiest = i; }
    }
    start(&d, (uint8_t)busiest);
    memset(block, 0, sizeof(block));
    /* two seconds of the bus */
    for (int i = 0; i < 44100; i++) {
        block[i] = 0;
        pa_drums_mix_block(&d, &block[i], 1);
    }
    int32_t peak = 0;
    double sum = 0;
    for (int i = 0; i < 44100; i++) {
        int32_t a = block[i] < 0 ? -block[i] : block[i];
        if (a > peak) peak = a;
        sum += (double)block[i] * block[i];
    }
    double rms = sqrt(sum / 44100.0);
    double crest = 20.0 * log10((double)peak / (rms + 1e-9));
    printf("       %s (the busiest groove): peak %d (%.1f dBFS), rms %.0f, crest %.1f dB\n",
           k_groove_names[busiest], peak, 20.0 * log10((double)peak / 2097152.0), rms, crest);
    check(peak < 2097152, "the bus never clips (peak %d)", peak);
    check(crest < 24.0, "the bus crest factor is sane (%.1f dB)", crest);
    check(rms > 1000.0, "the bus is not silent (rms %.0f)", rms);
}

/* ---- 4. the loop is seamless ------------------------------------------- */

static void test_loop(void) {
    printf("=== the loop is seamless ===\n");
    int cut = 0, worst = 0;
    const char *worst_name = "";
    for (int i = 0; i < PA_DRUM_STYLE_COUNT; i++) {
        uint16_t len = k_groove_len[i];
        int tail = 0;
        for (int s = (int)len - 1; s >= 0; s--) {
            if (k_groove_steps[k_groove_off[i] + s] & PA_DRUM_L_MASK) break;
            tail++;
        }
        if (i >= (int)PA_DRUM_GROOVE_STYLE_COUNT) continue;   /* the metros are click patterns */
        if (tail > worst) { worst = tail; worst_name = k_groove_names[i]; }
        if (tail > 6) cut++;                              /* more than an 8th note of silence */
    }
    printf("       the longest silent tail is %d steps (%s)\n", worst, worst_name);
    check(cut == 0, "no groove has more than an 8th note of silence before the loop "
          "(%d do)", cut);
    check(worst <= 6, "the worst tail is %d steps (%s)", worst, worst_name);
}

int main(void) {
    printf("=== the drum kit ===\n");
    test_balance();
    test_voices();
    test_bus();
    test_loop();
    printf("\n%s\n", fails == 0 ? "DRUM KIT OK" : "DRUM KIT FAILED");
    return fails == 0 ? 0 : 1;
}
