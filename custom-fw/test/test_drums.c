/* SPDX-License-Identifier: GPL-3.0-only */
/* Copyright (C) 2026 The Onyx Authors (@28spawn) */
/* custom-fw/test/test_drums.c — the Groove Station drum engine
 *
 * Two halves:
 *   1. the generated pattern table (src/dsp/drum_patterns.inc, the project's own library
 *      written in tools/gen_drum_patterns.py) against the invariants the engine and the UI
 *      rely on: loop lengths that end on a 16th, up to 8 bars, a velocity on every hit,
 *      unique names - and the library's design rules: three voices per groove (kick,
 *      snare, one timekeeper), no crash or ride bell in a loop, the ride only for swing
 *      and jazz, toms in at most two grooves.
 *   2. the engine itself: the step clock must walk the WHOLE pattern and wrap at its own
 *      length (the Groove Station bug was a UI-side assumption that the step stopped at
 *      16, so this is the engine half of that regression), the bar-block mapping the strip
 *      uses must repeat every bar, and a louder step must actually come out louder.
 *
 *   clang -O2 -Icustom-fw/include -DPA_HOST_SIM=1 custom-fw/test/test_drums.c <g_dsp> -o t
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

/* ---- 1. the table ------------------------------------------------------- */

static void test_table(void) {
    const uint32_t steps = (uint32_t)(sizeof(k_groove_steps) / sizeof(k_groove_steps[0]));
    printf("=== the pattern table (%u steps, %u bytes) ===\n", steps, steps * 2u);

    uint32_t expect = 0;
    int bad_len = 0, bad_off = 0, bad_vel = 0, silent = 0, bad_lane = 0, too_long = 0;
    for (int i = 0; i < PA_DRUM_STYLE_COUNT; i++) {
        if (k_groove_off[i] != expect) bad_off++;
        uint16_t len = k_groove_len[i];
        expect += len;
        if (len == 0 || len % 3u != 0u || len > PA_DRUM_MAX_STEPS) bad_len++;
        if (len > 4u * PA_DRUM_STEPS_PER_BAR) too_long++;
        int hits = 0;
        for (uint32_t s = 0; s < len; s++) {
            uint32_t w = k_groove_steps[k_groove_off[i] + s];
            if (w & ~(PA_DRUM_L_MASK | (PA_DRUM_VEL_MASK << PA_DRUM_VEL_SHIFT))) bad_lane++;
            if ((w & PA_DRUM_L_MASK) == 0) continue;
            hits++;
            if (((w >> PA_DRUM_VEL_SHIFT) & PA_DRUM_VEL_MASK) == 0) bad_vel++;
        }
        if (!hits) silent++;
    }
    check(bad_off == 0, "the style offsets tile the table exactly");
    check(expect == steps, "every style's steps are inside the table (%u of %u)",
          expect, steps);
    check(bad_len == 0, "every loop length is a whole 16ths count, 1..%u steps",
          PA_DRUM_MAX_STEPS);
    check(silent == 0, "no style is silent");
    check(bad_vel == 0, "every hit carries a velocity");
    check(bad_lane == 0, "no step uses an undefined lane or velocity bit");
    printf("       %d of %d styles are longer than 4 bars\n", too_long, PA_DRUM_STYLE_COUNT);

    /* The library's design rules (tools/gen_drum_patterns.py): a groove is kick, snare and
     * ONE timekeeper; no crash or ride bell inside a loop; the ride belongs to swing / jazz;
     * toms in at most two grooves. */
    int three = 0, over4 = 0, n_crash = 0, n_bell = 0, ride_bad = 0, tom_styles = 0;
    for (int i = 0; i < (int)PA_DRUM_GROOVE_STYLE_COUNT; i++) {
        uint32_t used = 0;
        for (uint32_t s = 0; s < k_groove_len[i]; s++)
            used |= k_groove_steps[k_groove_off[i] + s] & PA_DRUM_L_MASK;
        int nv = 0;
        for (int l = 0; l < 12; l++) nv += !!(used & (1u << l));
        three += (nv == 3);
        over4 += (nv > 4);
        n_crash += !!(used & PA_DRUM_L_CRASH);
        n_bell  += !!(used & PA_DRUM_L_BELL);
        if ((used & PA_DRUM_L_RIDE) && strcmp(k_groove_names[i], "SWING") && strcmp(k_groove_names[i], "JAZZ"))
            ride_bad++;
        tom_styles += !!(used & (PA_DRUM_L_TOM_LO | PA_DRUM_L_TOM_MID | PA_DRUM_L_TOM_HI));
    }
    printf("       %d of %u grooves use exactly 3 voices; toms in %d\n",
           three, PA_DRUM_GROOVE_STYLE_COUNT, tom_styles);
    check(three * 10 >= (int)PA_DRUM_GROOVE_STYLE_COUNT * 9 && over4 == 0,
          "grooves are kick + snare + one timekeeper (%d of %u use 3 voices, %d use more than 4)",
          three, PA_DRUM_GROOVE_STYLE_COUNT, over4);
    check(n_crash == 0 && n_bell == 0, "no crash and no ride bell inside a loop (%d / %d grooves)",
          n_crash, n_bell);
    check(ride_bad == 0, "the ride is only used by SWING and JAZZ (%d others)", ride_bad);
    check(tom_styles <= 2, "toms appear in at most two grooves (%d)", tom_styles);
    int dup = 0;
    for (int i = 0; i < PA_DRUM_STYLE_COUNT; i++)
        for (int j = i + 1; j < PA_DRUM_STYLE_COUNT; j++)
            if (!strcmp(k_groove_names[i], k_groove_names[j])) dup++;
    check(dup == 0, "every style name is unique (%d duplicates)", dup);

    /* 8 BEAT 1: the kick on step 0, and it is an accent, not a ghost */
    uint32_t w0 = k_groove_steps[k_groove_off[0]];
    check((w0 & PA_DRUM_L_KICK) != 0, "8 BEAT 1 step 0 has the kick");
    check(((w0 >> PA_DRUM_VEL_SHIFT) & PA_DRUM_VEL_MASK) >= 10u, "8 BEAT 1 step 0 is an accent");
    /* the 16ths of bar 1: every hat in the first bar sits on a 16th (every 3rd step) */
    int grid_ok = 1, hat_16ths = 0;
    for (uint32_t s = 0; s < PA_DRUM_STEPS_PER_BAR; s++) {
        uint32_t w = k_groove_steps[s];
        if ((w & PA_DRUM_L_HAT) && s % 3u == 0u) hat_16ths++;
        if ((w & PA_DRUM_L_HAT) && s % 3u != 0u) grid_ok = 0;
    }
    check(grid_ok && hat_16ths > 0, "the hats of 8 BEAT 1 bar 1 land on the 16th grid (%d)",
          hat_16ths);
}

/* ---- 2. the engine ------------------------------------------------------ */

/* Advance to the sample that triggers step `target`, and return the peak of the next `n`
 * samples (including that trigger sample).  The engine has no "jump to step" entry point,
 * by design, so a test has to walk to the step it wants. */
static int32_t peak_at_step(pa_drums_t *d, uint32_t target, int n) {
    static int32_t buf[64];
    uint32_t guard = 0;
    int32_t peak = 0;
    int found = 0;
    while (guard++ < 2000000u) {
        uint16_t before = d->current_step;
        buf[0] = 0;
        pa_drums_mix_block(d, buf, 1);
        /* the sample that triggered `target` is the one where the step moved on from it */
        if (before == (uint16_t)target && d->current_step != before) {
            peak = buf[0] < 0 ? -buf[0] : buf[0];
            found = 1;
            break;
        }
    }
    if (!found) return -1;
    for (int i = 0; i < n; i++) {
        buf[0] = 0;
        pa_drums_mix_block(d, buf, 1);
        int32_t a = buf[0] < 0 ? -buf[0] : buf[0];
        if (a > peak) peak = a;
    }
    return peak;
}

static void start(pa_drums_t *d, uint8_t style) {
    pa_drums_init(d);
    pa_drums_set_style(d, style);
    pa_drums_set_bpm(d, 120);
    pa_drums_set_playing(d, 1);
}

static void test_engine(void) {
    static pa_drums_t drums;
    static int32_t buf[64];
    printf("=== the engine ===\n");

    /* The step clock walks the whole loop and wraps at the style's own length. */
    start(&drums, 0);                              /* 8 BEAT 1, 192 steps */
    uint16_t len = pa_drums_style_length(0);
    check(len == 192u, "8 BEAT 1 is four bars (%u steps)", len);

    static int seen[PA_DRUM_MAX_STEPS];
    memset(seen, 0, sizeof(seen));
    uint16_t prev = drums.current_step;
    uint16_t next_expected = (uint16_t)((prev + 1u) % len);
    int visits = 1, order_ok = 1, wraps = 0;
    seen[prev] = 1;
    for (uint32_t i = 0; i < (uint32_t)len * 64u && visits < (int)len; i++) {
        pa_drums_mix_block(&drums, buf, 64);
        if (drums.current_step != prev) {
            if (drums.current_step != next_expected) order_ok = 0;
            if (drums.current_step == 0) wraps++;
            prev = drums.current_step;
            next_expected = (uint16_t)((prev + 1u) % len);
            if (!seen[prev]) { seen[prev] = 1; visits++; }
        }
    }
    check(visits == (int)len, "the clock visits all %u steps (%u seen)", len, visits);
    check(order_ok, "the clock walks the steps in order");
    check(wraps == 0, "the whole 4-bar loop runs before it wraps");

    /* A shorter style must not leave the step outside the pattern. */
    /* the first 2-bar groove in the library */
    uint8_t two = 0;
    for (uint8_t i = 0; i < PA_DRUM_GROOVE_STYLE_COUNT; i++)
        if (pa_drums_style_length(i) == 96u) { two = i; break; }
    pa_drums_set_style(&drums, two);
    check(pa_drums_style_length(two) == 96u, "%s is two bars (%u steps)",
          pa_drums_get_style_name(two), pa_drums_style_length(two));
    int in_range = 1;
    for (int i = 0; i < 400; i++) {
        pa_drums_mix_block(&drums, buf, 64);
        if (drums.current_step >= pa_drums_style_length(two)) in_range = 0;
    }
    check(in_range, "switching to a 2-bar style keeps the step in range");

    /* The strip's bar block repeats every bar: this is what makes bar 2 look like bar 1,
     * and it is the fix for "the bar goes away after the 4th". */
    check(pa_drums_bar_block(0) == 0u && pa_drums_bar_block(3) == 1u &&
          pa_drums_bar_block(47) == 15u, "the strip blocks are 3 steps wide");
    int repeat = 1;
    for (uint16_t s = 0; s < PA_DRUM_STEPS_PER_BAR; s++) {
        if (pa_drums_bar_block(s) !=
            pa_drums_bar_block((uint16_t)(s + PA_DRUM_STEPS_PER_BAR))) repeat = 0;
    }
    check(repeat, "the bar block is identical in every bar of the loop");

    /* The step clock is exact: a four-bar loop must be four bars of wall-clock time, so
     * the groove cannot drift over a long session.  The loop's length in samples has to
     * match len * 220500 / BPM to within one sample. */
    {
        start(&drums, 0);
        uint64_t samples = 0;
        uint64_t first = 0, second = 0;
        int seen = 0;
        while (seen < 2 && samples < 4000000u) {
            uint16_t before = drums.current_step;
            pa_drums_mix_block(&drums, buf, 1);
            samples++;
            if (before == 0u && drums.current_step != before) {
                if (!seen) first = samples; else second = samples;
                seen++;
            }
        }
        double want = (double)len * 220500.0 / 120.0;
        double got = (double)(second - first);
        printf("       one loop = %.1f samples (expected %.1f, %.4f%% off)\n",
               got, want, 100.0 * (got - want) / want);
        check(fabs(got - want) <= 1.0, "the loop is exactly as long as the pattern "
              "(%.1f vs %.1f samples)", got, want);
    }

    /* Velocity: METRO 3 accents beat 1 (velocity 120) over three plain clicks (100).  The
     * metronome is the click voice alone and its click is 18 ms inside a 43 ms step at
     * 120 BPM, so nothing else can leak into the measurement. */
    pa_drums_t m;
    start(&m, PA_DRUM_METRO_4_4);
    int32_t accent = peak_at_step(&m, 0, 300);
    start(&m, PA_DRUM_METRO_4_4);
    int32_t plain = peak_at_step(&m, 12, 300);
    printf("       METRO 3 click: accent %d, plain %d\n", accent, plain);
    check(accent > 0 && plain > 0 && accent > plain * 5 / 4,
          "the accented metronome click is louder than the plain one");

    /* A kick step must be far louder than a lone closed-hat step: the kit's balance
     * (kick and snare on top, hats and cymbals under them). */
    start(&drums, 0);
    int32_t kick = peak_at_step(&drums, 0, 600);
    start(&drums, 0);
    int32_t hat  = peak_at_step(&drums, 3, 600);   /* step 3 of 8 BEAT 1: a lone hat */
    printf("       a lone hat peaks at %d, the kick at %d\n", hat, kick);
    check(kick > hat * 2 && hat > 0, "the kick is far above a lone closed hat");

    /* The strip follows the sound: on the very sample beat 2 is triggered (step 12), the
     * sounding step is 12, whose strip block is 4 - one of the tall downbeat blocks - and
     * one sample earlier it is still block 3. (current_step is the NEXT step: drawn from it,
     * the tall block lit a whole step, ~42 ms at 120 BPM, before the beat was heard.) */
    {
        static pa_drums_t a;
        start(&a, 0);
        int before_blk = -1, at_blk = -1, found = 0;
        for (uint32_t guard = 0; guard < 2000000u && !found; guard++) {
            uint16_t prev = a.current_step;
            int blk_prev = pa_drums_bar_block(pa_drums_sounding_step(&a));
            buf[0] = 0;
            pa_drums_mix_block(&a, buf, 1);
            if (prev == 12u && a.current_step == 13u) {        /* this sample triggered step 12 */
                before_blk = blk_prev;
                at_blk = pa_drums_bar_block(pa_drums_sounding_step(&a));
                found = 1;
            }
        }
        printf("       beat 2: strip block %d just before, %d on the beat\n", before_blk, at_blk);
        check(found && before_blk == 3 && at_blk == 4,
              "the tall block for beat 2 lights on the sample the beat is played");
    }

    /* Switching the pattern restarts the loop: until step 0 is triggered nothing is sounding, so the strip must not jump to the LAST block (it did: sounding = (0 + len - 1) % len). */
    {
        static pa_drums_t b;
        start(&b, 0);
        for (int i = 0; i < 20000; i++) { buf[0] = 0; pa_drums_mix_block(&b, buf, 1); }
        pa_drums_set_style(&b, 5);
        int worst = 0, flashed = 0;
        for (int i = 0; i < 40000 && b.armed; i++) {
            int blk = pa_drums_bar_block(pa_drums_sounding_step(&b));
            if (blk > worst) worst = blk;
            if (blk >= 12) flashed = 1;
            buf[0] = 0; pa_drums_mix_block(&b, buf, 1);
        }
        printf("       after a pattern change the strip block stays at %d until step 0 plays\n", worst);
        check(!flashed, "a pattern change does not flash the last strip blocks");
    }

    /* Nothing plays while stopped. */
    static pa_drums_t stopped;
    pa_drums_init(&stopped);
    pa_drums_set_playing(&stopped, 0);
    int32_t nonzero = 0;
    for (int i = 0; i < 64; i++) {
        memset(buf, 0, sizeof(buf));
        pa_drums_mix_block(&stopped, buf, 64);
        for (int j = 0; j < 64; j++) if (buf[j] != 0) nonzero = 1;
    }
    check(nonzero == 0, "a stopped engine is silent");
}

int main(void) {
    printf("=== Groove Station drum engine ===\n");
    test_table();
    test_engine();
    printf("\n%s\n", fails == 0 ? "DRUM ENGINE OK" : "DRUM ENGINE FAILED");
    return fails == 0 ? 0 : 1;
}
