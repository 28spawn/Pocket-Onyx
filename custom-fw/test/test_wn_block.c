/* custom-fw/test/test_wn_block.c - the WaveNet block engine (accelerator conv mode, Q24 weights, Q21 samples) against the per-sample float engine.
 * Usage: test_wn_block <wave-blob-file>   (test_wn_block.py makes the blob from a .nam with emu/nam_loader.py) */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include "pa_wavenet.h"
#include "pa_hw_accel.h"

static pa_wavenet_model_t g_ref, g_blk;

int main(int argc, char **argv) {
    const int bn = getenv("WN_N") ? atoi(getenv("WN_N")) : 64;
    if (argc < 2) { printf("usage: test_wn_block <wave-blob>\n"); return 2; }
    FILE *f = fopen(argv[1], "rb");
    if (!f) { printf("cannot open %s\n", argv[1]); return 2; }
    static uint8_t blob[16384];
    const size_t n = fread(blob, 1, sizeof(blob), f);
    fclose(f);
    uint32_t count = 0; float head_scale = 1.0f, input_trim = 1.0f; uint32_t flags = 0;
    memcpy(&count, blob + 4, 4); memcpy(&head_scale, blob + 8, 4); memcpy(&flags, blob + 12, 4); memcpy(&input_trim, blob + 16, 4);
    (void)flags;
    const float *weights = (const float *)(blob + 20);
    if (n < 20 + count * 4) { printf("blob too short\n"); return 2; }

    pa_hw_accel_init();
    pa_wavenet_init_default(&g_ref); pa_wavenet_init_default(&g_blk);
    if (pa_wavenet_load_weights(&g_ref, weights, count, head_scale) != 0 || pa_wavenet_load_weights(&g_blk, weights, count, head_scale) != 0) { printf("FAIL load\n"); return 1; }
    g_ref.input_trim = g_blk.input_trim = input_trim;
    g_ref.makeup_gain = g_blk.makeup_gain = 1.0f;
    printf("weights %u, head_scale %.5f, input_trim %.3f, block engine ready: %d\n", count, g_blk.head_scale, g_blk.input_trim, pa_wavenet_block_ready(&g_blk));
    if (!pa_wavenet_block_ready(&g_blk)) { printf("FAIL block engine not ready (a weight did not fit Q24?)\n"); return 1; }
    g_ref.blk_ok = 0;                                  /* the reference runs the float per-sample path */
    if (getenv("WN_PERTURB")) { g_blk.blk_ok = 0; g_blk.input_trim *= 1.000001f; printf("PERTURB: both engines float, input trim x1.000001\n"); }

    const int blocks = 44100 * 3 / bn;
    double se = 0, sr = 0, peak = 0, maxerr = 0;
    uint32_t seed = 12345u;
    for (int b = 0; b < blocks; b++) {
        int32_t xa[64], xb[64];
        for (int i = 0; i < bn; i++) {
            const double t = (double)(b * bn + i) / 44100.0;
            double env = (b % 150 < 100) ? 1.0 : 0.15;                      /* picked notes with gaps */
            double v = env * (0.18 * sin(2 * 3.14159265 * 196.0 * t) + 0.10 * sin(2 * 3.14159265 * 392.0 * t) + 0.05 * sin(2 * 3.14159265 * 1180.0 * t));
            seed = seed * 1664525u + 1013904223u; v += ((double)(int32_t)(seed >> 8) / (double)(1 << 23) - 1.0) * 0.01;
            xa[i] = xb[i] = (int32_t)(v * 2097152.0);
        }
        pa_wavenet_process_block_q21(&g_ref, xa, bn);
        if (getenv("WN_DEFER")) {                       /* the core 0 split: condition before, engine, output stage after */
            int32_t scratch[64];
            pa_wavenet_set_defer(1);
            pa_wavenet_pre_core0(&g_blk, xb, bn, 1.0f);
            for (int i = 0; i < bn; i++) scratch[i] = 0x55555555;      /* the engine must not read or write the buffer */
            pa_wavenet_process_block_q21(&g_blk, scratch, bn);
            pa_wavenet_post_core0(&g_blk, xb, bn);
        } else
        pa_wavenet_process_block_q21(&g_blk, xb, bn);
        if (getenv("WN_DUMP") && b < 3) { for (int i = 0; i < 8; i++) printf("  b%d i%d ref %.6f blk %.6f\n", b, i, xa[i] / 2097152.0, xb[i] / 2097152.0); }
        if (getenv("WN_TRACE") && (b < 12 || b % 100 == 0)) {
            double me = 0, mr = 0;
            for (int i = 0; i < bn; i++) { double a = xa[i] / 2097152.0, c = xb[i] / 2097152.0; if (fabs(a - c) > me) me = fabs(a - c); if (fabs(a) > mr) mr = fabs(a); }
            printf("  block %4d: peak %.4f, largest error %.5f\n", b, mr, me);
        }
        for (int i = 0; i < bn; i++) {
            const double a = xa[i] / 2097152.0, c = xb[i] / 2097152.0;
            se += (a - c) * (a - c); sr += a * a;
            if (fabs(a) > peak) peak = fabs(a);
            if (fabs(a - c) > maxerr) maxerr = fabs(a - c);
        }
    }
    const double esr = sr > 0 ? se / sr : 1.0;
    printf("%d blocks: reference rms %.4f peak %.4f, error rms %.6f, largest error %.6f, ESR %.3e (%.1f dB)\n",
           blocks, sqrt(sr / (blocks * (double)bn)), peak, sqrt(se / (blocks * (double)bn)), maxerr, esr, 10.0 * log10(esr + 1e-30));
    const int ok = (sr > 0.0) && (esr < 1e-3);
    printf(ok ? "PASSED\n" : "FAILED (ESR must stay under 1e-3, -30 dB)\n");
    return ok ? 0 : 1;
}
