/* SPDX-License-Identifier: GPL-3.0-only */
/* Copyright (C) 2026 The Onyx Authors (@28spawn) */
/* test/test_reverb.c - Reverb tank layout + decay law regression tests.
 *
 * Two things used to be silently wrong here, and neither of them clicks:
 *
 *  1. THE TANK WAS OVER-SUBSCRIBED. The comb/allpass pointers walked
 *     comb_buf[0..3] (4 x 2200) then ap_buf[0..2] (3 x 600) = 10,600 samples,
 *     and then the shimmer was hard-placed at 10,240 - INSIDE the third
 *     allpass (10,000..10,599). Shimmer and allpass data overwrote each other
 *     and the Shimmer path was corrupted for every type.
 *
 *  2. THE DECAY TABLE ASSUMED A PER-SAMPLE GAIN. reverb_run_stereo writes
 *     comb_buf[c][idx] and reads it back D samples later, so the feedback gain
 *     g is applied once per comb period D. The table used
 *     g = 10^(-3 / (RT60 * 44100)), which is the per-sample law, and produced
 *     multi-minute tails with a decay knob that did nothing audible.
 *
 * The correct law, which this suite measures on the shipped code:
 *
 *      RT60 = (3 / |log10 g|) * D / 44100  seconds
 *
 * Sections:
 *   [1] tank plan: every region inside the tank, pairwise disjoint, strides
 *       never shorter than the ring they hold, frozen comb tables.
 *   [2] realized pointers of a created node match the plan (catches an
 *       allocation edit that forgot to move a pointer).
 *   [3] sentinel: distinct values written into every region, then read back.
 *   [4] decay gain mapping: g = 0.55 + 0.41 * knob, within 1 Q21 count.
 *   [5] measured RT60 per type: default, knob minimum, knob maximum.
 *   [6] no tail longer than 10 s at the maximum setting.
 *
 * Build/run (build.ps1 does both):
 *   clang -O2 -Wall -Wextra -Icustom-fw/include -DPA_HOST_SIM=1 \
 *         custom-fw/test/test_reverb.c <host_dsp sources> -o test_reverb.exe
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include "pa_config.h"
#include "pa_model.h"
#include "pa_fx.h"

/* The host link supplies its own allocator (pa_pool.c is compiled out under
 * PA_HOST_SIM unless PA_POOL_TEST is set) - same as test_dsp_chain.c. */
void *pa_alloc(uint32_t n) { return calloc(1, n); }
void  pa_free(void *p)     { free(p); }

#define CHECK(cond, ...) do {                                   \
        if (!(cond)) {                                          \
            printf("  FAIL: "); printf(__VA_ARGS__); printf("\n"); \
            fails++;                                            \
        }                                                       \
    } while (0)

#define SR_HZ        44100.0
#define MAX_RT60_MEAS 12.0             /* seconds captured per impulse */

static int fails = 0;

/* The four comb tables, frozen. Duplicated here on purpose: if someone edits
 * k_*_combs in fx_reverb.c, the decay table / documented RT60 must be revisited
 * and this test must be updated deliberately, not silently. */
static const uint16_t k_ref_combs[6][4] = {
    { 1167, 1212, 1118, 1067 },   /* Room   (1.13: 1.5 x the old table) */
    { 1557, 1617, 1491, 1422 },   /* Hall   */
    { 1557, 1617, 1491, 1422 },   /* Swell  (Hall voicing) */
    { 1874, 2196, 1245, 1475 },   /* Spring (1.13: 1.85 x the old table) */
    { 1557, 1617, 1491, 1422 },   /* Shimmer (Hall voicing) */
    { 1993, 2111, 1879, 1789 },   /* Cloud  */
};
static const char *k_type_name[7] = { "Room", "Hall", "Swell", "Spring", "Shimmer", "Cloud", "Plate" };

#define REGION_MAX 8

/* ------------------------------------------------------------------ [1] [2] */
static void test_layout(fx_node_t *nd) {
    const uint32_t tank = pa_rev_tank_samples();
    printf("[1/2] tank plan (%u samples, %u B) + realized pointers\n",
           (unsigned)tank, (unsigned)(tank * sizeof(int32_t)));
    CHECK(tank == 12648u, "tank is %u samples, expected 12648 (int32 Q21)", (unsigned)tank);

    for (int t = 0; t < 6; t++) {
        uint32_t off[REGION_MAX], len[REGION_MAX];
        uint32_t soff[REGION_MAX], slen[REGION_MAX];

        nd->set_param(nd, 10, t);                    /* ask for this type; the audio thread (or reset) makes it real */
        nd->reset(nd);                               /* realize it: nothing runs the node in this test */
        uint32_t n  = pa_rev_regions(t, off, len, REGION_MAX);
        uint32_t sn = pa_rev_state_regions(nd, soff, slen, REGION_MAX);

        CHECK(n == 8u,  "%s: plan has %u regions, expected 4 combs + 3 allpass + shimmer",
                        k_type_name[t], (unsigned)n);
        CHECK(sn == 8u, "%s: realized node has %u regions", k_type_name[t], (unsigned)sn);
        if (n != 8u || sn != 8u) continue;

        for (uint32_t r = 0; r < n; r++) {
            CHECK(off[r] == soff[r], "%s region %u: plan offset %u, realized %u",
                  k_type_name[t], (unsigned)r, (unsigned)off[r], (unsigned)soff[r]);
            CHECK(len[r] == slen[r], "%s region %u: plan length %u, realized %u",
                  k_type_name[t], (unsigned)r, (unsigned)len[r], (unsigned)slen[r]);
            CHECK(len[r] > 0u,       "%s region %u is empty", k_type_name[t], (unsigned)r);
            CHECK(off[r] + len[r] <= tank,
                  "%s region %u [%u..%u) runs past the %u-sample tank",
                  k_type_name[t], (unsigned)r, (unsigned)off[r],
                  (unsigned)(off[r] + len[r]), (unsigned)tank);
        }

        /* The comb tables are the D the decay law depends on. */
        for (int c = 0; c < 4; c++) {
            CHECK(len[c] == k_ref_combs[t][c],
                  "%s comb %d is %u samples, the frozen table says %u - "
                  "re-derive the DECAY comment/RT60 in fx_reverb.c",
                  k_type_name[t], c, (unsigned)len[c], (unsigned)k_ref_combs[t][c]);
        }

        /* Disjoint rings. Regions are ordered by offset, so the stride a region
         * may occupy is the distance to the next region. */
        for (uint32_t a = 0; a < n; a++) {
            for (uint32_t b = a + 1; b < n; b++) {
                int overlap = !(off[a] + len[a] <= off[b] || off[b] + len[b] <= off[a]);
                CHECK(!overlap,
                      "%s: region %u [%u..%u) overlaps region %u [%u..%u)",
                      k_type_name[t], (unsigned)a, (unsigned)off[a],
                      (unsigned)(off[a] + len[a]), (unsigned)b,
                      (unsigned)off[b], (unsigned)(off[b] + len[b]));
            }
        }
    }
}

/* --------------------------------------------------------------------- [3] */
static void test_sentinel(fx_node_t *nd) {
    printf("[3] sentinel: distinct pattern per region, read back\n");
    int32_t *base = pa_rev_tank_base(nd);
    CHECK(base != 0, "node has no tank");

    for (int t = 0; t < 6; t++) {
        uint32_t off[REGION_MAX], len[REGION_MAX];
        nd->set_param(nd, 10, t);
        nd->reset(nd);                               /* realize the requested type (nothing runs the node here) */
        uint32_t n = pa_rev_state_regions(nd, off, len, REGION_MAX);
        if (n == 0u) { CHECK(0, "no regions for %s", k_type_name[t]); continue; }

        for (uint32_t r = 0; r < n; r++)
            for (uint32_t i = 0; i < len[r]; i++)
                base[off[r] + i] = (int32_t)(0x1000 + (int)r);

        for (uint32_t r = 0; r < n; r++) {
            uint32_t bad = 0;
            for (uint32_t i = 0; i < len[r]; i++)
                if (base[off[r] + i] != (int32_t)(0x1000 + (int)r)) bad++;
            CHECK(bad == 0u, "%s: region %u had %u/%u samples overwritten by "
                             "another region", k_type_name[t], (unsigned)r,
                  (unsigned)bad, (unsigned)len[r]);
        }
    }
    nd->reset(nd);
}

/* --------------------------------------------------------------------- [4] */
/* Returns the Q21 gain the node produced for a knob value (0..PA_Q21_ONE). */
static void test_decay_mapping(fx_node_t *nd) {
    printf("[4] decay knob (Hall): RT60 law 0.3 / 0.6 / 1.7 / 3.0 / 4.5 s at 0 / 20 / 50 / 80 / 100 %%, g = 10^(-0.745 * 3 * D / 44100 / RT60), within 2 Q21 counts\n");
    nd->set_param(nd, 10, 1);      /* Hall: the feedback depends on the type, and the type is applied at the next reset / block */
    nd->reset(nd);
    int32_t prev = -1;
    for (int pct = 0; pct <= 100; pct += 5) {
        int32_t val  = (int32_t)(((int64_t)pct * PA_Q21_ONE) / 100);
        double  v    = (double)val / (double)PA_Q21_ONE;
        static const double kn[5] = { 0.0, 0.2, 0.5, 0.8, 1.0 }, rt5[5] = { 0.3, 0.6, 1.7, 3.0, 4.5 };
        int ki = 0; while (ki < 3 && v > kn[ki + 1]) ki++;
        const double rt = rt5[ki] * pow(rt5[ki + 1] / rt5[ki], (v - kn[ki]) / (kn[ki + 1] - kn[ki]));
        double gw = pow(10.0, -0.745 * 3.0 * (0.25 * (1557 + 1617 + 1491 + 1422) / 44100.0) / rt);
        if (gw < 0.55) gw = 0.55;
        if (gw > 0.975) gw = 0.975;
        int32_t want = (int32_t)llround(gw * (double)PA_Q21_ONE);
        nd->set_param(nd, 0, val);
        int32_t got = pa_rev_decay_gain(nd);
        CHECK(abs((int)(got - want)) <= 2,
              "knob %d%%: gain %d, the mapping says %d (diff %d)", pct,
              (int)got, (int)want, (int)(got - want));
        CHECK(got > prev, "knob %d%%: gain %d is not above the previous %d",
              pct, (int)got, (int)prev);
        prev = got;
    }
}

/* ------------------------------------------------------------------ [5] [6] */
static double theory_rt60(const uint16_t *combs, double g) {
    double sum = 0.0;
    for (int c = 0; c < 4; c++) sum += 3.0 * (double)combs[c] / (fabs(log10(g)) * SR_HZ);
    return sum / 4.0;
}

/* Measure RT60 of the shipped code with an impulse. Tone (HF damping) is at 0
 * and Mix at 100 %, so the only loss in the loop is g itself - that is what the
 * law describes.
 *
 * Two details of the measurement matter:
 *  - The wet high-pass is a Q21 one-pole. Its rounding leaves a static DC
 *    offset (up to ~100 Q21, about -86 dBFS) which the comb bank then amplifies
 *    by 1/(1-g). With Tone at 0 the decay is frequency-independent, so the
 *    analysis differentiates each sample instead of subtracting a DC estimate:
 *    that removes the offset completely without biasing the slope.
 *  - The tank stores int16, so its rounding leaves a permanent noise floor.
 *    The fit therefore uses the -5..-20 dB window, above the floor: 20 dB of
 *    slope x 3 = the 60 dB of RT60.
 */
static double measure_rt60(fx_node_t *nd, int type, int32_t knob, int use_default) {
    static double e[9000];
    double  edc = 0.0, tot = 0.0;
    int     nblk = (int)(MAX_RT60_MEAS * SR_HZ) / PA_BLOCK_N;
    int32_t buf[PA_BLOCK_N];
    int     k5 = -1, k20 = -1;

    if (nblk > (int)(sizeof(e) / sizeof(e[0]))) nblk = (int)(sizeof(e) / sizeof(e[0]));

    if (use_default) {                       /* recreate: factory defaults, decay 1761607 */
        nd->destroy(nd);
        nd->create(nd);
        if (!nd->state) return -1.0;
    }
    nd->set_param(nd, 10, type);
    nd->set_param(nd, 1, 0);                 /* HPass: fully open */
    nd->set_param(nd, 2, 0);                 /* Tone: no HF damping */
    nd->set_param(nd, 3, PA_Q21_ONE);        /* Mix: wet only */
    if (!use_default) nd->set_param(nd, 0, knob);
    nd->reset(nd);

    for (int b = 0; b < nblk; b++) {
        memset(buf, 0, sizeof(buf));
        if (b == 0) buf[0] = 1 << 20;        /* impulse, ~0 dBFS in Q21 */
        nd->run_stereo(nd, buf, 0, PA_BLOCK_N);
        double acc = 0.0;
        for (int i = 1; i < PA_BLOCK_N; i++) {
            double d = (double)buf[i] - (double)buf[i - 1];
            acc += d * d;
        }
        e[b] = acc;
        tot += acc;
    }
    if (tot <= 0.0) return -1.0;

    /* Backwards integration. Iterating from the END and keeping the LAST block
     * that satisfies the threshold yields the earliest crossing, which is the
     * one the slope needs. */
    for (int b = nblk - 1; b >= 0; b--) {
        edc += e[b];
        double db = 10.0 * log10(edc / tot);
        if (db <= -5.0)  k5  = b;
        if (db <= -20.0) k20 = b;
    }
    if (k5 < 0 || k20 < 0 || k20 < k5) return -1.0;
    double t5  = (double)k5  * PA_BLOCK_N / SR_HZ;
    double t20 = (double)k20 * PA_BLOCK_N / SR_HZ;
    return 3.0 * (t20 - t5);
}

static void test_rt60(fx_node_t *nd) {
    const int types[4] = { 0, 1, 3, 5 };     /* Room, Hall, Spring, Cloud */
    printf("[5] measured RT60 (impulse, no damping, 100%% wet)\n");

    for (int i = 0; i < 4; i++) {
        int t = types[i];
        for (int pos = 0; pos < 3; pos++) {
            const int  is_min = (pos == 0), is_def = (pos == 1), is_max = (pos == 2);
            int32_t    knob   = is_max ? PA_Q21_ONE : 0;
            double     got    = measure_rt60(nd, t, knob, is_def);
            double     g      = is_def ? (double)pa_rev_decay_gain(nd) / (double)PA_Q21_ONE
                                       : 0.55 + 0.41 * ((double)knob / (double)PA_Q21_ONE);
            double     want   = theory_rt60(k_ref_combs[t], g);
            const char *label = is_def ? "default" : (is_min ? "min " : "max ");

            printf("   %-6s %s  g=%.4f  RT60 %5.2f s (law %5.2f s, %+5.1f%%)\n",
                   k_type_name[t], label, g, got, want,
                   (want > 0.0) ? 100.0 * (got - want) / want : 0.0);

            CHECK(got > 0.0, "%s %s: the tail never decayed 20 dB in %.0f s",
                  k_type_name[t], label, MAX_RT60_MEAS);
            if (got > 0.0) {
                /* The law is the per-comb design target, using the mean D of
                 * the four combs. The measured Schroeder slope is 0.75x that
                 * (Hall default: 1.04 s measured, 1.37 s law) - an analytic
                 * EDC of the four summed combs predicts 1.03 s, so the factor
                 * is the energy sum, not the tank. Spring is the exception
                 * (+7 % at minimum) because its allpasses are fixed at g=0.7 and
                 * ring for ~0.24 s on their own. Hence the 35 % window: wide
                 * enough for that physics, far too narrow for the old
                 * per-sample table, which produced tails of minutes. */
                CHECK(fabs(got - want) / want < 0.35,
                      "%s %s: measured RT60 %.2f s is %.0f%% off the law (%.2f s)",
                      k_type_name[t], label, got,
                      fabs(got - want) / want * 100.0, want);
                if (is_def)
                    CHECK(got >= 0.4 && got <= 3.5,
                          "%s default RT60 %.2f s is outside the 0.4..3.5 s target (1.14: the default knob sits higher on the calibrated curve)",
                          k_type_name[t], got);
                if (is_min)
                    CHECK(got >= 0.15 && got <= 1.0,
                          "%s minimum RT60 %.2f s is outside 0.15..1.0 s",
                          k_type_name[t], got);
                if (is_max)
                    CHECK(got <= 10.0, "%s maximum RT60 %.2f s exceeds 10 s",
                          k_type_name[t], got);
            }
        }
    }

    printf("[6] measured RT60 monotonic across the knob (Hall)\n");
    double prev = -1.0;
    for (int pct = 0; pct <= 100; pct += 20) {
        int32_t val = (int32_t)(((int64_t)pct * PA_Q21_ONE) / 100);
        double got = measure_rt60(nd, 1, val, 0);
        printf("   knob %3d%%  RT60 %5.2f s\n", pct, got);
        CHECK(got > prev, "knob %d%%: RT60 %.2f s is not above the previous %.2f s",
              pct, got, prev);
        prev = got;
    }

    /* [7] the DECAY the screen shows (pa_rev_decay_ms) is the tail you hear */
    printf("[7] displayed DECAY vs measured RT60\n");
    const int types7[5] = { 0, 1, 3, 5, 6 };     /* + Plate */
    for (int i = 0; i < 5; i++) {
        int t = types7[i];
        for (int pct = 0; pct <= 100; pct += 50) {
            int32_t val   = (int32_t)(((int64_t)pct * PA_Q21_ONE) / 100);
            double  got   = measure_rt60(nd, t, val, 0);
            double  shown = pa_rev_decay_ms(t, val) / 1000.0;
            double  err   = (got > 0.0) ? (shown - got) / got : 1.0;
            printf("   %-6s knob %3d%%  shows %5.2f s, measured %5.2f s (%+5.1f%%)\n",
                   k_type_name[t], pct, shown, got, err * 100.0);
            /* Spring's minimum is set by its fixed allpasses (see [5]): 30 % there */
            double tol = (t == 3 && pct == 0) ? 0.30 : 0.12;
            CHECK(fabs(err) < tol, "%s knob %d%%: the screen shows %.2f s, the tail is %.2f s",
                  k_type_name[t], pct, shown, got);
        }
    }
}

/* --------------------------------------------------------------------- [8] */
static void test_spillover_tail(fx_node_t *nd) {
    printf("[8] spillover tail draining on bypass (Hall Q21 and Plate Float)\n");
    for (int mode = 0; mode < 2; mode++) {
        int t = (mode == 0) ? 1 : 6; /* Hall (Q21) and Plate (Float) */
        nd->destroy(nd);
        nd->create(nd);
        nd->set_param(nd, 10, t);
        nd->set_param(nd, 0, (int32_t)(0.7 * PA_Q21_ONE));
        nd->set_param(nd, 3, PA_Q21_ONE); /* 100% wet */
        nd->reset(nd);

        int32_t buf_L[PA_BLOCK_N], buf_R[PA_BLOCK_N];
        memset(buf_L, 0, sizeof(buf_L));
        memset(buf_R, 0, sizeof(buf_R));
        buf_L[0] = 1 << 20; /* impulse */

        /* Run 1 block with impulse */
        nd->run_stereo(nd, buf_L, buf_R, PA_BLOCK_N);

        /* Immediately bypass with tail_draining = 1 */
        nd->tail_draining = 1;
        nd->pending_destroy = 0;

        int blocks_drained = 0;
        int destroyed = 0;
        int32_t max_tail_peak = 0;

        for (int b = 0; b < 2000; b++) {
            memset(buf_L, 0, sizeof(buf_L));
            memset(buf_R, 0, sizeof(buf_R));
            nd->run_stereo(nd, buf_L, buf_R, PA_BLOCK_N);
            blocks_drained++;
            for (int i = 0; i < PA_BLOCK_N; i++) {
                int32_t a = abs(buf_L[i]);
                if (a > max_tail_peak) max_tail_peak = a;
            }
            if (nd->pending_destroy) {
                destroyed = 1;
                break;
            }
        }
        printf("   %-6s drained %d blocks (%.2f s), max wet peak %.4f FS, pending_destroy=%d\n",
               k_type_name[t], blocks_drained, (double)blocks_drained * PA_BLOCK_N / SR_HZ,
               (double)max_tail_peak / (double)PA_Q21_ONE, destroyed);
        CHECK(blocks_drained >= 64, "%s tail killed prematurely", k_type_name[t]);
        CHECK(max_tail_peak > 2048, "%s impulse echo never emerged during tail drain", k_type_name[t]);
        CHECK(destroyed == 1, "%s never finished draining", k_type_name[t]);
    }
}

int main(void) {
    fx_node_t *nd = fx_reverb_new();
    printf("=== Pocket Amp reverb tank + decay law ===\n");
    nd->create(nd);
    if (!nd->state) {
        printf("FAIL: reverb_create() produced no state\n");
        return 1;
    }

    test_layout(nd);
    test_sentinel(nd);
    test_decay_mapping(nd);
    test_rt60(nd);
    test_spillover_tail(nd);

    nd->destroy(nd);
    CHECK(nd->state == 0, "destroy() left a state pointer");

    if (fails) {
        printf("\nFAILED: %d check(s)\n", fails);
        return 1;
    }
    printf("\nALL PASSED\n");
    return 0;
}
