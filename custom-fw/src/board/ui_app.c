/* custom-fw/src/board/ui_app.c — Pocket Amp 240x240 RGB565 Onyx Design System Renderer
 *
 * Implements the authentic "Onyx" 240x240 theme for the M-VAVE Pocket Amp:
 *   - Dark palette (black BG 0x0000, neutral panels 0x18C3, yellow focus 0xF628)
 *   - 8-Slot configurable signal chain: DYN -> DRIVE -> AMP -> MOD -> DLY -> REV -> CAB -> EQ
 *   - Home screen:
 *       * 2x4 grid layout with 8 tiles (50x58 px) at X in [9, 66, 124, 181], Y in [100, 174]
 *       * Continuous signal path underlay entering left at Y=129, looping to row 2 at Y=203
 *       * Preset badge (P08 in 40px/3x display font), IN/OUT level meters, master volume bar
 *       * Alternative list view (Settings > Display > Home view = LIST)
 *   - Moving Mode: Lift block via PARA 2s hold, turn PARA to shift through chain, click to drop, HOME cancels
 *   - Bank Mode: 4 presets per bank across 20 banks (P01-P80); mapped directly to VOL-, VOL+, <, >
 *   - Block Editors (All 8 slots):
 *       * Header with ON/OFF switch toggle pill
 *       * Model selector card (TYPE / NAM A2 CAPTURE / Q18 IR)
 *       * Parameter knobs (1 to 6 knobs in 1 or 2 rows)
 *       * Dedicated 5-band vertical slider widget for EQ block (100, 400, 1k, 3k, 8k +/-12dB)
 *       * Quick bypass via PARA click wherever cursor is
 *   - Save / Saved Dialog: Overlaid destination picker P01-P80 with auto-dismiss toast
 *   - Tuner: High-visibility cents needle (+/-3ct green, +/-15ct yellow, red beyond), REF 430-450 Hz, MUTE toggle
 *   - Groove Station: Style card, 4-beat animated LEDs with beat 1 accent, BPM, and transport controls
 *   - Settings & Sub-pages: Global EQ, Input / Output, Display, Performance, and System
 */

#include "../../include/pa_build_id.h"
#include <string.h>
#include <stdio.h>
#include <math.h>
#include "pa_ui.h"
#include "pa_audio_core.h"
#include "pa_drums.h"
#include "pa_config.h"
#include "../../include/pa_stock_assets.h"
#include "../../include/pa_cab.h"
#include "../../include/pa_user_nam.h"
#include "../../include/pa_user_cab.h"
#include "../../include/pa_bond.h"
#include "../../include/pa_sysex.h"
#include "../../include/pa_evlog.h"
#include "../../include/pa_wavenet.h"
#include "../dsp/fixedpoint.h"
#include "onyx_block_icons.h"
#include "../../include/pa_settings.h"
#include "../../include/pa_audio.h"

extern void plat_backlight_set(uint8_t pct);      /* IMP-06 (gpio_scan.c) */
extern uint32_t plat_ms(void);

/* IMP-06: runtime grey palette (ONYX / HIGH CONTRAST) */
uint16_t g_onyx_greys[3] = { 0x2945u, 0x528Au, 0x8C51u };
void pa_ui_set_theme(uint8_t theme) {
    static const uint16_t k_pal[2][3] = {
        { 0x2945u, 0x528Au, 0x8C51u },   /* ONYX          #292929 #525252 #8a8a8a */
        { 0x4A69u, 0x8410u, 0xC618u },   /* HIGH CONTRAST #4c4c4c #808080 #c0c0c0 */
    };
    for (int i = 0; i < 3; i++) g_onyx_greys[i] = k_pal[theme ? 1 : 0][i];
}

/* IMP-12: LiPo open-circuit curve (mV at 100,90..0 %), replaces the linear 3.5..4.2 V guess */
/* Battery capacity formula: the average of SARADC channel 9, 10 bit.
 * Below 528 counts 0 %, above 635 counts 100 %, in between (adc * 100 - 52646) / 109 with an unsigned divide. 255 = no reading. */
uint8_t pa_battery_pct_from_adc(uint16_t adc) {
    if (adc == 0u) return 255u;
    if (adc < 528u) return 0u;
    if (adc > 635u) return 100u;
    return (uint8_t)(((uint32_t)adc * 100u - 52646u) / 109u);
}

/* mV -> percent for the callers that only have a voltage (the host builds, the old tests): the stock's slope is about 6.6 mV per count, so a voltage maps back to the count it came from. */
uint8_t pa_battery_pct_from_mv(uint16_t mv) {
    if (mv == 0) return 255u;
    return pa_battery_pct_from_adc((uint16_t)(((uint32_t)mv * 5u + 16u) / 33u));
}

void pa_ui_mark_settings_dirty(pa_ui_ctx_t *ui) {
    ui->settings_dirty = 1;
    ui->settings_dirty_ms = plat_ms();
}

extern void st7735s_fill_rect(uint16_t x, uint16_t y, uint16_t w, uint16_t h, uint16_t rgb565);
extern void ui_draw_char(uint16_t x, uint16_t y, char c, uint16_t fg, uint16_t bg);
extern void ui_draw_str(uint16_t x, uint16_t y, const char *s, uint16_t fg, uint16_t bg);
extern void ui_draw_str_scaled(uint16_t x, uint16_t y, const char *s, uint16_t fg, uint16_t bg, uint8_t scale);
extern uint32_t plat_ms(void);

#define DISP_W 240
#define DISP_H 240



static const char * const k_note_names[12] = {
    "C", "C#", "D", "D#", "E", "F", "F#", "G", "G#", "A", "A#", "B"
};

/* Fast formatting helpers */
static void fmt_u2(char out[3], uint32_t v) {
    if (v > 99u) v = 99u;
    out[0] = (char)('0' + (v / 10u));
    out[1] = (char)('0' + (v % 10u));
    out[2] = '\0';
}

static void fmt_u3(char out[4], uint32_t v) {
    if (v > 999u) v = 999u;
    out[0] = (char)('0' + (v / 100u));
    out[1] = (char)('0' + ((v / 10u) % 10u));
    out[2] = (char)('0' + (v % 10u));
    out[3] = '\0';
}

/* A plain decimal with no padding - the Groove Station's tempo is "116" and
 * "80", not "116" and "080". Returns the length. */
static uint8_t fmt_plain(char *out, uint32_t v) {
    char tmp[12];
    uint8_t n = 0;
    if (v == 0u) { out[0] = '0'; out[1] = '\0'; return 1u; }
    while (v && n < (uint8_t)sizeof(tmp)) {
        tmp[n++] = (char)('0' + (v % 10u));
        v /= 10u;
    }
    for (uint8_t i = 0; i < n; i++) out[i] = tmp[n - 1u - i];
    out[n] = '\0';
    return n;
}

static void fmt_preset(char out[4], uint32_t preset_0_to_98) {
    uint32_t num = preset_0_to_98 + 1u;
    if (num > 99u) num = 99u;
    out[0] = 'P';
    out[1] = (char)('0' + (num / 10u));
    out[2] = (char)('0' + (num % 10u));
    out[3] = '\0';
}

/* Slot Metadata Functions */
static const char *get_slot_label(uint8_t slot) {
    switch (slot) {
        case PA_SLOT_DYNAMICS: return "DYN";
        case PA_SLOT_DRIVE:    return "DRIVE";
        case PA_SLOT_AMP:      return "AMP";
        case PA_SLOT_CAB:      return "CAB";
        case PA_SLOT_EQ:       return "EQ";
        case PA_SLOT_MOD:      return "MOD";
        case PA_SLOT_DELAY:    return "DLY";
        case PA_SLOT_REVERB:   return "REV";
        default: return "???";
    }
}

static const char *get_slot_short(uint8_t slot) {
    switch (slot) {
        case PA_SLOT_DYNAMICS: return "DYN";
        case PA_SLOT_DRIVE:    return "DRV";
        case PA_SLOT_AMP:      return "AMP";
        case PA_SLOT_CAB:      return "CAB";
        case PA_SLOT_EQ:       return "EQ";
        case PA_SLOT_MOD:      return "MOD";
        case PA_SLOT_DELAY:    return "DLY";
        case PA_SLOT_REVERB:   return "REV";
        default: return "???";
    }
}

static uint16_t get_slot_color(uint8_t slot) {
    switch (slot) {
        case PA_SLOT_DYNAMICS: return ONYX_COL_DYN;
        case PA_SLOT_DRIVE:    return ONYX_COL_DRIVE;
        case PA_SLOT_AMP:      return ONYX_COL_AMP;
        case PA_SLOT_CAB:      return ONYX_COL_CAB;
        case PA_SLOT_EQ:       return ONYX_COL_EQ;
        case PA_SLOT_MOD:      return ONYX_COL_MOD;
        case PA_SLOT_DELAY:    return ONYX_COL_DLY;
        case PA_SLOT_REVERB:   return ONYX_COL_REV;
        default: return ONYX_COL_TEXT;
    }
}

static uint8_t get_slot_model_count(const pa_ui_ctx_t *ui, uint8_t slot) {
    (void)ui;
    switch (slot) {
        case PA_SLOT_AMP:      return (uint8_t)(PA_STOCK_AMP_COUNT + pa_user_nam_get_count());
        case PA_SLOT_CAB:      return pa_cab_total_count();
        default:               return pa_fx_slot_type_count((pa_fx_slot_id_t)slot);   /* IMP-13: DSP is the source of truth */
    }
}

/* A deleted capture or cabinet leaves a hole in the type numbering (presets name them by it). The screen counts the models that exist: "cur" is the 1-based position of the type among them, "total" how many there are. */
/* The first cabinet type that is a stored one: 20 stock + the cabinets linked into the image. */
static uint8_t cab_first_user(void) { return (uint8_t)(pa_cab_total_count() - pa_user_cab_span()); }
static int model_exists(uint8_t slot, uint8_t idx) {
    if (slot == PA_SLOT_AMP) return idx < PA_STOCK_AMP_COUNT || pa_user_nam_is_active((uint8_t)(idx - PA_STOCK_AMP_COUNT));
    if (slot == PA_SLOT_CAB) { const uint8_t f = cab_first_user(); return idx < f || pa_user_cab_is_active((uint8_t)(idx - f)); }
    return 1;
}
static void model_position(const pa_ui_ctx_t *ui, uint8_t slot, uint8_t *cur, uint8_t *total) {
    const uint8_t span = get_slot_model_count(ui, slot);
    uint8_t c = 0, t = 0;
    for (uint8_t i = 0; i < span; i++) { if (model_exists(slot, i)) { t++; if (i <= ui->slot_type[slot]) c++; } }
    *cur = c ? c : 1u; *total = t ? t : 1u;
}

static const char *get_slot_model_name(const pa_ui_ctx_t *ui, uint8_t slot, uint8_t idx) {
    (void)ui;
    /* IMP-13: order MUST match the DSP enums (fx_dynamics/drive/mod/delay/reverb.c) */
    static const char * const dyn_models[5] = { "GATE", "BOOST+GATE", "COMP+GATE", "AI GATE MS", "COMP" };
    static const char * const drv_models[6] = { "TS-808", "KLON", "OCD", "RAT", "BIG MUFF", "FUZZ FACE" };
    static const char * const eq_models[3]  = { "5-BAND", "LOW/HIGH CUT", "PARAMETRIC-3" };
    static const char * const mod_models[7] = { "CHORUS", "PHASER", "FLANGER", "TREMOLO", "VIBRATO", "UNIVIBE", "AUTO FILTER" };
    static const char * const dly_models[9] = { "ANALOG", "DUCKING", "TAPE", "DUAL", "LO-FI",
                                                "DIGITAL", "PING PONG", "REVERSE", "SLAPBACK" };
    static const char * const rev_models[8] = { "ROOM", "HALL", "SWELL", "SPRING", "SHIMMER", "CLOUD",
                                                "PLATE", "AMBIENCE" };

    switch (slot) {
        case PA_SLOT_DYNAMICS: return dyn_models[idx % 5u];
        case PA_SLOT_DRIVE:    return drv_models[idx % 6u];
        case PA_SLOT_AMP:
            if (idx < PA_STOCK_AMP_COUNT) {
                return pa_stock_get_amp_name(idx);
            } else {
                uint8_t u_idx = idx - PA_STOCK_AMP_COUNT;
                return pa_user_nam_get_name(u_idx);
            }
        case PA_SLOT_CAB: { extern volatile uint8_t g_pa_user_cab_active;
                            if (g_pa_user_cab_active) return "USER CAB";          /* a live cabinet from an upload is loaded (pa_audio_core_load_user_cab) */
                            return pa_cab_name(idx % pa_cab_total_count()); }
        case PA_SLOT_EQ:       return eq_models[idx % 3u];
        case PA_SLOT_MOD:      return mod_models[idx % 7u];
        case PA_SLOT_DELAY:    return dly_models[idx % 9u];
        case PA_SLOT_REVERB:   return rev_models[idx % 8u];
        default: return "STANDARD";
    }
}

static const char *get_slot_model_kind(const pa_ui_ctx_t *ui, uint8_t slot) {
    switch (slot) {
        case PA_SLOT_AMP:
            if (ui && ui->slot_type[PA_SLOT_AMP] >= PA_STOCK_AMP_COUNT) {
                return "NAM A2 CAPTURE";
            }
            return "STOCK MODEL";
        case PA_SLOT_CAB: return "Q18 IR";
        default: return "TYPE";
    }
}

static int is_a2_capture_active(const pa_ui_ctx_t *ui) {
    if (!ui) return 0;
    /* Stock factory models (indices 0..19) are factory AM3 models, not A2 captures */
    if (ui->slot_type[PA_SLOT_AMP] < PA_STOCK_AMP_COUNT) {
        return 0;
    }
    /* Index >= PA_STOCK_AMP_COUNT is a persistent user NAM A2 capture */
    return 1;
}

static uint8_t get_slot_param_count(const pa_ui_ctx_t *ui, uint8_t slot) {
    switch (slot) {
        case PA_SLOT_DYNAMICS: return 4;
        case PA_SLOT_DRIVE:    return 3;
        case PA_SLOT_AMP:      return 6;
        case PA_SLOT_CAB:      return 3;
        case PA_SLOT_EQ: {                 /* 5-BAND: 5 bands; LOW/HIGH CUT: 3; PARAMETRIC-3: 4 */
            static const uint8_t n[3] = { 5, 3, 4 };
            return n[ui ? (ui->slot_type[PA_SLOT_EQ] % 3u) : 0];
        }
        case PA_SLOT_MOD:      return 4;   /* + FDBK (was hidden) */
        case PA_SLOT_DELAY:    return 4;
        case PA_SLOT_REVERB:   return 4;
        default: return 0;
    }
}

static const char *get_slot_param_label(const pa_ui_ctx_t *ui, uint8_t slot, uint8_t p) {
    /* DYN: the knobs mean different things per type (see fx_dynamics.c). Index = DSP type:
     * GATE, BOOST+GATE, COMP+GATE, AI GATE MS, COMP. "---" = the knob does nothing on that type. */
    static const char * const dyn_params[5][4] = {
        { "THRESH", "RELEASE", "ATTACK", "LEVEL"  },   /* GATE */
        { "THRESH", "RELEASE", "---",    "BOOST"  },   /* BOOST+GATE */
        { "THRESH", "RATIO",   "ATTACK", "LEVEL"  },   /* COMP+GATE: knobs 2-4 are the compressor's */
        { "THRESH", "RELEASE", "ATTACK", "LEVEL"  },   /* AI GATE MS: THRESH = lowest threshold */
        { "THRESH", "RATIO",   "ATTACK", "LEVEL"  },   /* COMP: THRESH is the compressor's */
    };
    static const char * const drv_params[3] = { "DRIVE", "TONE", "LEVEL" };
    static const char * const amp_params[6] = { "GAIN", "BASS", "MID", "TREBLE", "PRES", "MASTER" };
    static const char * const cab_params[3] = { "LEVEL", "LOWCUT", "HICUT" };
    static const char * const eq_bands[5]   = { "100", "400", "1k", "3k", "8k" };
    static const char * const eq_cut[3]     = { "LOWCUT", "HICUT", "LEVEL" };
    static const char * const eq_par[4]     = { "LOW", "MID", "FREQ", "HIGH" };
    /* MOD: per type, in the DSP's type order (fx_mod.c). "---" = the knob does nothing. */
    static const char * const mod_params[7][4] = {
        { "RATE", "DEPTH", "MIX", "---"  },   /* CHORUS */
        { "RATE", "DEPTH", "MIX", "FDBK" },   /* PHASER */
        { "RATE", "DEPTH", "MIX", "FDBK" },   /* FLANGER */
        { "RATE", "DEPTH", "---", "---"  },   /* TREMOLO */
        { "RATE", "DEPTH", "---", "---"  },   /* VIBRATO */
        { "RATE", "DEPTH", "MIX", "FDBK" },   /* UNIVIBE */
        { "RATE", "RANGE", "MIX", "RESO" },   /* AUTO FILTER: DEPTH = sweep range, FDBK = resonance */
    };
    /* DELAY: the 4th knob is a different control per type (fx_delay.c) */
    static const char * const dly_aux[9] = { "MOD", "RELEASE", "GRIT", "RATIO", "GRIT",
                                             "TONE", "TONE", "TONE", "TONE" };
    static const char * const dly_params[4] = { "TIME", "FDBK", "MIX", "" };
    static const char * const rev_params[4] = { "DECAY", "LOWCUT", "TONE", "MIX" };

    switch (slot) {
        case PA_SLOT_DYNAMICS: return (p < 4) ? dyn_params[ui ? (ui->slot_type[PA_SLOT_DYNAMICS] % 5u) : 0][p] : "";
        case PA_SLOT_DRIVE:    return (p < 3) ? drv_params[p] : "";
        case PA_SLOT_AMP:      return (p < 6) ? amp_params[p] : "";
        case PA_SLOT_CAB:      return (p < 3) ? cab_params[p] : "";
        case PA_SLOT_EQ: {
            uint8_t et = ui ? (uint8_t)(ui->slot_type[PA_SLOT_EQ] % 3u) : 0u;
            if (et == 1u) return (p < 3) ? eq_cut[p] : "";
            if (et == 2u) return (p < 4) ? eq_par[p] : "";
            return (p < 5) ? eq_bands[p] : "";
        }
        case PA_SLOT_MOD:      return (p < 4) ? mod_params[ui ? (ui->slot_type[PA_SLOT_MOD] % 7u) : 0][p] : "";
        case PA_SLOT_DELAY:    return (p == 3) ? dly_aux[ui ? (ui->slot_type[PA_SLOT_DELAY] % 9u) : 0]
                                              : (p < 3) ? dly_params[p] : "";
        case PA_SLOT_REVERB:   /* AMBIENCE's first knob is the room SIZE, not a decay */
            if (p == 0 && ui && (ui->slot_type[PA_SLOT_REVERB] % 8u) == 7u) return "SIZE";
            return (p < 4) ? rev_params[p] : "";
        default: return "";
    }
}

/* UI Primitives */
static void draw_rounded_rect(uint16_t x, uint16_t y, uint16_t w, uint16_t h, uint16_t c) {
    if (w < 4 || h < 4) return;
    st7735s_fill_rect(x + 2, y, w - 4, 1, c);
    st7735s_fill_rect(x + 2, y + h - 1, w - 4, 1, c);
    st7735s_fill_rect(x, y + 2, 1, h - 4, c);
    st7735s_fill_rect(x + w - 1, y + 2, 1, h - 4, c);
    st7735s_fill_rect(x + 1, y + 1, 1, 1, c);
    st7735s_fill_rect(x + w - 2, y + 1, 1, 1, c);
    st7735s_fill_rect(x + 1, y + h - 2, 1, 1, c);
    st7735s_fill_rect(x + w - 2, y + h - 2, 1, 1, c);
}

static void fill_rounded_box(uint16_t x, uint16_t y, uint16_t w, uint16_t h, uint16_t c) {
    if (w < 4 || h < 4) return;
    st7735s_fill_rect(x + 2, y, w - 4, h, c);
    st7735s_fill_rect(x, y + 2, 2, h - 4, c);
    st7735s_fill_rect(x + w - 2, y + 2, 2, h - 4, c);
    st7735s_fill_rect(x + 1, y + 1, 1, 1, c);
    st7735s_fill_rect(x + w - 2, y + 1, 1, 1, c);
    st7735s_fill_rect(x + 1, y + h - 2, 1, 1, c);
    st7735s_fill_rect(x + w - 2, y + h - 2, 1, 1, c);
}

static void fill_circle(int16_t cx, int16_t cy, int16_t r, uint16_t col) {
    for (int16_t dy = -r; dy <= r; dy++) {
        int16_t dx = 0;
        while (dx * dx + dy * dy <= r * r) dx++;
        dx--;
        if (dx >= 0) {
            st7735s_fill_rect((uint16_t)(cx - dx), (uint16_t)(cy + dy), (uint16_t)(dx * 2 + 1), 1, col);
        }
    }
}

/* Kept for the tuner/drum screens; the current views draw their rings with
 * fill_rect, so this is not called yet. */
__attribute__((unused))
static void draw_circle(int16_t cx, int16_t cy, int16_t r, uint16_t col) {
    for (int16_t dy = -r; dy <= r; dy++) {
        int16_t dx = 0;
        while (dx * dx + dy * dy <= r * r) dx++;
        dx--;
        if (dx >= 0) {
            st7735s_fill_rect((uint16_t)(cx - dx), (uint16_t)(cy + dy), 1, 1, col);
            st7735s_fill_rect((uint16_t)(cx + dx), (uint16_t)(cy + dy), 1, 1, col);
        }
    }
}

/* Draw a segmented horizontal meter (IN / OUT) reflecting live audio measurements */
#define PA_UI_SRAM_TOTAL_KB 336u      /* the effect pool's region (0x01C20000, 0x54000 bytes): the SRAM figure on the Performance page is out of this */
static void draw_seg_meter(uint16_t x, uint16_t y, uint16_t w, uint16_t h, uint8_t level_pct, const char *label) {
    if (label) {
        uint16_t tx = x;
        if (label[0] == 'I' && label[1] == 'N' && label[2] == '\0') tx += 6; /* Right-align "IN" (12px) with "OUT" (18px) */
        ui_draw_str(tx, y, label, ONYX_COL_MUTE, ONYX_COL_BG);
        x += 22;
        w = (w > 22) ? (w - 22) : 10;
    }
    const uint8_t segs = 12;
    uint16_t seg_w = (uint16_t)((w - (segs - 1) * 2) / segs);
    uint8_t lit = (uint8_t)((level_pct * segs + 50) / 100);
    if (lit > segs) lit = segs;

    for (uint8_t i = 0; i < segs; i++) {
        uint16_t sx = (uint16_t)(x + i * (seg_w + 2));
        uint16_t col = ONYX_COL_LINE;
        if (i < lit) {
            if (i < 8) col = ONYX_COL_OK;
            else if (i < 11) col = ONYX_COL_WARN;
            else col = ONYX_COL_BAD;
        }
        st7735s_fill_rect(sx, y + 1, seg_w, h, col);
    }
}

/* IMP-04: ONE battery widget used on every screen (the old code had two variants and four
 * different anchor points, so icon / % / Bluetooth jumped between screens).
 * Anchored by its RIGHT edge and vertical CENTRE. Blinks red when critical (IMP-12). */
#define STATUS_RIGHT_X  232u
static uint8_t s_draw_charging;      /* set by draw_status_cluster for the battery icon: USB power is present, the charge bolt is drawn in the body */
#define STATUS_CY       14u        /* the cluster fits in y 0..29: the design kit's screens (Groove Station) own everything below y 30 */
static uint16_t draw_battery_r(uint16_t right_x, uint16_t cy, uint8_t pct, uint8_t show_pct, uint8_t warn, uint8_t pct_below) {
    const uint16_t x = (uint16_t)(right_x - 22u), y = (uint16_t)(cy - 5u);
    int blink_off = (warn >= 2) && (((plat_ms() / 400u) & 1u) != 0);
    uint16_t frame = (warn >= 1) ? ONYX_COL_BAD : ONYX_COL_TEXT;
    if (!blink_off) {
        draw_rounded_rect(x, y, 20, 11, frame);
        st7735s_fill_rect(x + 20, y + 3, 1, 5, frame);
        st7735s_fill_rect(x + 21, y + 4, 1, 3, frame);
        if (pct != 255u) {
            uint16_t fill_w = (uint16_t)((pct * 14u + 50u) / 100u);
            if (fill_w > 0) st7735s_fill_rect(x + 3, y + 3, fill_w, 5, (pct <= 15u) ? ONYX_COL_BAD : ONYX_COL_TEXT);
        }
        if (s_draw_charging) {                                   /* the charge bolt, 5 x 5 px, green over the bar */
            static const uint8_t bolt[5] = { 0x06, 0x0C, 0x1E, 0x06, 0x0C };
            for (uint16_t r = 0; r < 5u; r++)
                for (uint16_t c = 0; c < 5u; c++)
                    if ((bolt[r] >> (4u - c)) & 1u) st7735s_fill_rect((uint16_t)(x + 8u + c), (uint16_t)(y + 3u + r), 1, 1, ONYX_COL_OK);
        }
    }
    uint16_t left = x;
    if (show_pct) {
        char bstr[6];
        if (pct == 255u) { bstr[0] = '-'; bstr[1] = '-'; bstr[2] = '\0'; }
        else if (pct >= 100u) { bstr[0] = '1'; bstr[1] = '0'; bstr[2] = '0'; bstr[3] = '%'; bstr[4] = '\0'; }
        else             { fmt_u2(bstr, pct); bstr[2] = '%'; bstr[3] = '\0'; }
        uint16_t w = 0; while (bstr[w]) w++;
        /* the % sits BELOW the battery, centred under its body and between it and the line at y=39 (the cluster is a small square: battery top right, its % under it, Bluetooth beside the battery, the USB diamond under Bluetooth) */
        if (pct_below) {
            ui_draw_str((uint16_t)(x + 11u - (w * 6u) / 2u), (uint16_t)(cy + 12u), bstr, (warn ? ONYX_COL_BAD : ONYX_COL_TEXT), ONYX_COL_BG);
        } else {                                           /* in a row: the % sits to the left of the battery, on its centre line */
            left = (uint16_t)(x - 5u - w * 6u);
            ui_draw_str(left, (uint16_t)(cy - 3u), bstr, (warn ? ONYX_COL_BAD : ONYX_COL_TEXT), ONYX_COL_BG);
        }
    }
    return left;
}

/* Bluetooth Icon Color resolver (the device sets bt_state from the radio every tick, main.c):
 *   - bt_state == 2: connected -> solid cyan
 *   - bt_state == 1: broadcasting (advertising, no link) -> slow flash, cyan / gray, 0.8 s each
 *   - bt_state == 0: off (switched off in Settings, or no radio running) -> gray (so the position stays fixed)
 */
static uint16_t get_bluetooth_color(uint8_t bt_state) {
    if (bt_state == 2) {
        return ONYX_COL_BLUETOOTH;
    } else if (bt_state == 1) {
        return (((plat_ms() / 800u) & 1u) == 0) ? ONYX_COL_BLUETOOTH : ONYX_COL_DIM;
    } else {
        return ONYX_COL_DIM;
    }
}

/* The Bluetooth icon: two interlocked rings (the original), 15 x 10 px, centred on cx, cy. A ring is 9 x 7 px with a 2 px wall; the lower-left one at (0,3), the upper-right one at (6,0). */
#define BT_ICON_HALF_W 7u          /* the glyph spans cx-7 .. cx+7 */
static void draw_bluetooth(uint16_t cx, uint16_t cy, uint16_t col) {
    static const uint16_t ring[7] = {
        0x007C, /* ..#####.. */
        0x00FE, /* .#######. */
        0x0183, /* ##.....## */
        0x0183, /* ##.....## */
        0x0183, /* ##.....## */
        0x00FE, /* .#######. */
        0x007C  /* ..#####.. */
    };
    const int x0 = (int)cx - 7, y0 = (int)cy - 5;
    for (int y = 0; y < 10; y++) {
        for (int x = 0; x < 15; x++) {
            int on = 0;
            if (y >= 3 && y - 3 < 7 && x < 9)           on |= (ring[y - 3] >> (8 - x)) & 1;      /* the lower-left ring */
            if (y < 7 && x >= 6)                         on |= (ring[y] >> (8 - (x - 6))) & 1;    /* the upper-right ring */
            if (on) st7735s_fill_rect((uint16_t)(x0 + x), (uint16_t)(y0 + y), 1, 1, col);
        }
    }
}


/* USB connected: a very small yellow diamond (7 x 7 px), centred on the status line; it replaces the plug icon. cx, cy = its centre. */
static void draw_usb_diamond(uint16_t cx, uint16_t cy, uint16_t col) {
    static const uint8_t half_w[7] = { 0, 1, 2, 3, 2, 1, 0 };          /* half-widths of the rows from the top: 1, 3, 5, 7, 5, 3, 1 pixels */
    for (int r = 0; r < 7; r++) {
        const int w = half_w[r] * 2 + 1;
        st7735s_fill_rect((uint16_t)(cx - half_w[r]), (uint16_t)(cy - 3 + r), (uint16_t)w, 1, col);
    }
}

/* A menu cursor moved by `delta` rows in a list of `count` rows: it wraps at both ends (up from the first row is the last row, down from the last is the first), like the preset list. */
static uint8_t menu_wrap(int cur, int delta, int count) {
    int c = (cur + delta) % count;
    if (c < 0) c += count;
    return (uint8_t)c;
}

/* The Bluetooth setting changed (0 OFF, 1 BLE, 2 BLE + A2DP): the board turns the radio's advertising on or off (main.c). The host builds have no radio. */
__attribute__((weak)) void pa_board_bt_mode_changed(uint8_t mode) { (void)mode; }

/* IMP-04: status cluster (USB + Bluetooth + battery) at fixed anchors on every screen */
/* `square`: the small square of the preset list (battery top right, its % under it, Bluetooth beside the battery, the USB diamond under Bluetooth). Every other screen: one row on the
 * centre line, evenly spaced (8 px between items), from the right: battery, its % to the left of it, Bluetooth, the USB diamond (only while a host is connected). */
static void draw_status_cluster(const pa_ui_ctx_t *ui, int square) {
    s_draw_charging = ui->charging;
    /* The square only exists while the battery % is shown (it needs the second row for the %). Without the %, the preset list uses the very same row as every other screen, so the icons sit at the
     * same x on the list, the grid and the Settings screens (the square's Bluetooth / diamond used to be 3 px off the row's). */
    if (square && ui->display_battery_pct) {
        if (ui->usb_connected) draw_usb_diamond(197u, (uint16_t)(STATUS_CY + 15u), ONYX_COL_WARN);   /* the second row is centred between the battery (rows 9..19) and the line at y=39 */
        draw_bluetooth(197u, STATUS_CY, get_bluetooth_color(ui->bt_state));
        (void)draw_battery_r(STATUS_RIGHT_X, STATUS_CY, ui->batt_pct, ui->display_battery_pct, ui->batt_warn, 1u);
        return;
    }
    const uint16_t left = draw_battery_r(STATUS_RIGHT_X, STATUS_CY, ui->batt_pct, ui->display_battery_pct, ui->batt_warn, 0u);
    const uint16_t bt_x = (uint16_t)(left - 8u - BT_ICON_HALF_W - 1u);   /* 8 px of space between the glyph's right edge (bt_x+7) and what is to its right */
    draw_bluetooth(bt_x, STATUS_CY, get_bluetooth_color(ui->bt_state));
    if (ui->usb_connected) draw_usb_diamond((uint16_t)(bt_x - BT_ICON_HALF_W - 8u - 4u), STATUS_CY, ONYX_COL_WARN);   /* the glyph's left edge + 8 px of space + the diamond's half width */
}

/* Speaker icon */
static void draw_speaker(uint16_t x, uint16_t y, uint16_t col) {
    st7735s_fill_rect(x, y + 3, 3, 4, col);
    st7735s_fill_rect(x + 3, y + 2, 2, 6, col);
    st7735s_fill_rect(x + 5, y + 1, 2, 8, col);
    st7735s_fill_rect(x + 9, y + 2, 1, 6, col);
    st7735s_fill_rect(x + 11, y, 1, 10, col);
}

/* Format 0..100 percentage as 0.0..10.0 (like 6.5, 5.0, 4.5) */
static void fmt_val_10(char *buf, int32_t pct) {
    if (pct < 0) pct = 0;
    if (pct > 100) pct = 100;
    if (pct >= 100) {
        buf[0] = '1'; buf[1] = '0'; buf[2] = '.'; buf[3] = '0'; buf[4] = '\0';
    } else {
        buf[0] = (char)('0' + (pct / 10));
        buf[1] = '.';
        buf[2] = (char)('0' + (pct % 10));
        buf[3] = '\0';
    }
}

/* Onyx modular parameter card widget (replacing radial arc wheels) */
/* The value a knob card shows. Most knobs are 0.0 .. 10.0; the ones with a real unit
 * show it: delay TIME and reverb DECAY in ms. Returns the unit ("" for none). */
static const char *param_value_text(const pa_ui_ctx_t *ui, uint8_t slot, uint8_t p,
                                    int32_t val_q21, int32_t pct, char *buf) {
    uint32_t ms = 0;
    int has_ms = 0;
    if (slot == PA_SLOT_DELAY && p == 0) {
        ms = pa_dly_time_ms(ui ? (int)ui->slot_type[PA_SLOT_DELAY] : 0, val_q21); has_ms = 1;
    } else if (slot == PA_SLOT_REVERB && p == 0) {
        ms = pa_rev_decay_ms(ui ? (int)ui->slot_type[PA_SLOT_REVERB] : 1, val_q21); has_ms = 1;
    }
    if (slot == PA_SLOT_EQ && ui && ui->slot_type[PA_SLOT_EQ] != 0u) {
        int et = (int)ui->slot_type[PA_SLOT_EQ];
        uint32_t hz = pa_eq_freq_hz(et, p, val_q21);
        int32_t  td = pa_eq_gain_tenth_db(et, p, val_q21);
        int n = 0;
        if (hz) {
            if (hz < 1000u) {                                  /* "120" Hz */
                char t[4]; int k = 0;
                do { t[k++] = (char)('0' + hz % 10u); hz /= 10u; } while (hz && k < 3);
                while (k) buf[n++] = t[--k];
                buf[n] = '\0';
                return "Hz";
            }
            uint32_t d = (hz + 50u) / 100u;                   /* "6.3" / "20.0" kHz */
            if (d >= 100u) buf[n++] = (char)('0' + (d / 100u) % 10u);
            buf[n++] = (char)('0' + (d / 10u) % 10u);
            buf[n++] = '.';
            buf[n++] = (char)('0' + d % 10u);
            buf[n] = '\0';
            return "kHz";
        }
        if (td != INT32_MIN) {                                 /* "+2.4" / "-12" dB */
            buf[n++] = (td < 0) ? '-' : '+';
            uint32_t a = (uint32_t)(td < 0 ? -td : td);
            if (a >= 100u) {                                   /* whole dB from 10 up */
                uint32_t w = (a + 5u) / 10u;
                buf[n++] = (char)('0' + (w / 10u) % 10u);
                buf[n++] = (char)('0' + w % 10u);
            } else {
                buf[n++] = (char)('0' + a / 10u);
                buf[n++] = '.';
                buf[n++] = (char)('0' + a % 10u);
            }
            buf[n] = '\0';
            return "dB";
        }
    }
    if (slot == PA_SLOT_DYNAMICS && p < 3) {
        /* THRESH in dBFS (OFF at 0), RATIO as x:1, the times in ms (pa_dyn_display) */
        int unit;
        int32_t v = pa_dyn_display(ui ? (int)ui->slot_type[PA_SLOT_DYNAMICS] : 0, p, val_q21, &unit);
        if (unit == PA_DYN_U_OFF) { buf[0] = 'O'; buf[1] = 'F'; buf[2] = 'F'; buf[3] = '\0'; return ""; }
        if (unit != PA_DYN_U_NONE) {
            int n = 0;
            if (v < 0) { buf[n++] = '-'; v = -v; }
            uint32_t a = (uint32_t)v;
            if (a >= 100u) {                                   /* whole units from 10 up */
                uint32_t w = (a + 5u) / 10u;
                char tmp[5]; int k = 0;
                do { tmp[k++] = (char)('0' + w % 10u); w /= 10u; } while (w && k < 4);
                while (k) buf[n++] = tmp[--k];
            } else {                                           /* "4.5", "0.5" */
                buf[n++] = (char)('0' + a / 10u);
                buf[n++] = '.';
                buf[n++] = (char)('0' + a % 10u);
            }
            buf[n] = '\0';
            return unit == PA_DYN_U_DB ? "dB" : unit == PA_DYN_U_RATIO ? ":1" : "ms";
        }
    }
    if (slot == PA_SLOT_MOD && p == 0) {
        /* RATE in Hz, two decimals: 0.10 .. 8.10 */
        uint32_t c = (pa_mod_rate_mhz(val_q21) + 5u) / 10u;          /* centi-Hz */
        buf[0] = (char)('0' + (c / 100u) % 10u);
        buf[1] = '.';
        buf[2] = (char)('0' + (c / 10u) % 10u);
        buf[3] = (char)('0' + c % 10u);
        buf[4] = '\0';
        return "Hz";
    }
    if (!has_ms) { fmt_val_10(buf, pct); return ""; }
    if (ms > 99999u) ms = 99999u;
    char tmp[6]; int n = 0;
    do { tmp[n++] = (char)('0' + ms % 10u); ms /= 10u; } while (ms && n < 5);
    for (int i = 0; i < n; i++) buf[i] = tmp[n - 1 - i];
    buf[n] = '\0';
    return "ms";
}

static void draw_param_card(uint16_t x, uint16_t y, uint16_t w, uint16_t h,
                            const char *label, int32_t pct, const char *vtxt, const char *unit,
                            uint16_t col, int is_sel) {
    /* 1. Background tile */
    fill_rounded_box(x, y, w, h, ONYX_COL_PANEL);

    /* 2. Focus / Edit outline */
    if (is_sel == 1) {
        /* Hover focus: yellow border */
        draw_rounded_rect(x, y, w, h, ONYX_COL_SELECT);
    } else if (is_sel == 2) {
        /* Value adjust edit mode: green border */
        draw_rounded_rect(x, y, w, h, ONYX_COL_OK);
        draw_rounded_rect(x + 1, y + 1, w - 2, h - 2, ONYX_COL_OK);
    } else {
        draw_rounded_rect(x, y, w, h, ONYX_COL_LINE);
    }

    /* 3. Parameter label at top left */
    uint16_t lcol = (is_sel == 2) ? ONYX_COL_OK : ((is_sel == 1) ? ONYX_COL_SELECT : ONYX_COL_MUTE);
    ui_draw_str(x + 6, y + 6, label, lcol, ONYX_COL_PANEL);

    /* 4. Large bold value in center (0.0 .. 10.0, or a number + a small unit) */
    uint16_t vlen = 0, ulen = 0;
    while (vtxt[vlen]) vlen++;
    while (unit && unit[ulen]) ulen++;
    uint16_t text_w = (uint16_t)(vlen * 12u - 2u);           /* 2x font width */
    if (ulen) text_w = (uint16_t)(text_w + 3u + ulen * 6u - 1u);  /* + a gap + the 1x unit */
    uint16_t vx = (w > text_w) ? (x + (w - text_w) / 2) : (x + 4);
    uint16_t vy = y + (h / 2) - 7;
    ui_draw_str_scaled(vx, vy, vtxt, ONYX_COL_TEXT, ONYX_COL_PANEL, 2);
    if (ulen)
        ui_draw_str((uint16_t)(vx + vlen * 12u + 1u), (uint16_t)(vy + 7u), unit, ONYX_COL_MUTE, ONYX_COL_PANEL);

    /* 5. Horizontal mini-slider progress bar at bottom */
    uint16_t bar_pad = 6;
    uint16_t bar_w_max = (w > bar_pad * 2) ? (w - bar_pad * 2) : 20;
    uint16_t bar_y = y + h - 9;
    st7735s_fill_rect(x + bar_pad, bar_y, bar_w_max, 4, ONYX_COL_LINE);

    uint16_t fill_w = (uint16_t)((pct * (int32_t)bar_w_max) / 100);
    if (fill_w > bar_w_max) fill_w = bar_w_max;
    if (fill_w > 0) {
        st7735s_fill_rect(x + bar_pad, bar_y, fill_w, 4, col);
    }
}

/* 5-band vertical EQ slider widget for EQ block and Global EQ */
static void draw_eq_bands(uint16_t x0, uint16_t x1, uint16_t top, uint16_t bot,
                          const int8_t bands[5], int8_t cursor_band, uint16_t col, int is_editing) {
    uint16_t mid = (top + bot) / 2;
    st7735s_fill_rect(x0, mid, x1 - x0, 1, ONYX_COL_LINE);

    /* +/-12 dB scale markers */
    ui_draw_str(x1 - 18, top - 6, "+12", ONYX_COL_DIM, ONYX_COL_BG);
    ui_draw_str(x1 - 18, bot + 2, "-12", ONYX_COL_DIM, ONYX_COL_BG);

    static const char * const freqs[5] = { "100", "400", "1k", "3k", "8k" };
    uint16_t step = (x1 - x0 - 24) / 5;

    for (int i = 0; i < 5; i++) {
        uint16_t cx = x0 + 12 + i * step + step / 2;
        int is_sel = (cursor_band == i);
        uint16_t ink = is_sel ? (is_editing ? ONYX_COL_OK : ONYX_COL_SELECT) : col;

        /* Rail */
        st7735s_fill_rect(cx - 1, top, 2, bot - top, ONYX_COL_LINE);

        /* Value offset: -12..+12 dB */
        int8_t db = bands[i];
        if (db < -12) db = -12;
        if (db > 12)  db = 12;

        int16_t thumb_y = (int16_t)(mid - (db * (int32_t)(bot - top)) / 24);
        if (thumb_y < top) thumb_y = top;
        if (thumb_y > bot) thumb_y = bot;

        /* Colored bar from center to thumb */
        if (thumb_y < mid) {
            st7735s_fill_rect(cx - 1, (uint16_t)thumb_y, 2, mid - (uint16_t)thumb_y, ink);
        } else if (thumb_y > mid) {
            st7735s_fill_rect(cx - 1, mid, 2, (uint16_t)thumb_y - mid, ink);
        }

        /* Thumb box */
        fill_rounded_box(cx - 9, (uint16_t)(thumb_y - 4), 18, 8, ink);

        /* Frequency label */
        uint16_t flen = 0;
        while (freqs[i][flen]) flen++;
        uint16_t fx = cx - (flen * 6) / 2;
        ui_draw_str(fx, bot + 8, freqs[i], is_sel ? (is_editing ? ONYX_COL_OK : ONYX_COL_SELECT) : ONYX_COL_MUTE, ONYX_COL_BG);

        /* dB value */
        char vbuf[6];
        if (db > 0) {
            vbuf[0] = '+';
            fmt_u2(&vbuf[1], (uint32_t)db);
        } else if (db < 0) {
            vbuf[0] = '-';
            fmt_u2(&vbuf[1], (uint32_t)(-db));
        } else {
            vbuf[0] = '0';
            vbuf[1] = '\0';
        }
        uint16_t vlen = 0;
        while (vbuf[vlen]) vlen++;
        uint16_t vx = cx - (vlen * 6) / 2;
        ui_draw_str(vx, bot + 20, vbuf, ONYX_COL_TEXT, ONYX_COL_BG);
    }
}



/* Map PA_SLOT_* to Onyx icon array index in onyx_block_icons.h */
static uint8_t slot_to_onyx_icon_idx(uint8_t slot) {
    switch (slot) {
        case PA_SLOT_DYNAMICS: return ONYX_SLOT_DYN;   /* 0 */
        case PA_SLOT_DRIVE:    return ONYX_SLOT_DRIVE; /* 1 */
        case PA_SLOT_AMP:      return ONYX_SLOT_AMP;   /* 2 */
        case PA_SLOT_CAB:      return ONYX_SLOT_CAB;   /* 6 */
        case PA_SLOT_EQ:       return ONYX_SLOT_EQ;    /* 7 */
        case PA_SLOT_MOD:      return ONYX_SLOT_MOD;   /* 3 */
        case PA_SLOT_DELAY:    return ONYX_SLOT_DLY;   /* 4 */
        case PA_SLOT_REVERB:   return ONYX_SLOT_REV;   /* 5 */
        default:               return 0;
    }
}

/* 8 Bit-Exact 28x28 Pixel-Art Icons for Home Grid View matching Onyx ICON_SPEC.md */
/* What the screens show as ON: a block that is fading out (the audio thread clears `active` only when the fade ends, up to a block later and with no redraw after it) is
 * off already. The home grid used this test; the editor, the list view and the block title did not and kept saying ON until something else redrew them. */
#define NODE_SHOWN_ON(nd) ((nd) && (nd)->active && (nd)->fade_dir >= 0)

static void draw_grid_icon(uint16_t tile_x, uint16_t tile_y, uint8_t slot, int is_on) {
    uint8_t icon_idx = slot_to_onyx_icon_idx(slot);
    const uint8_t *px = onyx_icons[icon_idx];
    const uint16_t *pal = onyx_icon_palette[icon_idx][is_on ? 0 : 1];
    const int x0 = (int)tile_x + ONYX_ICON_TILE_DX;
    const int y0 = (int)tile_y + ONYX_ICON_TILE_DY;

    for (int y = 0; y < ONYX_ICON_H; y++) {
        int x = 0;
        const uint8_t *row = &px[y * ONYX_ICON_W];
        while (x < ONYX_ICON_W) {
            uint8_t idx = row[x];
            if (idx == 0) {
                x++;
                continue;
            }
            int run = 1;
            while (x + run < ONYX_ICON_W && row[x + run] == idx) {
                run++;
            }
            st7735s_fill_rect((uint16_t)(x0 + x), (uint16_t)(y0 + y), (uint16_t)run, 1, pal[idx]);
            x += run;
        }
    }
}

/* Render Home Screen Header */
static void render_home_header(const pa_ui_ctx_t *ui) {
    /* 1. "PRESET" label at (10, 19) */
    ui_draw_str(10, 19, (ui->play_mode == 1) ? "STOMP" : "PRESET", (ui->play_mode == 1) ? ONYX_COL_OK : ONYX_COL_MUTE, ONYX_COL_BG);

    /* 2. Yellow preset identifier "P08" at (8, 32) in 3x scale */
    char pstr[4];
    fmt_preset(pstr, ui->preset_index);
    ui_draw_str_scaled(8, 32, pstr, ONYX_COL_SELECT, ONYX_COL_BG, 3);

    /* 3. Status icons: USB + Bluetooth + Battery (one row on the y=14 centre line) */
    draw_status_cluster(ui, 0);

    /* 4. Segmented IN / OUT level meters (symmetrically framing preset number) */
    draw_seg_meter(119, 38, 104, 5, ui->in_peak_pct, "IN");
    draw_seg_meter(119, 51, 104, 5, ui->out_peak_pct, "OUT");

    /* 5. Moving Mode Header Banner OR Master Volume Bar */
    if (ui->view == PA_UI_VIEW_MOVING) {
        fill_rounded_box(8, 66, 224, 20, ONYX_COL_PANEL);
        draw_rounded_rect(8, 66, 224, 20, ONYX_COL_SELECT);
        char mov_buf[20] = "MOVING ";
        const char *slbl = get_slot_label(ui->moving_slot);
        int mi = 7;
        while (*slbl && mi < 18) mov_buf[mi++] = *slbl++;
        mov_buf[mi] = '\0';
        ui_draw_str(14, 72, mov_buf, ONYX_COL_SELECT, ONYX_COL_PANEL);
    } else {
        /* IMP-04: speaker icon (10 px), bar (4 px) and % text (7 px) share ONE centre line.
         * The old code had the icon centred at y=75.5 and the bar at y=76.5. */
        const uint16_t cy = 76;
        draw_speaker(10, (uint16_t)(cy - 5), ONYX_COL_MUTE);
        st7735s_fill_rect(28, (uint16_t)(cy - 2), 164, 4, ONYX_COL_LINE);
        uint16_t vw = (uint16_t)((ui->preset_volume * 164u) / 100u);
        if (vw > 164u) vw = 164u;
        if (vw > 0) {
            st7735s_fill_rect(28, (uint16_t)(cy - 2), vw, 4, ONYX_COL_TEXT);
        }
        char vstr[6];
        if (ui->preset_volume >= 100u) {
            vstr[0] = '1'; vstr[1] = '0'; vstr[2] = '0'; vstr[3] = '%'; vstr[4] = '\0';
        } else {
            fmt_u2(vstr, ui->preset_volume);
            vstr[2] = '%'; vstr[3] = '\0';
        }
        ui_draw_str(198, (uint16_t)(cy - 3), vstr, ONYX_COL_TEXT, ONYX_COL_BG);
    }

    /* 6. Divider line across y = 90 in ONYX_COL_LINE */
    st7735s_fill_rect(0, 90, DISP_W, 1, ONYX_COL_LINE);

    /* IMP-12: low-battery banner replaces the volume row while the warning is active */
    if (ui->batt_warn && ui->view != PA_UI_VIEW_MOVING) {
        fill_rounded_box(8, 66, 224, 20, ONYX_COL_BAD);
        ui_draw_str(16, 72, (ui->batt_warn >= 2) ? "BATTERY CRITICAL - CHARGE NOW" : "LOW BATTERY - CHARGE SOON",
                    ONYX_COL_TEXT, ONYX_COL_BAD);
    }
}

/* IMP-07: STOMP mode footer - which button toggles which block, and its state */
static void render_stomp_strip(const pa_ui_ctx_t *ui, const pa_fx_chain_t *chain) {
    static const char * const k_keys[4] = { "VOL-", "VOL+", "<", ">" };
    st7735s_fill_rect(0, 64, DISP_W, 25, ONYX_COL_BG);          /* replaces the volume row */
    for (int i = 0; i < 4; i++) {
        uint8_t slot = ui->stomp_map[i] < PA_SLOT_COUNT ? ui->stomp_map[i] : PA_SLOT_DRIVE;
        const fx_node_t *nd = chain ? chain->slots[slot] : 0;
        int on = NODE_SHOWN_ON(nd);
        uint16_t x = (uint16_t)(6 + i * 58), col = get_slot_color(slot);
        fill_rounded_box(x, 66, 54, 20, on ? col : ONYX_COL_PANEL);
        if (!on) draw_rounded_rect(x, 66, 54, 20, ONYX_COL_DIM);
        ui_draw_str((uint16_t)(x + 4), 72, k_keys[i], on ? ONYX_COL_BG : ONYX_COL_MUTE, on ? col : ONYX_COL_PANEL);
        ui_draw_str((uint16_t)(x + 30), 72, get_slot_short(slot), on ? ONYX_COL_BG : ONYX_COL_TEXT, on ? col : ONYX_COL_PANEL);
    }
}

/* Render Home Screen 2x4 Grid matching tokens.py & screens.py */
static void render_home_grid(const pa_ui_ctx_t *ui, const pa_fx_chain_t *chain) {
    /* 1. Continuous Signal Path Underlay */
    /* Enters at (2, 129) -> across row 1 to (235, 129) -> down to gap y = 166 ->
     * returns left across gap to (5, 166) -> down to row 2 at (5, 203) -> exits at (238, 203) */
    st7735s_fill_rect(2, 128, 233, 2, ONYX_COL_DIM);
    st7735s_fill_rect(234, 128, 2, 38, ONYX_COL_DIM);
    st7735s_fill_rect(5, 165, 231, 2, ONYX_COL_DIM);
    st7735s_fill_rect(5, 165, 2, 38, ONYX_COL_DIM);
    st7735s_fill_rect(5, 202, 233, 2, ONYX_COL_DIM);

    if (ui->play_mode == 1 && ui->view == PA_UI_VIEW_MAIN_GRID && !ui->batt_warn) render_stomp_strip(ui, chain);

    /* 2. 8 Tiles in 2x4 Grid at GRID_COLS = [9, 66, 124, 181], GRID_ROWS = [100, 174] */
    const uint16_t cols[4] = { 9, 66, 124, 181 };
    const uint16_t rows[2] = { 100, 174 };

    for (int i = 0; i < 8; i++) {
        uint16_t x = cols[i % 4];
        uint16_t y = rows[i / 4];
        uint8_t slot = (chain && chain->slot_order[i] < PA_SLOT_COUNT) ? chain->slot_order[i] : i;
        const fx_node_t *nd = chain ? chain->slots[slot] : 0;
        int on = NODE_SHOWN_ON(nd) ? 1 : 0;
        /* The CAB can be muted by the output routing (LINE out + "CAB ON LINE OUT"
         * off). Show that on the tile instead of pretending the cabinet is there -
         * this is the "the cab disappeared" case. */
        if (slot == PA_SLOT_CAB && chain && chain->force_cab_off) on = 0;
        int is_moving = (ui->view == PA_UI_VIEW_MOVING && i == ui->cursor_slot_idx);
        int is_cursor = (ui->view != PA_UI_VIEW_MOVING && i == ui->cursor_slot_idx);

        if (is_moving) {
            y -= 4; /* Raised when lifted */
            /* Yellow outline */
            draw_rounded_rect(x - 2, y - 2, 54, 62, ONYX_COL_SELECT);
            /* Left/Right side arrow indicators */
            ui_draw_str(x - 6, y + 24, "<", ONYX_COL_SELECT, ONYX_COL_BG);
            ui_draw_str(x + 52, y + 24, ">", ONYX_COL_SELECT, ONYX_COL_BG);
        } else if (is_cursor) {
            draw_rounded_rect(x - 2, y - 2, 54, 62, ONYX_COL_SELECT);
            draw_rounded_rect(x - 3, y - 3, 56, 64, ONYX_COL_SELECT);
        }

        /* Tile card box (50x58 px) */
        fill_rounded_box(x, y, 50, 58, ONYX_COL_PANEL);

        /* Border */
        uint16_t col = get_slot_color(slot);
        draw_rounded_rect(x, y, 50, 58, on ? col : ONYX_COL_DIM);

        /* Corner LED dot */
        fill_circle(x + 42, y + 8, 2, on ? col : ONYX_COL_LINE);

        /* Center glyph: 28x28 icon placed at (x + 11, y + 10) per ICON_SPEC.md */
        draw_grid_icon(x, y, slot, on);

        /* Bottom label */
        const char *lbl = get_slot_label(slot);
        if (slot == PA_SLOT_CAB && chain && chain->force_cab_off) {
            /* explicit "no cab" marker so the state is never ambiguous */
            ui_draw_str(x + 8, y + 46, "NOCAB", ONYX_COL_BAD, ONYX_COL_PANEL);
        }
        int is_a2 = (slot == PA_SLOT_AMP && is_a2_capture_active(ui));
        if (is_a2) {
            /* Draw "AMP" and "A2" side-by-side centered in 50px tile */
            /* "AMP" (18px) + 2px space + "A2" (12px) = 32px width */
            uint16_t tx = (uint16_t)(x + 25 - 16);
            ui_draw_str(tx, y + 46, "AMP", on ? ONYX_COL_TEXT : ONYX_COL_MUTE, ONYX_COL_PANEL);
            ui_draw_str(tx + 20, y + 46, "A2", on ? ONYX_COL_SELECT : ONYX_COL_DIM, ONYX_COL_PANEL);
        } else {
            uint16_t len = 0;
            while (lbl[len]) len++;
            uint16_t tx = (uint16_t)(x + 25 - (len * 6) / 2);
            ui_draw_str(tx, y + 46, lbl, on ? ONYX_COL_TEXT : ONYX_COL_MUTE, ONYX_COL_PANEL);
        }
    }
}

/* Alternative Home List View (Settings > Display > Home view = LIST) */
static void render_home_list(const pa_ui_ctx_t *ui, const pa_fx_chain_t *chain) {
    /* Header */
    char pstr[4];
    fmt_preset(pstr, ui->preset_index);
    ui_draw_str_scaled(8, 8, pstr, ONYX_COL_SELECT, ONYX_COL_BG, 3);
    if (ui->view == PA_UI_VIEW_MOVING) {
        ui_draw_str(8, 30, "MOVING", ONYX_COL_SELECT, ONYX_COL_BG);
    } else if (ui->play_mode == 1) {
        ui_draw_str(8, 30, "STOMP", ONYX_COL_OK, ONYX_COL_BG);
    }

    /* Middle empty space: IN, OUT, and VOL stacked vertically with clean spacing */
    /* 1. IN level bar */
    ui_draw_str(74, 6, "IN", ONYX_COL_MUTE, ONYX_COL_BG);
    st7735s_fill_rect(98, 8, 64, 4, ONYX_COL_LINE);
    uint16_t in_w = (uint16_t)((ui->in_peak_pct * 64u) / 100u);
    if (in_w > 64u) in_w = 64u;
    if (in_w > 0) st7735s_fill_rect(98, 8, in_w, 4, (ui->in_peak_pct > 92) ? ONYX_COL_BAD : ONYX_COL_OK);

    /* 2. OUT level bar */
    ui_draw_str(74, 16, "OUT", ONYX_COL_MUTE, ONYX_COL_BG);
    st7735s_fill_rect(98, 18, 64, 4, ONYX_COL_LINE);
    uint16_t out_w = (uint16_t)((ui->out_peak_pct * 64u) / 100u);
    if (out_w > 64u) out_w = 64u;
    if (out_w > 0) st7735s_fill_rect(98, 18, out_w, 4, (ui->out_peak_pct > 92) ? ONYX_COL_BAD : ONYX_COL_OK);

    /* 3. Preset Volume bar */
    ui_draw_str(74, 26, "VOL", ONYX_COL_MUTE, ONYX_COL_BG);
    st7735s_fill_rect(98, 28, 46, 4, ONYX_COL_LINE);
    uint16_t vw = (uint16_t)((ui->preset_volume * 46u) / 100u);
    if (vw > 46u) vw = 46u;
    if (vw > 0) st7735s_fill_rect(98, 28, vw, 4, ONYX_COL_TEXT);
    char vstr[6];
    if (ui->preset_volume >= 100u) {
        vstr[0] = '1'; vstr[1] = '0'; vstr[2] = '0'; vstr[3] = '%'; vstr[4] = '\0';
    } else {
        fmt_u2(vstr, ui->preset_volume);
        vstr[2] = '%'; vstr[3] = '\0';
    }
    ui_draw_str(148, 26, vstr, ONYX_COL_TEXT, ONYX_COL_BG);

    draw_status_cluster(ui, 1);

    st7735s_fill_rect(0, 39, DISP_W, 1, ONYX_COL_LINE);

    /* 8 rows of 24px */
    for (int i = 0; i < 8; i++) {
        uint8_t slot = (chain && chain->slot_order[i] < PA_SLOT_COUNT) ? chain->slot_order[i] : i;
        const fx_node_t *nd = chain ? chain->slots[slot] : 0;
        int on = NODE_SHOWN_ON(nd) ? 1 : 0;
        int sel = (i == ui->cursor_slot_idx);
        uint16_t y = (uint16_t)(44 + i * 24);

        if (sel) {
            fill_rounded_box(4, y + 1, 232, 22, ONYX_COL_PANEL);
            st7735s_fill_rect(4, y + 5, 3, 14, ONYX_COL_SELECT);
            if (ui->view == PA_UI_VIEW_MOVING) {
                draw_rounded_rect(4, y + 1, 232, 22, ONYX_COL_SELECT);
            }
        }

        /* Block chip */
        uint16_t bcol = get_slot_color(slot);
        fill_rounded_box(12, y + 3, 38, 16, on ? bcol : ONYX_COL_PANEL);
        if (!on) draw_rounded_rect(12, y + 3, 38, 16, ONYX_COL_DIM);
        const char *sh = get_slot_short(slot);
        uint16_t shl = 0; while (sh[shl]) shl++;
        /* IMP-04: centre the 2/3-letter label in the 38 px chip (was left-aligned at x=20) */
        ui_draw_str((uint16_t)(12 + (38 - (shl * 6 - 1)) / 2), y + 7, sh, on ? ONYX_COL_BG : ONYX_COL_MUTE, on ? bcol : ONYX_COL_PANEL);

        /* Model name, truncated so it never runs into the OFF / chevron column */
        char mbuf[24];
        const char *mname_full = get_slot_model_name(ui, slot, ui->slot_type[slot]);
        {
            int max_c = on ? 26 : 21;                 /* (196-58)/6 = 23 with OFF, (218-58)/6 = 26 */
            if (ui->play_mode == 1) max_c = 16;       /* STOMP mode: the key tag sits at x 158..194 */
            int k = 0;
            while (mname_full[k] && k < max_c && k < (int)sizeof(mbuf) - 1) { mbuf[k] = mname_full[k]; k++; }
            if (mname_full[k] && k >= 2) { mbuf[k - 1] = '.'; mbuf[k - 2] = '.'; }
            mbuf[k] = '\0';
        }
        const char *mname = mbuf;
        if (slot == PA_SLOT_AMP && is_a2_capture_active(ui)) {
            char base[20];
            int max_base_len = 15;
            strncpy(base, mname, max_base_len);
            base[max_base_len] = '\0';
            int blen = (int)strlen(base);
            while (blen > 0 && base[blen - 1] == ' ') { base[--blen] = '\0'; }
            ui_draw_str(58, y + 7, base, on ? ONYX_COL_TEXT : ONYX_COL_MUTE, sel ? ONYX_COL_PANEL : ONYX_COL_BG);
            uint16_t tag_x = (uint16_t)(58 + (blen + 1) * 6);
            ui_draw_str(tag_x, y + 7, "[A2]", on ? ONYX_COL_SELECT : ONYX_COL_DIM, sel ? ONYX_COL_PANEL : ONYX_COL_BG);
        } else {
            ui_draw_str(58, y + 7, mname, on ? ONYX_COL_TEXT : ONYX_COL_MUTE, sel ? ONYX_COL_PANEL : ONYX_COL_BG);
        }

        /* STOMP mode: the key(s) that toggle this effect, in a small tag after the model name (the grid has the strip for this) */
        if (ui->play_mode == 1) {
            static const char * const k_tag[4] = { "V-", "V+", "<", ">" };
            char tag[8];
            int tn = 0;
            for (int k = 0; k < 4; k++) {
                if (ui->stomp_map[k] != slot) continue;
                for (const char *c = k_tag[k]; *c && tn < 7; c++) tag[tn++] = *c;
            }
            tag[tn] = '\0';
            if (tn > 0) {
                const uint16_t tw = (uint16_t)(tn * 6 + 6);
                const uint16_t tx = (uint16_t)(194 - tw);
                fill_rounded_box(tx, y + 3, tw, 16, on ? bcol : ONYX_COL_PANEL_HI);
                if (!on) draw_rounded_rect(tx, y + 3, tw, 16, ONYX_COL_DIM);
                ui_draw_str((uint16_t)(tx + 3), y + 7, tag, on ? ONYX_COL_BG : ONYX_COL_MUTE, on ? bcol : ONYX_COL_PANEL_HI);
            }
        }

        /* Status & chevron / movement indicator */
        if (!on) {
            ui_draw_str(196, y + 7, "OFF", ONYX_COL_MUTE, sel ? ONYX_COL_PANEL : ONYX_COL_BG);
        }
        if (ui->view == PA_UI_VIEW_MOVING && sel) {
            ui_draw_str(218, y + 7, "< >", ONYX_COL_SELECT, ONYX_COL_PANEL);
        } else {
            ui_draw_str(224, y + 7, ">", sel ? ONYX_COL_SELECT : ONYX_COL_DIM, sel ? ONYX_COL_PANEL : ONYX_COL_BG);
        }
    }
}

/* Sub-screen Title Bar */
static void render_sub_title_bar(const char *title, uint16_t color, int has_toggle, int is_on, int is_toggle_sel, const pa_ui_ctx_t *ui) {
    ui_draw_str(10, 10, "<", ONYX_COL_MUTE, ONYX_COL_BG);
    ui_draw_str(28, 10, title, color, ONYX_COL_BG);

    if (has_toggle) {
        if (is_toggle_sel) {
            draw_rounded_rect(184, 3, 48, 24, ONYX_COL_SELECT);
        }
        if (is_on) {
            fill_rounded_box(186, 5, 44, 20, ONYX_COL_TEXT);
            ui_draw_str(192, 11, "ON", ONYX_COL_BG, ONYX_COL_TEXT);
            fill_circle(220, 15, 7, ONYX_COL_BG);
        } else {
            fill_rounded_box(186, 5, 44, 20, ONYX_COL_PANEL);
            draw_rounded_rect(186, 5, 44, 20, ONYX_COL_DIM);
            fill_circle(196, 15, 7, ONYX_COL_DIM);
            ui_draw_str(206, 11, "OFF", ONYX_COL_MUTE, ONYX_COL_PANEL);
        }
    } else if (ui) {
        draw_status_cluster(ui, 0);
    }

    st7735s_fill_rect(0, 30, DISP_W, 1, ONYX_COL_LINE);
}

/* Render Block Parameter Editor */
static void render_param_edit(pa_ui_ctx_t *ui, const pa_fx_chain_t *chain) {
    uint8_t slot = (chain && ui->cursor_slot_idx < 8) ? chain->slot_order[ui->cursor_slot_idx] : PA_SLOT_AMP;
    if (slot >= PA_SLOT_COUNT) slot = PA_SLOT_AMP;
    ui->selected_slot = slot;

    const fx_node_t *nd = chain ? chain->slots[slot] : 0;
    int is_active = NODE_SHOWN_ON(nd) ? 1 : 0;
    uint16_t blk_col = get_slot_color(slot);
    const char *blk_lbl = get_slot_label(slot);

    int is_toggle_sel = (ui->editor_cursor == -2);
    int is_model_sel  = (ui->editor_cursor == -1);

    /* Title Bar with module icon glyph & ON/OFF switch toggle pill */
    render_sub_title_bar(blk_lbl, is_active ? blk_col : ONYX_COL_MUTE, 1, is_active, is_toggle_sel, ui);

    /* Model Selector Card at (8, 36, w=224, h=48) */
    fill_rounded_box(8, 36, 224, 48, ONYX_COL_PANEL);
    uint16_t m_border = is_model_sel ? (ui->edit_mode ? ONYX_COL_OK : ONYX_COL_SELECT) : ONYX_COL_LINE;
    draw_rounded_rect(8, 36, 224, 48, m_border);
    if (is_model_sel && ui->edit_mode) {
        draw_rounded_rect(7, 35, 226, 50, ONYX_COL_OK);
    }

    /* Left accent vertical bar */
    st7735s_fill_rect(8, 40, 4, 40, is_active ? blk_col : ONYX_COL_DIM);

    /* Left and Right navigation chevrons */
    ui_draw_str(16, 52, "<", is_model_sel ? (ui->edit_mode ? ONYX_COL_OK : ONYX_COL_SELECT) : ONYX_COL_MUTE, ONYX_COL_PANEL);
    ui_draw_str(218, 52, ">", is_model_sel ? (ui->edit_mode ? ONYX_COL_OK : ONYX_COL_SELECT) : ONYX_COL_MUTE, ONYX_COL_PANEL);

    /* Big model name */
    const char *mname = get_slot_model_name(ui, slot, ui->slot_type[slot]);
    uint16_t mlen = 0;
    while (mname[mlen]) mlen++;
    if (mlen <= 12) {
        ui_draw_str_scaled(30, 42, mname, ONYX_COL_TEXT, ONYX_COL_PANEL, 2);
    } else {
        ui_draw_str(30, 45, mname, ONYX_COL_TEXT, ONYX_COL_PANEL);
    }

    /* Sub-row: [ STOCK MODEL ]  AMP 09 OF 20  OR  [ NAM A2 CAPTURE ]  NAM 21 OF 21 */
    const char *kind = get_slot_model_kind(ui, slot);
    uint16_t klen = 0;
    while (kind[klen]) klen++;
    uint16_t badge_w = (uint16_t)(klen * 6 + 10);
    fill_rounded_box(30, 64, badge_w, 14, is_active ? blk_col : ONYX_COL_DIM);
    ui_draw_str(35, 67, kind, ONYX_COL_BG, is_active ? blk_col : ONYX_COL_DIM);

    /* Detail text */
    uint8_t total_m, cur_m;
    model_position(ui, slot, &cur_m, &total_m);
    char det_buf[24];
    int di = 0;
    const char *det_prefix = (slot == PA_SLOT_AMP) ?
        ((cur_m > PA_STOCK_AMP_COUNT) ? "NAM " : "AMP ") :
        ((slot == PA_SLOT_CAB) ? "IR " : "MODEL ");
    while (det_prefix[di]) { det_buf[di] = det_prefix[di]; di++; }
    det_buf[di++] = (char)('0' + (cur_m / 10u));
    det_buf[di++] = (char)('0' + (cur_m % 10u));
    det_buf[di++] = ' '; det_buf[di++] = 'O'; det_buf[di++] = 'F'; det_buf[di++] = ' ';
    det_buf[di++] = (char)('0' + (total_m / 10u));
    det_buf[di++] = (char)('0' + (total_m % 10u));
    det_buf[di] = '\0';
    ui_draw_str(32 + badge_w + 6, 68, det_buf, ONYX_COL_MUTE, ONYX_COL_PANEL);

    /* Dedicated rendering for EQ block: 5-band vertical sliders! */
    if (slot == PA_SLOT_EQ && ui->slot_type[PA_SLOT_EQ] == 0u) {
        draw_eq_bands(12, 228, 92, 192, ui->slot_bands_db,
                      (ui->editor_cursor >= 0 && ui->editor_cursor < 5) ? ui->editor_cursor : -1,
                      is_active ? blk_col : ONYX_COL_DIM, ui->edit_mode);
        return;
    }

    /* Modular parameter cards (replacing circles / radial wheels) */
    uint8_t nparams = get_slot_param_count(ui, slot);
    if (nparams == 6) {
        /* 6 Parameters (AMP): 2 rows of 3 cards (w=68, h=66) */
        const uint16_t cx[6] = { 8, 86, 164, 8, 86, 164 };
        const uint16_t cy[6] = { 90, 90, 90, 162, 162, 162 };
        for (int p = 0; p < 6; p++) {
            int is_sel = (ui->editor_cursor == p) ? (ui->edit_mode ? 2 : 1) : 0;
            int32_t val_q21 = ui->slot_params[slot][p];
            int32_t pct = (int32_t)(((int64_t)val_q21 * 100) / PA_Q21_ONE);
            if (pct < 0) pct = 0; if (pct > 100) pct = 100;
            char vtxt[8];
            const char *unit = param_value_text(ui, slot, (uint8_t)p, val_q21, pct, vtxt);
            draw_param_card(cx[p], cy[p], 68, 66,
                            get_slot_param_label(ui, slot, p),
                            pct, vtxt, unit, is_active ? blk_col : ONYX_COL_DIM, is_sel);
        }
    } else if (nparams == 4) {
        /* 4 Parameters (DYN, MOD, DELAY, REVERB): 2 rows of 2 cards (w=108, h=66) */
        const uint16_t cx[4] = { 8, 124, 8, 124 };
        const uint16_t cy[4] = { 90, 90, 162, 162 };
        for (int p = 0; p < 4; p++) {
            int is_sel = (ui->editor_cursor == p) ? (ui->edit_mode ? 2 : 1) : 0;
            int32_t val_q21 = ui->slot_params[slot][p];
            int32_t pct = (int32_t)(((int64_t)val_q21 * 100) / PA_Q21_ONE);
            if (pct < 0) pct = 0; if (pct > 100) pct = 100;
            char vtxt[8];
            const char *unit = param_value_text(ui, slot, (uint8_t)p, val_q21, pct, vtxt);
            draw_param_card(cx[p], cy[p], 108, 66,
                            get_slot_param_label(ui, slot, p),
                            pct, vtxt, unit, is_active ? blk_col : ONYX_COL_DIM, is_sel);
        }
    } else {
        /* 3 Parameters (DRV, CAB): 1 row of 3 cards (w=68, h=88) */
        const uint16_t cx[3] = { 8, 86, 164 };
        const uint16_t cy[3] = { 108, 108, 108 };
        for (int p = 0; p < nparams; p++) {
            int is_sel = (ui->editor_cursor == p) ? (ui->edit_mode ? 2 : 1) : 0;
            int32_t val_q21 = ui->slot_params[slot][p];
            int32_t pct = (int32_t)(((int64_t)val_q21 * 100) / PA_Q21_ONE);
            if (pct < 0) pct = 0; if (pct > 100) pct = 100;
            char vtxt[8];
            const char *unit = param_value_text(ui, slot, (uint8_t)p, val_q21, pct, vtxt);
            draw_param_card(cx[p], cy[p], 68, 88,
                            get_slot_param_label(ui, slot, p),
                            pct, vtxt, unit, is_active ? blk_col : ONYX_COL_DIM, is_sel);
        }
    }
}

/* Render Bank Mode Screen (4 presets per bank, 20 banks P01-P80) */
static void render_bank_mode(const pa_ui_ctx_t *ui, const pa_fx_chain_t *chain) {
    char bstr[12];
    bstr[0] = 'B'; bstr[1] = 'A'; bstr[2] = 'N'; bstr[3] = 'K'; bstr[4] = ' ';
    fmt_u2(&bstr[5], ui->bank_group + 1u);
    ui_draw_str(12, 14, bstr, ONYX_COL_MUTE, ONYX_COL_BG);

    uint16_t bstart = (uint16_t)(ui->bank_group * 4u + 1u);
    uint16_t bend = bstart + 3u;
    if (bend > PA_PRESET_COUNT) bend = PA_PRESET_COUNT;
    char rng_str[12];
    rng_str[0] = 'P'; fmt_u2(&rng_str[1], bstart);
    rng_str[3] = '-'; rng_str[4] = 'P'; fmt_u2(&rng_str[5], bend);
    ui_draw_str(74, 14, rng_str, ONYX_COL_DIM, ONYX_COL_BG);

    draw_status_cluster(ui, 0);

    /* Giant active preset number */
    char pstr[4];
    fmt_preset(pstr, ui->preset_index);
    ui_draw_str_scaled(80, 48, pstr, ONYX_COL_SELECT, ONYX_COL_BG, 4);
    ui_draw_str(94, 94, "BANK MODE", ONYX_COL_MUTE, ONYX_COL_BG);

    /* 8-block on/off strip */
    uint16_t sx = 10;
    for (int i = 0; i < 8; i++) {
        uint8_t s = (chain && chain->slot_order[i] < PA_SLOT_COUNT) ? chain->slot_order[i] : i;
        const fx_node_t *nd = chain ? chain->slots[s] : 0;
        int on = NODE_SHOWN_ON(nd) ? 1 : 0;
        uint16_t col = get_slot_color(s);

        fill_rounded_box(sx, 126, 25, 16, on ? col : ONYX_COL_PANEL);
        if (!on) draw_rounded_rect(sx, 126, 25, 16, ONYX_COL_DIM);
        const char *sh = get_slot_short(s);
        ui_draw_str(sx + 4, 130, sh, on ? ONYX_COL_BG : ONYX_COL_MUTE, on ? col : ONYX_COL_PANEL);
        sx += 28;
    }

    /* 4 Slots with dedicated hardware keycaps */
    static const char * const key_names[4] = { "VOL-", "VOL+", "<", ">" };
    for (int i = 0; i < 4; i++) {
        uint16_t x = (uint16_t)(14 + i * 56);
        uint16_t cur_p = (uint16_t)(bstart + i);
        int is_cur = (ui->preset_index == (cur_p - 1u));

        /* Keycap label underneath */
        fill_rounded_box(x + 8, 208, 32, 16, ONYX_COL_PANEL);
        draw_rounded_rect(x + 8, 208, 32, 16, is_cur ? ONYX_COL_SELECT : ONYX_COL_DIM);
        const char *kname = key_names[i];
        uint16_t klen = 0;
        while (kname[klen]) klen++;
        ui_draw_str(x + 24 - (klen * 6) / 2, 212, kname, is_cur ? ONYX_COL_SELECT : ONYX_COL_TEXT, ONYX_COL_PANEL);

        /* Slot card */
        if (cur_p <= PA_PRESET_COUNT) {
            fill_rounded_box(x, 154, 48, 48, is_cur ? ONYX_COL_SELECT : ONYX_COL_PANEL);
            if (!is_cur) draw_rounded_rect(x, 154, 48, 48, ONYX_COL_LINE);

            char num_str[3];
            fmt_u2(num_str, cur_p);
            ui_draw_str_scaled(x + 12, 168, num_str, is_cur ? ONYX_COL_BG : ONYX_COL_TEXT,
                               is_cur ? ONYX_COL_SELECT : ONYX_COL_PANEL, 2);
        } else {
            draw_rounded_rect(x, 154, 48, 48, ONYX_COL_LINE);
            ui_draw_str(x + 18, 172, "--", ONYX_COL_DIM, ONYX_COL_BG);
        }
    }

    ui_draw_str(14, 228, "< >: BANK   VOL -/+: SLOT   HOME: EXIT", ONYX_COL_MUTE, ONYX_COL_BG);
}

/* Render Tuner Screen */
static void render_tuner(const pa_ui_ctx_t *ui) {
    const pa_tuner_state_t *t = &ui->tuner;
    render_sub_title_bar("TUNER", ONYX_COL_TEXT, 0, 1, 0, ui);

    draw_seg_meter(12, 38, 216, 6, ui->in_peak_pct, "IN");

    if (t->signal_locked) {
        int8_t c = t->cents_disp;
        uint16_t col = (c >= -3 && c <= 3) ? ONYX_COL_OK : ((c >= -15 && c <= 15) ? ONYX_COL_WARN : ONYX_COL_BAD);
        if (t->held) col = ONYX_COL_MUTE;                    /* the signal has ended: the last reading stays (4 s), greyed, so a string can be played once and the peg turned */

        char nstr[4];
        nstr[0] = k_note_names[t->note_index % 12u][0];
        nstr[1] = k_note_names[t->note_index % 12u][1];
        if (nstr[1] == ' ') nstr[1] = '\0';
        nstr[2] = '\0';
        /* IMP-04: centre the note on x = 120 (was fixed at x = 96, so "C#" sat off-centre) */
        uint16_t nlen = nstr[1] ? 2u : 1u;
        uint16_t nx = (uint16_t)(120u - (nlen * 24u - 4u) / 2u);
        ui_draw_str_scaled(nx, 70, nstr, col, ONYX_COL_BG, 4);

        char ostr[2] = { (char)('0' + t->octave), '\0' };
        ui_draw_str((uint16_t)(nx + nlen * 24u + 2u), 88, ostr, ONYX_COL_MUTE, ONYX_COL_BG);

        /* IMP-04: the 5x7 font upper-cases everything, so "b" rendered as "B". Use the flat glyph. */
        char flat[2] = { UI_GLYPH_FLAT, '\0' };
        ui_draw_str(46, 84, flat, (c < -3) ? ONYX_COL_SELECT : ONYX_COL_LINE, ONYX_COL_BG);
        ui_draw_str(190, 84, "#", (c > 3) ? ONYX_COL_SELECT : ONYX_COL_LINE, ONYX_COL_BG);

        /* Cents scale -50..+50 */
        st7735s_fill_rect(20, 154, 200, 2, ONYX_COL_LINE);
        for (int i = -50; i <= 50; i += 10) {
            uint16_t tx = (uint16_t)(20 + (i + 50) * 200 / 100);
            uint16_t th = (i == 0) ? 14 : ((i % 25 == 0) ? 8 : 4);
            st7735s_fill_rect(tx, (uint16_t)(154 - th / 2), 1, th, (i == 0) ? ONYX_COL_OK : ONYX_COL_DIM);
        }

        int16_t px = (int16_t)(20 + (c + 50) * 200 / 100);
        if (px < 20) px = 20;
        if (px > 220) px = 220;
        st7735s_fill_rect((uint16_t)(px - 1), 160, 3, 10, col);

        char cbuf[8];
        cbuf[0] = (c >= 0) ? '+' : '-';
        uint32_t ac = (c >= 0) ? (uint32_t)c : (uint32_t)(-c);
        fmt_u2(&cbuf[1], ac);
        cbuf[3] = ' '; cbuf[4] = 'c'; cbuf[5] = 't'; cbuf[6] = '\0';
        ui_draw_str(104, 136, cbuf, ONYX_COL_MUTE, ONYX_COL_BG);
        if (t->held) ui_draw_str(160, 136, "HOLD", ONYX_COL_SELECT, ONYX_COL_BG);
    } else {
        ui_draw_str(76, 92, "PLUCK A STRING", ONYX_COL_MUTE, ONYX_COL_BG);
    }

    /* IMP-04: REF button was selectable (tuner_cursor 0) but never drawn; MUTE was always
     * highlighted. Two buttons, highlight follows the cursor. */
    int ref_sel = (ui->tuner_cursor == 0);
    fill_rounded_box(12, 196, 100, 32, ONYX_COL_PANEL);
    draw_rounded_rect(12, 196, 100, 32, ref_sel ? ONYX_COL_SELECT : ONYX_COL_LINE);
    char rbuf[12] = "REF 440HZ";
    fmt_u3(&rbuf[4], t->ref_a4_hz); rbuf[7] = 'H'; rbuf[8] = 'Z'; rbuf[9] = '\0';
    ui_draw_str(35, 209, rbuf, ref_sel ? ONYX_COL_SELECT : ONYX_COL_TEXT, ONYX_COL_PANEL);

    fill_rounded_box(128, 196, 100, 32, t->mute_output ? ONYX_COL_BAD : ONYX_COL_PANEL);
    draw_rounded_rect(128, 196, 100, 32, !ref_sel ? ONYX_COL_SELECT : ONYX_COL_LINE);
    draw_speaker(138, 207, ONYX_COL_TEXT);
    ui_draw_str(158, 209, t->mute_output ? "MUTED" : "LIVE", ONYX_COL_TEXT, t->mute_output ? ONYX_COL_BAD : ONYX_COL_PANEL);
}

/* Render Groove Station
 *
 * The "big transport" layout of the redesign (GROOVE_SPEC.md, option 3):
 *
 *   1  the header (y 0-29)          the existing one
 *   2  the style card   (8, 34)-(231, 63)
 *   3  the tempo digits 5x, centred on x 112, top y 80
 *   4  the BPM label   1x, top y 108
 *   5  the tempo chevrons (14, 95) and (223, 95)
 *   6  the step strip  16 blocks, y 128-136
 *   7  the play/stop button (8, 150)-(116, 200)
 *   8  the drum volume box (124, 150)-(231, 200)
 *   9  the key hints   y 215-227
 *
 * The reference render the design kit shipped is groove_px_3_transport.png
 * (and _stopped.png); the screen is compared with it pixel by pixel below the
 * header, because the header is the device's own.
 */
static void render_drums(const pa_ui_ctx_t *ui) {
    render_sub_title_bar("GROOVE STATION", ONYX_COL_TEXT, 0, ui->drum_playing, 0, ui);

    /* ---- 2. the style card ------------------------------------------------ */
    ui_onyx_cut_box(8, 34, 231, 63, 2, ONYX_COL_PANEL, ONYX_COL_LINE);
    const char *sname = pa_drums_get_style_name(ui->drum_pattern % PA_DRUM_STYLE_COUNT);
    ui_onyx_draw_str(120, 40, sname, 1, ONYX_COL_TEXT, UI_ALIGN_CENTER);
    char idx_buf[16];
    idx_buf[0] = 'S'; idx_buf[1] = 'T'; idx_buf[2] = 'Y'; idx_buf[3] = 'L';
    idx_buf[4] = 'E'; idx_buf[5] = ' ';
    fmt_u2(&idx_buf[6], (ui->drum_pattern % PA_DRUM_STYLE_COUNT) + 1u);
    idx_buf[8] = '/';
    idx_buf[9] = '4'; idx_buf[10] = '0';
    idx_buf[11] = '\0';
    ui_onyx_draw_str(120, 51, idx_buf, 1, ONYX_COL_MUTE, UI_ALIGN_CENTER);
    ui_onyx_draw_sprite(16, 46, UI_SPRITE_CHEV_L, 1, ONYX_COL_MUTE);
    ui_onyx_draw_sprite(221, 46, UI_SPRITE_CHEV_R, 1, ONYX_COL_MUTE);

    /* ---- 3 + 4. the tempo -------------------------------------------------- */
    char bstr[6];
    fmt_plain(bstr, ui->drum_bpm);
    uint16_t bwid = ui_onyx_text_w(bstr, 5);
    ui_onyx_draw_str(112, 80, bstr, 5, ONYX_COL_SELECT, UI_ALIGN_CENTER);
    ui_onyx_draw_str((uint16_t)(112 + bwid / 2u + 6u), 108, "BPM", 1, ONYX_COL_MUTE, UI_ALIGN_LEFT);

    /* ---- 5. the tempo chevrons --------------------------------------------- */
    ui_onyx_draw_sprite(14, 95, UI_SPRITE_CHEV_L, 1, ONYX_COL_DIM);
    ui_onyx_draw_sprite(223, 95, UI_SPRITE_CHEV_R, 1, ONYX_COL_DIM);

    /* ---- 6. the step strip ------------------------------------------------
     * The current step comes from the drum engine's own clock (ui->drum_step
     * is copied from drums->current_step), so the strip can never drift from
     * the audio.
     *
     * FW-FIX "the bar goes away after the 4th": the engine counts steps across
     * the WHOLE pattern, and the reference rhythms are up to 8 bars long, so
     * current_step runs to 383. The strip is one 4/4 bar (16 blocks,
     * GROOVE_SPEC.md section 6), so the playhead has to be taken relative to
     * the bar. Without that it lit up for the first bar and then vanished for
     * the remaining bars, until the loop came round again. */
    int step = ui->drum_playing ? (int)pa_drums_bar_block(ui->drum_step) : -1;
    for (int i = 0; i < 16; i++) {
        int x = 12 + i * 13 + (i / 4) * 4;
        int down = (i % 4 == 0);
        uint16_t c;
        if (i == step) {
            c = ONYX_COL_SELECT;
        } else if (step >= 0 && (i / 4) == (step / 4) && i < step) {
            c = ONYX_COL_TEXT;         /* the beat fills in as it plays */
        } else if (down) {
            c = ONYX_COL_DIM;
        } else {
            c = ONYX_COL_LINE;
        }
        st7735s_fill_rect((uint16_t)x, (uint16_t)(down ? 128 : 130), 10,
                          (uint16_t)(down ? 9 : 7), c);
    }

    /* ---- 7. the play / stop button ----------------------------------------- */
    uint16_t pcol = ui->drum_playing ? ONYX_COL_OK : ONYX_COL_TEXT;
    ui_onyx_cut_box(8, 150, 116, 200, 2, ONYX_COL_PANEL, pcol);
    if (ui->drum_playing) {
        st7735s_fill_rect(18, 168, 12, 12, pcol);      /* the stop square */
    } else {
        ui_onyx_draw_sprite(18, 167, UI_SPRITE_PLAY, 2, pcol);
    }
    ui_onyx_draw_str(38, 162, ui->drum_playing ? "STOP" : "PLAY", 2, pcol, UI_ALIGN_LEFT);
    ui_onyx_draw_str(38, 182, "PRESS PARA", 1, ONYX_COL_MUTE, UI_ALIGN_LEFT);

    /* ---- 8. the drum volume ------------------------------------------------ */
    uint8_t dvol = ui->drum_volume;
    ui_onyx_cut_box(124, 150, 231, 200, 2, ONYX_COL_PANEL, ONYX_COL_LINE);
    ui_onyx_draw_str(132, 160, "DRUM VOL", 1, ONYX_COL_MUTE, UI_ALIGN_LEFT);
    char dvol_str[6];
    uint8_t dlen = fmt_plain(dvol_str, dvol);
    dvol_str[dlen] = '%';
    dvol_str[dlen + 1] = '\0';
    ui_onyx_draw_str(224, 160, dvol_str, 1, ONYX_COL_TEXT, UI_ALIGN_RIGHT);
    ui_onyx_segments(132, 172, 10, 7, 6, 2, (uint8_t)((dvol + 5u) / 10u),
                     ONYX_COL_TEXT, ONYX_COL_LINE);
    ui_onyx_draw_str(178, 184, "VOL-/+", 1, ONYX_COL_DIM, UI_ALIGN_CENTER);

    /* ---- 9. the key hints -------------------------------------------------- */
    const char *k1 = "< >";
    const char *a1 = "STYLE";
    const char *k2 = "TURN";
    const char *a2 = "BPM";
    uint16_t w1 = (uint16_t)(ui_onyx_text_w(k1, 1) + 10u + ui_onyx_text_w(a1, 1));
    uint16_t w2 = (uint16_t)(ui_onyx_text_w(k2, 1) + 10u + ui_onyx_text_w(a2, 1));
    uint16_t hx = (uint16_t)(120u - (w1 + 20u + w2) / 2u);
    uint16_t kwid = ui_onyx_text_w(k1, 1);
    ui_onyx_cut_box(hx, 215, (uint16_t)(hx + kwid + 5u), 227, 1, ONYX_COL_PANEL, ONYX_COL_DIM);
    ui_onyx_draw_str((uint16_t)(hx + 3u), 218, k1, 1, ONYX_COL_TEXT, UI_ALIGN_LEFT);
    ui_onyx_draw_str((uint16_t)(hx + kwid + 10u), 218, a1, 1, ONYX_COL_MUTE, UI_ALIGN_LEFT);
    hx = (uint16_t)(hx + w1 + 20u);
    kwid = ui_onyx_text_w(k2, 1);
    ui_onyx_cut_box(hx, 215, (uint16_t)(hx + kwid + 5u), 227, 1, ONYX_COL_PANEL, ONYX_COL_DIM);
    ui_onyx_draw_str((uint16_t)(hx + 3u), 218, k2, 1, ONYX_COL_TEXT, UI_ALIGN_LEFT);
    ui_onyx_draw_str((uint16_t)(hx + kwid + 10u), 218, a2, 1, ONYX_COL_MUTE, UI_ALIGN_LEFT);
}

/* Render Save Preset Dialog */
static void render_save_dialog(const pa_ui_ctx_t *ui) {
    fill_rounded_box(16, 40, 208, 174, ONYX_COL_BG);
    draw_rounded_rect(16, 40, 208, 174, ONYX_COL_TEXT);

    ui_draw_str(76, 56, "SAVE PRESET", ONYX_COL_TEXT, ONYX_COL_BG);

    ui_draw_str(50, 94, "<", ONYX_COL_MUTE, ONYX_COL_BG);
    ui_draw_str(180, 94, ">", ONYX_COL_MUTE, ONYX_COL_BG);

    char pstr[4];
    fmt_preset(pstr, ui->save_target);
    ui_draw_str_scaled(96, 86, pstr, ONYX_COL_SELECT, ONYX_COL_BG, 3);

    const char *note = (ui->save_target == ui->preset_index) ? "OVERWRITE CURRENT" : "COPY TO NEW SLOT";
    uint16_t nlen = 0;
    while (note[nlen]) nlen++;
    ui_draw_str(120 - (nlen * 6) / 2, 126, note, ONYX_COL_MUTE, ONYX_COL_BG);

    /* CANCEL Button (Left-aligned box w=80, centered text) */
    fill_rounded_box(32, 148, 80, 26, ONYX_COL_PANEL);
    draw_rounded_rect(32, 148, 80, 26, ONYX_COL_DIM);
    ui_draw_str(45, 157, "VOL- BACK", ONYX_COL_MUTE, ONYX_COL_PANEL);

    /* SAVE Button (Right-aligned box w=80, centered text) */
    fill_rounded_box(128, 148, 80, 26, ONYX_COL_TEXT);
    draw_rounded_rect(128, 148, 80, 26, ONYX_COL_SELECT);
    ui_draw_str(138, 157, "CLICK SAVE", ONYX_COL_BG, ONYX_COL_TEXT);

    ui_draw_str(51, 184, "TURN / < >: SELECT SLOT", ONYX_COL_MUTE, ONYX_COL_BG);
    ui_draw_str(48, 196, "CLICK: SAVE   VOL-: BACK", ONYX_COL_MUTE, ONYX_COL_BG);
}

/* Render Saved Confirmation (IMP-04 redesign)
 * - full-screen card (no half-hidden grid behind it), solid accent header band
 * - tells the user WHERE it was saved (P12) and WHAT (preset name)
 * - checkmark drawn as two 3 px thick rect runs (no 1 px stair-stepping) */
static void draw_check(uint16_t cx, uint16_t cy, uint16_t col) {
    for (int k = 0; k <= 7; k++)  st7735s_fill_rect((uint16_t)(cx - 12 + k), (uint16_t)(cy + k - 2), 3, 4, col);
    for (int k = 0; k <= 14; k++) st7735s_fill_rect((uint16_t)(cx - 5 + k), (uint16_t)(cy + 5 - k), 3, 4, col);
}
static void render_saved_toast(const pa_ui_ctx_t *ui) {
    const int ok = !ui->save_failed;
    const uint16_t acc = ok ? ONYX_COL_OK : ONYX_COL_BAD;
    st7735s_fill_rect(0, 0, DISP_W, DISP_H, ONYX_COL_BG);
    fill_rounded_box(20, 40, 200, 160, ONYX_COL_PANEL);
    fill_rounded_box(20, 40, 200, 34, acc);
    if (ui->toast_kind == 3u) ui_draw_str_scaled((uint16_t)(ok ? 66 : 42), 50, ok ? "FORGOTTEN" : "NOT FORGOTTEN", ONYX_COL_BG, acc, 2);
    else if (ui->toast_kind == 2u) ui_draw_str_scaled((uint16_t)(ok ? 84 : 66), 50, ok ? "CLEAN" : "NOT CLEAN", ONYX_COL_BG, acc, 2);
    else if (ui->toast_kind == 1u) ui_draw_str_scaled((uint16_t)(ok ? 84 : 72), 50, ok ? "RESET" : "NOT RESET", ONYX_COL_BG, acc, 2);
    else                      ui_draw_str_scaled((uint16_t)(ok ? 84 : 66), 50, ok ? "SAVED" : "NOT SAVED", ONYX_COL_BG, acc, 2);

    fill_circle(120, 108, 20, ONYX_COL_PANEL_HI);
    if (ok) {
        draw_check(120, 108, acc);
    } else {
        st7735s_fill_rect(118, 96, 5, 15, acc);
        st7735s_fill_rect(118, 115, 5, 5, acc);
    }

    if (ok && ui->toast_kind == 3u) {
        ui_draw_str(60, 144, "BLUETOOTH PAIRING", ONYX_COL_SELECT, ONYX_COL_PANEL);
        ui_draw_str(60, 160, "REMOVE THE DEVICE", ONYX_COL_TEXT, ONYX_COL_PANEL);
        ui_draw_str(60, 176, "ON THE PC / PHONE", ONYX_COL_MUTE, ONYX_COL_PANEL);
    } else if (ok && ui->toast_kind == 2u) {
        ui_draw_str(72, 144, "USER PRESETS", ONYX_COL_SELECT, ONYX_COL_PANEL);
        ui_draw_str(72, 160, "P41-P80 ERASED", ONYX_COL_TEXT, ONYX_COL_PANEL);
        ui_draw_str(66, 176, "P01-P40 UNTOUCHED", ONYX_COL_MUTE, ONYX_COL_PANEL);
    } else if (ok && ui->toast_kind == 1u) {
        ui_draw_str(78, 144, "STOCK PRESETS", ONYX_COL_SELECT, ONYX_COL_PANEL);
        ui_draw_str(72, 160, "P01-P40 RESTORED", ONYX_COL_TEXT, ONYX_COL_PANEL);
        ui_draw_str(66, 176, "P41-P80 UNTOUCHED", ONYX_COL_MUTE, ONYX_COL_PANEL);
    } else if (ok) {
        char pstr[4];
        fmt_preset(pstr, ui->saved_index);
        ui_draw_str_scaled(102, 140, pstr, ONYX_COL_SELECT, ONYX_COL_PANEL, 2);
        uint16_t nlen = 0;
        while (ui->preset_name[nlen] && nlen < 23u) nlen++;
        char nm[24];
        for (uint16_t k = 0; k < nlen; k++) nm[k] = ui->preset_name[k];
        nm[nlen] = '\0';
        ui_draw_str((uint16_t)(120 - (nlen * 6) / 2), 170, nm, ONYX_COL_MUTE, ONYX_COL_PANEL);
    } else {
        ui_draw_str(66, 150, "FLASH WRITE FAILED", ONYX_COL_TEXT, ONYX_COL_PANEL);
        ui_draw_str(66, 166, "CHECK BATTERY LEVEL", ONYX_COL_MUTE, ONYX_COL_PANEL);
    }
}

/* Render Settings Menu Screen (9 rows from tokens.py) */
static void render_settings_menu(const pa_ui_ctx_t *ui) {
    render_sub_title_bar("SETTINGS", ONYX_COL_TEXT, 0, 1, 0, ui);

    #define SETTINGS_ROW_COUNT 11
    static const char * const menu_labels[SETTINGS_ROW_COUNT] = {
        "Input / Output", "Bluetooth", "Display", "Play Mode",
        "Fizz Filter", "USB Audio", "Performance", "System",
        "Output Limit", "USB Loopback", "USB Mode"
    };

    char cpu_str[8];
    fmt_u2(cpu_str, (ui->cpu_total_permille + 5u) / 10u);
    cpu_str[2] = '%'; cpu_str[3] = '\0';

    uint8_t top = ui->settings_scroll_top;
    if (top > (SETTINGS_ROW_COUNT - 7)) top = SETTINGS_ROW_COUNT - 7;

    for (int i = 0; i < 7; i++) {
        uint8_t idx = top + i;
        if (idx >= SETTINGS_ROW_COUNT) break;

        uint16_t y = (uint16_t)(34 + i * 28);
        int sel = (ui->settings_cursor == idx);

        if (sel) {
            fill_rounded_box(4, y + 1, 230, 26, ONYX_COL_PANEL);
            st7735s_fill_rect(4, y + 5, 3, 18, ONYX_COL_SELECT);
        }

        ui_draw_str(16, y + 9, menu_labels[idx], sel ? ONYX_COL_SELECT : ONYX_COL_TEXT,
                    sel ? ONYX_COL_PANEL : ONYX_COL_BG);

        const char *val = ">";
        if (idx == 0) val = ">";
        /* Bluetooth: OFF / BLE / BLE+A2DP. OFF stops the radio's advertising (and drops a link); BLE is the BLE-MIDI radio the unit starts at power-on. A2DP (audio) is not implemented in this
         * firmware: BLE+A2DP selects the same radio as BLE. The icon shows the radio's real state, not this setting. */
        else if (idx == 1) { static const char * const k_bt[3] = { "OFF", "BLE", "BLE+A2DP" };
                             val = k_bt[ui->bt_mode > 2u ? 1u : ui->bt_mode]; }
        else if (idx == 2) val = ">";
        else if (idx == 3) val = (ui->play_mode == 1) ? "STOMP" : "PRESET";
        else if (idx == 4) { static const char * const k_fz[4] = { "OFF", "LOW", "MED", "HIGH" };
                             val = k_fz[ui->fizz_filter_enabled & 3u]; }
        else if (idx == 5) val = "44.1k";
        else if (idx == 6) val = cpu_str;
        else if (idx == 7) val = ">";
        else if (idx == 8) { static const char * const k_lm[3] = { "BRICK", "SOFT", "OFF" };
                             val = k_lm[ui->output_limit_mode % 3u]; }
        else if (idx == 9) val = ui->usb_loopback ? "ON" : "OFF";
        /* USB Mode (what an audio interface calls direct monitoring): AMP = the unit's own sound, INTERFACE = only the PC's audio is played and the PC records the dry guitar, IF+MONITOR = the same
         * with the dry guitar also heard. Click cycles. */
        else if (idx == 10) { static const char * const k_um[3] = { "AMP", "INTERFACE", "IF+MONITOR" };
                              val = k_um[ui->usb_mode % 3u]; }

        uint16_t vlen = 0;
        while (val[vlen]) vlen++;
        uint16_t vx = (uint16_t)(224 - vlen * 6);
        ui_draw_str(vx, y + 9, val, sel ? ONYX_COL_SELECT : ONYX_COL_MUTE,
                    sel ? ONYX_COL_PANEL : ONYX_COL_BG);
    }

    /* Scrollbar on right edge */
    st7735s_fill_rect(236, 36, 2, 196, ONYX_COL_LINE);
    uint16_t thumb_h = (uint16_t)((7 * 196) / SETTINGS_ROW_COUNT);
    uint16_t thumb_y = (uint16_t)(36 + (top * (196 - thumb_h)) / (SETTINGS_ROW_COUNT - 7));
    st7735s_fill_rect(235, thumb_y, 4, thumb_h, ONYX_COL_DIM);
}


/* Render Input / Output Settings Screen. Eight rows, 32 px apart, six of them on screen (the list scrolls with the cursor):
 *   0 INPUT GAIN  1 NOISE GATE  2 MASTER  3 STEREO WIDTH  4 BALANCE  (sliders)   5 ROUTING (click cycles)   6 OUTPUT   7 CAB ON LINE OUT */
#define IO_ROWS 8
#define IO_VISIBLE 6
static void io_slider(uint16_t y, int sel, const pa_ui_ctx_t *ui, const char *label, const char *val, uint16_t bw) {
    if (sel) {
        fill_rounded_box(4, y, 232, 30, ONYX_COL_PANEL);
        st7735s_fill_rect(4, y + 4, 3, 22, ui->sub_edit_mode ? ONYX_COL_OK : ONYX_COL_SELECT);
        if (ui->sub_edit_mode) draw_rounded_rect(4, y, 232, 30, ONYX_COL_OK);   /* IMP-04: edit state */
    }
    const uint16_t bg = sel ? ONYX_COL_PANEL : ONYX_COL_BG;
    ui_draw_str(14, y + 4, label, sel ? ONYX_COL_SELECT : ONYX_COL_MUTE, bg);
    ui_draw_str((uint16_t)(220 - 6 * (int)strlen(val)), y + 4, val, ONYX_COL_TEXT, bg);   /* right-aligned */
    st7735s_fill_rect(14, y + 18, 212, 4, ONYX_COL_LINE);
    if (bw > 212) bw = 212;
    st7735s_fill_rect(14, y + 18, bw, 4, sel ? ONYX_COL_SELECT : ONYX_COL_TEXT);
    fill_circle(14 + bw, y + 20, 4, sel ? ONYX_COL_SELECT : ONYX_COL_TEXT);
}

/* an option row: a label on the left, a boxed value on the right */
static void io_option(uint16_t y, int sel, const char *label, const char *val) {
    if (sel) {
        fill_rounded_box(4, y, 232, 28, ONYX_COL_PANEL);
        st7735s_fill_rect(4, y + 4, 3, 20, ONYX_COL_SELECT);
    }
    const uint16_t bg = sel ? ONYX_COL_PANEL : ONYX_COL_BG;
    ui_draw_str(14, y + 10, label, sel ? ONYX_COL_SELECT : ONYX_COL_MUTE, bg);
    fill_rounded_box(114, y + 4, 114, 20, ONYX_COL_PANEL);
    draw_rounded_rect(114, y + 4, 114, 20, sel ? ONYX_COL_SELECT : ONYX_COL_LINE);
    ui_draw_str((uint16_t)(171 - 3 * (int)strlen(val)), y + 10, val, ONYX_COL_TEXT, ONYX_COL_PANEL);
}

static void render_input_output(const pa_ui_ctx_t *ui) {
    render_sub_title_bar("INPUT / OUTPUT", ONYX_COL_TEXT, 0, 1, 0, ui);
    const int cur = (int)ui->sub_cursor;
    const int top = (cur >= IO_VISIBLE) ? cur - (IO_VISIBLE - 1) : 0;
    for (int row = top; row < top + IO_VISIBLE && row < IO_ROWS; row++) {
        const uint16_t y = (uint16_t)(34 + (row - top) * 32);
        const int sel = (cur == row);
        char v[12];
        switch (row) {
        case 0: {
            v[0] = (ui->input_trim_db >= 0) ? '+' : '-';
            uint32_t atr = (ui->input_trim_db >= 0) ? (uint32_t)ui->input_trim_db : (uint32_t)(-ui->input_trim_db);
            fmt_u2(&v[1], atr);
            v[3] = ' '; v[4] = 'd'; v[5] = 'B'; v[6] = '\0';
            io_slider(y, sel, ui, "INPUT GAIN", v, (uint16_t)(((int32_t)(ui->input_trim_db + 12) * 212) / 24));
            break; }
        case 1: {
            v[0] = '-';
            uint32_t agt = (ui->gate_thresh_db < 0) ? (uint32_t)(-ui->gate_thresh_db) : (uint32_t)ui->gate_thresh_db;
            fmt_u2(&v[1], agt);
            v[3] = ' '; v[4] = 'd'; v[5] = 'B'; v[6] = '\0';
            if (ui->gate_thresh_db <= -80) { v[0] = 'O'; v[1] = 'F'; v[2] = 'F'; v[3] = '\0'; }
            io_slider(y, sel, ui, "NOISE GATE", v, (uint16_t)(((int32_t)(ui->gate_thresh_db + 80) * 212) / 50));
            break; }
        case 2:
            /* The front-panel MASTER pot, shown here as well: one number, the master (the output level and the master volume used to be two
             * controls that could disagree). */
            fmt_u3(v, ui->master_volume);
            v[3] = '%'; v[4] = '\0';
            io_slider(y, sel, ui, "MASTER", v, (uint16_t)((ui->master_volume * 212u) / 100u));
            break;
        case 3: {
            /* STEREO WIDTH: 0 = MONO (the old sound, no extra work), 100 = the two channels fully decorrelated (for headphones). */
            const uint8_t w = ui->stereo_width, d2 = (uint8_t)(w / 100u), d1 = (uint8_t)((w / 10u) % 10u), d0 = (uint8_t)(w % 10u);
            int k = 0;
            if (w == 0) { v[0] = 'M'; v[1] = 'O'; v[2] = 'N'; v[3] = 'O'; v[4] = '\0'; }
            else {
                if (d2) v[k++] = (char)('0' + d2);
                if (d2 || d1) v[k++] = (char)('0' + d1);
                v[k++] = (char)('0' + d0); v[k++] = '%'; v[k] = '\0';
            }
            io_slider(y, sel, ui, "STEREO WIDTH", v, (uint16_t)((w * 212u) / 100u));
            break; }
        case 4: {
            /* BALANCE: C in the middle, L50 .. R50 */
            const int b = ui->out_balance, ab = b < 0 ? -b : b;
            if (b == 0) { v[0] = 'C'; v[1] = '\0'; }
            else { v[0] = (b < 0) ? 'L' : 'R'; fmt_u2(&v[1], (uint32_t)ab); v[3] = '\0'; }
            io_slider(y, sel, ui, "BALANCE", v, (uint16_t)(((b + 50) * 212) / 100));
            break; }
        case 5: {
            static const char *const k_route[5] = { "STEREO", "MONO", "LEFT", "RIGHT", "SWAP" };
            io_option(y, sel, "ROUTING", k_route[ui->out_route > 4u ? 0u : ui->out_route]);
            break; }
        case 6:
            io_option(y, sel, "OUTPUT", ui->io_output_type ? "LINE" : "PHONES");
            break;
        default:
            io_option(y, sel, "CAB ON LINE", ui->io_cab_on_line ? "ON" : "OFF");
            break;
        }
    }
}

/* Render Display Settings Screen */
static void render_display_settings(const pa_ui_ctx_t *ui) {
    render_sub_title_bar("DISPLAY", ONYX_COL_TEXT, 0, 1, 0, ui);

    /* Row 0: Brightness slider */
    int sel0 = (ui->sub_cursor == 0);
    if (sel0) {
        fill_rounded_box(4, 34, 232, 38, ONYX_COL_PANEL);
        st7735s_fill_rect(4, 40, 3, 26, ui->edit_mode ? ONYX_COL_OK : ONYX_COL_SELECT);
        if (ui->edit_mode) {
            draw_rounded_rect(4, 34, 232, 38, ONYX_COL_OK);
        }
    }
    ui_draw_str(14, 40, "BRIGHTNESS", sel0 ? (ui->edit_mode ? ONYX_COL_OK : ONYX_COL_SELECT) : ONYX_COL_MUTE, sel0 ? ONYX_COL_PANEL : ONYX_COL_BG);
    char br_str[8];
    fmt_u3(br_str, ui->display_brightness);
    br_str[3] = '%'; br_str[4] = '\0';
    ui_draw_str(190, 40, br_str, ONYX_COL_TEXT, sel0 ? ONYX_COL_PANEL : ONYX_COL_BG);
    st7735s_fill_rect(14, 56, 212, 4, ONYX_COL_LINE);
    uint16_t bw = (uint16_t)((ui->display_brightness * 212u) / 100u);
    if (bw > 212) bw = 212;
    st7735s_fill_rect(14, 56, bw, 4, sel0 ? (ui->edit_mode ? ONYX_COL_OK : ONYX_COL_SELECT) : ONYX_COL_TEXT);
    fill_circle(14 + bw, 58, 4, sel0 ? (ui->edit_mode ? ONYX_COL_OK : ONYX_COL_SELECT) : ONYX_COL_TEXT);

    /* Row 1: Auto Dim */
    int sel1 = (ui->sub_cursor == 1);
    if (sel1) {
        fill_rounded_box(4, 76, 232, 32, ONYX_COL_PANEL);
        st7735s_fill_rect(4, 80, 3, 24, ONYX_COL_SELECT);
    }
    ui_draw_str(14, 86, "AUTO DIM", sel1 ? ONYX_COL_SELECT : ONYX_COL_MUTE, sel1 ? ONYX_COL_PANEL : ONYX_COL_BG);
    const char *dim_txt = (ui->display_auto_dim == 0) ? "OFF" : ((ui->display_auto_dim == 1) ? "30 s" : ((ui->display_auto_dim == 2) ? "1 m" : "5 m"));
    ui_draw_str(198, 86, dim_txt, ONYX_COL_TEXT, sel1 ? ONYX_COL_PANEL : ONYX_COL_BG);

    /* Row 2: Home View (GRID / LIST) */
    int sel2 = (ui->sub_cursor == 2);
    if (sel2) {
        fill_rounded_box(4, 112, 232, 32, ONYX_COL_PANEL);
        st7735s_fill_rect(4, 116, 3, 24, ONYX_COL_SELECT);
    }
    ui_draw_str(14, 122, "HOME VIEW", sel2 ? ONYX_COL_SELECT : ONYX_COL_MUTE, sel2 ? ONYX_COL_PANEL : ONYX_COL_BG);
    fill_rounded_box(128, 118, 100, 20, ONYX_COL_PANEL);
    draw_rounded_rect(128, 118, 100, 20, sel2 ? ONYX_COL_SELECT : ONYX_COL_LINE);
    if (ui->home_view_mode == 0) {
        fill_rounded_box(130, 120, 48, 16, ONYX_COL_TEXT);
        ui_draw_str(138, 124, "GRID", ONYX_COL_BG, ONYX_COL_TEXT);
        ui_draw_str(188, 124, "LIST", ONYX_COL_MUTE, ONYX_COL_PANEL);
    } else {
        fill_rounded_box(178, 120, 48, 16, ONYX_COL_TEXT);
        ui_draw_str(138, 124, "GRID", ONYX_COL_MUTE, ONYX_COL_PANEL);
        ui_draw_str(188, 124, "LIST", ONYX_COL_BG, ONYX_COL_TEXT);
    }

    /* Row 3: Battery % */
    int sel3 = (ui->sub_cursor == 3);
    if (sel3) {
        fill_rounded_box(4, 148, 232, 32, ONYX_COL_PANEL);
        st7735s_fill_rect(4, 152, 3, 24, ONYX_COL_SELECT);
    }
    ui_draw_str(14, 158, "BATTERY %", sel3 ? ONYX_COL_SELECT : ONYX_COL_MUTE, sel3 ? ONYX_COL_PANEL : ONYX_COL_BG);
    /* The pill is 38 x 21 (x 190..227, y 154..174, centre y 164); the knob is 11 px (radius 5), 5 px from the pill's edge on every side (it used to be 13 px at 222, touching the right edge). */
    if (ui->display_battery_pct) {
        fill_rounded_box(190, 154, 38, 21, ONYX_COL_TEXT);
        ui_draw_str(195, 160, "ON", ONYX_COL_BG, ONYX_COL_TEXT);
        fill_circle(217, 164, 5, ONYX_COL_BG);
    } else {
        fill_rounded_box(190, 154, 38, 21, ONYX_COL_PANEL);
        draw_rounded_rect(190, 154, 38, 21, ONYX_COL_DIM);
        fill_circle(200, 164, 5, ONYX_COL_DIM);
        ui_draw_str(208, 160, "OFF", ONYX_COL_MUTE, ONYX_COL_PANEL);
    }

    /* Row 4: Theme */
    int sel4 = (ui->sub_cursor == 4);
    if (sel4) {
        fill_rounded_box(4, 184, 232, 32, ONYX_COL_PANEL);
        st7735s_fill_rect(4, 188, 3, 24, ONYX_COL_SELECT);
    }
    ui_draw_str(14, 194, "THEME", sel4 ? ONYX_COL_SELECT : ONYX_COL_MUTE, sel4 ? ONYX_COL_PANEL : ONYX_COL_BG);
    ui_draw_str(ui->theme ? 172 : 196, 194, ui->theme ? "CONTRAST" : "ONYX", ONYX_COL_TEXT, sel4 ? ONYX_COL_PANEL : ONYX_COL_BG);
}

/* Render Performance Telemetry Screen */
#if !defined(PA_HOST_BUILD) && !defined(PA_HOST_SIM) && !defined(PA_EMU_DLL) && !defined(__wasm__)
extern volatile uint32_t g_pa_isr_peak_us;
static uint32_t perf_peak_permille(void) { return (g_pa_isr_peak_us * 1000u) / 1451u; }
static uint32_t perf_slot_us(unsigned slot) { return ((volatile uint32_t *)(uintptr_t)0x01C7C500u)[4u * slot + 3u]; }   /* last run, us * 24 (fx_graph.c) */
#define PA_PERF_MEASURED 1
#else
#define PA_PERF_MEASURED 0
#endif

static void render_performance(const pa_ui_ctx_t *ui, const pa_fx_chain_t *chain, const pa_cpu_meter_t *meter) {
    /* The per-block bars come from the real cycle meter when it has data, and
     * fall back to the previous static table before the first block completes. */
    render_sub_title_bar("PERFORMANCE", ONYX_COL_TEXT, 0, 1, 0, ui);

    uint32_t cpu = (ui->cpu_total_permille + 5u) / 10u;
    if (cpu > 99u) cpu = 99u;
    uint16_t ccol = (cpu < 70) ? ONYX_COL_OK : ((cpu < 85) ? ONYX_COL_WARN : ONYX_COL_BAD);

    ui_draw_str(14, 40, "CPU LOAD", ONYX_COL_MUTE, ONYX_COL_BG);
    char cstr[8];
    fmt_u2(cstr, cpu);
    cstr[2] = '%'; cstr[3] = '\0';
    ui_draw_str_scaled(14, 56, cstr, ccol, ONYX_COL_BG, 3);

#if PA_PERF_MEASURED
    uint32_t peak = (perf_peak_permille() + 5u) / 10u;
    (void)meter;
#else
    uint32_t peak = meter ? (cpu_meter_peak_permille(meter) + 5u) / 10u : cpu;
#endif
    if (peak > 99u) peak = 99u;
    char pstr[8];
    fmt_u2(pstr, peak);
    pstr[2] = '%'; pstr[3] = '\0';
    ui_draw_str(190, 56, "PEAK", ONYX_COL_MUTE, ONYX_COL_BG);
    ui_draw_str(190, 70, pstr, ONYX_COL_TEXT, ONYX_COL_BG);

    /* CPU bar */
    st7735s_fill_rect(14, 90, 212, 6, ONYX_COL_LINE);
    uint16_t cbw = (uint16_t)((cpu * 212u) / 100u);
    if (cbw > 212) cbw = 212;
    st7735s_fill_rect(14, 90, cbw, 6, ccol);

    /* Process time: dynamically computed from measured/estimated DSP cycle load
     * Block budget at 44.1 kHz for PA_BLOCK_N=64 is 64 / 44100 = 1.45 ms */
    ui_draw_str(14, 108, "PROCESS TIME", ONYX_COL_MUTE, ONYX_COL_BG);
    uint32_t proc_100th = (ui->cpu_total_permille * 145u) / 1000u;
    uint32_t ms_int     = proc_100th / 100u;
    uint32_t ms_frac    = proc_100th % 100u;
    char tbuf[24];
    tbuf[0] = (char)('0' + (ms_int % 10u));
    tbuf[1] = '.';
    tbuf[2] = (char)('0' + (ms_frac / 10u));
    tbuf[3] = (char)('0' + (ms_frac % 10u));
    tbuf[4] = ' '; tbuf[5] = '/'; tbuf[6] = ' ';
    tbuf[7] = '1'; tbuf[8] = '.'; tbuf[9] = '4'; tbuf[10] = '5';
    tbuf[11] = ' '; tbuf[12] = 'm'; tbuf[13] = 's'; tbuf[14] = '\0';
    ui_draw_str(136, 108, tbuf, ONYX_COL_TEXT, ONYX_COL_BG);
    st7735s_fill_rect(14, 122, 212, 6, ONYX_COL_LINE);
    uint16_t pbw = (uint16_t)((ui->cpu_total_permille * 212u) / 1000u);
    if (pbw > 212) pbw = 212;
    st7735s_fill_rect(14, 122, pbw, 6, ccol);

    /* SRAM */
    ui_draw_str(14, 140, "SRAM", ONYX_COL_MUTE, ONYX_COL_BG);
    uint32_t kb = ui->sram_used_bytes / 1024u;
    char sbuf[24];
    fmt_u3(sbuf, kb);
    sbuf[3] = ' '; sbuf[4] = '/'; sbuf[5] = ' ';
    sbuf[6] = '3'; sbuf[7] = '3'; sbuf[8] = '6';                 /* PA_UI_SRAM_TOTAL_KB: the effect pool's region, 336 KB (pa_plat.h PA_POOL_BUDGET_BYTES); it said 1024 KB, the chip has 512 KB */
    sbuf[9] = ' '; sbuf[10] = 'K'; sbuf[11] = 'B'; sbuf[12] = '\0';
    ui_draw_str(132, 140, sbuf, ONYX_COL_TEXT, ONYX_COL_BG);
    st7735s_fill_rect(14, 154, 212, 6, ONYX_COL_LINE);
    uint16_t sbw = (uint16_t)((kb * 212u) / PA_UI_SRAM_TOTAL_KB);
    if (sbw > 212) sbw = 212;
    st7735s_fill_rect(14, 154, sbw, 6, ONYX_COL_BLUETOOTH);

    /* CPU by block: stacked bar + legend */
    ui_draw_str(14, 174, "CPU BY BLOCK", ONYX_COL_MUTE, ONYX_COL_BG);
    st7735s_fill_rect(14, 186, 212, 8, ONYX_COL_LINE);

    /* Block proportions from the live cycle meter (per-stage EMA), normalised to
     * the total so the stacked bar always fills the same width. */
    uint16_t cur_x = 14;
    uint8_t  blk_pcts[8] = { 0, 0, 0, 0, 0, 0, 0, 0 };
    {
        static const uint8_t k_stage_of_slot[8] = {   /* by slot: DYN DRV AMP CAB EQ MOD DLY REV (the CAB shares the MISC bucket) */
            PA_ST_DYN, PA_ST_DRIVE, PA_ST_NAM, PA_ST_MISC, PA_ST_EQ, PA_ST_MOD, PA_ST_DELAY, PA_ST_REVERB
        };
        uint32_t c[8], total = 0;
        for (int b = 0; b < 8; b++) {
            const fx_node_t *nd = chain ? chain->slots[b] : 0;
            const int on = nd && (nd->active || nd->tail_draining);
#if PA_PERF_MEASURED
            c[b] = on ? perf_slot_us((unsigned)b) / 24u : 0u;                 /* what the slot really took in its last block */
#else
            c[b] = (on && meter) ? meter->ema[k_stage_of_slot[b]] : 0u;
#endif
            if (c[b] > 100000u) c[b] = 100000u;
            total += c[b];
        }
        if (total > 0u) {
            for (int b = 0; b < 8; b++) blk_pcts[b] = (uint8_t)((c[b] * 100u) / total);
        }
    }
    for (int b = 0; b < 8; b++) {
        uint16_t w = (uint16_t)((blk_pcts[b] * 212u) / 100u);
        if (w > 0) {
            st7735s_fill_rect(cur_x, 186, w, 8, get_slot_color(b));
            cur_x += w;
        }
    }

    /* 8-slot legend underneath */
    for (int b = 0; b < 8; b++) {
        uint16_t lx = (uint16_t)(14 + b * 26);
        const char *sh = get_slot_short(b);
        ui_draw_str(lx, 202, sh, blk_pcts[b] ? get_slot_color(b) : ONYX_COL_DIM, ONYX_COL_BG);
        if (blk_pcts[b]) {
            char p_str[4];
            fmt_u2(p_str, blk_pcts[b]);
            p_str[2] = '%'; p_str[3] = '\0';
            ui_draw_str(lx, 214, p_str, ONYX_COL_TEXT, ONYX_COL_BG);
        } else {
            ui_draw_str(lx, 214, "OFF", ONYX_COL_DIM, ONYX_COL_BG);
        }
    }
}

/* The update session's progress (src/board/ota_hw.c): two lines on a black screen. */
void pa_ui_ota_notice(const char *a, const char *b) {
    st7735s_fill_rect(0, 0, 240, 240, ONYX_COL_BG);
    uint16_t la = 0, lb = 0; while (a[la]) la++; while (b[lb]) lb++;
    ui_draw_str((uint16_t)(120 - la * 3), 100, a, ONYX_COL_TEXT, ONYX_COL_BG);
    ui_draw_str((uint16_t)(120 - lb * 3), 124, b, ONYX_COL_SELECT, ONYX_COL_BG);
}

/* Render Technical System Info Screen */
static void render_sys_info(const pa_ui_ctx_t *ui) {
    render_sub_title_bar("SYSTEM", ONYX_COL_TEXT, 0, 1, 0, ui);

    static const char * const info_keys[5] = { "MODEL", "FIRMWARE", "PRESETS", "BATTERY", "BUILD" };
    char batt[12];                                   /* IMP-12: was a hard-coded "82%" */
    fmt_u3(batt, ui->batt_pct); batt[3] = '%'; batt[4] = '\0';
    char fwv[6] = { (char)('0' + (PA_FW_VERSION / 100) % 10), '.', (char)('0' + (PA_FW_VERSION / 10) % 10), (char)('0' + PA_FW_VERSION % 10), 0, 0 };     /* PA_FW_VERSION 103 -> "1.03" */
    const char *info_vals[5] = { "POCKET AMP", fwv, "80", batt[0] == '0' ? &batt[batt[1] == '0' ? 2 : 1] : batt, PA_BUILD_CODE };

    for (int i = 0; i < 5; i++) {
        uint16_t y = (uint16_t)(36 + i * 20);
        ui_draw_str(14, y, info_keys[i], ONYX_COL_MUTE, ONYX_COL_BG);
        uint16_t vlen = 0;
        while (info_vals[i][vlen]) vlen++;
        ui_draw_str(224 - vlen * 6, y, info_vals[i], (i == 1 || i == 4) ? ONYX_COL_SELECT : ONYX_COL_TEXT, ONYX_COL_BG);
    }

    st7735s_fill_rect(12, 134, 216, 1, ONYX_COL_LINE);

    /* CLEAN USER PRESETS button */
    fill_rounded_box(12, 142, 216, 26, ONYX_COL_PANEL);
    draw_rounded_rect(12, 142, 216, 26, (ui->sys_cursor == 0) ? ONYX_COL_SELECT : ONYX_COL_DIM);
    ui_draw_str(66, 150, "CLEAN USER PRESETS", (ui->sys_cursor == 0) ? ONYX_COL_SELECT : ONYX_COL_TEXT, ONYX_COL_PANEL);

    /* FACTORY RESET button */
    fill_rounded_box(12, 172, 216, 26, ONYX_COL_BAD);
    draw_rounded_rect(12, 172, 216, 26, (ui->sys_cursor == 1) ? ONYX_COL_SELECT : ONYX_COL_BAD);
    ui_draw_str(76, 180, "FACTORY RESET", ONYX_COL_TEXT, ONYX_COL_BAD);

    /* FORGET BT PAIRING button: the paired central must pair again (pa_bond.h) */
    fill_rounded_box(12, 202, 216, 26, ONYX_COL_PANEL);
    draw_rounded_rect(12, 202, 216, 26, (ui->sys_cursor == 2) ? ONYX_COL_SELECT : ONYX_COL_DIM);
    ui_draw_str(69, 210, "FORGET BT PAIRING", (ui->sys_cursor == 2) ? ONYX_COL_SELECT : ONYX_COL_TEXT, ONYX_COL_PANEL);
}

/* STOMP assign menu: the eight effects, the one the key toggles now is marked, each row shows the keys that already toggle it */
static void render_stomp_assign(const pa_ui_ctx_t *ui) {
    static const char * const k_keys[4] = { "VOL-", "VOL+", "<", ">" };
    char title[16] = "ASSIGN ";
    { const char *k = k_keys[ui->stomp_assign_key & 3u]; int n = 7; while (*k) title[n++] = *k++; title[n] = '\0'; }
    render_sub_title_bar(title, ONYX_COL_TEXT, 0, 1, 0, ui);

    for (int r = 0; r < PA_SLOT_COUNT; r++) {
        const uint16_t y = (uint16_t)(34 + r * 22);
        const int sel = (ui->stomp_assign_cursor == r);
        fill_rounded_box(8, y, 224, 20, ONYX_COL_PANEL);
        draw_rounded_rect(8, y, 224, 20, sel ? ONYX_COL_SELECT : ONYX_COL_DIM);
        fill_circle(22, (uint16_t)(y + 10), 4, get_slot_color((uint8_t)r));
        ui_draw_str(34, (uint16_t)(y + 6), get_slot_label((uint8_t)r), sel ? ONYX_COL_SELECT : ONYX_COL_TEXT, ONYX_COL_PANEL);
        uint16_t x = 226;                                    /* the keys on this effect, right-aligned */
        for (int k = 3; k >= 0; k--) {
            if (ui->stomp_map[k] != r) continue;
            uint16_t len = 0;
            while (k_keys[k][len]) len++;
            x = (uint16_t)(x - len * 6 - 6);
            ui_draw_str(x, (uint16_t)(y + 6), k_keys[k], (k == (int)ui->stomp_assign_key) ? ONYX_COL_OK : ONYX_COL_MUTE, ONYX_COL_PANEL);
        }
    }
    ui_draw_str(14, 214, "PARA: ASSIGN    VOL-: BACK", ONYX_COL_MUTE, ONYX_COL_BG);
    ui_draw_str(14, 226, "SAVE THE PRESET TO KEEP IT", ONYX_COL_DIM, ONYX_COL_BG);
}

/* Render Factory Reset Confirmation Modal */
static void render_reset_confirm(const pa_ui_ctx_t *ui) {
    render_sys_info(ui);

    fill_rounded_box(16, 46, 208, 154, ONYX_COL_BG);
    draw_rounded_rect(16, 46, 208, 154, ONYX_COL_TEXT);

    ui_draw_str(116, 60, "!", ONYX_COL_BAD, ONYX_COL_BG);
    if (ui->confirm_kind == 2u) {
        ui_draw_str(70, 76, "FORGET PAIRING?", ONYX_COL_TEXT, ONYX_COL_BG);
        ui_draw_str(32, 94, "The paired device must pair", ONYX_COL_MUTE, ONYX_COL_BG);
        ui_draw_str(32, 108, "again. Remove it on the PC", ONYX_COL_OK, ONYX_COL_BG);
        ui_draw_str(32, 122, "or phone too.", ONYX_COL_OK, ONYX_COL_BG);
        ui_draw_str(32, 136, "CLICK FORGET TO CONFIRM", ONYX_COL_WARN, ONYX_COL_BG);
    } else if (ui->confirm_kind == 1u) {
        ui_draw_str(64, 76, "CLEAN USER PRESETS?", ONYX_COL_TEXT, ONYX_COL_BG);
        ui_draw_str(32, 94, "Erase presets P41-P80.", ONYX_COL_BAD, ONYX_COL_BG);
        ui_draw_str(32, 108, "Stock P01-P40 preserved.", ONYX_COL_OK, ONYX_COL_BG);
        ui_draw_str(32, 126, "CLICK ERASE TO CONFIRM", ONYX_COL_WARN, ONYX_COL_BG);
    } else {
        ui_draw_str(76, 76, "FACTORY RESET?", ONYX_COL_TEXT, ONYX_COL_BG);
        ui_draw_str(32, 94, "Restore stock presets P01-P40.", ONYX_COL_MUTE, ONYX_COL_BG);
        ui_draw_str(32, 108, "User slots P41-P80 preserved.", ONYX_COL_OK, ONYX_COL_BG);
        ui_draw_str(32, 126, "CLICK RESET TO CONFIRM", ONYX_COL_WARN, ONYX_COL_BG);
    }

    /* CANCEL Button (default safe choice) */
    fill_rounded_box(30, 154, 76, 26, ONYX_COL_PANEL);
    draw_rounded_rect(30, 154, 76, 26, (ui->reset_confirm_cursor == 0) ? ONYX_COL_SELECT : ONYX_COL_DIM);
    ui_draw_str(48, 162, "CANCEL", (ui->reset_confirm_cursor == 0) ? ONYX_COL_SELECT : ONYX_COL_TEXT, ONYX_COL_PANEL);

    /* RESET Button (danger) */
    fill_rounded_box(130, 154, 76, 26, ONYX_COL_BAD);
    draw_rounded_rect(130, 154, 76, 26, (ui->reset_confirm_cursor == 1) ? ONYX_COL_SELECT : ONYX_COL_BAD);
    ui_draw_str(ui->confirm_kind == 0u ? 153 : 150, 162, ui->confirm_kind == 2u ? "FORGET" : ui->confirm_kind == 1u ? "ERASE" : "RESET", ONYX_COL_TEXT, ONYX_COL_BAD);
}

/* The OVERLOAD popup: a momentary card over any screen (pa_audio_core.c decides when). */
static void render_overload_popup(void) {
    fill_rounded_box(18, 92, 204, 62, ONYX_COL_BG);
    draw_rounded_rect(18, 92, 204, 62, ONYX_COL_BAD);
    draw_rounded_rect(19, 93, 202, 60, ONYX_COL_BAD);
    ui_draw_str(36, 100, "! OVERLOAD", ONYX_COL_BAD, ONYX_COL_BG);
    ui_draw_str(36, 116, "TURN OFF AN EFFECT", ONYX_COL_TEXT, ONYX_COL_BG);
    ui_draw_str(36, 130, "OR USE A LIGHTER MODEL", ONYX_COL_MUTE, ONYX_COL_BG);
}

/* 1-Second Volume Popup Overlay outside Home screen */
static void render_volume_popup(const pa_ui_ctx_t *ui) {
    fill_rounded_box(40, 66, 160, 108, ONYX_COL_BG);
    draw_rounded_rect(40, 66, 160, 108, ONYX_COL_TEXT);
    draw_speaker(114, 82, ONYX_COL_TEXT);
    ui_draw_str(98, 102, "VOLUME", ONYX_COL_MUTE, ONYX_COL_BG);
    char vstr[4];
    fmt_u3(vstr, ui->preset_volume);
    ui_draw_str_scaled(102, 116, vstr, ONYX_COL_TEXT, ONYX_COL_BG, 2);
    st7735s_fill_rect(60, 148, 120, 5, ONYX_COL_LINE);
    uint16_t vw = (uint16_t)((ui->preset_volume * 120u) / 100u);
    if (vw > 120u) vw = 120u;
    if (vw > 0) st7735s_fill_rect(60, 148, vw, 5, ONYX_COL_SELECT);
}

/* Initialize UI Context */
void pa_ui_init(pa_ui_ctx_t *ui) {
    if (!ui) return;
    ui->view              = PA_UI_VIEW_MAIN_GRID;
    ui->prev_view         = PA_UI_VIEW_MAIN_GRID;
    ui->preset_index      = 7;    /* P08 default */
    ui->preset_volume     = 85;   /* Vol 85 */
    ui->master_volume     = 100;  /* the MASTER (front-panel knob / OUTPUT LEVEL): 100 % until a saved setting or the knob says otherwise */
    ui->bank_group        = 1;    /* Bank 02 (P05-P08) */
    ui->save_target       = 7;
    ui->save_btn          = 1;    /* 1 = SAVE selected by default */
    ui->saved_toast_timer_ms = 0;
    ui->cursor_slot_idx   = 2;    /* Focus on AMP by default */
    ui->home_view_mode    = 0;    /* 0 = GRID */
    ui->moving_slot       = PA_SLOT_AMP;
    ui->moving_orig_idx   = 2;
    ui->selected_slot     = PA_SLOT_AMP;
    ui->editor_cursor     = -1;   /* the model / type row */
    ui->selected_param    = 2;
    ui->edit_mode         = 0;
    ui->sub_cursor        = 0;
    ui->sub_edit_mode     = 0;
    ui->show_vol_popup    = 0;
    ui->vol_popup_timer_ms= 0;
    ui->in_peak_pct       = 0;
    ui->out_peak_pct      = 0;
    ui->settings_cursor   = 0;
    ui->settings_scroll_top = 0;
    ui->input_trim_db     = 0;    /* IMP-05: global trim now real -> 0 dB default (was +12 "calibrated") */
    ui->gate_thresh_db    = -80;  /* -80 dB = OFF; the preset's DYN block does the gating */
    ui->out_level_pct     = 100;  /* 100 % = 0 dB at the DAC (IMP-05: the setting is real now) */
    ui->io_output_type    = 0;    /* 0 = PHONES */
    ui->io_cab_on_line    = 1;    /* 1 = ON */
    ui->display_brightness= 80;   /* 80% */
    ui->display_auto_dim  = 0;    /* OFF: on the bench unit the 15 % soft-PWM dim looked black */
    ui->display_battery_pct = 1;  /* 1 = ON */
    ui->stomp_assign_key  = 0;
    ui->stomp_assign_cursor = 0;
    ui->fizz_filter_enabled = 1;  /* 1 = Fizz Filter ON */
    ui->ai_eq_enabled     = 1;
    ui->drum_volume       = 70;   /* 70% dedicated drum volume */
    ui->eq_bands_db[0]    = 2;
    ui->eq_bands_db[1]    = -1;
    ui->eq_bands_db[2]    = 0;
    ui->eq_bands_db[3]    = 3;
    ui->eq_bands_db[4]    = -2;
    ui->eq_low_cut        = 80;
    ui->eq_high_cut       = 12;
    ui->eq_enabled        = 1;
    ui->slot_bands_db[0]  = 2;
    ui->slot_bands_db[1]  = -1;
    ui->slot_bands_db[2]  = 0;
    ui->slot_bands_db[3]  = 3;
    ui->slot_bands_db[4]  = -2;
    ui->tuner_cursor      = 0;
    ui->sys_cursor        = 0;
    ui->confirm_kind      = 0;
    ui->reset_confirm_cursor = 0;
    ui->drum_step         = 0;
    ui->dirty             = 1;
    ui->bt_state          = 0;    /* the icon: the device sets it from the radio every tick */
    ui->bt_mode           = 1;    /* the setting: 1 = BLE (the radio starts at power-on) */
    ui->usb_connected     = 1;
    ui->play_mode         = 0;    /* 0 = PRESET mode, 1 = STOMP mode */
    ui->usb_sample_rate   = 1;    /* 1 = 44.1 kHz (fixed by USB descriptor) */
    ui->output_limit_mode = 0;    /* 0 = BRICKWALL */
    ui->usb_loopback      = 1;    /* the stock ships with the loopback ON */
    ui->battery_mv        = 0;    /* 0 until SARADC is sampled */
    ui->drum_bpm          = 120;
    ui->drum_playing      = 0;
    ui->drum_drums_on     = 1;    /* 1 = DRUMS ON */
    ui->drum_pattern      = 0;
    /* The SRAM figure is the POOL's, not a constant: the portable core's tick
     * publishes bsp->pool_used() before every render. The pool is 147,456 B, and this field is published from bsp->pool_used() on every render tick. */
    ui->sram_used_bytes   = 0;
    ui->theme             = 0;
    ui->dimmed            = 0;
    ui->last_input_ms     = 0;
    ui->settings_dirty    = 0;
    ui->toast_kind        = 0;
    ui->overload_ms       = 0;
    ui->batt_pct          = pa_battery_pct_from_mv(ui->battery_mv);
    ui->batt_warn         = 0;
    ui->batt_warn_shown   = 0;
    ui->stomp_map[0] = PA_SLOT_DRIVE; ui->stomp_map[1] = PA_SLOT_MOD;
    ui->stomp_map[2] = PA_SLOT_DELAY; ui->stomp_map[3] = PA_SLOT_REVERB;
    ui->preset_name[0] = '\0';
    ui->saved_index = 0;
    ui->cpu_total_permille= 520;
    ui->user_nam_loaded   = 0;
    ui->user_nam_name[0]  = '\0';

    for (int s = 0; s < PA_SLOT_COUNT; s++) {
        ui->slot_type[s] = 0;
        ui->slot_params[s][0] = (int32_t)(((int64_t)65 * PA_Q21_ONE) / 100);
        ui->slot_params[s][1] = (int32_t)(((int64_t)50 * PA_Q21_ONE) / 100);
        ui->slot_params[s][2] = (int32_t)(((int64_t)45 * PA_Q21_ONE) / 100);
        ui->slot_params[s][3] = (int32_t)(((int64_t)60 * PA_Q21_ONE) / 100);
        ui->slot_params[s][4] = (int32_t)(((int64_t)55 * PA_Q21_ONE) / 100);
        ui->slot_params[s][5] = (int32_t)(((int64_t)70 * PA_Q21_ONE) / 100);
    }
    pa_tuner_init(&ui->tuner);
}

/* ---- Preset / model glue (Blk-02, IMP-01, IMP-02, IMP-13) ------------------------------- */
void pa_ui_sync_from_preset(pa_ui_ctx_t *ui, const pa_preset_t *pr) {
    ui->preset_volume = pr->preset_vol;      /* the preset's own volume (VOL- / VOL+); the MASTER is the output level / the knob, not this */
    for (int s = 0; s < PA_SLOT_COUNT; s++) {
        ui->slot_type[s] = pr->slot_type[s];
        for (int k = 0; k < 6; k++) ui->slot_params[s][k] = pa_q16_to_q21(pr->params[s][k]);
    }
    ui->slot_type[PA_SLOT_AMP] = pr->amp_model_idx;
    ui->slot_type[PA_SLOT_CAB] = pr->cab_model_idx;
    for (int b = 0; b < 5; b++)                    /* EQ block: Q16 0..1 -> -12..+12 dB */
        ui->slot_bands_db[b] = (int8_t)((((int32_t)pr->params[PA_SLOT_EQ][b] * 24 + 32767) / 65535) - 12);
    for (int i = 0; i < 4; i++)
        ui->stomp_map[i] = (pr->stomp_map[i] < PA_SLOT_COUNT) ? pr->stomp_map[i] : (uint8_t)(PA_SLOT_DRIVE + i);
    memcpy(ui->preset_name, pr->name, sizeof(ui->preset_name));
    ui->preset_name[sizeof(ui->preset_name) - 1] = '\0';
}

void pa_ui_load_preset(pa_ui_ctx_t *ui, pa_fx_chain_t *chain, uint8_t idx) {
    pa_preset_t pr;
    ui->preset_index = idx;
    ui->bank_group   = idx / 4u;
    if (pa_preset_load(idx, &pr) == 0) {
        pa_preset_apply_smooth(&pr, chain);          /* Blk-02: fade out -> apply -> fade in */
        pa_ui_sync_from_preset(ui, &pr);
    }
    pa_ui_mark_settings_dirty(ui);                   /* IMP-10: remember last preset */
    ui->dirty = 1;
}

/* Model / type change from the block editor */
static void ui_select_model(pa_ui_ctx_t *ui, pa_fx_chain_t *chain, uint8_t slot, uint8_t type) {
    ui->slot_type[slot] = type;
    if (slot == PA_SLOT_AMP || slot == PA_SLOT_CAB) {
        /* Rewriting 3.8 KB of GRU weights / 1024 IR taps under a running ISR = click.
         * The change is handed to the AUDIO thread, which applies it at the next
         * block boundary once the output gain has reached zero. The UI thread
         * used to BLOCK here waiting for the mute - a visible 30 ms freeze on
         * every model change, and a deadlock on any build where the clock only
         * advances between ticks (host / WebAssembly). */
        pa_audio_core_request_model(ui, chain, slot, type);
    } else if (chain && chain->slots[slot] && NODE_SHOWN_ON(chain->slots[slot])) {
        /* a sounding effect changes type under the same fade-out / swap / fade-in as an amp or a cab (a live change clicked: MOD, DLY and EQ in the click probe) */
        pa_audio_core_request_model(ui, chain, slot, type);
    } else {
        pa_fx_chain_set_slot_type(chain, (pa_fx_slot_id_t)slot, type);   /* IMP-13: correct param id (a slot that is off: nothing to hear) */
    }
}

/* IMP-05: push Settings > Input / Output into the DSP */
void pa_ui_apply_io_settings(pa_ui_ctx_t *ui, pa_fx_chain_t *chain) {
    pa_fx_chain_set_global_input(chain, ui->input_trim_db, ui->gate_thresh_db);
    pa_fx_chain_set_output_routing(chain, ui->io_output_type, ui->io_cab_on_line);
    /* OUTPUT LEVEL -> DAC stage master (Q15, 0x8000 = unity). Audio-taper: -40 dB .. 0 dB.
     * The same taper is used by the front-panel MASTER pot (pa_audio_core_master_q15). */
    g_pa_audio.master_vol_q15 = pa_audio_core_master_q15(ui->out_level_pct);
    pa_fx_chain_set_fizz_filter(chain, ui->fizz_filter_enabled);
    /* The width is made by the stereo spread at the end of the chain (src/dsp/pa_stereo_spread.h), the same for every preset: two quadrature signals, so the two ears are
     * equally loud for any input. The stereo cabinet (a second, phase-smeared impulse response mixed with the first) is kept in the code but left at width 0: its two
     * outputs are strongly correlated, which made one ear louder (about 5 dB at width 60 by calculation), and it cost 109 us a block and the memory of a second IR. */
    pa_fx_chain_set_stereo_width(chain, 0);
    pa_audio_core_set_routing(ui->out_route, ui->out_balance);
}

/* Complete Front Panel Input Handler Matching NAVIGATION.md */
/* Adjust whatever is under the block editor's cursor by `delta` steps.
 * Returns 1 if something changed.
 *
 * The < and > buttons and the PARA encoder both come through here, so the
 * effect window behaves like the Settings screen: the buttons DIAL THE
 * SETTING IN, and the encoder is the way you move to the next setting.
 *
 *   editor_cursor == -2   the ON/OFF row: the PARA click toggles it, and
 *                         the buttons do nothing (a held button would
 *                         strobe the bypass)
 *   editor_cursor == -1   the model / type row
 *   editor_cursor >= 0    a parameter knob (or an EQ band) */
static int block_editor_adjust(pa_ui_ctx_t *ui, pa_fx_chain_t *chain,
                                uint8_t slot, int delta) {
    if (!ui || delta == 0) return 0;

    if (ui->editor_cursor == -1) {
        /* the model / type */
        uint8_t total_m = get_slot_model_count(ui, slot);
        if (total_m == 0u) return 0;
        int m = (int)ui->slot_type[slot] + delta;
        while (m < 0) m += total_m;
        m %= total_m;
        if (slot == PA_SLOT_AMP || slot == PA_SLOT_CAB) {                      /* the holes of deleted captures and cabinets are stepped over */
            int dir = delta > 0 ? 1 : -1;
            int steps = 0;
            while (steps < total_m && !model_exists(slot, (uint8_t)m)) {
                m += dir;
                while (m < 0) m += total_m;
                m %= total_m;
                steps++;
            }
        }
        ui_select_model(ui, chain, slot, (uint8_t)(m % total_m));
        return 1;
    }

    if (ui->editor_cursor >= 0) {
        if (slot == PA_SLOT_EQ && ui->slot_type[PA_SLOT_EQ] == 0u) {
            /* an EQ band, -12..+12 dB */
            int b = ui->editor_cursor;
            int db = (int)ui->slot_bands_db[b] + delta;
            if (db < -12) db = -12;
            if (db > 12)  db = 12;
            ui->slot_bands_db[b] = (int8_t)db;
            pa_fx_chain_set_param(chain, PA_SLOT_EQ, b,
                                  (int32_t)(((int64_t)(db + 12) * PA_Q21_ONE) / 24));
            return 1;
        }
        /* a parameter knob */
        int p = ui->editor_cursor;
        int32_t v = ui->slot_params[slot][p];
        v += delta * (PA_Q21_ONE / 50);
        if (v < 0) v = 0;
        if (v > PA_Q21_ONE) v = PA_Q21_ONE;
        ui->slot_params[slot][p] = v;
        pa_fx_chain_set_param(chain, (pa_fx_slot_id_t)slot, p, v);
        return 1;
    }

    return 0;   /* the ON/OFF row */
}

/* ---- Save the current sound into a preset slot: the Save-to dialog and the MIDI "save" command both come here. Returns the pa_preset_save() result (0 = saved). ---- */
static int ui_save_current_to(pa_ui_ctx_t *ui, pa_fx_chain_t *chain, uint8_t target) {
    pa_preset_t cur_p;
    memset(&cur_p, 0, sizeof(cur_p));
    cur_p.magic = PA_PRESET_MAGIC;
    cur_p.bpm = 120;
    pa_preset_load(ui->preset_index, &cur_p);
    /* IMP-02/13: EQ bands, all 6 AMP knobs (PRES, MASTER were dropped) and every block's type are now part of the preset */
    if (ui->slot_type[PA_SLOT_EQ] == 0u)            /* the 5-band keeps its bands in dB */
        for (int b = 0; b < 5; b++)
            ui->slot_params[PA_SLOT_EQ][b] = (int32_t)(((int64_t)(ui->slot_bands_db[b] + 12) * PA_Q21_ONE) / 24);
    pa_preset_capture_from_chain(&cur_p, chain, ui->preset_volume, ui->slot_params, ui->slot_type);
    for (int i = 0; i < 4; i++) cur_p.stomp_map[i] = ui->stomp_map[i];
    if (pa_preset_is_empty(target) || target != ui->preset_index) {
        /* keep the source name when copying; EMPTY slots get "USER NN" */
        if (cur_p.name[0] == 'E' && cur_p.name[1] == 'M') {
            memset(cur_p.name, 0, sizeof(cur_p.name));
            memcpy(cur_p.name, "USER ", 5); fmt_u2(&cur_p.name[5], target + 1u);
        }
    }
    const int save_rc = pa_preset_save(target, &cur_p);
    ui->saved_index = target;
    memcpy(ui->preset_name, cur_p.name, sizeof(ui->preset_name));
    ui->preset_name[sizeof(ui->preset_name) - 1] = '\0';
    ui->preset_index = target;
    ui->bank_group = ui->preset_index / 4u;
    ui->save_failed = (save_rc != 0);
    ui->toast_kind = 0u;
    ui->view = PA_UI_VIEW_SAVED_TOAST;
    ui->saved_toast_timer_ms = ui->save_failed ? 2000 : 1000;
    ui->dirty = 1;
    return save_rc;
}

/* ---- MIDI control (USB and BLE): one event from the queue (include/pa_usb_ble.h pa_midi_ev_t, decoded in usb_ble.c) applied through the same paths the front panel uses. The numbers are in
 * MIDI-MAP-CUSTOM.md. A value out of range is clamped, an unknown parameter or a knob the block does not have is ignored. ---- */
static int midi_clamp(int v, int lo, int hi) { return v < lo ? lo : (v > hi ? hi : v); }

static void midi_slot_set(pa_ui_ctx_t *ui, pa_fx_chain_t *chain, uint8_t slot, int on) {
    if (slot >= PA_SLOT_COUNT || !chain->slots[slot]) return;
    fx_node_t *nd = chain->slots[slot];
    const int shown_on = NODE_SHOWN_ON(nd) ? 1 : 0;
    if (shown_on == (on ? 1 : 0)) return;
    if (on) {                                         /* as the stomp buttons do when they turn a block on */
        if (slot != PA_SLOT_AMP && slot != PA_SLOT_CAB) pa_fx_chain_set_slot_type(chain, (pa_fx_slot_id_t)slot, ui->slot_type[slot]);
        for (int k = 0; k < 6; k++) pa_fx_chain_set_param(chain, (pa_fx_slot_id_t)slot, k, ui->slot_params[slot][k]);
    }
    pa_fx_chain_set_slot_enabled(chain, (pa_fx_slot_id_t)slot, on ? 1 : 0);
    ui->dirty = 1;
}

void pa_ui_midi_control(pa_ui_ctx_t *ui, pa_fx_chain_t *chain, const pa_midi_ev_t *e) {
    if (!ui || !chain || !e) return;
    const int v = e->v;
    int io = 0, settings = 0;
    switch (e->kind) {
    case PA_MEV_PRESET_VOL:
        ui->preset_volume = (uint8_t)midi_clamp(v, 0, 100);
        pa_fx_chain_set_preset_vol(chain, ui->preset_volume);
        ui->dirty = 1;
        break;
    case PA_MEV_PRESET_STEP: {
        int p = (int)ui->preset_index + (e->a ? 1 : -1);
        while (p < 0) p += PA_PRESET_COUNT;
        pa_ui_load_preset(ui, chain, (uint8_t)(p % PA_PRESET_COUNT));
        break; }
    case PA_MEV_TUNER:
        if (v && ui->view != PA_UI_VIEW_TUNER) { ui->prev_view = ui->view; ui->view = PA_UI_VIEW_TUNER; ui->tuner.active = 1; ui->tuner_cursor = 0; ui->dirty = 1; }
        else if (!v && ui->view == PA_UI_VIEW_TUNER) { ui->view = ui->prev_view; ui->tuner.active = 0; ui->dirty = 1; }
        break;
    case PA_MEV_GROOVE_PLAY:
        ui->drum_playing = v ? 1u : 0u; ui->dirty = 1;
        break;
    case PA_MEV_PLAY_MODE:
        ui->play_mode = v ? 1u : 0u; settings = 1; ui->dirty = 1;
        break;
    case PA_MEV_SLOT_SET:
        midi_slot_set(ui, chain, e->a, v);
        break;
    case PA_MEV_KNOB: {
        const uint8_t slot = e->a, k = e->b;
        if (slot >= PA_SLOT_COUNT || !chain->slots[slot]) break;
        if (slot == PA_SLOT_EQ && ui->slot_type[PA_SLOT_EQ] == 0u) {            /* the 5-band: -12..+12 dB */
            if (k > 4u) break;
            const int db = midi_clamp((int)(((uint32_t)v * 24u + 8191u) / 16383u) - 12, -12, 12);
            ui->slot_bands_db[k] = (int8_t)db;
            pa_fx_chain_set_param(chain, PA_SLOT_EQ, k, (int32_t)(((int64_t)(db + 12) * PA_Q21_ONE) / 24));
            ui->dirty = 1;
            break;
        }
        if (k >= get_slot_param_count(ui, slot)) break;                        /* the block has no such knob */
        const int32_t q = (int32_t)(((int64_t)midi_clamp(v, 0, 16383) * PA_Q21_ONE) / 16383);
        ui->slot_params[slot][k] = q;
        pa_fx_chain_set_param(chain, (pa_fx_slot_id_t)slot, k, q);
        ui->dirty = 1;
        break; }
    case PA_MEV_TYPE: {
        const uint8_t slot = e->a;
        const int type = v >= 0x20 ? v - 0x20 : v;                             /* >= 0x20: the firmware's own id + 0x20 (MIDI-MAP-CUSTOM.md 2.3) */
        if (slot >= PA_SLOT_COUNT || type < 0 || type >= (int)get_slot_model_count(ui, slot)) break;
        if ((slot == PA_SLOT_AMP || slot == PA_SLOT_CAB) && !model_exists(slot, (uint8_t)type)) break;    /* a capture or cabinet slot with nothing in it */
        ui_select_model(ui, chain, slot, (uint8_t)type);
        ui->dirty = 1;
        break; }
    case PA_MEV_GLOBAL:
        switch (e->a) {
        case 0:  ui->input_trim_db  = (int8_t)(midi_clamp(v, 0, 24) - 12); io = 1; break;          /* input gain: D = dB + 12 */
        case 1:  ui->gate_thresh_db = (int8_t)(midi_clamp(v, 0, 50) - 80); io = 1; break;          /* gate: D = dB + 80, -80 = off */
        case 2:  ui->out_level_pct = ui->master_volume = (uint8_t)midi_clamp(v, 0, 100); io = 1; break;
        case 3:  ui->io_output_type = v ? 1u : 0u; io = 1; break;
        case 4:  ui->io_cab_on_line = v ? 1u : 0u; io = 1; break;
        case 5:  ui->output_limit_mode = (uint8_t)midi_clamp(v, 0, 2); settings = 1; break;
        case 6:  ui->fizz_filter_enabled = (uint8_t)midi_clamp(v, 0, 3); io = 1; break;
        case 7:  ui->stereo_width = (uint8_t)midi_clamp(v, 0, 100); io = 1; break;
        case 8:  ui->out_route = (uint8_t)midi_clamp(v, 0, 4); io = 1; break;
        case 9:  ui->out_balance = (int8_t)(midi_clamp(v, 0, 100) - 50); io = 1; break;
        case 10: ui->eq_enabled = v ? 1u : 0u; settings = 1; break;
        case 11: case 12: case 13: case 14: case 15:
                 ui->eq_bands_db[e->a - 11u] = (int8_t)(midi_clamp(v, 0, 24) - 12); settings = 1; break;
        case 16: ui->tuner.ref_a4_hz = (uint16_t)(430 + midi_clamp(v, 0, 20)); settings = 1; break;
        case 17: ui->tuner.mute_output = v ? 1u : 0u; settings = 1; break;
        case 19: ui->usb_mode = (uint8_t)midi_clamp(v, 0, 2); settings = 1; break;                  /* USB audio mode: 0 AMP, 1 INTERFACE, 2 INTERFACE + DIRECT MONITOR */
        case 18: ui->usb_loopback = v ? 1u : 0u; settings = 1; break;                              /* USB loopback: the recording includes what the PC plays (the Settings row, the stock's factory ON) */
        default: break;
        }
        break;
    case PA_MEV_GROOVE:
        switch (e->a) {
        case 0: ui->drum_playing = v ? 1u : 0u; break;
        case 1: ui->drum_bpm = (uint16_t)(40 + midi_clamp(v, 0, 220)); settings = 1; break;
        case 2: ui->drum_pattern = (uint8_t)midi_clamp(v, 0, (int)PA_DRUM_GROOVE_STYLE_COUNT - 1); settings = 1; break;
        case 3: ui->drum_volume = (uint8_t)midi_clamp(v, 0, 100); settings = 1; break;
        case 4: ui->drum_drums_on = v ? 1u : 0u; settings = 1; break;
        default: break;
        }
        ui->dirty = 1;
        break;
    case PA_MEV_PRESET_PARAM:
        if (e->a == 0u) { ui->preset_volume = (uint8_t)midi_clamp(v, 0, 100); pa_fx_chain_set_preset_vol(chain, ui->preset_volume); ui->dirty = 1; }
        else if (e->a >= 3u && e->a <= 6u && v <= 7) {                          /* the stomp map: the module number 0..7 in the stock order */
            static const int8_t mod_slot[8] = { PA_SLOT_DYNAMICS, PA_SLOT_AMP, PA_SLOT_MOD, PA_SLOT_DELAY, PA_SLOT_REVERB, PA_SLOT_CAB, PA_SLOT_DRIVE, PA_SLOT_EQ };
            ui->stomp_map[e->a - 3u] = (uint8_t)mod_slot[v];
            ui->dirty = 1;
        }
        else if (e->a == 7u && v < 64 && ui->view != PA_UI_VIEW_MOVING) {                 /* the chain: move the slot (v >> 3) to the position (v & 7), the others shift as on the unit's own move (the editor's drag and drop) */
            const uint8_t slot = (uint8_t)(v >> 3), to = (uint8_t)(v & 7);
            uint8_t from = 8u;
            for (uint8_t k = 0; k < PA_SLOT_COUNT; k++) if (chain->slot_order[k] == slot) { from = k; break; }
            if (from < 8u && from != to) {
                if (from < to) for (uint8_t k = from; k < to; k++) chain->slot_order[k] = chain->slot_order[k + 1];
                else           for (uint8_t k = from; k > to; k--) chain->slot_order[k] = chain->slot_order[k - 1];
                chain->slot_order[to] = slot;
                pa_fx_chain_relink_ordered(chain);
                ui->dirty = 1;
            }
        }
        /* parameters 1 and 2 of the map (the preset's input trim and gate threshold) are stored in a preset but not used by the DSP: no control */
        break;
    case PA_MEV_SAVE: {
        const uint8_t target = e->a ? (uint8_t)midi_clamp(v, 0, (int)PA_PRESET_COUNT - 1) : ui->preset_index;
        (void)ui_save_current_to(ui, chain, target);
        break; }
    default: break;
    }
    if (io) pa_ui_apply_io_settings(ui, chain);
    if (io || settings) { pa_ui_mark_settings_dirty(ui); ui->dirty = 1; }
}

/* ---- what an editor page reads (include/pa_sysex.h documents the record) ---- */
static uint8_t q21_pct(int32_t v) {
    int32_t p = (int32_t)(((int64_t)v * 100 + 1048576) / 2097152);
    return (uint8_t)(p < 0 ? 0 : (p > 100 ? 100 : p));
}

uint32_t pa_ui_state_snapshot(const pa_ui_ctx_t *ui, const pa_fx_chain_t *chain, uint8_t *out, uint32_t cap) {
    extern volatile uint8_t g_pa_user_cab_active;
    if (!ui || !out || cap < PA_STATE_REC_SIZE) return 0u;
    memset(out, 0, PA_STATE_REC_SIZE);
    out[0] = 1u;
    out[1] = (uint8_t)ui->view;
    out[2] = ui->preset_index;
    out[3] = (uint8_t)((ui->play_mode == 1u ? 1u : 0u) | (ui->tuner.active ? 2u : 0u) | (ui->drum_playing ? 4u : 0u) | (ui->usb_loopback ? 8u : 0u) | 16u | (uint8_t)((ui->usb_mode & 3u) << 5));
    out[4] = ui->preset_volume;
    out[5] = ui->out_level_pct;
    for (int i = 0; i < 16 && ui->preset_name[i]; i++) out[6 + i] = (uint8_t)ui->preset_name[i];
    for (int s = 0; s < PA_SLOT_COUNT; s++) {
        uint8_t *r = out + 22 + 8 * s;
        const fx_node_t *nd = chain ? chain->slots[s] : 0;
        r[0] = NODE_SHOWN_ON(nd) ? 1u : 0u;
        r[1] = ui->slot_type[s];
        for (int k = 0; k < 6; k++) r[2 + k] = q21_pct(ui->slot_params[s][k]);
    }
    out[86] = (uint8_t)(ui->input_trim_db + 12);
    out[87] = (uint8_t)(ui->gate_thresh_db + 80);
    out[88] = ui->io_output_type;
    out[89] = ui->io_cab_on_line;
    out[90] = ui->output_limit_mode;
    out[91] = ui->fizz_filter_enabled;
    out[92] = ui->stereo_width;
    out[93] = ui->out_route;
    out[94] = (uint8_t)(ui->out_balance + 50);
    out[95] = ui->eq_enabled;
    for (int b = 0; b < 5; b++) out[96 + b] = (uint8_t)(ui->eq_bands_db[b] + 12);
    out[101] = (uint8_t)(ui->tuner.ref_a4_hz - 430u);
    out[102] = ui->tuner.mute_output;
    out[103] = ui->bt_mode;
    out[104] = ui->display_brightness;
    out[105] = ui->home_view_mode;
    out[106] = (uint8_t)(ui->drum_bpm & 0xFFu);
    out[107] = (uint8_t)(ui->drum_bpm >> 8);
    out[108] = ui->drum_pattern;
    out[109] = ui->drum_volume;
    out[110] = ui->drum_drums_on;
    for (int k = 0; k < 4; k++) out[111 + k] = ui->stomp_map[k];
    out[115] = pa_user_nam_get_active_count();                                                         /* the captures that exist */
    out[116] = g_pa_user_cab_active ? 1u : 0u;
    if (chain) for (uint32_t i = 0; i < 8u; i++) out[117 + i] = chain->slot_order[i];                     /* the chain, in signal order: slot ids */
    out[125] = pa_user_cab_active_count();                                                              /* the stored cabinets that exist */
    out[126] = (uint8_t)(PA_FLASH_USER_NAM_BLOB_SLOTS - (pa_user_nam_get_active_count() > PA_FLASH_USER_NAM_BLOB_SLOTS ? PA_FLASH_USER_NAM_BLOB_SLOTS : pa_user_nam_get_active_count()));   /* free capture slots */
    out[127] = (uint8_t)(PA_USER_CAB_SLOTS - pa_user_cab_active_count());                               /* free cabinet slots */
    return PA_STATE_REC_SIZE;
}

/* a page of names: [kind, total, start, n] + n x 16 bytes. kind 0 = the user amp captures, 1 = the cabinets beyond the 20 stock ones */
uint32_t pa_ui_name_list(uint8_t kind, uint8_t start, uint8_t *out, uint32_t cap) {
    if (!out || cap < 5u + 16u) return 0u;
    /* kind 0: every amp type in the order of the type knob (the stock ones, then the stored captures: a deleted one is an empty name); kind 1: every cabinet type the same way.
     * out[4] = the first type a user can delete (the first stored capture / cabinet), so the editor needs no table of its own. */
    const uint32_t total = (kind == 0u) ? (uint32_t)(PA_STOCK_AMP_COUNT + pa_user_nam_get_count()) : (uint32_t)pa_cab_total_count();
    uint32_t n = 0;
    if (kind == 255u) {                                       /* the build code, as text: [255, 0, 0, length, 0] + the characters (the editor shows it) */
        const char *bc = PA_BUILD_CODE; uint32_t l = 0; while (bc[l] && l < 40u) l++;
        out[0] = 255u; out[1] = 0u; out[2] = 0u; out[3] = (uint8_t)l; out[4] = (uint8_t)PA_PACK_NUM;
        memcpy(out + 5, bc, l);
        return 5u + l;
    }
    out[0] = kind; out[1] = (uint8_t)total; out[2] = start;
    out[4] = (kind == 0u) ? (uint8_t)PA_STOCK_AMP_COUNT : (uint8_t)(total ? pa_cab_total_count() - pa_user_cab_span() : 0u);
    for (uint32_t i = start; i < total && (5u + (n + 1u) * 16u) <= cap && n < 15u; i++, n++) {
        const char *nm = (kind == 0u) ? (i < PA_STOCK_AMP_COUNT ? pa_stock_get_amp_name((uint8_t)i) : (pa_user_nam_is_active((uint8_t)(i - PA_STOCK_AMP_COUNT)) ? pa_user_nam_get_name((uint8_t)(i - PA_STOCK_AMP_COUNT)) : ""))
                                      : (i < out[4] ? pa_cab_name((uint8_t)i) : (pa_user_cab_is_active((uint8_t)(i - out[4])) ? pa_user_cab_name((uint8_t)(i - out[4])) : ""));
        uint8_t *d = out + 5 + n * 16u;
        memset(d, 0, 16);
        for (int k = 0; k < 15 && nm && nm[k]; k++) d[k] = (uint8_t)nm[k];
    }
    out[3] = (uint8_t)n;
    return 5u + n * 16u;
}

/* A stored capture (kind 0) or cabinet (kind 1) was deleted: a slot that plays it goes back to the first stock model, so nothing keeps sounding from a sector that is gone. */
void pa_ui_drop_user_model(pa_ui_ctx_t *ui, pa_fx_chain_t *chain, uint8_t kind, uint8_t idx) {
    (void)idx;
    if (!ui || !chain) return;
    const uint8_t slot = kind == 0u ? PA_SLOT_AMP : PA_SLOT_CAB;
    if (!model_exists(slot, ui->slot_type[slot])) { ui_select_model(ui, chain, slot, 0u); ui->dirty = 1; }
}

void pa_ui_handle_inputs(pa_ui_ctx_t *ui, pa_fx_chain_t *chain,
                         uint32_t buttons_pressed, int32_t enc_delta,
                         const uint16_t adc_pots[4]) {
    if (!ui || !chain) return;

    /* Blk-01: the Pocket Amp has no master pot. gpio_scan.c never sampled the ADC, so pots[0]
     * stayed at 2048, and 2048*100/1023 clamped to 100 % -> every preset_vol was overwritten to
     * 100 one tick after loading and VOL+/- were undone. Only honour a pot on hardware that has
     * one, scale it as Q12, and route it to the OUTPUT level (not the preset volume). */
#if defined(PA_HAS_MASTER_POT) && PA_HAS_MASTER_POT
    if (adc_pots && adc_pots[0] != 0xFFFFu) {                  /* 0xFFFF: the knob has not been read yet */
        uint8_t m_vol = (uint8_t)(((uint32_t)adc_pots[0] * 100u + 2047u) / 4095u);
        if (m_vol > 100u) m_vol = 100u;
        /* The LAST CHANGE wins: the knob sets the level when IT moves (2 % or more since the reading it last set, or the first reading), and a level set from the menu or over MIDI (NRPN GLOBAL
         * output level) stays until the knob is turned again. It used to compare with the level itself, so any other source was overwritten on the next tick. */
        static uint8_t s_pot_pct = 255u;
        const int dv = (s_pot_pct == 255u) ? 100 : (int)m_vol - (int)s_pot_pct;
        if (dv >= 2 || dv <= -2 || (m_vol == 0u && s_pot_pct != 0u) || (m_vol == 100u && s_pot_pct != 100u)) {
            s_pot_pct = m_vol;
            ui->out_level_pct = m_vol;
            ui->master_volume = m_vol;
            pa_ui_apply_io_settings(ui, chain);
            ui->dirty = 1;
        }
    }
#else
    (void)adc_pots;
#endif

    /* IMP-06: activity tracking for auto-dim. The first input after dimming only wakes the screen. */
    if (buttons_pressed || enc_delta) {
        ui->last_input_ms = plat_ms();
        if (ui->dimmed) {
            ui->dimmed = 0;
            plat_backlight_set(ui->display_brightness);
            ui->dirty = 1;
            return;
        }
    }

    /* Global Combos (Active everywhere except modal dialogs) */
    if (ui->view != PA_UI_VIEW_SAVE_DIALOG && ui->view != PA_UI_VIEW_SAVED_TOAST && ui->view != PA_UI_VIEW_FACTORY_RESET) {
        /* VOL- & VOL+ -> Enter / Toggle Tuner Mode */
        if ((buttons_pressed & PA_COMBO_TUNER) == PA_COMBO_TUNER) {
            if (ui->view == PA_UI_VIEW_TUNER) {
                ui->view = ui->prev_view;
                ui->tuner.active = 0;
            } else {
                ui->prev_view = ui->view;
                ui->view = PA_UI_VIEW_TUNER;
                ui->tuner.active = 1;
                ui->tuner_cursor = 0;
            }
            ui->dirty = 1;
            return;
        }

        /* < & > -> Enter / Leave Bank Mode */
        if ((buttons_pressed & PA_COMBO_BANK) == PA_COMBO_BANK) {
            if (ui->view == PA_UI_VIEW_BANK_MODE) {
                ui->view = PA_UI_VIEW_MAIN_GRID;
            } else {
                ui->prev_view = ui->view;
                ui->bank_group = ui->preset_index / 4u;
                ui->view = PA_UI_VIEW_BANK_MODE;
            }
            ui->dirty = 1;
            return;
        }

        /* HOME & VOL- -> Enter / Leave Groove Station */
        if ((buttons_pressed & PA_COMBO_DRUMS) == PA_COMBO_DRUMS) {
            if (ui->view == PA_UI_VIEW_DRUMS) {
                ui->view = PA_UI_VIEW_MAIN_GRID;
            } else {
                ui->prev_view = ui->view;
                ui->view = PA_UI_VIEW_DRUMS;
            }
            ui->dirty = 1;
            return;
        }
    }

    /* HOME held 1.5 s on the home screen: switch between PRESET and STOMP mode (the Live Stomp Lock this replaced is gone).
     * On any other screen a long press is just HOME: it goes home. */
    if (buttons_pressed & PA_BTN_HOME_HOLD) {
        if (ui->view == PA_UI_VIEW_MAIN_GRID) {
            ui->play_mode = (ui->play_mode == 1u) ? 0u : 1u;
            pa_ui_mark_settings_dirty(ui);
            ui->dirty = 1;
            return;
        }
        buttons_pressed |= PA_BTN_HOME;
    }

    /* No PARA 3 s hold any more (2026-11-04, owner's request: it fired by accident and was unreliable): the factory reset is reached only through Settings > System > FACTORY RESET, and confirmed
     * there with a normal click on RESET (the dialog opens on CANCEL, the safe choice). Holding PARA still does 1 s Save-to and 2 s Move; past 3 s nothing more happens. */

    /* Universal HOME button rule:
     * "HOME always goes to Home. Pressed on Home, it opens Settings." */
    if (buttons_pressed & PA_BTN_HOME) {
        if (ui->view == PA_UI_VIEW_MAIN_GRID) {
            ui->prev_view = PA_UI_VIEW_MAIN_GRID;
            ui->view = PA_UI_VIEW_SETTINGS;
            ui->settings_cursor = 0;
        } else if (ui->view == PA_UI_VIEW_MOVING) {
            /* Cancel moving: restore original position */
            if (ui->moving_orig_idx < 8) {
                /* Restore chain order */
                uint8_t cur_idx = ui->cursor_slot_idx;
                uint8_t orig_idx = ui->moving_orig_idx;
                if (cur_idx != orig_idx) {
                    uint8_t moved_slot = chain->slot_order[cur_idx];
                    if (cur_idx < orig_idx) {
                        for (uint8_t k = cur_idx; k < orig_idx; k++) {
                            chain->slot_order[k] = chain->slot_order[k + 1];
                        }
                    } else {
                        for (uint8_t k = cur_idx; k > orig_idx; k--) {
                            chain->slot_order[k] = chain->slot_order[k - 1];
                        }
                    }
                    chain->slot_order[orig_idx] = moved_slot;
                }
                ui->cursor_slot_idx = orig_idx;
            }
            ui->view = PA_UI_VIEW_MAIN_GRID;
        } else {
            ui->view = PA_UI_VIEW_MAIN_GRID;
            ui->tuner.active = 0;
        }
        ui->dirty = 1;
        return;
    }

    /* Master Volume adjustment via VOL- / VOL+ on Home Screen (PRESET Mode) */
    if (ui->view == PA_UI_VIEW_MAIN_GRID && ui->play_mode == 0) {
        if (buttons_pressed & (PA_BTN_VOL_MINUS | PA_BTN_VOL_PLUS)) {
            if (buttons_pressed & PA_BTN_VOL_MINUS) {
                ui->preset_volume = (ui->preset_volume >= 5u) ? (uint8_t)(ui->preset_volume - 5u) : 0u;
            }
            if (buttons_pressed & PA_BTN_VOL_PLUS) {
                ui->preset_volume = (ui->preset_volume <= 95u) ? (uint8_t)(ui->preset_volume + 5u) : 100u;
            }
            pa_fx_chain_set_preset_vol(chain, ui->preset_volume);
            ui->dirty = 1;
            return;
        }
    }

    /* IMP-04: any key dismisses the SAVED card early */
    if (ui->view == PA_UI_VIEW_SAVED_TOAST && (buttons_pressed || enc_delta)) {
        ui->saved_toast_timer_ms = 0;
        ui->view = (ui->prev_view == PA_UI_VIEW_PARAM_EDIT) ? PA_UI_VIEW_PARAM_EDIT : PA_UI_VIEW_MAIN_GRID;
        ui->dirty = 1;
        return;
    }

    /* ---------------------------------------------------- SCREEN DISPATCH HANDLERS */

    /* 1. Home Screen (Grid & List) */
    if (ui->view == PA_UI_VIEW_MAIN_GRID) {
        if (ui->play_mode == 1) {
            /* STOMP MODE: Footswitches directly toggle blocks in the chain. A long press of a key opens the menu that assigns the effect it toggles. */
            static const uint32_t k_stomp_hold[4] = { PA_BTN_VOLM_HOLD, PA_BTN_VOLP_HOLD, PA_BTN_LEFT_HOLD, PA_BTN_RIGHT_HOLD };
            for (int i = 0; i < 4; i++) {
                if (buttons_pressed & k_stomp_hold[i]) {
                    ui->stomp_assign_key = (uint8_t)i;
                    ui->stomp_assign_cursor = ui->stomp_map[i] < PA_SLOT_COUNT ? ui->stomp_map[i] : (uint8_t)PA_SLOT_DRIVE;
                    ui->view = PA_UI_VIEW_STOMP_ASSIGN;
                    ui->dirty = 1;
                    return;
                }
            }
            /* IMP-07: four footswitches toggle the preset's stomp_map[] blocks (default DRV, MOD,
             * DLY, REV). Toggles are crossfaded in the ISR and DLY/REV tails ring out. */
            static const uint32_t k_stomp_btn[4] = { PA_BTN_VOL_MINUS, PA_BTN_VOL_PLUS, PA_BTN_LEFT, PA_BTN_RIGHT };
            for (int i = 0; i < 4; i++) {
                if (buttons_pressed & k_stomp_btn[i]) {
                    uint8_t sid = ui->stomp_map[i] < PA_SLOT_COUNT ? ui->stomp_map[i] : PA_SLOT_DRIVE;
                    fx_node_t *nd = chain->slots[sid];
                    if (nd) {
                        int turning_on = !(nd->active && nd->fade_dir >= 0);
                        if (turning_on) {
                            if (sid != PA_SLOT_AMP && sid != PA_SLOT_CAB) {
                                pa_fx_chain_set_slot_type(chain, (pa_fx_slot_id_t)sid, ui->slot_type[sid]);
                            }
                            for (int k = 0; k < 6; k++) {
                                pa_fx_chain_set_param(chain, (pa_fx_slot_id_t)sid, k, ui->slot_params[sid][k]);
                            }
                        }
                        pa_fx_chain_set_slot_enabled(chain, (pa_fx_slot_id_t)sid, turning_on);
                    }
                    ui->dirty = 1;
                    return;
                }
            }
        } else {
            /* PRESET MODE: < > pick the previous / next preset P01-P80 (loads at once, click-free); the PARA knob moves the block cursor along the chain (swapped on the owner's request: it
             * was the other way round) */
            int dp = 0;
            if (buttons_pressed & PA_BTN_LEFT)  dp -= 1;
            if (buttons_pressed & PA_BTN_RIGHT) dp += 1;
            if (dp != 0) {
                int p = (int)ui->preset_index + dp;
                while (p < 0) p += PA_PRESET_COUNT;
                pa_ui_load_preset(ui, chain, (uint8_t)(p % PA_PRESET_COUNT));
            }
            if (enc_delta != 0) {
                int c = (int)ui->cursor_slot_idx + enc_delta;
                while (c < 0) c += 8;
                ui->cursor_slot_idx = (uint8_t)(c % 8);
                ui->dirty = 1;
            }
        }

        /* STOMP mode: < > are footswitches (above); the PARA turn still picks the previous / next preset */
        if (ui->play_mode == 1 && enc_delta != 0) {
            int p = (int)ui->preset_index + enc_delta;
            while (p < 0) p += PA_PRESET_COUNT;
            pa_ui_load_preset(ui, chain, (uint8_t)(p % PA_PRESET_COUNT));
        }
        /* PARA press: open editor of block under cursor */
        if (buttons_pressed & PA_BTN_PARA_CLICK) {
            ui->prev_view = PA_UI_VIEW_MAIN_GRID;
            ui->view = PA_UI_VIEW_PARAM_EDIT;
            /* Land on the MODEL / TYPE row (the standard on every effect page):
             * the type is what you usually change first. Note: < and > dial in
             * whatever is under the cursor, so from here they change the model;
             * the PARA encoder (or VOL+) steps down to the knobs. */
            ui->editor_cursor = -1;
            ui->edit_mode = 0;     /* Start in hover mode */
            ui->dirty = 1;
        }
        /* PARA 1s hold: open Save dialog */
        if (buttons_pressed & PA_BTN_PARA_HOLD1) {
            ui->prev_view = PA_UI_VIEW_MAIN_GRID;
            ui->save_target = ui->preset_index;
            ui->save_btn = 1; /* Default on SAVE */
            ui->view = PA_UI_VIEW_SAVE_DIALOG;
            ui->dirty = 1;
        }
        /* PARA 2s hold: enter Moving Mode (pick up block under cursor) */
        if (buttons_pressed & PA_BTN_PARA_HOLD2) {
            ui->moving_slot = chain->slot_order[ui->cursor_slot_idx];
            ui->moving_orig_idx = ui->cursor_slot_idx;
            ui->view = PA_UI_VIEW_MOVING;
            ui->dirty = 1;
        }
        return;
    }

    /* 2. Moving Mode */
    if (ui->view == PA_UI_VIEW_MOVING) {
        /* VOL- cancels and restores position */
        if (buttons_pressed & PA_BTN_VOL_MINUS) {
            if (ui->moving_orig_idx < 8) {
                uint8_t cur_idx = ui->cursor_slot_idx;
                uint8_t orig_idx = ui->moving_orig_idx;
                if (cur_idx != orig_idx) {
                    uint8_t moved_slot = chain->slot_order[cur_idx];
                    if (cur_idx < orig_idx) {
                        for (uint8_t k = cur_idx; k < orig_idx; k++) {
                            chain->slot_order[k] = chain->slot_order[k + 1];
                        }
                    } else {
                        for (uint8_t k = cur_idx; k > orig_idx; k--) {
                            chain->slot_order[k] = chain->slot_order[k - 1];
                        }
                    }
                    chain->slot_order[orig_idx] = moved_slot;
                }
                ui->cursor_slot_idx = orig_idx;
            }
            ui->view = PA_UI_VIEW_MAIN_GRID;
            ui->dirty = 1;
            return;
        }

        /* PARA turn OR < > buttons: move lifted block left / right through chain */
        int step_delta = enc_delta;
        if (buttons_pressed & PA_BTN_LEFT)  step_delta = -1;
        if (buttons_pressed & PA_BTN_RIGHT) step_delta = 1;

        if (step_delta != 0) {
            int new_idx = (int)ui->cursor_slot_idx + step_delta;
            if (new_idx < 0) new_idx = 0;
            if (new_idx > 7) new_idx = 7;
            if ((uint8_t)new_idx != ui->cursor_slot_idx) {
                uint8_t old_idx = ui->cursor_slot_idx;
                uint8_t moved_slot = chain->slot_order[old_idx];
                if (old_idx < new_idx) {
                    for (uint8_t k = old_idx; k < (uint8_t)new_idx; k++) {
                        chain->slot_order[k] = chain->slot_order[k + 1];
                    }
                } else {
                    for (uint8_t k = old_idx; k > (uint8_t)new_idx; k--) {
                        chain->slot_order[k] = chain->slot_order[k - 1];
                    }
                }
                chain->slot_order[new_idx] = moved_slot;
                ui->cursor_slot_idx = (uint8_t)new_idx;
                ui->dirty = 1;
            }
        }
        /* PARA press: drop here */
        if (buttons_pressed & PA_BTN_PARA_CLICK) {
            pa_fx_chain_relink_ordered(chain);
            ui->view = PA_UI_VIEW_MAIN_GRID;
            ui->dirty = 1;
        }
        return;
    }

    /* 3. Block Parameter Editor */
    if (ui->view == PA_UI_VIEW_PARAM_EDIT) {
        /* VOL- universal Back button */
        if (buttons_pressed & PA_BTN_VOL_MINUS) {
            if (ui->edit_mode == 1) {
                ui->edit_mode = 0;
            } else {
                ui->view = ui->prev_view ? ui->prev_view : PA_UI_VIEW_MAIN_GRID;
            }
            ui->dirty = 1;
            return;
        }

        uint8_t slot = (chain && ui->cursor_slot_idx < 8) ? chain->slot_order[ui->cursor_slot_idx] : PA_SLOT_AMP;
        uint8_t nparams = get_slot_param_count(ui, slot);

        if (ui->edit_mode == 0) {
            /* HOVER MODE */
            /* < > DIAL IN the setting under the cursor - the same thing the
             * Settings screen does - so an effect can be set up with the
             * buttons alone. The PARA encoder moves to the next setting,
             * and so does VOL+ (VOL- is the universal Back). */
            if (buttons_pressed & PA_BTN_LEFT) {
                if (block_editor_adjust(ui, chain, slot, -1)) ui->dirty = 1;
            }
            if (buttons_pressed & PA_BTN_RIGHT) {
                if (block_editor_adjust(ui, chain, slot, +1)) ui->dirty = 1;
            }
            if (buttons_pressed & PA_BTN_VOL_PLUS) {
                int cur = (int)ui->editor_cursor + 1;
                if (cur > (int)(nparams - 1)) cur = -2;     /* wrap round to ON/OFF */
                ui->editor_cursor = (int8_t)cur;
                ui->dirty = 1;
            }
            /* PARA turn also steps hover cursor */
            if (enc_delta != 0) {
                int cur = (int)ui->editor_cursor + enc_delta;
                if (cur < -2) cur = -2;
                if (cur > (int)(nparams - 1)) cur = (int)(nparams - 1);
                ui->editor_cursor = (int8_t)cur;
                ui->dirty = 1;
            }
            /* PARA click: if ON/OFF, toggle bypass; if Model/Param, enter edit mode */
            if (buttons_pressed & PA_BTN_PARA_CLICK) {
                if (ui->editor_cursor == -2) {
                    fx_node_t *nd = chain ? chain->slots[slot] : 0;
                    if (nd) {
                        int turning_on = !NODE_SHOWN_ON(nd);                       /* (a press during a fade-out turns it back on) */
                        if (turning_on) {
                            if (slot != PA_SLOT_AMP && slot != PA_SLOT_CAB) {
                                pa_fx_chain_set_slot_type(chain, (pa_fx_slot_id_t)slot, ui->slot_type[slot]);
                            }
                            for (int k = 0; k < 6; k++) {
                                pa_fx_chain_set_param(chain, (pa_fx_slot_id_t)slot, k, ui->slot_params[slot][k]);
                            }
                        }
                        pa_fx_chain_set_slot_enabled(chain, (pa_fx_slot_id_t)slot, turning_on);
                    }
                } else {
                    ui->edit_mode = 1; /* Enter value adjust mode */
                }
                ui->dirty = 1;
            }
        } else {
            /* VALUE ADJUST MODE */
            /* The PARA encoder and the < > buttons both change the value
             * under the cursor (the Settings screen works the same way). */
            int d = enc_delta;
            if (buttons_pressed & PA_BTN_LEFT)  d--;
            if (buttons_pressed & PA_BTN_RIGHT) d++;
            if (d != 0) {
                if (block_editor_adjust(ui, chain, slot, d)) ui->dirty = 1;
            }
            /* PARA click: exit edit mode, resume < > navigation */
            if (buttons_pressed & PA_BTN_PARA_CLICK) {
                ui->edit_mode = 0;
                ui->dirty = 1;
            }
            /* < > while editing adjusts the value (handled above); the PARA
             * click leaves the edit mode. */
            if (buttons_pressed & PA_BTN_PARA_CLICK) {
                ui->edit_mode = 0;
                ui->dirty = 1;
            }
        }

        /* PARA 1s hold: Save preset */
        if (buttons_pressed & PA_BTN_PARA_HOLD1) {
            ui->prev_view = PA_UI_VIEW_PARAM_EDIT;
            ui->save_target = ui->preset_index;
            ui->save_btn = 1;
            ui->view = PA_UI_VIEW_SAVE_DIALOG;
            ui->dirty = 1;
        }
        return;
    }

    /* 4. Bank Mode Screen */
    if (ui->view == PA_UI_VIEW_BANK_MODE) {
        uint16_t bstart = (uint16_t)(ui->bank_group * 4u + 1u);
        /* VOL- = Slot 1, VOL+ = Slot 2, < = Slot 3, > = Slot 4 */
        int selected_slot = -1;
        if (buttons_pressed & PA_BTN_VOL_MINUS) selected_slot = 0;
        if (buttons_pressed & PA_BTN_VOL_PLUS)  selected_slot = 1;
        if (buttons_pressed & PA_BTN_LEFT)      selected_slot = 2;
        if (buttons_pressed & PA_BTN_RIGHT)     selected_slot = 3;

        if (selected_slot >= 0) {
            uint16_t target_p = bstart + (uint16_t)selected_slot;
            if (target_p <= PA_PRESET_COUNT) {
                pa_ui_load_preset(ui, chain, (uint8_t)(target_p - 1u));
            }
        }

        /* PARA turn: change bank (1..20) */
        if (enc_delta != 0) {
            int b = (int)ui->bank_group + enc_delta;
            while (b < 0) b += 20;
            ui->bank_group = (uint8_t)(b % 20);
            ui->dirty = 1;
        }

        /* PARA press: open Home grid of the playing preset */
        if (buttons_pressed & PA_BTN_PARA_CLICK) {
            ui->view = PA_UI_VIEW_MAIN_GRID;
            ui->dirty = 1;
        }
        return;
    }

    /* 5. Save Preset Dialog */
    if (ui->view == PA_UI_VIEW_SAVE_DIALOG) {
        if (buttons_pressed & (PA_BTN_VOL_MINUS | PA_BTN_HOME)) {
            ui->view = ui->prev_view ? ui->prev_view : PA_UI_VIEW_MAIN_GRID;
            ui->dirty = 1;
            return;
        }
        /* PARA turn: rapidly scroll destination slot P01-P80 */
        if (enc_delta != 0) {
            int tgt = (int)ui->save_target + enc_delta;
            while (tgt < 0) tgt += PA_PRESET_COUNT;
            ui->save_target = (uint8_t)(tgt % PA_PRESET_COUNT);
            ui->dirty = 1;
        }
        /* < > step destination slot by 1 */
        if (buttons_pressed & PA_BTN_LEFT) {
            ui->save_target = (ui->save_target == 0u) ? (PA_PRESET_COUNT - 1u) : (uint8_t)(ui->save_target - 1u);
            ui->dirty = 1;
        }
        if (buttons_pressed & PA_BTN_RIGHT) {
            ui->save_target = (uint8_t)((ui->save_target + 1u) % PA_PRESET_COUNT);
            ui->dirty = 1;
        }
        /* PARA press: execute Save to selected slot */
        if (buttons_pressed & PA_BTN_PARA_CLICK) {
            (void)ui_save_current_to(ui, chain, ui->save_target);
        }
        return;
    }

    /* 6. Tuner Screen */
    if (ui->view == PA_UI_VIEW_TUNER) {
        if (buttons_pressed & PA_BTN_VOL_MINUS) {
            ui->view = ui->prev_view ? ui->prev_view : PA_UI_VIEW_MAIN_GRID;
            ui->tuner.active = 0;
            ui->dirty = 1;
            return;
        }
        /* PARA turn: select REF (0) or MUTE (1) */
        if (enc_delta != 0) {
            ui->tuner_cursor ^= 1;
            ui->dirty = 1;
        }
        /* < > when REF selected: adjust 430-450 Hz */
        if (ui->tuner_cursor == 0) {
            if (buttons_pressed & PA_BTN_LEFT) {
                if (ui->tuner.ref_a4_hz > 430) {
                    ui->tuner.ref_a4_hz--;
                    pa_ui_mark_settings_dirty(ui);
                    ui->dirty = 1;
                }
            }
            if (buttons_pressed & PA_BTN_RIGHT) {
                if (ui->tuner.ref_a4_hz < 450) {
                    ui->tuner.ref_a4_hz++;
                    pa_ui_mark_settings_dirty(ui);
                    ui->dirty = 1;
                }
            }
        }
        /* PARA press: when MUTE selected, toggle mute */
        if (buttons_pressed & PA_BTN_PARA_CLICK) {
            if (ui->tuner_cursor == 1) {
                ui->tuner.mute_output ^= 1;
                ui->dirty = 1;
            }
        }
        return;
    }

    /* 7. Groove Station Screen
     *
     * The redesign's controls: VOL- / VOL+ is the drum volume (one segment of
     * 10 % per press), < > is the style (40 of them, wrapping), the PARA knob
     * is the tempo and a PARA press is play / stop. There is no drums on /
     * off and no click mode: the metronome patterns are gone from the UI. */
    if (ui->view == PA_UI_VIEW_DRUMS) {
        /* VOL- / VOL+ adjusts dedicated Drum Volume (0..100%) */
        if (buttons_pressed & PA_BTN_VOL_MINUS) {
            ui->drum_volume = (ui->drum_volume >= 10u) ? (uint8_t)(ui->drum_volume - 10u) : 0u;
            ui->dirty = 1;
        }
        if (buttons_pressed & PA_BTN_VOL_PLUS) {
            ui->drum_volume = (ui->drum_volume <= 90u) ? (uint8_t)(ui->drum_volume + 10u) : 100u;
            ui->dirty = 1;
        }
        /* < > = Previous / next style (40 of them, wrapping) */
        if (buttons_pressed & PA_BTN_LEFT) {
            ui->drum_pattern = (ui->drum_pattern == 0) ? (PA_DRUM_GROOVE_STYLE_COUNT - 1u)
                                                        : (uint8_t)(ui->drum_pattern - 1u);
            pa_ui_mark_settings_dirty(ui);
            ui->dirty = 1;
        }
        if (buttons_pressed & PA_BTN_RIGHT) {
            ui->drum_pattern = (uint8_t)((ui->drum_pattern + 1u) % PA_DRUM_GROOVE_STYLE_COUNT);
            pa_ui_mark_settings_dirty(ui);
            ui->dirty = 1;
        }
        /* PARA turn = BPM 40-240 */
        if (enc_delta != 0) {
            int bpm = (int)ui->drum_bpm + enc_delta;
            if (bpm < 40)  bpm = 40;
            if (bpm > 240) bpm = 240;
            ui->drum_bpm = (uint16_t)bpm;
            ui->dirty = 1;
        }
        /* PARA click = Play / Stop */
        if (buttons_pressed & PA_BTN_PARA_CLICK) {
            ui->drum_playing ^= 1;
            ui->dirty = 1;
        }
        return;
    }

    /* 8. Settings Menu Screen (8 rows) */
    if (ui->view == PA_UI_VIEW_SETTINGS) {
        /* VOL- universal Back button returns to Home */
        if (buttons_pressed & PA_BTN_VOL_MINUS) {
            ui->view = PA_UI_VIEW_MAIN_GRID;
            ui->dirty = 1;
            return;
        }

        /* < > OR PARA rotary turn selects which row to change (0..7) */
        {
            int d = enc_delta;
            if (buttons_pressed & PA_BTN_LEFT)  d -= 1;
            if (buttons_pressed & PA_BTN_RIGHT) d += 1;
            if (d != 0) { ui->settings_cursor = menu_wrap(ui->settings_cursor, d, SETTINGS_ROW_COUNT); ui->dirty = 1; }   /* wraps at both ends */
        }

        /* Update scroll window (7 visible items in 8-item menu) */
        if (ui->settings_cursor < ui->settings_scroll_top) {
            ui->settings_scroll_top = ui->settings_cursor;
        }
        if (ui->settings_cursor >= (ui->settings_scroll_top + 7)) {
            ui->settings_scroll_top = (uint8_t)(ui->settings_cursor - 6);
        }

        /* PARA press: activate selected row */
        if (buttons_pressed & PA_BTN_PARA_CLICK) {
            if (ui->settings_cursor == 0) {
                /* Input / Output sub-page */
                ui->prev_view = PA_UI_VIEW_SETTINGS;
                ui->view = PA_UI_VIEW_INPUT_OUTPUT;
                ui->sub_cursor = 0;
            } else if (ui->settings_cursor == 1) {
                /* Bluetooth: OFF -> BLE -> BLE+A2DP -> OFF. The radio follows (pa_board_bt_mode_changed); the icon follows the radio. */
                ui->bt_mode = (uint8_t)((ui->bt_mode + 1u) % 3u);
                pa_board_bt_mode_changed(ui->bt_mode);
                pa_ui_mark_settings_dirty(ui);
                ui->dirty = 1;
            } else if (ui->settings_cursor == 2) {
                /* Display sub-page */
                ui->prev_view = PA_UI_VIEW_SETTINGS;
                ui->view = PA_UI_VIEW_DISPLAY;
                ui->sub_cursor = 0;
                ui->edit_mode = 0;
            } else if (ui->settings_cursor == 3) {
                /* Play Mode: PRESET (0) vs STOMP (1) inline toggle */
                ui->play_mode = (ui->play_mode == 1) ? 0 : 1;
                pa_ui_mark_settings_dirty(ui);
            } else if (ui->settings_cursor == 4) {
                /* IMP-09: Fizz Filter OFF -> LOW -> MED -> HIGH */
                ui->fizz_filter_enabled = (uint8_t)((ui->fizz_filter_enabled + 1u) & 3u);
                ui->ai_eq_enabled = ui->fizz_filter_enabled ? 1 : 0;
                pa_fx_chain_set_fizz_filter(chain, ui->fizz_filter_enabled);
                pa_ui_mark_settings_dirty(ui);
            } else if (ui->settings_cursor == 5) {
                /* USB Audio: fixed at 44.1 kHz by USB descriptor (270 B packet) */
            } else if (ui->settings_cursor == 8) {
                /* The output limiter's mode: BRICKWALL -> SOFT -> OFF.
                 * The limiter is the last stage of the chain, so this decides
                 * whether the cab's peaks are limited, softly compressed or
                 * left alone - it could not be A/B-ed at all before. */
                ui->output_limit_mode = (uint8_t)((ui->output_limit_mode + 1u) % 3u);
                pa_fx_chain_set_output_limit_mode(chain, (pa_lmt_mode_t)ui->output_limit_mode);
                pa_ui_mark_settings_dirty(ui);
            } else if (ui->settings_cursor == 9) {
                /* The USB audio loopback: the guitar to the host and the
                 * host's audio back. The stock ships with it ON and there was
                 * no way to turn it off. */
                ui->usb_loopback = ui->usb_loopback ? 0u : 1u;
                pa_ui_mark_settings_dirty(ui);
            } else if (ui->settings_cursor == 10) {
                /* The USB audio mode: AMP -> INTERFACE -> INTERFACE + DIRECT MONITOR -> AMP (pa_settings.h) */
                ui->usb_mode = (uint8_t)((ui->usb_mode + 1u) % 3u);
                if (ui->usb_mode == 0u) {
                    ui->usb_loopback = 0u;
                }
                pa_ui_mark_settings_dirty(ui);
            } else if (ui->settings_cursor == 6) {
                /* Performance / Telemetry sub-page */
                ui->prev_view = PA_UI_VIEW_SETTINGS;
                ui->view = PA_UI_VIEW_TELEMETRY;
            } else if (ui->settings_cursor == 7) {
                /* System sub-page (contains Factory Reset) */
                ui->prev_view = PA_UI_VIEW_SETTINGS;
                ui->view = PA_UI_VIEW_SYS_INFO;
                ui->sys_cursor = 0;
            }
            ui->dirty = 1;
        }
        return;
    }


    /* 10. Input / Output Sub-page
     * IMP-04/IMP-05: same navigation model as DISPLAY - turn = move between rows, click = edit
     * (green outline), turn = change value, click / VOL- = done. Every change is pushed to the
     * DSP immediately and persisted. */
    if (ui->view == PA_UI_VIEW_INPUT_OUTPUT) {
        if (buttons_pressed & (PA_BTN_VOL_MINUS | PA_BTN_PARA_HOLD1)) {
            if (ui->sub_edit_mode) { ui->sub_edit_mode = 0; ui->dirty = 1; return; }
            ui->view = PA_UI_VIEW_SETTINGS;
            ui->dirty = 1;
            return;
        }
        int changed = 0;
        if (!ui->sub_edit_mode) {
            int dd = enc_delta;
            if (buttons_pressed & PA_BTN_LEFT)  dd--;
            if (buttons_pressed & PA_BTN_RIGHT) dd++;
            if (dd != 0) { ui->sub_cursor = menu_wrap(ui->sub_cursor, dd, IO_ROWS); ui->dirty = 1; }      /* wraps at both ends */
            if (buttons_pressed & PA_BTN_PARA_CLICK) {
                if (ui->sub_cursor == 5)      { ui->out_route = (uint8_t)((ui->out_route + 1u) % 5u); changed = 1; }   /* toggles: no edit state */
                else if (ui->sub_cursor == 6) { ui->io_output_type ^= 1; changed = 1; }
                else if (ui->sub_cursor == 7) { ui->io_cab_on_line ^= 1; changed = 1; }
                else                          { ui->sub_edit_mode = 1; }
                ui->dirty = 1;
            }
        } else {
            int d = enc_delta;
            if (buttons_pressed & PA_BTN_LEFT)  d--;
            if (buttons_pressed & PA_BTN_RIGHT) d++;
            if (d != 0) {
                if (ui->sub_cursor == 0) {
                    int tr = (int)ui->input_trim_db + d;
                    if (tr < -12) tr = -12;
                    if (tr > 12)  tr = 12;
                    ui->input_trim_db = (int8_t)tr;
                } else if (ui->sub_cursor == 1) {
                    int gt = (int)ui->gate_thresh_db + d;
                    if (gt < -80) gt = -80;               /* -80 = OFF */
                    if (gt > -30) gt = -30;
                    ui->gate_thresh_db = (int8_t)gt;
                } else if (ui->sub_cursor == 2) {
                    /* the same control as the front-panel MASTER pot */
                    int ol = (int)ui->master_volume + d * 2;
                    if (ol < 0)   ol = 0;
                    if (ol > 100) ol = 100;
                    ui->master_volume = (uint8_t)ol;
                    ui->out_level_pct = (uint8_t)ol;
                } else if (ui->sub_cursor == 3) {
                    int w = (int)ui->stereo_width + d * 5;            /* 0 = MONO .. 100 */
                    if (w < 0)   w = 0;
                    if (w > 100) w = 100;
                    ui->stereo_width = (uint8_t)w;
                } else if (ui->sub_cursor == 4) {
                    int bl = (int)ui->out_balance + d * 2;            /* -50 (left) .. +50 (right) */
                    if (bl < -50) bl = -50;
                    if (bl > 50)  bl = 50;
                    ui->out_balance = (int8_t)bl;
                }
                changed = 1;
                ui->dirty = 1;
            }
            if (buttons_pressed & PA_BTN_PARA_CLICK) { ui->sub_edit_mode = 0; ui->dirty = 1; }
        }
        if (changed) {
            pa_ui_apply_io_settings(ui, chain);
            pa_ui_mark_settings_dirty(ui);
        }
        return;
    }

    /* 11. Display Sub-page */
    if (ui->view == PA_UI_VIEW_DISPLAY) {
        if (buttons_pressed & (PA_BTN_VOL_MINUS | PA_BTN_PARA_HOLD1)) {
            if (ui->edit_mode) {
                ui->edit_mode = 0;
                ui->dirty = 1;
                return;
            }
            ui->view = PA_UI_VIEW_SETTINGS;
            ui->dirty = 1;
            return;
        }

        if (ui->edit_mode == 0) {
            /* Hover mode: PARA knob and < > buttons step between rows 0..4 */
            {
                int dd = enc_delta;
                if (buttons_pressed & PA_BTN_LEFT)  dd--;
                if (buttons_pressed & PA_BTN_RIGHT) dd++;
                if (dd != 0) { ui->sub_cursor = menu_wrap(ui->sub_cursor, dd, 5); ui->dirty = 1; }        /* wraps at both ends */
            }
            if (buttons_pressed & PA_BTN_PARA_CLICK) {
                if (ui->sub_cursor == 0) {
                    ui->edit_mode = 1; /* Enter Edit Mode for Brightness */
                } else if (ui->sub_cursor == 1) {
                    ui->display_auto_dim = (uint8_t)((ui->display_auto_dim + 1u) % 4u);
                } else if (ui->sub_cursor == 2) {
                    ui->home_view_mode ^= 1;
                } else if (ui->sub_cursor == 3) {
                    ui->display_battery_pct ^= 1;
                } else if (ui->sub_cursor == 4) {
                    ui->theme ^= 1;                       /* IMP-06: ONYX <-> HIGH CONTRAST */
                    pa_ui_set_theme(ui->theme);
                }
                if (ui->sub_cursor != 0) pa_ui_mark_settings_dirty(ui);   /* IMP-10 */
                ui->dirty = 1;
            }
        } else {
            /* Edit mode (Brightness): PARA knob and < > adjust value, PARA click exits edit mode */
            if (enc_delta != 0) {
                int b = (int)ui->display_brightness + enc_delta * 5;
                if (b < 10)  b = 10;
                if (b > 100) b = 100;
                ui->display_brightness = (uint8_t)b;
                ui->dirty = 1;
            }
            if (buttons_pressed & PA_BTN_LEFT) {
                int b = (int)ui->display_brightness - 5;
                if (b < 10) b = 10;
                ui->display_brightness = (uint8_t)b;
                ui->dirty = 1;
            }
            if (buttons_pressed & PA_BTN_RIGHT) {
                int b = (int)ui->display_brightness + 5;
                if (b > 100) b = 100;
                ui->display_brightness = (uint8_t)b;
                ui->dirty = 1;
            }
            if (buttons_pressed & PA_BTN_PARA_CLICK) {
                ui->edit_mode = 0; /* Exit Edit Mode */
                ui->dirty = 1;
            }
            if (enc_delta || (buttons_pressed & (PA_BTN_LEFT | PA_BTN_RIGHT))) {
                plat_backlight_set(ui->display_brightness);   /* IMP-06: brightness was UI-only */
                pa_ui_mark_settings_dirty(ui);
            }
        }
        return;
    }

    /* 12. Performance Sub-page */
    if (ui->view == PA_UI_VIEW_TELEMETRY) {
        if (buttons_pressed & (PA_BTN_VOL_MINUS | PA_BTN_PARA_HOLD1)) {
            ui->view = PA_UI_VIEW_SETTINGS;
            ui->dirty = 1;
            return;
        }
        return;
    }

    /* 13. System Sub-page */
    if (ui->view == PA_UI_VIEW_SYS_INFO) {
        if (buttons_pressed & (PA_BTN_VOL_MINUS | PA_BTN_PARA_HOLD1)) {
            ui->view = PA_UI_VIEW_SETTINGS;
            ui->dirty = 1;
            return;
        }
        if (buttons_pressed & PA_BTN_LEFT) {
            ui->sys_cursor = (uint8_t)((ui->sys_cursor + 2u) % 3u);
            ui->dirty = 1;
        }
        if (buttons_pressed & PA_BTN_RIGHT) {
            ui->sys_cursor = (uint8_t)((ui->sys_cursor + 1u) % 3u);
            ui->dirty = 1;
        }
        if (enc_delta != 0) {
            ui->sys_cursor = (uint8_t)((ui->sys_cursor + (enc_delta > 0 ? 1u : 2u)) % 3u);
            ui->dirty = 1;
        }
        if (buttons_pressed & PA_BTN_PARA_CLICK) {
            /* CLEAN USER PRESETS (0), FACTORY RESET (1) or FORGET BT PAIRING (2) clicked -> open the confirmation modal */
            ui->confirm_kind = (ui->sys_cursor == 1) ? 0u : (ui->sys_cursor == 2) ? 2u : 1u;
            ui->view = PA_UI_VIEW_FACTORY_RESET;
            ui->reset_confirm_cursor = 0; /* Default on safe choice CANCEL */
            ui->dirty = 1;
        }
        return;
    }

    /* 15. STOMP assign menu: PARA turn (or < >) picks the effect, a PARA click assigns it to the key, VOL- or HOME goes back */
    if (ui->view == PA_UI_VIEW_STOMP_ASSIGN) {
        int step = enc_delta;
        if (buttons_pressed & PA_BTN_LEFT)  step -= 1;
        if (buttons_pressed & PA_BTN_RIGHT) step += 1;
        if (step != 0) {
            int c = (int)ui->stomp_assign_cursor + step;
            while (c < 0) c += PA_SLOT_COUNT;
            ui->stomp_assign_cursor = (uint8_t)(c % PA_SLOT_COUNT);
            ui->dirty = 1;
        }
        if (buttons_pressed & PA_BTN_PARA_CLICK) {
            ui->stomp_map[ui->stomp_assign_key & 3u] = ui->stomp_assign_cursor;      /* kept with the preset when it is saved */
            ui->view = PA_UI_VIEW_MAIN_GRID;
            ui->dirty = 1;
            return;
        }
        if (buttons_pressed & PA_BTN_VOL_MINUS) {
            ui->view = PA_UI_VIEW_MAIN_GRID;
            ui->dirty = 1;
        }
        return;
    }

    /* 14. Factory Reset Modal */
    if (ui->view == PA_UI_VIEW_FACTORY_RESET) {
        if (buttons_pressed & PA_BTN_VOL_MINUS) {
            ui->view = PA_UI_VIEW_SYS_INFO;
            ui->dirty = 1;
            return;
        }
        if (buttons_pressed & (PA_BTN_LEFT | PA_BTN_RIGHT)) {
            ui->reset_confirm_cursor ^= 1;
            ui->dirty = 1;
        }
        if (enc_delta != 0) {
            ui->reset_confirm_cursor ^= 1;
            ui->dirty = 1;
        }
        if (buttons_pressed & PA_BTN_PARA_CLICK) {
            if (ui->reset_confirm_cursor == 0) {
                /* CANCEL */
                ui->view = PA_UI_VIEW_SYS_INFO;
                ui->dirty = 1;
                return;
            }
            if (ui->confirm_kind == 2u) {                /* FORGET BT PAIRING: the stored bond and the stack's live key go; nothing else is touched */
                const int fr = pa_bond_forget();
                pa_evlog(PA_EV_BLE, PA_EV_BLE_BOND_FORGOTTEN, (uint32_t)(fr == 0 ? 1 : 0xFF));
                ui->save_failed = (fr != 0);
                ui->toast_kind = 3u;
                ui->view = PA_UI_VIEW_SAVED_TOAST;
                ui->saved_toast_timer_ms = 2500;
                ui->dirty = 1;
                return;
            }
            /* RESET / ERASE: one click does it. Factory reset restores stock P01..P40 and keeps P41..P80;
             * clean erases P41..P80 and keeps P01..P40 */
            const uint8_t cleaning = ui->confirm_kind;
            int reset_rc = cleaning ? pa_preset_clean_user() : pa_preset_factory_reset();
            uint8_t keep_view = ui->home_view_mode;      /* don't wipe display preferences */
            pa_ui_init(ui);
            ui->home_view_mode = keep_view;
            pa_ui_load_preset(ui, chain, 0);
            ui->saved_index = 0;
            ui->save_failed = (reset_rc != 0);
            ui->toast_kind = cleaning ? 2u : 1u;         /* "RESET / P01-P40 RESTORED" or "CLEAN / P41-P80 ERASED", not "SAVED P01" */
            ui->view = PA_UI_VIEW_SAVED_TOAST;
            ui->saved_toast_timer_ms = ui->save_failed ? 2000 : 1000;
            ui->dirty = 1;
            return;
        }
        return;
    }
}

/* Master UI Renderer */
typedef struct { pa_ui_ctx_t *ui; const pa_fx_chain_t *chain; const pa_cpu_meter_t *meter; } render_ctx_t;
static void render_frame(pa_ui_ctx_t *ui, const pa_fx_chain_t *chain, const pa_cpu_meter_t *meter);
static void render_frame_cb(void *p) { const render_ctx_t *c = (const render_ctx_t *)p; render_frame(c->ui, c->chain, c->meter); }

/* The device draws a frame in bands (st7735s.c): this whole function runs once per band, clipped to it, so a frame never shows
 * a blank-then-redraw flash and unchanged bands are not sent. The host builds run it once. None of the view renderers
 * changes the UI state, so running it several times is the same as running it once. */
#if (!defined(PA_HOST_BUILD) && !defined(PA_HOST_SIM)) || defined(PA_BAND_HARNESS)   /* PA_BAND_HARNESS: the host check of the banded drawing */
extern void st7735s_present_bands(void (*draw)(void *), void *ctx);
extern int  st7735s_present_step(void (*draw)(void *), void *ctx, uint32_t max_bands);
#else
static void st7735s_present_bands(void (*draw)(void *), void *ctx) { draw(ctx); }
static int  st7735s_present_step(void (*draw)(void *), void *ctx, uint32_t max_bands) { (void)max_bands; draw(ctx); return 1; }
#endif

/* A frame is drawn a few bands per UI tick (not all 20 at once): the tick then stays short even when the audio interrupt leaves the main
 * loop a small share of the CPU, so the buttons are handled every tick instead of after each full redraw. Something that changes the
 * screen while a frame is in progress sets `dirty` again and a new frame starts after this one. */
#ifndef PA_UI_BANDS_PER_TICK
#define PA_UI_BANDS_PER_TICK 4u
#endif
/* ...and when the audio leaves the main loop plenty of CPU (the interrupt's average cost is low) more bands go per tick, so a frame
 * finishes sooner and fast movement shows less lag. At the full load it falls back to PA_UI_BANDS_PER_TICK. */
#if !(defined(PA_EMU_DLL) || defined(PA_HOST_SIM) || defined(__wasm__) || defined(__EMSCRIPTEN__))   /* device builds (audio_alnk.c keeps the stats) */
extern volatile uint32_t g_pa_isr_avg_us;
extern uint32_t plat_ticks24(void);
#endif
/* How long one UI tick may spend drawing, in microseconds, by how much CPU the audio leaves the main loop. */
static uint8_t s_bl_dipped;                        /* 1 while the backlight is dipped for a screen change */
static uint8_t s_view_boost;                       /* 1 while the frame being drawn is a change of screen: every band changes, so it is drawn with a bigger budget */
static uint32_t ui_draw_budget_us(void) {
    uint32_t b = 2500u;
#if !(defined(PA_EMU_DLL) || defined(PA_HOST_SIM) || defined(__wasm__) || defined(__EMSCRIPTEN__))
    const uint32_t a = g_pa_isr_avg_us;
    if (a != 0u) {
        if (a < 700u)       b = 14000u;
        else if (a < 950u)  b = 9000u;
        else if (a < 1150u) b = 5000u;
    }
#endif
    if (s_view_boost && b < 12000u) b = 12000u;        /* a screen change should not be seen being painted band by band */
    return b;
}
#if !(defined(PA_EMU_DLL) || defined(PA_HOST_SIM) || defined(__wasm__) || defined(__EMSCRIPTEN__))
extern void st7735s_present_abort(void);
#endif
static pa_ui_ctx_t *s_rend_ui; static const pa_fx_chain_t *s_rend_chain; static const pa_cpu_meter_t *s_rend_meter;
void pa_ui_render_pass(void) {      /* from the main loop, between UI ticks: a frame in progress goes on (or a new one starts) without waiting for the next tick */
    if (s_rend_ui) pa_ui_render(s_rend_ui, s_rend_chain, s_rend_meter);
}
void pa_ui_render(pa_ui_ctx_t *ui, const pa_fx_chain_t *chain,
                  const pa_cpu_meter_t *meter) {
    static int s_frame_open;
    static uint32_t s_f_t0, s_f_slices;
    static uint8_t s_f_view;
    if (!ui) return;
#if !(defined(PA_EMU_DLL) || defined(PA_HOST_SIM) || defined(__wasm__) || defined(__EMSCRIPTEN__))
    if (s_frame_open && (uint8_t)ui->view != s_f_view) {      /* the screen changed under a frame in progress: its lower bands would show the old screen under the new one */
        st7735s_present_abort(); s_frame_open = 0; ui->dirty = 1;
    }
#endif
    s_rend_ui = ui; s_rend_chain = chain; s_rend_meter = meter;
    if (!s_frame_open) {
        if (!ui->dirty) return;
        ui->dirty = 0;
        s_frame_open = 1;
        { static uint8_t s_prev_view = 0xFFu; s_view_boost = (s_prev_view != (uint8_t)ui->view); s_prev_view = (uint8_t)ui->view; }
#if !(defined(PA_EMU_DLL) || defined(PA_HOST_SIM) || defined(__wasm__) || defined(__EMSCRIPTEN__))
        if (s_view_boost && plat_ms() > 3000u && !s_bl_dipped) {    /* a full-screen write takes ~21 ms and the panel scans in 16.7 ms with no tearing-sync line: the new screen would be seen arriving. Dip the light (not at boot, which fades it in) */
            extern void plat_backlight_duty10(uint8_t d);
            plat_backlight_duty10(1u); s_bl_dipped = 1u;
        }
#endif
        s_f_view = (uint8_t)ui->view;
#if !(defined(PA_EMU_DLL) || defined(PA_HOST_SIM) || defined(__wasm__) || defined(__EMSCRIPTEN__))
        s_f_t0 = plat_ticks24(); s_f_slices = 0u;
        {   static uint8_t s_stats_init;                                         /* SRAM is not cleared at boot */
            if (!s_stats_init) { volatile uint32_t *d = (volatile uint32_t *)(uintptr_t)0x01C7D5A0u; d[0] = d[1] = d[2] = d[3] = 0u; s_stats_init = 1u; }
        }
#endif
    }
    render_ctx_t rc = { ui, chain, meter };
#if !(defined(PA_EMU_DLL) || defined(PA_HOST_SIM) || defined(__wasm__) || defined(__EMSCRIPTEN__))
    {   /* draw band after band until the tick's time budget is spent (or the frame is done): a frame then takes as many ticks as the
         * CPU needs, instead of a fixed count */
        const uint32_t t0 = plat_ticks24(), budget = ui_draw_budget_us() * 24u;
        int done;
        do { done = st7735s_present_step(render_frame_cb, &rc, 1u); s_f_slices++; }
        while (!done && (uint32_t)(plat_ticks24() - t0) < budget);
        if (done) {
            volatile uint32_t *d = (volatile uint32_t *)(uintptr_t)0x01C7D5A0u;     /* frames, last us, longest us, slices in the last */
            const uint32_t us = (plat_ticks24() - s_f_t0) / 24u;
            d[0]++; d[1] = us; if (us > d[2]) d[2] = us; d[3] = s_f_slices;
            {   /* frame latency histogram (SRAM 0x01C7E740 [1..5]): from the frame opening to its last band, <17, <34, <67, <100, 100+ ms */
                volatile uint32_t *U = (volatile uint32_t *)(uintptr_t)0x01C7E740u;
                U[1u + (us < 17000u ? 0u : us < 34000u ? 1u : us < 67000u ? 2u : us < 100000u ? 3u : 4u)]++;
            }
            s_frame_open = 0;
            if (s_bl_dipped) { plat_backlight_set(ui->display_brightness); s_bl_dipped = 0u; }      /* the new screen is complete: the light back to the user's setting */
        }
    }
#else
    if (st7735s_present_step(render_frame_cb, &rc, PA_UI_BANDS_PER_TICK)) s_frame_open = 0;
#endif
}

static void render_frame(pa_ui_ctx_t *ui, const pa_fx_chain_t *chain, const pa_cpu_meter_t *meter) {
    /* 1. Clear screen to Onyx black */
    st7735s_fill_rect(0, 0, DISP_W, DISP_H, ONYX_COL_BG);

    /* 2. Dispatch view renderer */
    switch (ui->view) {
        case PA_UI_VIEW_MAIN_GRID:
        case PA_UI_VIEW_MOVING:
            if (ui->home_view_mode == 1) {
                render_home_list(ui, chain);
            } else {
                render_home_header(ui);
                render_home_grid(ui, chain);
            }
            break;
        case PA_UI_VIEW_PARAM_EDIT:
            render_param_edit(ui, chain);
            break;
        case PA_UI_VIEW_BANK_MODE:
            render_bank_mode(ui, chain);
            break;
        case PA_UI_VIEW_TUNER:
            render_tuner(ui);
            break;
        case PA_UI_VIEW_SAVE_DIALOG:
            render_home_header(ui);
            render_home_grid(ui, chain);
            render_save_dialog(ui);
            break;
        case PA_UI_VIEW_SAVED_TOAST:
            render_saved_toast(ui);           /* IMP-04: full-screen card */
            break;
        case PA_UI_VIEW_DRUMS:
            render_drums(ui);
            break;
        case PA_UI_VIEW_SETTINGS:
            render_settings_menu(ui);
            break;
            break;
        case PA_UI_VIEW_INPUT_OUTPUT:
            render_input_output(ui);
            break;
        case PA_UI_VIEW_DISPLAY:
            render_display_settings(ui);
            break;
        case PA_UI_VIEW_TELEMETRY:
            render_performance(ui, chain, meter);
            break;
        case PA_UI_VIEW_SYS_INFO:
            render_sys_info(ui);
            break;
        case PA_UI_VIEW_FACTORY_RESET:
            render_reset_confirm(ui);
            break;
        case PA_UI_VIEW_STOMP_ASSIGN:
            render_stomp_assign(ui);
            break;
        default:
            render_home_header(ui);
            render_home_grid(ui, chain);
            break;
    }

    /* 3. Render 1-second Volume Popup Overlay if active outside Home */
    if (ui->show_vol_popup && ui->view != PA_UI_VIEW_MAIN_GRID && ui->view != PA_UI_VIEW_DRUMS) {
        render_volume_popup(ui);
    }

    /* 4. The OVERLOAD popup, over everything */
    if (ui->overload_ms) render_overload_popup();
}
