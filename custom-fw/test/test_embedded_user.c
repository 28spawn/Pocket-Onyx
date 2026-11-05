/* custom-fw/test/test_embedded_user.c - the imported-content path: registry seeding, blob load, cabinet list.
 * Run by test/test_embedded_user.py, which builds a fixture .inc with tools/pa_import.py and passes it in. */
#include <stdio.h>
#include <string.h>
#include "pa_user_nam.h"
#include "pa_stock_assets.h"
#include "pa_sysex.h"
#include "pa_ui.h"
#include "pa_fx.h"
#include "pa_model.h"
#include "pa_wavenet.h"
#include "pa_audio.h"
#include "pa_user_cab.h"

/* LCD / sysex stubs, copied from test_full_firmware.c: ui_app.c links in but nothing here draws */
/* Mock LCD hooks for UI verification */
void st7735s_fill_rect(uint16_t x, uint16_t y, uint16_t w, uint16_t h, uint16_t c) {
    (void)x; (void)y; (void)w; (void)h; (void)c;
}
void ui_draw_str(uint16_t x, uint16_t y, const char *s, uint16_t fg, uint16_t bg) {
    (void)x; (void)y; (void)s; (void)fg; (void)bg;
}
void ui_draw_char(uint16_t x, uint16_t y, char c, uint16_t fg, uint16_t bg) {
    (void)x; (void)y; (void)c; (void)fg; (void)bg;
}
void ui_draw_char_scaled(uint16_t x, uint16_t y, char c, uint16_t fg, uint16_t bg, uint8_t scale) {
    (void)x; (void)y; (void)c; (void)fg; (void)bg; (void)scale;
}
void ui_draw_str_scaled(uint16_t x, uint16_t y, const char *s, uint16_t fg, uint16_t bg, uint8_t scale) {
    (void)x; (void)y; (void)s; (void)fg; (void)bg; (void)scale;
}

/* The Onyx pixel primitives (the Groove Station redesign) - the host suite
 * does not draw, so they are stubs here. The real ones are in
 * src/board/ui_5x7.c, which the host build leaves out. */
uint16_t ui_onyx_text_w(const char *s, uint8_t scale) {
    (void)scale;
    if (!s) return 0;
    uint16_t n = 0;
    while (s[n]) n++;
    return (uint16_t)((n * 6u - 1u) * (scale ? scale : 1u));
}
void ui_onyx_draw_glyph(uint16_t x, uint16_t y, char c, uint8_t scale, uint16_t fg) {
    (void)x; (void)y; (void)c; (void)scale; (void)fg;
}
void ui_onyx_draw_str(uint16_t x, uint16_t y, const char *s, uint8_t scale, uint16_t fg, uint8_t align) {
    (void)x; (void)y; (void)s; (void)scale; (void)fg; (void)align;
}
void ui_onyx_cut_box(uint16_t x0, uint16_t y0, uint16_t x1, uint16_t y1,
                     uint8_t cut, uint16_t fill, uint16_t outline) {
    (void)x0; (void)y0; (void)x1; (void)y1; (void)cut; (void)fill; (void)outline;
}
void ui_onyx_draw_sprite(uint16_t x, uint16_t y, uint8_t sprite, uint8_t scale, uint16_t fg) {
    (void)x; (void)y; (void)sprite; (void)scale; (void)fg;
}
void ui_onyx_segments(uint16_t x, uint16_t y, uint8_t n, uint8_t w, uint8_t h,
                       uint8_t gap, uint8_t lit, uint16_t on, uint16_t off) {
    (void)x; (void)y; (void)n; (void)w; (void)h; (void)gap; (void)lit; (void)on; (void)off;
}
void pa_sysex_input_byte(uint8_t b) { (void)b; }
/* The host suite does not link src/sys/sysex.c (it stubs the parser above),
 * so the board driver's reply-sink registration needs a stub too. */
void pa_sysex_set_reply_sink(pa_sysex_reply_sink_t sink) { (void)sink; }
int  pa_sysex_reply_sink_set(void) { return 0; }
int plat_battery_is_brownout(void) { return 0; }
#include <stdlib.h>
void *pa_alloc(uint32_t n) { return calloc(1, n); }
void  pa_free(void *p) { free(p); }
pa_wavenet_model_t g_wn_model;
int g_active_amp_engine = 0;
uint32_t plat_ms(void) { return 0; }
void plat_wdt_feed(void) {}
void plat_backlight_set(uint8_t pct) { (void)pct; }
pa_fx_chain_t g_fx_chain;
pa_nam_model_t g_nam_model;
pa_audio_state_t g_pa_audio;


static int fails = 0;
#define CHECK(c, ...) do { if (!(c)) { printf("  FAIL " __VA_ARGS__); printf("\n"); fails++; } else { printf("  ok   " __VA_ARGS__); printf("\n"); } } while (0)

int main(void) {
    pa_stock_assets_init();
    pa_user_nam_init();
    const int n = pa_user_nam_get_count();
    printf("registry: %d capture(s), cabinets: %d\n", n, pa_cab_total_count());
    CHECK(n == (int)pa_emb_nam_count() && n >= 1, "the registry holds the %d imported capture(s)", (int)pa_emb_nam_count());
    for (int i = 0; i < n; i++) {
        pa_user_nam_slot_t info;
        CHECK(pa_user_nam_get_info((uint8_t)i, &info) == 0 && info.active, "capture %d active: %s", i, pa_user_nam_get_name((uint8_t)i));
        static pa_nam_model_t gru; static pa_wavenet_model_t wn;
        const int rc = pa_user_nam_load_into_dsp((uint8_t)i, &gru, &wn);
        CHECK(rc == 0, "capture %d loads into the DSP (rc %d)", i, rc);
    }
    CHECK(pa_cab_total_count() == PA_STOCK_CAB_COUNT + (int)(pa_cab_total_count() - PA_STOCK_CAB_COUNT), "cabinet count consistent");
    for (uint8_t c = PA_STOCK_CAB_COUNT; c < pa_cab_total_count(); c++) {
        static int32_t taps[PA_STOCK_CAB_TAPS]; uint32_t nt = 0;
        CHECK(pa_cab_load(c, taps, &nt) == 0 && nt == PA_STOCK_CAB_TAPS, "cab %u '%s' loads 1024 taps", c, pa_cab_name(c));
        double acc = 0; for (int i = 0; i < PA_STOCK_CAB_TAPS; i++) acc += (double)taps[i] * taps[i];
        CHECK(acc > 399000.0 * 399000.0 && acc < 401500.0 * 401500.0, "cab %u energy (L2) is the reference 400150", c);
    }
    CHECK(pa_cab_load((uint8_t)pa_cab_total_count(), (int32_t[1024]){0}, NULL) != 0, "one past the last cabinet is refused");
    /* a STORED cabinet comes after the imported ones (the device has one imported cabinet, so its type is 21: the editor showed its name EMPTY until this was tested with the imported cabinet present) */
    {
        static int32_t t64[64]; for (int i = 0; i < 64; i++) t64[i] = (i == 0) ? 200000 : i * 10;
        const uint8_t before = pa_cab_total_count();
        CHECK(pa_user_cab_add("USER CAB", 0xFF, t64, 64) == 0, "a stored cabinet is added to the first free slot");
        CHECK(pa_cab_total_count() == before + 1, "the cabinet types grew by one (%u)", pa_cab_total_count());
        CHECK(pa_cab_is_active(before) && strcmp(pa_cab_name(before), "USER CAB") == 0, "type %u is the stored cabinet, named '%s'", before, pa_cab_name(before));
        uint8_t out[5 + 15 * 16];
        const uint32_t len = pa_ui_name_list(1, 0, out, sizeof out);
        CHECK(len > 0 && out[4] == before, "the editor's list says the first stored type is %u (it is %u)", out[4], before);
        const uint32_t len2 = pa_ui_name_list(1, (uint8_t)(before - 1u), out, sizeof out);
        CHECK(len2 >= 5u + 2u * 16u && memcmp(out + 5 + 16, "USER CAB", 8) == 0, "the editor's list shows the stored cabinet's name at its type (not empty)");
        CHECK(pa_user_cab_rename(0, "TF ORANGE 4X12") == 0, "the stored cabinet is renamed");
        (void)pa_ui_name_list(1, (uint8_t)(before - 1u), out, sizeof out);
        CHECK(memcmp(out + 5 + 16, "TF ORANGE 4X12", 14) == 0, "the list shows the new name");
        static int32_t back[PA_STOCK_CAB_TAPS]; uint32_t nt = 0;
        CHECK(pa_cab_load(before, back, &nt) == 0 && back[0] != 0, "the stored cabinet loads through pa_cab_load");
        CHECK(pa_user_cab_delete(0) == 0 && !pa_cab_is_active(before) && pa_cab_total_count() == before, "deleting it gives the type back");
    }
    printf(fails ? "FAILED\n" : "PASSED\n");
    return fails ? 1 : 0;
}
