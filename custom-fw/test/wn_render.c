/* SPDX-License-Identifier: GPL-3.0-only */
/* Copyright (C) 2026 The Onyx Authors (@28spawn) */
/* custom-fw/test/wn_render.c - render a float32 mono file through a WaveNet capture with the block engine (what the unit's engine computes).
 * Usage: wn_render <wave-blob> <in.f32> <out.f32>      (tools/wn_render.py wraps it: .nam + .wav in, .wav out) */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "pa_wavenet.h"
#include "pa_hw_accel.h"

static pa_wavenet_model_t g_m;

int main(int argc, char **argv) {
    if (argc < 4) { printf("usage: wn_render <wave-blob> <in.f32> <out.f32>\n"); return 2; }
    FILE *f = fopen(argv[1], "rb");
    if (!f) return 2;
    static uint8_t blob[16384];
    const size_t nb = fread(blob, 1, sizeof(blob), f);
    fclose(f);
    uint32_t count = 0, flags = 0; float head_scale = 1.0f, input_trim = 1.0f;
    memcpy(&count, blob + 4, 4); memcpy(&head_scale, blob + 8, 4); memcpy(&flags, blob + 12, 4); memcpy(&input_trim, blob + 16, 4);
    if (nb < 20 + count * 4) return 2;
    pa_hw_accel_init();
    pa_wavenet_init_default(&g_m);
    if (pa_wavenet_load_weights(&g_m, (const float *)(blob + 20), count, head_scale) != 0) { printf("load failed\n"); return 1; }
    g_m.input_trim = input_trim;
    g_m.is_amp_cab = (flags & 1u) ? 1 : 0;
    g_m.makeup_gain = (argc > 4) ? (float)atof(argv[4]) : 1.0f;
    if (argc > 5) g_m.input_trim = (float)atof(argv[5]);
    if (!pa_wavenet_block_ready(&g_m)) { printf("block engine not ready\n"); return 1; }
    FILE *fi = fopen(argv[2], "rb"), *fo = fopen(argv[3], "wb");
    if (!fi || !fo) return 2;
    float in[64];
    size_t got;
    while ((got = fread(in, sizeof(float), 64, fi)) > 0) {
        int32_t q[64];
        for (size_t i = 0; i < got; i++) q[i] = (int32_t)(in[i] * 2097152.0f);
        pa_wavenet_process_block_q21(&g_m, q, (int)got);
        for (size_t i = 0; i < got; i++) in[i] = (float)q[i] / 2097152.0f;
        fwrite(in, sizeof(float), got, fo);
    }
    fclose(fi); fclose(fo);
    return 0;
}
