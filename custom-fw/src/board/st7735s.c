/*
 * src/board/st7735s.c — Pocket Amp ST7789 240x240 RGB565 SPI1 DMA Display Driver (QW-6 / BR-4)
 *
 * Register and pin sequence:
 *   - Window set (CASET 0x2A, RASET 0x2B, RAMWR 0x2C)
 *   - GPIO init (PB1=CS, PB5=DC, PB6=SCK, PB7=MOSI, PA9=BL),
 *     SPI1_CON=0x4021, SPI1_BAUD=0, 22-entry init table
 *   - Non-blocking SPI1 DMA blit (JL_SPI1_ADR / JL_SPI1_CNT, IRQ 16)
 */
#include <stdint.h>
#include "../../include/pa_config.h"
#include "../../include/pa_hw.h"
#include "../../include/pa_plat.h"
#include "../../include/pa_ui.h"

typedef struct {
    uint8_t cmd;
    uint8_t len;
    uint8_t data[14];
} st7789_init_cmd_t;

/* Standard 22-entry ST7789 initialization command table */
static const st7789_init_cmd_t k_st7789_init_table[] = {
    { 0x28,  0, {0} },                                                                     /* Display OFF */
    { 0x11,  0, {0} },                                                                     /* Sleep OUT (+ 120 ms delay) */
    { 0x2A,  4, {0x00, 0x00, 0x00, 0xEF} },                                                /* CASET: 0..239 */
    { 0x2B,  4, {0x00, 0x28, 0x01, 0x17} },                                                /* RASET: 40..279 */
    { 0xB2,  5, {0x0C, 0x0C, 0x0C, 0x00, 0x33} },                                          /* Porch Setting */
    { 0x20,  0, {0} },                                                                     /* Inversion OFF */
    { 0xB7,  1, {0x56} },                                                                  /* Gate Control */
    { 0xBB,  1, {0x18} },                                                                  /* VCOM Setting */
    { 0xC0,  1, {0x2C} },                                                                  /* LCM Control */
    { 0xC2,  1, {0x01} },                                                                  /* VDV/VRH Enable */
    { 0xC3,  1, {0x1F} },                                                                  /* VRH Set */
    { 0xC4,  1, {0x20} },                                                                  /* VDV Set */
    { 0xC6,  1, {0x0F} },                                                                  /* Frame Rate: 60 Hz */
    { 0xD0,  2, {0xA6, 0xA1} },                                                            /* Power Control 1 */
    { 0xE0, 14, {0xD0,0x0D,0x14,0x0B,0x0B,0x07,0x3A,0x44,0x50,0x08,0x13,0x13,0x2D,0x32} }, /* PVGAMCTRL */
    { 0xE1, 14, {0xD0,0x0D,0x14,0x0B,0x0B,0x07,0x3A,0x44,0x50,0x08,0x13,0x13,0x2D,0x32} }, /* NVGAMCTRL */
    { 0x36,  1, {0x60} },                                                                  /* MADCTL: MV|MX landscape */
    { 0x3A,  1, {0x55} },                                                                  /* COLMOD: 16-bit RGB565 */
    { 0xE7,  1, {0x00} },
    { 0x51,  1, {0xFF} },                                                                  /* Brightness 100% */
    { 0x21,  0, {0} },                                                                     /* IPS Inversion ON */
    { 0x29,  0, {0} }                                                                      /* Display ON */
};

/* Transfers that did not finish within their time budget (see spi1_wait_done).
 * Non-zero means the panel or the SPI1 setup is wrong; the UI can show it. */
static volatile uint32_t g_lcd_spi_timeouts = 0;

uint32_t st7735s_spi_timeouts(void) {
    return g_lcd_spi_timeouts;
}

#if !defined(PA_HOST_BUILD) && !defined(PA_HOST_SIM)
/*
 * Wait for SPI1 to finish a transfer of `len` bytes, bounded by TIME, not by loop
 * count. A loop count says nothing about how long a DMA transfer takes: a full
 * 480-byte line can outlast a few thousand register polls, and cutting it short
 * leaves chip-select low and the DMA still reading a buffer the caller is about to
 * reuse.
 *
 * Budget: 1 ms + 8 us per byte, i.e. enough for a 1 MHz SPI clock (far slower than
 * any setting the panel uses). For a 480-byte line that is about 4.8 ms.
 *
 * Timer4 (24 MHz) is the clock. If Timer4 itself is not running, the tick count never
 * advances, so a large poll-count backstop still ends the wait.
 *
 * On timeout the pending flag is cleared, chip-select is released and the error is
 * counted, so the bus is left in a known state and the next transfer starts clean.
 * Returns 0 when the transfer completed, -1 on timeout.
 */
static int spi1_wait_done(uint32_t len) {
    const uint32_t budget = 24000u + len * 192u;      /* ticks of 24 MHz: 1 ms + 8 us/byte */
    const uint32_t t0 = plat_ticks24();
    uint32_t polls = 0;
    while ((PA_READ32(JL_SPI1_CON) & 0x8000u) == 0u) {
        if ((uint32_t)(plat_ticks24() - t0) > budget || ++polls >= 4000000u) {
            PA_WRITE32(JL_SPI1_CON,  PA_READ32(JL_SPI1_CON) | 0x4000u);   /* clear pending */
            PA_WRITE32(JL_PORTB_OUT, PA_READ32(JL_PORTB_OUT) | PA_LCD_CS_BIT);
            g_lcd_spi_timeouts++;
            return -1;
        }
    }
    PA_WRITE32(JL_SPI1_CON,  PA_READ32(JL_SPI1_CON) | 0x4000u);           /* clear pending */
    PA_WRITE32(JL_PORTB_OUT, PA_READ32(JL_PORTB_OUT) | PA_LCD_CS_BIT);
    return 0;
}

static void spi1_send_cmd(uint8_t cmd) {
    PA_WRITE32(JL_PORTB_OUT, PA_READ32(JL_PORTB_OUT) & ~(PA_LCD_DC_BIT | PA_LCD_CS_BIT));
    PA_WRITE32(JL_SPI1_CON,  PA_READ32(JL_SPI1_CON) | 0x4000u);
    PA_WRITE32(JL_SPI1_BUF,  cmd);
    (void)spi1_wait_done(1u);
}

static void spi1_send_data(const void *buf, uint32_t len) {
    PA_WRITE32(JL_PORTB_OUT, (PA_READ32(JL_PORTB_OUT) | PA_LCD_DC_BIT) & ~PA_LCD_CS_BIT);
    PA_WRITE32(JL_SPI1_CON,  PA_READ32(JL_SPI1_CON) | 0x4000u);
    PA_WRITE32(JL_SPI1_ADR,  (uint32_t)(uintptr_t)buf);
    PA_WRITE32(JL_SPI1_CNT,  len);
    (void)spi1_wait_done(len);
}

static void st7789_set_window(uint16_t x0, uint16_t x1, uint16_t y0, uint16_t y1) {
    uint8_t coord[8];
    coord[0] = (uint8_t)(y0 >> 8);
    coord[1] = (uint8_t)(y0 & 0xFFu);
    coord[2] = (uint8_t)(y1 >> 8);
    coord[3] = (uint8_t)(y1 & 0xFFu);
    coord[4] = (uint8_t)(x0 >> 8);
    coord[5] = (uint8_t)(x0 & 0xFFu);
    coord[6] = (uint8_t)(x1 >> 8);
    coord[7] = (uint8_t)(x1 & 0xFFu);

    spi1_send_cmd(0x2Au);
    spi1_send_data(&coord[4], 4u);
    spi1_send_cmd(0x2Bu);
    spi1_send_data(&coord[0], 4u);
    spi1_send_cmd(0x2Cu);
}
#endif

void st7735s_fill_rect(uint16_t x, uint16_t y, uint16_t w, uint16_t h, uint16_t rgb565);
/* The real one is in gpio_scan.c; the RAM benchmark links this empty default. */
__attribute__((weak)) void plat_backlight_on(void) { }

void st7735s_init(void) {
#if !defined(PA_HOST_BUILD) && !defined(PA_HOST_SIM)
    /* 1. Configure PB1 (CS), PB5 (DC), PB6 (SCK), PB7 (MOSI) as outputs; PA9 (BL) high (OFF) */
    const uint32_t pb_pins = PA_LCD_CS_BIT | PA_LCD_DC_BIT | PA_LCD_SCK_BIT | PA_LCD_SDA_BIT;
    PA_WRITE32(JL_PORTB_OUT, (PA_READ32(JL_PORTB_OUT) | PA_LCD_CS_BIT | PA_LCD_SDA_BIT) & ~(PA_LCD_DC_BIT | PA_LCD_SCK_BIT));
    PA_WRITE32(JL_PORTB_DIR, PA_READ32(JL_PORTB_DIR) & ~pb_pins);

    PA_WRITE32(JL_PORTA_OUT, PA_READ32(JL_PORTA_OUT) | PA_LCD_BL_PORTA_BIT);
    PA_WRITE32(JL_PORTA_DIR, PA_READ32(JL_PORTA_DIR) & ~PA_LCD_BL_PORTA_BIT);

    /* 2. Initialize SPI1 (CON = 0x4021, BAUD = 0) */
    PA_WRITE32(JL_SPI1_CON,  0x4021u);
    PA_WRITE32(JL_SPI1_BAUD, 0u);

    /* 3. Replay the 22-entry ST7789 init sequence
     *
     * The controller needs the delays the table's comments name: after Sleep Out
     * (0x11) the ST7789 needs at least 120 ms before the rest of the sequence, and
     * Display On (0x29) needs about 20 ms.  Without them the panel often ignores
     * what follows and stays blank or shows garbage. */
    for (unsigned i = 0; i < sizeof(k_st7789_init_table) / sizeof(k_st7789_init_table[0]); i++) {
        spi1_send_cmd(k_st7789_init_table[i].cmd);
        if (k_st7789_init_table[i].len > 0) {
            spi1_send_data(k_st7789_init_table[i].data, k_st7789_init_table[i].len);
        }
        if (k_st7789_init_table[i].cmd == 0x11u) {
            plat_delay_ms(120u);        /* Sleep Out -> 120 ms */
        } else if (k_st7789_init_table[i].cmd == 0x29u) {
            plat_delay_ms(20u);         /* Display On -> 20 ms */
        }
    }

    ((volatile uint32_t *)(uintptr_t)0x01C7C600u)[13] = plat_ticks24();   /* boot trace: init table sent */
    /* 4. Blank the panel before the light comes on: the controller's memory holds noise after power-up
     * and nothing has been drawn yet, so lighting it first shows a flash of garbage. */
    st7735s_fill_rect(0u, 0u, 240u, 240u, 0x0000u);

    ((volatile uint32_t *)(uintptr_t)0x01C7C600u)[14] = plat_ticks24();   /* boot trace: panel blanked */
    /* 5. The backlight stays OFF here (PA9 high). The main loop switches it on with plat_backlight_on() once the
     *    UI has drawn its first frames, so no half-drawn first frame is ever lit. */
    ((volatile uint32_t *)(uintptr_t)0x01C7C600u)[15] = plat_ticks24();   /* boot trace: init done, light still off */
#else
    (void)k_st7789_init_table;
#endif
}

/* ---- banded drawing (device only) --------------------------------------------------------------------
 * Drawing straight to the panel cost one LCD command sequence per pixel of text and a visible blank-then-redraw flash every frame.
 * Instead the whole frame is drawn PA_BAND_ROWS rows at a time into a small RAM band; each band is sent to the panel once, and a
 * band whose pixels are the same as last time is not sent at all (a moving level meter only touches one or two bands). */
#if !defined(PA_HOST_BUILD) && !defined(PA_HOST_SIM)
#define PA_BAND_ROWS 12u
#define PA_BAND_COUNT (240u / PA_BAND_ROWS)
static uint16_t s_band[240u * PA_BAND_ROWS] __attribute__((section(".pool"), aligned(8)));
static uint32_t s_band_hash[PA_BAND_COUNT];
static uint8_t  s_band_hash_ok;           /* 0 = the panel may hold something the hashes do not know (direct drawing happened) */
static uint8_t  s_band_on;
static uint32_t s_by0, s_by1;
#endif

int st7735s_band_hit(uint16_t y, uint16_t h) {
#if !defined(PA_HOST_BUILD) && !defined(PA_HOST_SIM)
    if (!s_band_on) return 1;
    return ((uint32_t)y < s_by1) && ((uint32_t)y + h > s_by0);
#else
    (void)y; (void)h; return 1;
#endif
}

extern volatile uint8_t g_pa_hash_mode;                  /* ui_5x7.c: the hash pass */
extern void pa_dl_begin(void);                           /* ui_5x7.c: the display list recorded during the hash pass */
extern int  pa_dl_ok(void);
extern void pa_dl_replay(uint32_t by0, uint32_t by1);
extern void pa_dl_fill(uint16_t x, uint16_t y, uint16_t w, uint16_t h, uint16_t rgb);
extern void pa_dl_stat(int ok);
extern uint32_t g_pa_cmd_hash[20];
extern void pa_cmd_add(uint32_t y0, uint32_t y1, uint32_t h);
extern volatile uint16_t g_pa_band_y0, g_pa_band_y1;     /* ui_5x7.c */

/* Draw and send up to max_bands more bands of the frame that is in progress; returns 1 when the frame is complete. Called by
 * pa_ui_render a few bands per UI tick, so the main loop (buttons, the preset change, everything the tick does) is not stuck behind
 * a redraw that the audio interrupt stretches to a third of a second. */
static uint32_t s_next_band;                              /* the next band of the frame in progress */
void st7735s_present_abort(void) { s_next_band = 0u; }    /* drop the frame in progress (the bands already drawn stay valid; a new frame starts at band 0) */
int st7735s_present_step(void (*draw)(void *), void *ctx, uint32_t max_bands) {
#if !defined(PA_HOST_BUILD) && !defined(PA_HOST_SIM)
    static uint32_t s_t_draw, s_t_spi, s_sent, s_skipped;       /* this frame's split, shown by jl_ram_read.py (SRAM 0x01C7D590) */
    static uint32_t s_cur_cmd[PA_BAND_COUNT], s_prev_cmd[PA_BAND_COUNT];
    if (s_next_band == 0u) {                                    /* the hash pass: one run of the screen routine that draws nothing */
        const uint32_t th0 = plat_ticks24();
        for (uint32_t b = 0; b < PA_BAND_COUNT; b++) g_pa_cmd_hash[b] = 2166136261u;
        pa_dl_begin();
        g_pa_hash_mode = 1u;
        draw(ctx);
        g_pa_hash_mode = 0u;
        pa_dl_stat(pa_dl_ok());
        for (uint32_t b = 0; b < PA_BAND_COUNT; b++) s_cur_cmd[b] = g_pa_cmd_hash[b];
        s_t_draw += plat_ticks24() - th0;
    }
    for (uint32_t n = 0; n < max_bands && s_next_band < PA_BAND_COUNT; n++, s_next_band++) {
        const uint32_t b = s_next_band;
        if (s_band_hash_ok && s_cur_cmd[b] == s_prev_cmd[b]) { s_skipped++; continue; }   /* the same drawing commands as last time: the panel is right */
        s_prev_cmd[b] = s_cur_cmd[b];
        s_by0 = b * PA_BAND_ROWS; s_by1 = s_by0 + PA_BAND_ROWS;
        g_pa_band_y0 = (uint16_t)s_by0; g_pa_band_y1 = (uint16_t)s_by1;
        const uint32_t tb0 = plat_ticks24();
        s_band_on = 1u;
        if (pa_dl_ok()) pa_dl_replay(s_by0, s_by1);              /* the recorded commands that touch this band */
        else            draw(ctx);                               /* the list overflowed: the whole screen routine, as before */
        s_band_on = 0u;
        g_pa_band_y0 = 0u; g_pa_band_y1 = 0xFFFFu;
        uint32_t h = 2166136261u;
        const uint32_t *wp = (const uint32_t *)(const void *)s_band;
        for (uint32_t i = 0; i < (240u * PA_BAND_ROWS) / 2u; i++) h = (h ^ wp[i]) * 16777619u;
        const uint32_t tb1 = plat_ticks24();
        s_t_draw += tb1 - tb0;
        if (s_band_hash_ok && h == s_band_hash[b]) { s_skipped++; continue; }   /* the panel already shows exactly this band */
        s_band_hash[b] = h;
        st7789_set_window(0u, 239u, (uint16_t)s_by0, (uint16_t)(s_by1 - 1u));
        spi1_send_data(s_band, 480u * PA_BAND_ROWS);                 /* the whole band in one DMA transfer: one set-up and one wait, not twelve */
        s_t_spi += plat_ticks24() - tb1; s_sent++;
    }
    if (s_next_band >= PA_BAND_COUNT) {
        volatile uint32_t *d = (volatile uint32_t *)(uintptr_t)0x01C7D590u;   /* drawing us, sending us, bands sent, bands skipped (last frame) */
        d[0] = s_t_draw / 24u; d[1] = s_t_spi / 24u; d[2] = s_sent; d[3] = s_skipped;
        s_t_draw = s_t_spi = s_sent = s_skipped = 0u;
        s_next_band = 0u; s_band_hash_ok = 1u; return 1;
    }
    return 0;
#else
    (void)max_bands; draw(ctx); return 1;
#endif
}

void st7735s_present_bands(void (*draw)(void *), void *ctx) {
    while (!st7735s_present_step(draw, ctx, 1000u)) { }
}

/* On-device text self-test: draw "ABC" into band 0 through the real text routines and dump which pixels came out, at SRAM 0x01C7D440:
 *   [0] magic "TXST", then 7 words per test (one per pixel row, bit i = pixel i of the 24-pixel-wide strip), 4 tests:
 *   Onyx font from a flash literal / from a RAM copy, 5x7 font from a flash literal / from a RAM copy.
 * `jl_ram_read.py slots` prints them as pictures: if "ABC" does not come out as A, B, C here, the drawing itself is broken. */
extern void ui_onyx_draw_str(uint16_t x, uint16_t y, const char *s, uint8_t scale, uint16_t fg, uint8_t align);
extern void ui_draw_str(uint16_t x, uint16_t y, const char *s, uint16_t fg, uint16_t bg);
void st7735s_text_selftest(void) {
#if !defined(PA_HOST_BUILD) && !defined(PA_HOST_SIM)
    volatile uint32_t *R = (volatile uint32_t *)(uintptr_t)0x01C7D440u;
    char copy[8] = { 'A', 'B', 'C', 0, 0, 0, 0, 0 };
    for (uint32_t t = 0; t < 4u; t++) {
        for (uint32_t i = 0; i < 240u * PA_BAND_ROWS; i++) s_band[i] = 0u;
        s_by0 = 0u; s_by1 = PA_BAND_ROWS; s_band_on = 1u;
        g_pa_band_y0 = 0u; g_pa_band_y1 = PA_BAND_ROWS;
        if (t == 0u)      ui_onyx_draw_str(0u, 0u, "ABC", 1u, 0xFFFFu, 0u);
        else if (t == 1u) ui_onyx_draw_str(0u, 0u, copy, 1u, 0xFFFFu, 0u);
        else if (t == 2u) ui_draw_str(0u, 0u, "ABC", 0xFFFFu, 0xFFFFu);
        else              ui_draw_str(0u, 0u, copy, 0xFFFFu, 0xFFFFu);
        s_band_on = 0u;
        g_pa_band_y0 = 0u; g_pa_band_y1 = 0xFFFFu;
        for (uint32_t r = 0; r < 7u; r++) {
            uint32_t bits = 0u;
            for (uint32_t x = 0; x < 24u; x++) if (s_band[r * 240u + x]) bits |= 1u << x;
            R[1u + t * 7u + r] = bits;
        }
    }
    R[0] = 0x54585354u;                                      /* "TXST" */
    s_band_hash_ok = 0u;
#endif
}

#ifndef PA_HOST_SIM
void st7735s_fill_rect(uint16_t x, uint16_t y, uint16_t w, uint16_t h, uint16_t rgb565) {
    if (w == 0 || h == 0 || x >= 240u || y >= 240u) return;
    if (g_pa_hash_mode) {
        pa_dl_fill(x, y, w, h, rgb565);
        uint32_t hh = (0x10u ^ ((uint32_t)x | ((uint32_t)y << 16))) * 16777619u;
        hh = (hh ^ ((uint32_t)w | ((uint32_t)h << 16))) * 16777619u;
        hh = (hh ^ rgb565) * 16777619u;
        pa_cmd_add(y, (uint32_t)y + h, hh);
        return;
    }
    uint16_t max_w = (w > 240u) ? 240u : w;
    if ((uint32_t)x + max_w > 240u) max_w = (uint16_t)(240u - x);
    if ((uint32_t)y + h > 240u) h = (uint16_t)(240u - y);
#if !defined(PA_HOST_BUILD)
    if (s_band_on) {                                         /* into the RAM band, clipped to it */
        uint32_t y0 = y, y1 = (uint32_t)y + h;
        if (y0 < s_by0) y0 = s_by0;
        if (y1 > s_by1) y1 = s_by1;
        if (y0 >= y1) return;
        const uint16_t sw = (uint16_t)((rgb565 << 8) | (rgb565 >> 8));
        for (uint32_t yy = y0; yy < y1; yy++) {
            uint16_t *dst = &s_band[(yy - s_by0) * 240u + x];
            for (uint16_t i = 0; i < max_w; i++) dst[i] = sw;
        }
        return;
    }
    s_band_hash_ok = 0u;                                     /* drawn straight on the panel: the band hashes are stale */
    uint16_t line_buf[240];
    uint16_t swapped = (uint16_t)((rgb565 << 8) | (rgb565 >> 8));
    for (uint16_t i = 0; i < max_w; i++) line_buf[i] = swapped;

    st7789_set_window(x, (uint16_t)(x + max_w - 1u), y, (uint16_t)(y + h - 1u));
    for (uint16_t row = 0; row < h; row++) {
        spi1_send_data(line_buf, (uint32_t)max_w * 2u);
    }
#else
    (void)x; (void)y; (void)w; (void)h; (void)rgb565;
#endif
}
#endif
