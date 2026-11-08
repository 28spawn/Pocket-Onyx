/* SPDX-License-Identifier: GPL-3.0-only */
/* Copyright (C) 2026 The Onyx Authors (@28spawn) */
/* test/test_wild.c - the "wild value" checks (include/pa_wild.h) that guard the drive's output and the final limiter.
 * Wild = a number no signal can be (NaN, infinity, past the integer range, 64+ full scales). A loud REAL signal must never be taken for one: it is the limiter's to handle. */
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include "pa_wild.h"

static int fails;
#define CHECK(c, ...) do { if (c) printf("  ok   "); else { printf("  FAIL "); fails++; } printf(__VA_ARGS__); printf("\n"); } while (0)

int main(void) {
    volatile float zero = 0.0f;
    const float nanv = zero / zero, infp = 1.0f / zero, infn = -1.0f / zero;
    printf("[1] float samples (the drive's output stage)\n");
    CHECK(pa_f_wild(nanv) && pa_f_nonfinite(nanv), "NaN is wild and non-finite");
    CHECK(pa_f_wild(infp) && pa_f_nonfinite(infp) && pa_f_wild(infn) && pa_f_nonfinite(infn), "+-infinity is wild and non-finite");
    CHECK(pa_f_wild(3.0e9f) && !pa_f_nonfinite(3.0e9f) && pa_f_wild(-3.0e9f) && !pa_f_nonfinite(-3.0e9f), "a finite value past int32 is wild but finite (it saturates, its filter states are fine)");
    CHECK(!pa_f_wild(0.0f) && !pa_f_wild(2097152.0f) && !pa_f_wild(-2097152.0f) && !pa_f_wild(2.0e9f) && !pa_f_wild(-2.0e9f), "silence, full scale and every in-range value are not wild");
    printf("[2] Q21 samples (the final limiter's input)\n");
    CHECK(!pa_q21_wild(0) && !pa_q21_wild(2097152) && !pa_q21_wild(-2097152), "silence and full scale pass");
    CHECK(!pa_q21_wild(4194304) && !pa_q21_wild(-4194304) && !pa_q21_wild(2097152 * 10) && !pa_q21_wild(-2097152 * 10) && !pa_q21_wild(2097152 * 63), "2, 10 and 63 full scales over are real overshoot: left to the limiter");
    CHECK(pa_q21_wild(2097152 * 65) && pa_q21_wild(-2097152 * 65) && pa_q21_wild(INT32_MAX) && pa_q21_wild(INT32_MIN), "65+ full scales and a saturated conversion are wild");
    CHECK(PA_WILD_Q21 == 64 * 2097152, "the threshold is 64 full scales (+36 dBFS)");
    printf("[3] a drive-like output stage\n");
    {   /* the same lines as fx_drive.c, run on a stream with a NaN in the middle: it must come out as one silent sample and the filter must be clean afterwards */
        float dc_x1 = 0, dc_y1 = 0, tone_y1 = 0; int32_t out[8]; const float in[8] = { 0.1f, 0.2f, 0.3f, nanv, 0.3f, 0.2f, 0.1f, 0.0f };
        for (int i = 0; i < 8; i++) {
            tone_y1 += 0.5f * (in[i] - tone_y1);
            const float dc = tone_y1 - dc_x1 + 0.99857f * dc_y1; dc_x1 = tone_y1; dc_y1 = dc;
            float o = dc * 2097152.0f;
            if (pa_f_wild(o)) { if (pa_f_nonfinite(o)) { dc_x1 = dc_y1 = 0.0f; tone_y1 = 0.0f; o = 0.0f; } else o = o > 0.0f ? 2147483000.0f : -2147483000.0f; }
            out[i] = (int32_t)o;
        }
        int bad = 0; for (int i = 4; i < 8; i++) if (!(fabsf((float)out[i]) < 2097152.0f * 4)) bad++;
        CHECK(out[3] == 0 && !pa_f_nonfinite(dc_x1) && !pa_f_nonfinite(dc_y1) && !pa_f_nonfinite(tone_y1) && bad == 0, "one NaN gives one silent sample and the stream recovers (without the reset every later sample was NaN)");
    }
    printf(fails ? "FAILED (%d)\n" : "ALL PASSED\n", fails);
    return fails ? 1 : 0;
}
