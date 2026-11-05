/*
 * src/board/ui_5x7.c — Compact 5x7 ASCII Bitmap Font Renderer for ST7735S/ST7789 TFT (QW-6 / BR-4)
 */
#include <stdint.h>
#include "../../include/pa_config.h"
#include "../../include/pa_ui.h"
#include "../../include/pa_xip.h"

#if !defined(PA_HOST_SIM) || defined(PA_EMU_DLL)
extern void st7735s_fill_rect(uint16_t x, uint16_t y, uint16_t w, uint16_t h, uint16_t rgb565);
/* The rows of the band being drawn (st7735s.c sets them for each of the screen's bands; the full screen otherwise): a glyph that lies
 * wholly outside them is skipped before any pixel work. Plain data, not a function call. */
volatile uint16_t g_pa_band_y0 = 0u, g_pa_band_y1 = 0xFFFFu;
/* HASH PASS (st7735s.c): at the start of a frame the screen routine runs once with g_pa_hash_mode set. The drawing primitives then draw
 * nothing; each adds a hash of its arguments (and of a string's characters) to every band its rows touch, in call order. A band whose
 * hash equals the one it was drawn with last time is skipped without running the screen routine for it. 20 bands, 12 rows each. */
volatile uint8_t g_pa_hash_mode = 0u;
uint32_t g_pa_cmd_hash[20];
void pa_cmd_add(uint32_t y0, uint32_t y1, uint32_t h) {
    if (y1 <= y0) return;
    const uint32_t b0 = y0 / 12u;
    uint32_t b1 = (y1 - 1u) / 12u;
    if (b1 > 19u) b1 = 19u;
    for (uint32_t b = b0; b <= b1; b++) g_pa_cmd_hash[b] = (g_pa_cmd_hash[b] ^ h) * 16777619u;
}
static inline uint32_t hm(uint32_t h, uint32_t v) { return (h ^ v) * 16777619u; }
static uint32_t hstr(uint32_t h, const char *s) {
    for (uint32_t i = 0; i < 160u; i++) { const uint8_t c = pa_xip_byte(&s[i]); if (!c) break; h = hm(h, c); }
    return h;
}

/* DISPLAY LIST. The hash pass already calls every drawing primitive once with its arguments; each now also records the call here. A band is then drawn by replaying the
 * commands whose rows touch it, instead of running the whole screen routine again for every band (20 times a frame: ~56 ms of CPU at light load, 5-6 times that when the
 * audio interrupt owns the core). A full redraw costs about the same as drawing the pixels once. If the list overflows, st7735s.c draws the frame the old way. */
/* counters for the readout (SRAM 0x01C7E740 [13] frames from the list, [14] frames that overflowed it, [15] most commands in a frame): fixed words, not variables - the RAM is full */
#if !defined(PA_HOST_BUILD) && !defined(PA_HOST_SIM)
#define DL_STAT(i) (((volatile uint32_t *)(uintptr_t)0x01C7E740u)[(i)])
#else
static uint32_t s_dl_stat[16];
#define DL_STAT(i) (s_dl_stat[(i)])
#endif
void pa_dl_stat(int ok) { if (ok) DL_STAT(13)++; else DL_STAT(14)++; }
#define DL_POOL_CMDS 860u                                   /* what is left of the pool */
#define DL_GAP_CMDS  512u                                   /* the 8 KB of SRAM at 0x01C06000, which nothing uses */
#define DL_MAX_CMDS  (DL_POOL_CMDS + DL_GAP_CMDS)
#define DL_STR_BYTES 3072u
/* 16 bytes a command: the rows it covers are worked out from its arguments when a band is replayed, not stored */
typedef struct { uint16_t x, y, v0, v1, v2, v3; uint8_t op, k0, k1, k2; } dl_cmd_t;
#if !defined(PA_HOST_BUILD) && !defined(PA_HOST_SIM)
#define DL_POOL __attribute__((section(".pool"), aligned(4)))     /* the RAM is full; the pool holds the band buffer too */
#else
#define DL_POOL
#endif
static dl_cmd_t s_dl_pool[DL_POOL_CMDS] DL_POOL;
#if !defined(PA_HOST_BUILD) && !defined(PA_HOST_SIM)
#define s_dl_gap ((dl_cmd_t *)(uintptr_t)0x01C06000u)
#define s_dl_str ((char *)(uintptr_t)0x01C00400u)          /* the strings: 3 KB at the start of the low SRAM, past the core 1 stub (48 bytes) and below the memory probe's range (0x01C01000) */
#else
static dl_cmd_t s_dl_gap[DL_GAP_CMDS];
static char     s_dl_str[DL_STR_BYTES];
#endif
static inline dl_cmd_t *dl_at(uint16_t i) { return (i < DL_POOL_CMDS) ? &s_dl_pool[i] : &s_dl_gap[i - DL_POOL_CMDS]; }
static uint16_t s_dl_n, s_dl_sn, s_dl_want;
static uint8_t  s_dl_ovf;

void pa_dl_begin(void) { s_dl_n = 0u; s_dl_sn = 0u; s_dl_ovf = 0u; s_dl_want = 0u; }
int  pa_dl_ok(void) { return !s_dl_ovf && s_dl_n != 0u; }
static void dl_unsupported(void) { s_dl_ovf = 1u; }
static void dl_add(uint8_t op, uint32_t y0, uint32_t y1, uint16_t x, uint16_t y, uint16_t v0, uint16_t v1, uint16_t v2, uint16_t v3, uint8_t k0, uint8_t k1, uint8_t k2) {
    if (++s_dl_want > DL_STAT(15)) DL_STAT(15) = s_dl_want;      /* the most commands a frame asked for, even past the limit: how big the list has to be */
    if (s_dl_n >= DL_MAX_CMDS) { s_dl_ovf = 1u; return; }
    dl_cmd_t *c = dl_at(s_dl_n++);
    (void)y0; (void)y1;
    c->x = x; c->y = y; c->v0 = v0; c->v1 = v1; c->v2 = v2; c->v3 = v3; c->op = op; c->k0 = k0; c->k1 = k1; c->k2 = k2;
}
/* the string is copied (the screen routine builds its text in temporaries); the same limit as the hash: 160 characters */
static uint16_t dl_str(const char *s) {
    if (s_dl_sn + 161u > DL_STR_BYTES) { s_dl_ovf = 1u; return 0u; }
    const uint16_t at = s_dl_sn;
    uint32_t i = 0;
    for (; i < 160u; i++) { const uint8_t c = pa_xip_byte(&s[i]); if (!c) break; s_dl_str[at + i] = (char)c; }
    s_dl_str[at + i] = 0;
    s_dl_sn = (uint16_t)(at + i + 1u);
    return at;
}
void pa_dl_fill(uint16_t x, uint16_t y, uint16_t w, uint16_t h, uint16_t rgb) { dl_add(0x10u, y, (uint32_t)y + h, x, y, w, h, rgb, 0, 0, 0, 0); }

extern void st7735s_fill_rect(uint16_t x, uint16_t y, uint16_t w, uint16_t h, uint16_t rgb565);
void ui_draw_char(uint16_t x, uint16_t y, char c, uint16_t fg, uint16_t bg);
void ui_draw_str(uint16_t x, uint16_t y, const char *s, uint16_t fg, uint16_t bg);
void ui_draw_str_scaled(uint16_t x, uint16_t y, const char *s, uint16_t fg, uint16_t bg, uint8_t scale);
void ui_onyx_draw_str(uint16_t x, uint16_t y, const char *s, uint8_t scale, uint16_t fg, uint8_t align);
void ui_onyx_cut_box(uint16_t x0, uint16_t y0, uint16_t x1, uint16_t y1, uint8_t cut, uint16_t fill, uint16_t outline);
void ui_onyx_draw_sprite(uint16_t x, uint16_t y, uint8_t sprite, uint8_t scale, uint16_t fg);
void ui_onyx_segments(uint16_t x, uint16_t y, uint8_t n, uint8_t w, uint8_t h, uint8_t gap, uint8_t lit, uint16_t on, uint16_t off);

/* Replay the recorded commands that touch rows [by0, by1), in their original order (the painter's algorithm gives the same pixels as the direct draw). */
/* the first row below a command, from its own arguments (the same rows pa_cmd_add was given) */
static uint32_t dl_y_end(const dl_cmd_t *c) {
    switch (c->op) {
    case 0x10: return (uint32_t)c->y + c->v1;
    case 0x11: case 0x12: return (uint32_t)c->y + 8u;
    case 0x14: return (uint32_t)c->y + 8u * (c->k0 ? c->k0 : 1u);
    case 0x16: return (uint32_t)c->y + 7u * c->k0;
    case 0x17: return (uint32_t)c->v1 + 1u;
    case 0x18: return (uint32_t)c->y + 7u * c->k1;
    case 0x19: return (uint32_t)c->y + c->k2;
    default:   return 0xFFFFu;
    }
}

void pa_dl_replay(uint32_t by0, uint32_t by1) {
    for (uint16_t i = 0; i < s_dl_n; i++) {
        const dl_cmd_t *c = dl_at(i);
        if (c->y >= by1 || dl_y_end(c) <= by0) continue;
        switch (c->op) {
        case 0x10: st7735s_fill_rect(c->x, c->y, c->v0, c->v1, c->v2); break;
        case 0x11: ui_draw_char(c->x, c->y, (char)c->k0, c->v0, c->v1); break;
        case 0x12: ui_draw_str(c->x, c->y, &s_dl_str[c->v3], c->v0, c->v1); break;
        case 0x14: ui_draw_str_scaled(c->x, c->y, &s_dl_str[c->v3], c->v0, c->v1, c->k0); break;
        case 0x16: ui_onyx_draw_str(c->x, c->y, &s_dl_str[c->v3], c->k0, c->v0, c->k1); break;
        case 0x17: ui_onyx_cut_box(c->x, c->y, c->v0, c->v1, c->k0, c->v2, c->v3); break;
        case 0x18: ui_onyx_draw_sprite(c->x, c->y, c->k0, c->k1, c->v0); break;
        case 0x19: ui_onyx_segments(c->x, c->y, c->k0, c->k1, c->k2, (uint8_t)(c->v2 & 0xFFu), (uint8_t)(c->v2 >> 8), c->v0, c->v1); break;
        default: break;
        }
    }
}

/* 1 unless the screen is being drawn in bands and this rectangle is outside the current band (st7735s.c) */
__attribute__((weak)) int st7735s_band_hit(uint16_t y, uint16_t h) { (void)y; (void)h; return 1; }

/* 5x7 column-major ASCII font for characters 0x20 (' ') through 0x5F ('_') */
static const uint8_t k_font5x7[64][5] = {
    {0x00,0x00,0x00,0x00,0x00}, /* 0x20 ' ' */
    {0x00,0x00,0x5F,0x00,0x00}, /* 0x21 '!' */
    {0x00,0x07,0x00,0x07,0x00}, /* 0x22 '"' */
    {0x14,0x7F,0x14,0x7F,0x14}, /* 0x23 '#' */
    {0x24,0x2A,0x7F,0x2A,0x12}, /* 0x24 '$' */
    {0x23,0x13,0x08,0x64,0x62}, /* 0x25 '%' */
    {0x36,0x49,0x55,0x22,0x50}, /* 0x26 '&' */
    {0x00,0x05,0x03,0x00,0x00}, /* 0x27 '\'' */
    {0x00,0x1C,0x22,0x41,0x00}, /* 0x28 '(' */
    {0x00,0x41,0x22,0x1C,0x00}, /* 0x29 ')' */
    {0x14,0x08,0x3E,0x08,0x14}, /* 0x2A '*' */
    {0x08,0x08,0x3E,0x08,0x08}, /* 0x2B '+' */
    {0x00,0x50,0x30,0x00,0x00}, /* 0x2C ',' */
    {0x08,0x08,0x08,0x08,0x08}, /* 0x2D '-' */
    {0x00,0x60,0x60,0x00,0x00}, /* 0x2E '.' */
    {0x20,0x10,0x08,0x04,0x02}, /* 0x2F '/' */
    {0x3E,0x51,0x49,0x45,0x3E}, /* 0x30 '0' */
    {0x00,0x42,0x7F,0x40,0x00}, /* 0x31 '1' */
    {0x42,0x61,0x51,0x49,0x46}, /* 0x32 '2' */
    {0x21,0x41,0x45,0x4B,0x31}, /* 0x33 '3' */
    {0x18,0x14,0x12,0x7F,0x10}, /* 0x34 '4' */
    {0x27,0x45,0x45,0x45,0x39}, /* 0x35 '5' */
    {0x3C,0x4A,0x49,0x49,0x30}, /* 0x36 '6' */
    {0x01,0x71,0x09,0x05,0x03}, /* 0x37 '7' */
    {0x36,0x49,0x49,0x49,0x36}, /* 0x38 '8' */
    {0x06,0x49,0x49,0x29,0x1E}, /* 0x39 '9' */
    {0x00,0x36,0x36,0x00,0x00}, /* 0x3A ':' */
    {0x00,0x56,0x36,0x00,0x00}, /* 0x3B ';' */
    {0x08,0x14,0x22,0x41,0x00}, /* 0x3C '<' */
    {0x14,0x14,0x14,0x14,0x14}, /* 0x3D '=' */
    {0x00,0x41,0x22,0x14,0x08}, /* 0x3E '>' */
    {0x02,0x01,0x51,0x09,0x06}, /* 0x3F '?' */
    {0x32,0x49,0x79,0x41,0x3E}, /* 0x40 '@' */
    {0x7E,0x11,0x11,0x11,0x7E}, /* 0x41 'A' */
    {0x7F,0x49,0x49,0x49,0x36}, /* 0x42 'B' */
    {0x3E,0x41,0x41,0x41,0x22}, /* 0x43 'C' */
    {0x7F,0x41,0x41,0x22,0x1C}, /* 0x44 'D' */
    {0x7F,0x49,0x49,0x49,0x41}, /* 0x45 'E' */
    {0x7F,0x09,0x09,0x09,0x01}, /* 0x46 'F' */
    {0x3E,0x41,0x49,0x49,0x7A}, /* 0x47 'G' */
    {0x7F,0x08,0x08,0x08,0x7F}, /* 0x48 'H' */
    {0x00,0x41,0x7F,0x41,0x00}, /* 0x49 'I' */
    {0x20,0x40,0x41,0x3F,0x01}, /* 0x4A 'J' */
    {0x7F,0x08,0x14,0x22,0x41}, /* 0x4B 'K' */
    {0x7F,0x40,0x40,0x40,0x40}, /* 0x4C 'L' */
    {0x7F,0x02,0x0C,0x02,0x7F}, /* 0x4D 'M' */
    {0x7F,0x04,0x08,0x10,0x7F}, /* 0x4E 'N' */
    {0x3E,0x41,0x41,0x41,0x3E}, /* 0x4F 'O' */
    {0x7F,0x09,0x09,0x09,0x06}, /* 0x50 'P' */
    {0x3E,0x41,0x51,0x21,0x5E}, /* 0x51 'Q' */
    {0x7F,0x09,0x19,0x29,0x46}, /* 0x52 'R' */
    {0x46,0x49,0x49,0x49,0x31}, /* 0x53 'S' */
    {0x01,0x01,0x7F,0x01,0x01}, /* 0x54 'T' */
    {0x3F,0x40,0x40,0x40,0x3F}, /* 0x55 'U' */
    {0x1F,0x20,0x40,0x20,0x1F}, /* 0x56 'V' */
    {0x3F,0x40,0x38,0x40,0x3F}, /* 0x57 'W' */
    {0x63,0x14,0x08,0x14,0x63}, /* 0x58 'X' */
    {0x07,0x08,0x70,0x08,0x07}, /* 0x59 'Y' */
    {0x61,0x51,0x49,0x45,0x43}, /* 0x5A 'Z' */
    {0x00,0x7F,0x41,0x41,0x00}, /* 0x5B '[' */
    {0x02,0x04,0x08,0x10,0x20}, /* 0x5C '\\' */
    {0x00,0x41,0x41,0x7F,0x00}, /* 0x5D ']' */
    {0x04,0x02,0x01,0x02,0x04}, /* 0x5E '^' */
    {0x40,0x40,0x40,0x40,0x40}  /* 0x5F '_' */
};

#include "onyx_font5x7.h"
#define ONYX_FONT_CHARS_HAS ONYX_FONT_CHARS

/* IMP-04 glyph fixes */
static const uint8_t k_glyph_flat[5]    = { 0x00, 0x7F, 0x48, 0x48, 0x30 };   /* music flat, replaces 'B' */
static const uint8_t k_glyph_one_big[5] = { 0x40, 0x44, 0x7F, 0x40, 0x40 };   /* '1' with full-width foot:
    the narrow serif '1' looked thin/off-centre in P10..P19 at 3x-4x scale */

static const uint8_t *ui_glyph(char c, uint8_t scale) {
    uint8_t uc = (uint8_t)c;
    if (uc == (uint8_t)UI_GLYPH_FLAT) return k_glyph_flat;
    if (uc == '1' && scale >= 2u) return k_glyph_one_big;
    if (uc >= 'a' && uc <= 'z') uc = (uint8_t)(uc - 32u);
    if (uc < 0x20u || uc > 0x5Fu) uc = 0x20u;
    return k_font5x7[uc - 0x20u];
}

/* The Onyx glyph drawer (further down) is the text path the Groove Station page uses, and on the device it draws correctly. The
 * original 5x7 routine below it came out as the first letter repeated on every other page (2026-11-01, banded drawing), so the
 * small text now goes through the Onyx glyphs too. The Onyx table lacks a few punctuation marks; those still use the old glyphs. */
extern void ui_onyx_draw_glyph(uint16_t x, uint16_t y, char c, uint8_t scale, uint16_t fg);
static int ui_onyx_has(char c) {
    if (c >= 'a' && c <= 'z') c = (char)(c - 'a' + 'A');
    for (uint8_t i = 0; ONYX_FONT_CHARS_HAS[i]; i++) if (ONYX_FONT_CHARS_HAS[i] == c) return 1;
    return 0;
}

void ui_draw_char(uint16_t x, uint16_t y, char c, uint16_t fg, uint16_t bg) {
    if (g_pa_hash_mode) { dl_add(0x11u, y, (uint32_t)y + 8u, x, y, fg, bg, 0, 0, (uint8_t)c, 0, 0); pa_cmd_add(y, (uint32_t)y + 8u, hm(hm(hm(hm(0x11u, x | ((uint32_t)y << 16)), (uint8_t)c), fg), bg)); return; }
    if ((uint32_t)y + 8u <= g_pa_band_y0 || y >= g_pa_band_y1) return;
    if (bg != 0xFFFFu) {
        st7735s_fill_rect(x, y, 6u, 8u, bg);
    }
    if (ui_onyx_has(c)) { ui_onyx_draw_glyph(x, y, c, 1u, fg); return; }
    const uint8_t *glyph = ui_glyph(c, 1u);
    for (uint16_t col = 0; col < 5u; col++) {
        uint8_t bits = glyph[col];
        if (!bits) continue;
        for (uint16_t row = 0; row < 7u; row++) {
            if (bits & (1u << row)) {
                st7735s_fill_rect((uint16_t)(x + col), (uint16_t)(y + row), 1u, 1u, fg);
            }
        }
    }
}

void ui_draw_str(uint16_t x, uint16_t y, const char *s, uint16_t fg, uint16_t bg) {
    if (!s) return;
    if (g_pa_hash_mode) { dl_add(0x12u, y, (uint32_t)y + 8u, x, y, fg, bg, 0, dl_str(s), 0, 0, 0); pa_cmd_add(y, (uint32_t)y + 8u, hstr(hm(hm(hm(0x12u, x | ((uint32_t)y << 16)), fg), bg), s)); return; }
    if ((uint32_t)y + 8u <= g_pa_band_y0 || y >= g_pa_band_y1) return;
    uint16_t cx = x;
    for (char ch; (ch = (char)pa_xip_byte(s)) != 0; s++) {
        ui_draw_char(cx, y, ch, fg, bg);
        cx = (uint16_t)(cx + 6u);
    }
}

void ui_draw_char_scaled(uint16_t x, uint16_t y, char c, uint16_t fg, uint16_t bg, uint8_t scale) {
    if (g_pa_hash_mode) { dl_unsupported(); pa_cmd_add(y, (uint32_t)y + 8u * (scale ? scale : 1u), hm(hm(hm(hm(hm(0x13u, x | ((uint32_t)y << 16)), (uint8_t)c), fg), bg), scale)); return; }
    if (scale <= 1u) {
        ui_draw_char(x, y, c, fg, bg);
        return;
    }
    if ((uint32_t)y + 8u * scale <= g_pa_band_y0 || y >= g_pa_band_y1) return;
    if (bg != 0xFFFFu) {
        st7735s_fill_rect(x, y, (uint16_t)(6u * scale), (uint16_t)(8u * scale), bg);
    }
    if (ui_onyx_has(c) && c != '1') { ui_onyx_draw_glyph(x, y, c, scale, fg); return; }   /* '1' keeps its wide-foot glyph at large sizes */
    const uint8_t *glyph = ui_glyph(c, scale);
    for (uint16_t col = 0; col < 5u; col++) {
        uint8_t bits = glyph[col];
        if (!bits) continue;
        for (uint16_t row = 0; row < 7u; row++) {
            if (bits & (1u << row)) {
                st7735s_fill_rect((uint16_t)(x + col * scale), (uint16_t)(y + row * scale), scale, scale, fg);
            }
        }
    }
}

void ui_draw_str_scaled(uint16_t x, uint16_t y, const char *s, uint16_t fg, uint16_t bg, uint8_t scale) {
    if (!s) return;
    if (g_pa_hash_mode) { dl_add(0x14u, y, (uint32_t)y + 8u * (scale ? scale : 1u), x, y, fg, bg, 0, dl_str(s), scale, 0, 0); pa_cmd_add(y, (uint32_t)y + 8u * (scale ? scale : 1u), hstr(hm(hm(hm(hm(0x14u, x | ((uint32_t)y << 16)), fg), bg), scale), s)); return; }
    if ((uint32_t)y + 8u * (scale ? scale : 1u) <= g_pa_band_y0 || y >= g_pa_band_y1) return;
    uint16_t cx = x;
    for (char ch; (ch = (char)pa_xip_byte(s)) != 0; s++) {
        ui_draw_char_scaled(cx, y, ch, fg, bg, scale);
        cx = (uint16_t)(cx + 6u * scale);
    }
}

/* =============================================================================
 * The Onyx pixel primitives.
 *
 * The Groove Station redesign (GROOVE_SPEC.md) is drawn 1:1, with no
 * anti-aliasing and no fractional positions:
 *
 *   - a box has PIXEL-CUT corners, not rounded ones,
 *   - the text comes from the Onyx 5x7 table (src/board/onyx_font5x7.h), which
 *     is a different table from the one above: it is row-major, and it is the
 *     one the design's reference renders were made with,
 *   - a character advances 6 px x scale, so a string is (len * 6 - 1) * scale
 *     wide,
 *   - a sprite is drawn as whole scale x scale blocks.
 *
 * The reference is the Python canvas in the design kit; every rule here is
 * that canvas's rule, so the screen can be compared with its reference render
 * pixel by pixel.
 * ========================================================================== */

/* The Onyx font is row-major: 7 rows of 5 bits, bit 4 = the leftmost pixel. */
#include "onyx_font5x7.h"

static const uint8_t *ui_onyx_glyph(char c) {
    /* character -> table index, built once in RAM: the linear search read the flash for every character of every band's pass */
    static uint8_t s_idx[128]; static uint8_t s_idx_ok;
    static uint8_t s_font_ram[52][7];            /* the glyph bitmaps in RAM: every glyph row was a cold flash read (the audio code evicts the cache) */
    if (!s_idx_ok) {
        pa_xip_read(s_font_ram, onyx_font5x7, sizeof s_font_ram);
        for (uint32_t k = 0; k < 128u; k++) s_idx[k] = 0xFFu;
        for (uint8_t i = 0; pa_xip_byte(&ONYX_FONT_CHARS[i]); i++) {
            const uint8_t ch = pa_xip_byte(&ONYX_FONT_CHARS[i]);
            if (ch < 128u && s_idx[ch] == 0xFFu) s_idx[ch] = i;
        }
        s_idx_ok = 1u;
    }
    if (c >= 'a' && c <= 'z') c = (char)(c - 'a' + 'A');
    const uint8_t ix = ((uint8_t)c < 128u) ? s_idx[(uint8_t)c] : 0xFFu;
    return s_font_ram[ix == 0xFFu ? 0u : ix];   /* '?' is the last-resort glyph in the table */
}

uint16_t ui_onyx_text_w(const char *s, uint8_t scale) {
    if (!s || !pa_xip_byte(s)) return 0;
    uint16_t len = 0;
    while (pa_xip_byte(&s[len])) len++;
    return (uint16_t)((len * 6u - 1u) * scale);
}

void ui_onyx_draw_glyph(uint16_t x, uint16_t y, char c, uint8_t scale, uint16_t fg) {
    if (scale == 0u) scale = 1u;
    if (g_pa_hash_mode) { dl_unsupported(); pa_cmd_add(y, (uint32_t)y + 7u * scale, hm(hm(hm(hm(0x15u, x | ((uint32_t)y << 16)), (uint8_t)c), scale), fg)); return; }
    if ((uint32_t)y + 7u * scale <= g_pa_band_y0 || y >= g_pa_band_y1) return;
    const uint8_t *g = ui_onyx_glyph(c);
    for (uint8_t r = 0; r < 7u; r++) {
        const uint16_t ry = (uint16_t)(y + r * scale);
        if ((uint32_t)ry + scale <= g_pa_band_y0 || ry >= g_pa_band_y1) continue;     /* this pixel row is outside the band */
        uint8_t bits = g[r];                                    /* the glyph is in RAM (ui_onyx_glyph) */
        if (!bits) continue;
        for (uint8_t k = 0; k < 5u; k++) {                     /* one fill per run of set pixels, not one per pixel */
            if (bits & (1u << (4u - k))) {
                uint8_t e = k;
                while (e + 1u < 5u && (bits & (1u << (4u - (e + 1u))))) e++;
                st7735s_fill_rect((uint16_t)(x + k * scale), ry, (uint16_t)((e - k + 1u) * scale), scale, fg);
                k = e;
            }
        }
    }
}

/* align: 0 = left (x is the first pixel), 1 = centre (x is the middle),
 *         2 = right (x is the LAST pixel). */
void ui_onyx_draw_str(uint16_t x, uint16_t y, const char *s, uint8_t scale,
                      uint16_t fg, uint8_t align) {
    if (!s) return;
    if (scale == 0u) scale = 1u;
    if (g_pa_hash_mode) { dl_add(0x16u, y, (uint32_t)y + 7u * scale, x, y, fg, 0, 0, dl_str(s), scale, align, 0); pa_cmd_add(y, (uint32_t)y + 7u * scale, hstr(hm(hm(hm(hm(0x16u, x | ((uint32_t)y << 16)), scale), fg), align), s)); return; }
    if ((uint32_t)y + 7u * scale <= g_pa_band_y0 || y >= g_pa_band_y1) return;     /* not in this band: do not even read the string */
    if (!pa_xip_byte(s)) return;
    uint16_t w = ui_onyx_text_w(s, scale);
    uint16_t x0;
    if (align == 1u)      x0 = (uint16_t)(x - w / 2u);
    else if (align == 2u) x0 = (uint16_t)(x - w + 1u);
    else                  x0 = x;
    uint16_t cx = x0;
    for (char ch; (ch = (char)pa_xip_byte(s)) != 0; s++) {
        ui_onyx_draw_glyph(cx, y, ch, scale, fg);
        cx = (uint16_t)(cx + 6u * scale);
    }
}

/* A rectangle with `cut` corner pixels left out - the design's box. The fill
 * is the union of the two bands, the outline is four 1 px lines, and with
 * cut = 2 the four diagonal pixels are set in the fill colour and then again
 * in the outline colour (so the outline wins). */
void ui_onyx_cut_box(uint16_t x0, uint16_t y0, uint16_t x1, uint16_t y1,
                      uint8_t cut, uint16_t fill, uint16_t outline) {
    if (x1 < x0 || y1 < y0) return;
    if (g_pa_hash_mode) { dl_add(0x17u, y0, (uint32_t)y1 + 1u, x0, y0, x1, y1, fill, outline, cut, 0, 0); pa_cmd_add(y0, (uint32_t)y1 + 1u, hm(hm(hm(hm(hm(0x17u, x0 | ((uint32_t)y0 << 16)), x1 | ((uint32_t)y1 << 16)), cut), fill), outline)); return; }
    uint16_t w = (uint16_t)(x1 - x0 + 1u);
    uint16_t h = (uint16_t)(y1 - y0 + 1u);
    if (cut * 2u + 2u > w || cut * 2u + 2u > h) return;
    if ((uint32_t)y1 + 1u <= g_pa_band_y0 || y0 >= g_pa_band_y1) return;

    if (fill != 0xFFFFu) {
        st7735s_fill_rect((uint16_t)(x0 + cut), y0, (uint16_t)(w - 2u * cut), h, fill);
        st7735s_fill_rect(x0, (uint16_t)(y0 + cut), w, (uint16_t)(h - 2u * cut), fill);
        if (cut == 2u) {
            st7735s_fill_rect((uint16_t)(x0 + 1u), (uint16_t)(y0 + 1u), 1, 1, fill);
            st7735s_fill_rect((uint16_t)(x1 - 1u), (uint16_t)(y0 + 1u), 1, 1, fill);
            st7735s_fill_rect((uint16_t)(x0 + 1u), (uint16_t)(y1 - 1u), 1, 1, fill);
            st7735s_fill_rect((uint16_t)(x1 - 1u), (uint16_t)(y1 - 1u), 1, 1, fill);
        }
    }
    if (outline != 0xFFFFu) {
        st7735s_fill_rect((uint16_t)(x0 + cut), y0, (uint16_t)(w - 2u * cut), 1, outline);
        st7735s_fill_rect((uint16_t)(x0 + cut), y1, (uint16_t)(w - 2u * cut), 1, outline);
        st7735s_fill_rect(x0, (uint16_t)(y0 + cut), 1, (uint16_t)(h - 2u * cut), outline);
        st7735s_fill_rect(x1, (uint16_t)(y0 + cut), 1, (uint16_t)(h - 2u * cut), outline);
        if (cut == 2u) {
            st7735s_fill_rect((uint16_t)(x0 + 1u), (uint16_t)(y0 + 1u), 1, 1, outline);
            st7735s_fill_rect((uint16_t)(x1 - 1u), (uint16_t)(y0 + 1u), 1, 1, outline);
            st7735s_fill_rect((uint16_t)(x0 + 1u), (uint16_t)(y1 - 1u), 1, 1, outline);
            st7735s_fill_rect((uint16_t)(x1 - 1u), (uint16_t)(y1 - 1u), 1, 1, outline);
        }
    }
}

/* The small sprites of the design: the two chevrons and the play triangle.
 * '#' = ink. Every pixel is drawn as a scale x scale block. */
static const char k_sprite_chev_l[5][4] = { "..#", ".#.", "#..", ".#.", "..#" };
static const char k_sprite_chev_r[5][4] = { "#..", ".#.", "..#", ".#.", "#.." };
static const char k_sprite_play[7][6] = {
    "#....", "##...", "###..", "####.", "###..", "##...", "#...."
};

void ui_onyx_draw_sprite(uint16_t x, uint16_t y, uint8_t sprite, uint8_t scale, uint16_t fg) {
    if (scale == 0u) scale = 1u;
    if (g_pa_hash_mode) { dl_add(0x18u, y, (uint32_t)y + 7u * scale, x, y, fg, 0, 0, 0, sprite, scale, 0); pa_cmd_add(y, (uint32_t)y + 7u * scale, hm(hm(hm(hm(0x18u, x | ((uint32_t)y << 16)), sprite), scale), fg)); return; }
    if ((uint32_t)y + 7u * scale <= g_pa_band_y0 || y >= g_pa_band_y1) return;
    const char *rows[7];
    uint8_t h;
    if (sprite == UI_SPRITE_CHEV_L) {
        for (uint8_t i = 0; i < 5u; i++) rows[i] = k_sprite_chev_l[i];
        h = 5u;
    } else if (sprite == UI_SPRITE_CHEV_R) {
        for (uint8_t i = 0; i < 5u; i++) rows[i] = k_sprite_chev_r[i];
        h = 5u;
    } else {
        for (uint8_t i = 0; i < 7u; i++) rows[i] = k_sprite_play[i];
        h = 7u;
    }
    for (uint8_t r = 0; r < h; r++) {
        for (uint8_t k = 0; rows[r][k]; k++) {
            if (rows[r][k] == '#') {
                st7735s_fill_rect((uint16_t)(x + k * scale), (uint16_t)(y + r * scale),
                                  scale, scale, fg);
            }
        }
    }
}

/* A row of n blocks - the meters, the volume, the step strip. The first
 * `lit` blocks are drawn in `on`, the rest in `off`. */
void ui_onyx_segments(uint16_t x, uint16_t y, uint8_t n, uint8_t w, uint8_t h,
                      uint8_t gap, uint8_t lit, uint16_t on, uint16_t off) {
    if (g_pa_hash_mode) { dl_add(0x19u, y, (uint32_t)y + h, x, y, on, off, (uint16_t)(((uint16_t)lit << 8) | gap), 0, n, w, h); pa_cmd_add(y, (uint32_t)y + h, hm(hm(hm(hm(hm(hm(hm(0x19u, x | ((uint32_t)y << 16)), n), w), h), gap), lit), on | ((uint32_t)off << 16))); return; }
    if ((uint32_t)y + h <= g_pa_band_y0 || y >= g_pa_band_y1) return;
    for (uint8_t i = 0; i < n; i++) {
        st7735s_fill_rect((uint16_t)(x + i * (w + gap)), y, w, h, (i < lit) ? on : off);
    }
}
#endif

