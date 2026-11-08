/* SPDX-License-Identifier: GPL-3.0-only */
/* Copyright (C) 2026 The Onyx Authors (@28spawn) */
/*
 * test/test_display_list.c - the display list (ui_5x7.c) against direct drawing.
 *
 * A screen is drawn once straight to a 240x240 frame buffer, and once the way the device draws it now: a hash pass that records the commands, then every one of the
 * 20 bands replayed from the list through a fake panel that clips to the band. The two frame buffers must be identical, pixel for pixel, for many random screens that
 * use every recorded primitive (rectangles, small and scaled text, Onyx text in the three alignments, cut boxes, sprites, segment rows, single characters). The text is built
 * in a temporary that is overwritten after the call, which a recorded pointer would not survive. An oversized screen must report that the list overflowed.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "pa_config.h"
#include "pa_ui.h"

extern volatile uint8_t  g_pa_hash_mode;
extern volatile uint16_t g_pa_band_y0, g_pa_band_y1;
extern void pa_dl_begin(void);
extern int  pa_dl_ok(void);
extern void pa_dl_replay(uint32_t by0, uint32_t by1);
extern void pa_dl_fill(uint16_t x, uint16_t y, uint16_t w, uint16_t h, uint16_t rgb);

static uint16_t fb[240 * 240];
static uint32_t by0 = 0u, by1 = 240u;                    /* the rows the fake panel accepts */

void st7735s_fill_rect(uint16_t x, uint16_t y, uint16_t w, uint16_t h, uint16_t rgb565) {
    if (g_pa_hash_mode) { pa_dl_fill(x, y, w, h, rgb565); return; }
    if (w == 0 || h == 0 || x >= 240u || y >= 240u) return;
    if ((uint32_t)x + w > 240u) w = (uint16_t)(240u - x);
    if ((uint32_t)y + h > 240u) h = (uint16_t)(240u - y);
    uint32_t y0 = y, y1 = (uint32_t)y + h;
    if (y0 < by0) y0 = by0;
    if (y1 > by1) y1 = by1;
    for (uint32_t yy = y0; yy < y1; yy++)
        for (uint32_t xx = x; xx < (uint32_t)x + w; xx++) fb[yy * 240u + xx] = rgb565;
}

static uint32_t rnd_state;
static uint32_t rnd(void) { rnd_state = rnd_state * 1664525u + 1013904223u; return rnd_state >> 8; }

static void scene(uint32_t seed, int items) {
    rnd_state = seed;
    st7735s_fill_rect(0, 0, 240, 240, 0x0841u);
    for (int i = 0; i < items; i++) {
        const uint16_t x = (uint16_t)(rnd() % 230u), y = (uint16_t)(rnd() % 232u);
        const uint16_t fg = (uint16_t)(rnd() | 1u), bg = (uint16_t)(rnd() & 0xF7DEu);
        char tmp[40];
        const unsigned len = 1u + rnd() % 14u;
        for (unsigned k = 0; k < len; k++) tmp[k] = "ABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789 .-%/+#"[rnd() % 42u];
        tmp[len] = '\0';
        switch (rnd() % 8u) {
        case 0: st7735s_fill_rect(x, y, (uint16_t)(1u + rnd() % 80u), (uint16_t)(1u + rnd() % 40u), fg); break;
        case 1: ui_draw_str(x, y, tmp, fg, bg); break;
        case 2: ui_draw_str_scaled(x, y, tmp, fg, bg, (uint8_t)(1u + rnd() % 3u)); break;
        case 3: ui_onyx_draw_str(x, y, tmp, (uint8_t)(1u + rnd() % 2u), fg, (uint8_t)(rnd() % 3u)); break;
        case 4: ui_onyx_cut_box(x, y, (uint16_t)(x + 4u + rnd() % 60u), (uint16_t)(y + 4u + rnd() % 30u), (uint8_t)(rnd() % 3u), fg, bg); break;
        case 5: ui_onyx_draw_sprite(x, y, (uint8_t)(rnd() % 3u), (uint8_t)(1u + rnd() % 2u), fg); break;
        case 6: { const uint8_t n = (uint8_t)(2u + rnd() % 10u); ui_onyx_segments(x, y, n, (uint8_t)(2u + rnd() % 6u), (uint8_t)(2u + rnd() % 8u), (uint8_t)(rnd() % 3u), (uint8_t)(rnd() % (n + 1u)), fg, bg); } break;
        default: ui_draw_char(x, y, tmp[0], fg, bg); break;
        }
        memset(tmp, 'Z', sizeof tmp - 1u);                         /* a recorded pointer to this would now read garbage */
    }
}

int main(void) {
    int bad = 0, cases = 0;
    static uint16_t ref[240 * 240];
    for (uint32_t seed = 1; seed <= 60u; seed++) {
        const int items = 20 + (int)(seed * 3u);                  /* 23 .. 200 items */
        /* direct */
        memset(fb, 0, sizeof fb); by0 = 0u; by1 = 240u; g_pa_band_y0 = 0u; g_pa_band_y1 = 0xFFFFu; g_pa_hash_mode = 0u;
        scene(seed, items);
        memcpy(ref, fb, sizeof fb);
        /* the device's way: record in the hash pass, replay per band */
        memset(fb, 0, sizeof fb);
        pa_dl_begin();
        g_pa_hash_mode = 1u;
        scene(seed, items);
        g_pa_hash_mode = 0u;
        if (!pa_dl_ok()) { printf("seed %u: the list overflowed (%d items)\n", seed, items); bad++; continue; }
        for (uint32_t b = 0; b < 20u; b++) {
            by0 = b * 12u; by1 = by0 + 12u; g_pa_band_y0 = (uint16_t)by0; g_pa_band_y1 = (uint16_t)by1;
            pa_dl_replay(by0, by1);
        }
        g_pa_band_y0 = 0u; g_pa_band_y1 = 0xFFFFu;
        int diff = 0;
        for (uint32_t i = 0; i < 240u * 240u; i++) if (fb[i] != ref[i]) diff++;
        cases++;
        if (diff) { printf("seed %u (%d items): %d pixels differ\n", seed, items, diff); bad++; }
    }
    /* a screen that does not fit must say so (the caller then draws it the old way) */
    pa_dl_begin();
    g_pa_hash_mode = 1u;
    scene(777u, 2000);
    g_pa_hash_mode = 0u;
    if (pa_dl_ok()) { printf("an oversized screen did not report the overflow\n"); bad++; }
    printf("display list: %d screens compared, %d failures\n", cases, bad);
    printf(bad ? "RESULT: FAILED\n" : "RESULT: ALL PASSED\n");
    return bad ? 1 : 0;
}
