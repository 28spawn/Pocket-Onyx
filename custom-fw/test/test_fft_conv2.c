/* test/test_fft_conv2.c - the two-channel convolver (fc_block2) against two independent one-channel convolvers: the left output and the right output must match
 * fc_block with each IR on its own, bit for bit, for IRs of 1,024, 300 and 64 taps and with the partition limit set. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include "pa_config.h"
#include "fft_conv.h"

static fc_state_t sa, sb, sc;
static fc_cpx H2[FC_MAXP][FC_BINS];

static uint32_t rs = 7u;
static float rnd(void) { rs = rs * 1664525u + 1013904223u; return ((int32_t)(rs >> 8) - (1 << 23)) / (float)(1 << 23); }

int main(void) {
    int bad = 0, blocks = 0;
    fc_tables_init();
    static const int taps_list[3] = { 1024, 300, 64 };
    static const int limit_list[3] = { 0, 8, 3 };
    for (int t = 0; t < 3; t++)
        for (int li = 0; li < 3; li++) {
            float ir1[1024], ir2[1024];
            const int taps = taps_list[t];
            for (int i = 0; i < taps; i++) { ir1[i] = rnd() * expf(-(float)i / 200.0f); ir2[i] = rnd() * expf(-(float)i / 150.0f); }
            fc_init(&sa, ir1, taps); fc_init(&sb, ir2, taps);     /* the references: one channel each */
            fc_init(&sc, ir1, taps);                               /* the two-channel convolver */
            const int parts2 = fc_ir_spectra(H2, ir2, taps);
            fc_set_limit(&sa, limit_list[li]); fc_set_limit(&sb, limit_list[li]); fc_set_limit(&sc, limit_list[li]);
            for (int b = 0; b < 40; b++) {
                float in[64], ra[64], rb[64], oL[64], oR[64];
                for (int i = 0; i < 64; i++) in[i] = rnd() * 1.0e6f;
                fc_block(&sa, in, ra);
                fc_block(&sb, in, rb);
                fc_block2(&sc, (const fc_cpx (*)[FC_BINS])H2, parts2, in, oL, oR);
                blocks++;
                if (memcmp(ra, oL, sizeof ra)) bad++;
                if (memcmp(rb, oR, sizeof rb)) bad++;
            }
        }
    printf("fc_block2: %d blocks, %d differ from two one-channel convolvers\n", blocks, bad);
    printf(bad ? "RESULT: FAILED\n" : "RESULT: ALL PASSED\n");
    return bad ? 1 : 0;
}
