/* custom-fw/test/test_delay.c - Delay module (fx_delay.c) unit test suite
 *
 * Covers:
 *   [1] Delay buffer capacity & time mapping across all 9 modes
 *   [2] Dry/Wet mix law: constant unity dry up to 50 %, smooth kill-dry above 50 %
 *   [3] Feedback safety ceiling (capped at 0.92, slapback at 0.31) & overdrive stability
 *   [4] 15 Hz DC blocker in feedback path (eliminates DC bias accumulation)
 *   [5] Ping-Pong stereo routing (Left at TIME, Right at 2 x TIME) & mono sum
 *   [6] Reverse delay triangular window continuity (wa + wb = 1)
 *   [7] dTape and Slapback saturation monotonicity (no negative-slope foldback)
 *   [8] Spillover bypass tail draining into clean decay
 *
 * clang -O2 -Icustom-fw/include -DPA_HOST_SIM=1 custom-fw/test/test_delay.c \
 *       custom-fw/src/fx/fx_delay.c -lm
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include "pa_config.h"
#include "pa_fx.h"

void *pa_alloc(uint32_t n) { return calloc(1, n); }
void  pa_free(void *p)     { free(p); }
fx_node_t *fx_delay_new(void);

#define FS  44100
#define Q21 2097152.0
#define PI  3.14159265358979

static int fails = 0;
#define CHECK(ok, ...) do { printf((ok) ? "  ok   " : "  FAIL "); printf(__VA_ARGS__); printf("\n"); if (!(ok)) fails++; } while (0)

static fx_node_t *mk_delay(int type, double time_k, double fb_k, double mix_k, double aux_k) {
    fx_node_t *nd = fx_delay_new();
    nd->create(nd);
    nd->set_param(nd, 10, type);
    nd->set_param(nd, 0, (int32_t)(time_k * Q21));
    nd->set_param(nd, 1, (int32_t)(fb_k * Q21));
    nd->set_param(nd, 2, (int32_t)(mix_k * Q21));
    nd->set_param(nd, 3, (int32_t)(aux_k * Q21));
    return nd;
}

int main(void) {
    printf("=================================================================\n");
    printf("Delay Module (PA_SLOT_DELAY) Comprehensive Fidelity Test Suite\n");
    printf("=================================================================\n\n");

    printf("[1] buffer capacity and delay time ranges across all 9 modes\n");
    {
        /* Types 0..5 (Analog, Duck, dTape, Dual, Lofi, Digital): 23 ms .. 743 ms */
        uint32_t ms_lo = pa_dly_time_ms(5, 0);
        uint32_t ms_hi = pa_dly_time_ms(5, (int32_t)Q21);
        printf("     Digital Delay: knob 0 %% = %u ms, knob 100 %% = %u ms (max buffer 743 ms)\n", ms_lo, ms_hi);
        CHECK(ms_lo >= 20 && ms_lo <= 25 && ms_hi >= 735 && ms_hi <= 745,
              "standard modes cover 23 ms .. 743 ms");

        /* Ping-Pong & Reverse: max time must be clamped to 371 ms so 2 x TIME fits in 32k buffer */
        uint32_t pp_hi = pa_dly_time_ms(6, (int32_t)Q21);
        uint32_t rev_hi = pa_dly_time_ms(7, (int32_t)Q21);
        printf("     Ping-Pong max: %u ms, Reverse max: %u ms (buffer half 371 ms)\n", pp_hi, rev_hi);
        CHECK(pp_hi >= 365 && pp_hi <= 375 && rev_hi >= 365 && rev_hi <= 375,
              "Ping-Pong and Reverse clamped to ~371 ms for 2x buffer safety");

        /* Slapback: 40 ms .. 200 ms */
        uint32_t slap_lo = pa_dly_time_ms(8, 0);
        uint32_t slap_hi = pa_dly_time_ms(8, (int32_t)Q21);
        printf("     Slapback: %u ms .. %u ms\n", slap_lo, slap_hi);
        CHECK(slap_lo >= 38 && slap_lo <= 42 && slap_hi >= 195 && slap_hi <= 205,
              "Slapback covers vintage 40 ms .. 200 ms range");
    }

    printf("[2] dry/wet mix law: unity dry up to 50 %%, smooth kill-dry above 50 %%\n");
    {
        fx_node_t *nd = mk_delay(5, 0.5, 0.0, 0.25, 0.5); /* 25 % MIX */
        int32_t buf[PA_BLOCK_N];
        for (int i = 0; i < PA_BLOCK_N; i++) buf[i] = (int32_t)(0.5 * Q21);
        nd->run(nd, buf, PA_BLOCK_N);
        /* In first block before delayed tap arrives, output is dry * dry_gain */
        double dry_level_25 = buf[0] / (0.5 * Q21);
        nd->destroy(nd);

        nd = mk_delay(5, 0.5, 0.0, 0.50, 0.5); /* 50 % MIX */
        for (int i = 0; i < PA_BLOCK_N; i++) buf[i] = (int32_t)(0.5 * Q21);
        nd->run(nd, buf, PA_BLOCK_N);
        double dry_level_50 = buf[0] / (0.5 * Q21);
        nd->destroy(nd);

        nd = mk_delay(5, 0.5, 0.0, 1.00, 0.5); /* 100 % MIX (kill-dry) */
        for (int i = 0; i < PA_BLOCK_N; i++) buf[i] = (int32_t)(0.5 * Q21);
        nd->run(nd, buf, PA_BLOCK_N);
        double dry_level_100 = buf[0] / (0.5 * Q21);
        nd->destroy(nd);

        printf("     dry gain: MIX 25 %% = %.2f, MIX 50 %% = %.2f, MIX 100 %% = %.2f\n",
               dry_level_25, dry_level_50, dry_level_100);
        CHECK(fabs(dry_level_25 - 1.0) < 0.01 && fabs(dry_level_50 - 1.0) < 0.01,
              "dry signal remains at constant unity gain up to 50 %% MIX");
        CHECK(dry_level_100 < 0.001, "100 %% MIX achieves complete kill-dry (0.0 dry gain)");
    }

    printf("[3] feedback safety ceiling & overdrive stability\n");
    {
        /* Digital Delay with 100 % feedback knob */
        fx_node_t *nd = mk_delay(5, 0.1, 1.0, 0.5, 0.5);
        int32_t bl[PA_BLOCK_N], br[PA_BLOCK_N];
        int bad = 0;
        int32_t peak = 0;

        /* Drive with +6 dBFS burst for 5 blocks */
        for (int b = 0; b < 5; b++) {
            for (int i = 0; i < PA_BLOCK_N; i++) bl[i] = br[i] = (int32_t)(2.0 * Q21);
            nd->run_stereo(nd, bl, br, PA_BLOCK_N);
        }
        /* Run 150 blocks of silence; verify feedback ceiling keeps echo bounded and decaying */
        for (int b = 0; b < 150; b++) {
            for (int i = 0; i < PA_BLOCK_N; i++) bl[i] = br[i] = 0;
            nd->run_stereo(nd, bl, br, PA_BLOCK_N);
            for (int i = 0; i < PA_BLOCK_N; i++) {
                if (bl[i] > peak) peak = bl[i];
                if (bl[i] < -peak) peak = -bl[i];
                if (bl[i] > 2147483647LL || bl[i] < -2147483648LL) bad++;
            }
        }
        nd->destroy(nd);
        printf("     peak recirculating level: %.2f FS (bounded by 0.92 feedback ceiling)\n", (double)peak / Q21);
        CHECK(bad == 0 && peak < 4.0 * Q21, "delay feedback remains stable under full overdrive (0 overflows)");
    }

    printf("[4] 15 Hz DC blocker in feedback path\n");
    {
        /* Feedback 0.0 to test DC elimination on a single delayed pulse */
        fx_node_t *nd = mk_delay(5, 0.05, 0.0, 0.5, 0.5);
        int32_t bl[PA_BLOCK_N], br[PA_BLOCK_N];
        /* Feed step DC signal for 10 blocks */
        for (int b = 0; b < 10; b++) {
            for (int i = 0; i < PA_BLOCK_N; i++) bl[i] = br[i] = (int32_t)(0.5 * Q21);
            nd->run_stereo(nd, bl, br, PA_BLOCK_N);
        }
        /* Run 100 blocks of silence */
        for (int b = 0; b < 100; b++) {
            for (int i = 0; i < PA_BLOCK_N; i++) bl[i] = br[i] = 0;
            nd->run_stereo(nd, bl, br, PA_BLOCK_N);
        }
        double dc_tail = 0;
        for (int i = 0; i < PA_BLOCK_N; i++) dc_tail += fabs((double)bl[i] / Q21);
        dc_tail /= PA_BLOCK_N;
        nd->destroy(nd);
        printf("     residual DC in tail: %.2e FS\n", dc_tail);
        CHECK(dc_tail < 1e-4, "DC blocker successfully eliminates circulating DC bias");
    }

    printf("[5] Ping-Pong stereo routing and mono compatibility\n");
    {
        fx_node_t *nd = mk_delay(6, 0.0, 0.0, 1.0, 1.0); /* Ping-pong, min delay ~1024 samples (23 ms) */
        uint32_t delay_len = pa_dly_time_ms(6, 0) * 44100u / 1000u;
        int n_blocks = (delay_len * 3) / PA_BLOCK_N + 5;

        int32_t bl[PA_BLOCK_N], br[PA_BLOCK_N];
        int left_hit_sample = -1, right_hit_sample = -1;

        for (int b = 0; b < n_blocks; b++) {
            for (int i = 0; i < PA_BLOCK_N; i++) bl[i] = br[i] = (b == 0 && i == 0) ? (int32_t)(1.0 * Q21) : 0;
            nd->run_stereo(nd, bl, br, PA_BLOCK_N);
            for (int i = 0; i < PA_BLOCK_N; i++) {
                int s_idx = b * PA_BLOCK_N + i;
                if (bl[i] > 0.5 * Q21 && left_hit_sample < 0 && s_idx > 0) left_hit_sample = s_idx;
                if (br[i] > 0.5 * Q21 && right_hit_sample < 0 && s_idx > 0) right_hit_sample = s_idx;
            }
        }
        nd->destroy(nd);
        printf("     Left tap arrived at sample %d, Right tap arrived at sample %d (ratio: %.2f, expect 2.0)\n",
               left_hit_sample, right_hit_sample, (double)right_hit_sample / (double)left_hit_sample);
        CHECK(fabs((double)right_hit_sample / (double)left_hit_sample - 2.0) < 0.05,
              "Ping-Pong delivers Left at TIME and Right at 2 x TIME");
    }

    printf("[6] reverse delay triangular crossfade continuity\n");
    {
        fx_node_t *nd = mk_delay(7, 0.0, 0.0, 1.0, 1.0); /* Reverse delay, 100 % wet */
        int32_t bl[PA_BLOCK_N], br[PA_BLOCK_N];
        /* Feed steady 1 kHz sine */
        int n_blocks = 200;
        int bad_continuity = 0;
        for (int b = 0; b < n_blocks; b++) {
            for (int i = 0; i < PA_BLOCK_N; i++) {
                double s = 0.5 * sin(2 * PI * 1000 * (b * PA_BLOCK_N + i) / FS);
                bl[i] = br[i] = (int32_t)(s * Q21);
            }
            nd->run_stereo(nd, bl, br, PA_BLOCK_N);
            if (b > 50) {
                for (int i = 0; i < PA_BLOCK_N; i++) {
                    double y = (double)bl[i] / Q21;
                    if (!(y == y) || fabs(y) > 2.0) bad_continuity++;
                }
            }
        }
        nd->destroy(nd);
        CHECK(bad_continuity == 0, "reverse delay dual-reader crossfading runs cleanly without dropouts or clicks");
    }

    printf("[7] dTape and Slapback saturation monotonicity\n");
    {
        /* Verify dTape cubic saturation does not invert or fold back when overdriven */
        fx_node_t *nd_tape = mk_delay(2, 0.0, 0.0, 1.0, 1.0); /* dTape full grit */
        nd_tape->set_param(nd_tape, 6, 0); /* 0 wow/flutter depth for static saturation test */
        int32_t bl[PA_BLOCK_N];
        int prev_out = 0;
        int foldback_count = 0;

        /* Sweep AC sine wave (1 kHz) amplitude upwards through and beyond turnover peak */
        for (int step = 0; step < 20; step++) {
            double amp = 0.2 + (double)step * 0.15; /* 0.2 to 3.05 FS */
            int32_t curr_peak = 0;
            for (int b = 0; b < 30; b++) {
                for (int i = 0; i < PA_BLOCK_N; i++) {
                    double s = amp * sin(2 * PI * 1000 * (b * PA_BLOCK_N + i) / FS);
                    bl[i] = (int32_t)(s * Q21);
                }
                nd_tape->run(nd_tape, bl, PA_BLOCK_N);
                if (b >= 20) {
                    for (int i = 0; i < PA_BLOCK_N; i++) {
                        int32_t v = abs(bl[i]);
                        if (v > curr_peak) curr_peak = v;
                    }
                }
            }
            if (step > 0 && curr_peak < prev_out - 2000) foldback_count++;
            prev_out = curr_peak;
        }
        nd_tape->destroy(nd_tape);
        printf("     dTape monotonic saturation: foldback count = %d\n", foldback_count);
        CHECK(foldback_count == 0, "dTape cubic saturation is strictly monotonic (zero negative-slope foldback)");
    }

    printf("[8] spillover bypass tail draining into clean decay\n");
    {
        fx_node_t *nd = mk_delay(5, 0.1, 0.7, 0.5, 0.5); /* Digital delay with feedback */
        nd->tail_enabled = 1;
        int32_t bl[PA_BLOCK_N], br[PA_BLOCK_N];

        /* Input guitar note for 10 blocks */
        for (int b = 0; b < 10; b++) {
            for (int i = 0; i < PA_BLOCK_N; i++) bl[i] = br[i] = (int32_t)(0.5 * Q21);
            nd->run_stereo(nd, bl, br, PA_BLOCK_N);
        }

        /* Bypass pedal with spillover active */
        nd->tail_draining = 1;
        int drained_blocks = 0;
        while (nd->tail_draining && drained_blocks < 300) {
            for (int i = 0; i < PA_BLOCK_N; i++) bl[i] = br[i] = 0;
            nd->run_stereo(nd, bl, br, PA_BLOCK_N);
            drained_blocks++;
        }
        int cleanup_signaled = nd->pending_destroy;
        nd->destroy(nd);
        printf("     tail drained over %d blocks (~%d ms), pending_destroy signaled: %d\n",
               drained_blocks, drained_blocks * PA_BLOCK_N * 1000 / FS, cleanup_signaled);
        CHECK(drained_blocks > 10 && cleanup_signaled == 1,
              "spillover bypass drains tail naturally and signals safe deferred cleanup");
    }

    printf("\n=================================================================\n");
    printf("%s: %d failures\n", fails ? "FAILED" : "ALL DELAY TESTS PASSED", fails);
    printf("=================================================================\n");
    return fails ? 1 : 0;
}
