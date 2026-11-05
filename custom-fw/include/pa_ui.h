/* custom-fw/include/pa_ui.h — Pocket Amp Front Panel, Bank Mode, Save/Swap & Tuner Interface
 *
 * Matches User Manual Pages 5–7:
 *   - 5 Illuminated Buttons: HOME (bit 0), VOL- (bit 1), VOL+ (bit 2), < (bit 3), > (bit 4)
 *   - PARA Rotary Encoder + Push Button: click (bit 5), hold 1s -> "Save to" (bit 6), hold 2s -> "Swap to" (bit 7)
 *   - Simultaneous Combos:
 *       * VOL- + VOL+ -> Enter Tuner Mode (or BANK- when in Bank Mode)
 *       * <    + >    -> Enter BANK Mode (4 presets/group: 01 | 02 | 03 | 04, or BANK+ in Bank Mode)
 *   - Tuner Screen (Page 6):
 *       * 7-dot IN jack input volume real-time display bar
 *       * `Ref 430Hz..450Hz` standard A calibration
 *       * `Volume 0..100` bypass pass-through volume
 */
#ifndef PA_UI_H
#define PA_UI_H

/* The stereo width a unit comes up with when none was saved: the headphone jack is stereo unless the owner turns it off (Settings > Input/Output > WIDTH). */
#define PA_STEREO_WIDTH_DEFAULT 60u

#include <stdint.h>
#include "pa_fx.h"
#include "pa_cpu_meter.h"
#include "pa_preset.h"

/* Physical Button Bitmasks (Manual Page 5) */
/* The mask the UI sees. Every bit is a ONE-TICK PULSE produced by the
 * portable input layer (include/pa_input.h), so a tap is exactly one action
 * and a held button auto-repeats after PA_IN_REPEAT_DELAY_MS. */
#include "pa_input.h"   /* PA_BTN_* / PA_COMBO_* / PA_IN_* */
#include "pa_usb_ble.h" /* pa_midi_ev_t (the MIDI control events) */

typedef enum {
    PA_UI_VIEW_MAIN_GRID    = 0, /* Home screen: 8 slots as 2x4 grid or list view */
    PA_UI_VIEW_MOVING       = 1, /* Moving a block (lifted block on Home grid) */
    PA_UI_VIEW_PARAM_EDIT   = 2, /* Block Parameter Editor (all 8 slots, with 5-band EQ sliders) */
    PA_UI_VIEW_BANK_MODE    = 3, /* Bank Mode (4 presets/bank, 20 banks, VOL-, VOL+, <, >) */
    PA_UI_VIEW_TUNER        = 4, /* Full-screen Chromatic Tuner */
    PA_UI_VIEW_SAVE_DIALOG  = 5, /* Save Preset Dialog (P01-P80 destination picker) */
    PA_UI_VIEW_SAVED_TOAST  = 6, /* Saved confirmation toast */
    PA_UI_VIEW_DRUMS        = 7, /* Groove Station / Metronome */
    PA_UI_VIEW_SETTINGS     = 8, /* Settings Menu (9 rows) */
    PA_UI_VIEW_GLOBAL_EQ    = 9, /* retired: the Global EQ page was removed (the number stays so the others keep theirs) */
    PA_UI_VIEW_INPUT_OUTPUT = 10,/* Settings: Input / Output */
    PA_UI_VIEW_DISPLAY      = 11,/* Settings: Display (Brightness, Dim, Grid/List view, Theme) */
    PA_UI_VIEW_TELEMETRY    = 12,/* Settings: Performance (CPU%, SRAM, per-block load) */
    PA_UI_VIEW_SYS_INFO     = 13,/* Settings: System */
    PA_UI_VIEW_FACTORY_RESET= 14,/* Settings: Factory Reset / Clean User Presets Confirmation Dialog */
    PA_UI_VIEW_STOMP_ASSIGN = 15 /* STOMP mode: pick the effect a key (VOL-, VOL+, <, >) toggles */
} pa_ui_view_t;

#define PA_TUNER_RING 1024u   /* the decimated input ring: 702 samples for a detection plus what arrives while it is computed */

typedef struct {
    uint8_t  active;
    uint8_t  mute_output;      /* 1 = mute guitar output while tuning */
    uint16_t ref_a4_hz;        /* Reference A4 frequency 430..450 Hz (default 440 Hz) */
    uint8_t  bypass_volume;    /* Volume 0..100 in Bypass status (0 = mute) */
    uint8_t  input_level_dots; /* Real-time display bar (0..7 dots) */
    uint8_t  signal_locked;
    uint8_t  note_index;       /* 0..11: C..B */
    int8_t   octave;
    int8_t   cents;            /* -50 .. +50, as measured (a few cents of jitter) */
    int8_t   cents_disp;       /* what the screen shows: smoothed at the screen rate (pa_tuner_ui_tick) */
    int16_t  cents_q4;         /* the smoothing filter's state: cents x 16 */
    uint16_t freq_tenths_hz;
    uint32_t confidence;       /* the detections in a row that agree */
    uint8_t  held;             /* 1 = the signal has ended: the last reading stays on the screen (4 s) so a string can be played once and the peg turned */
    uint16_t hold_blocks;
    uint16_t quiet_blocks;
    /* the YIN detector (src/dsp/tuner.c): the input decimated by 4 into a ring, the difference function, the last five results */
    int32_t  dec_acc;
    uint8_t  dec_n;
    uint16_t ring_w;
    uint16_t det_new;
    uint8_t  det_state;
    uint16_t det_tau;
    uint32_t det_w0;
    int8_t   det_rsh, det_lsh;
    uint8_t  hist_n, hist_i;
    float    hist[5];
    uint32_t det_d[192];
    int16_t  ring[PA_TUNER_RING];
} pa_tuner_state_t;

typedef struct {
    pa_ui_view_t     view;
    pa_ui_view_t     prev_view;           /* For '<' back button and volume popup dismissal */
    uint8_t          preset_index;        /* 0..79 (P01..P80) */
    uint8_t          preset_volume;       /* 0..100 */
    uint8_t          master_volume;       /* 0..100 from SARADC Master Pot knob */
    uint8_t          bank_group;          /* 0..19 (20 banks x 4 presets = 80 presets) */
    uint8_t          save_target;         /* Target preset 0..79 for Save dialog */
    uint8_t          save_btn;            /* 0 = CANCEL, 1 = SAVE */
    uint16_t         saved_toast_timer_ms;/* 1-second auto-dismiss timer */
    uint8_t          save_failed;         /* 1 = last save/reset did not reach flash */
    uint8_t          cursor_slot_idx;     /* 0..7 index along the chain for Home cursor */
    uint8_t          selected_slot;       /* Current slot ID (0..7) */
    uint8_t          selected_param;      /* Parameter index compatibility alias */
    uint8_t          home_view_mode;      /* 0 = GRID, 1 = LIST */
    uint8_t          moving_slot;         /* Slot ID (0..7) being moved */
    uint8_t          moving_orig_idx;     /* Original index along chain (for cancel) */
    int8_t           editor_cursor;       /* -2 = ON/OFF toggle, -1 = Model, 0..N-1 = Param/Band */
    uint8_t          edit_mode;           /* 0 = Hover navigation, 1 = Adjusting selected value */
    uint8_t          sub_cursor;          /* Generic sub-screen cursor (0 = Back '<', 1..N = items) */
    uint8_t          sub_edit_mode;       /* Sub-screen edit state */
    uint8_t          show_vol_popup;      /* 1 = show volume overlay outside home */
    uint16_t         vol_popup_timer_ms;  /* 1-second auto-dismiss timer */
    uint8_t          in_peak_pct;         /* 0..100 live audio in meter */
    uint8_t          out_peak_pct;        /* 0..100 live audio out meter */
    uint8_t          settings_cursor;     /* 0..8 selected row in Settings Menu */
    uint8_t          settings_scroll_top; /* 0..2 scroll window offset */
    int8_t           input_trim_db;       /* -12..+24 dB (default +12 dB calibrated) */
    int8_t           gate_thresh_db;      /* -80..-30 dB (default -60 dB) */
    uint8_t          out_level_pct;       /* 0..100% output master level */
    uint8_t          io_output_type;      /* 0 = PHONES, 1 = LINE */
    uint8_t          io_cab_on_line;      /* 0 = OFF, 1 = ON */
    uint8_t          display_brightness;  /* 0..100% (default 80%) */
    uint8_t          display_auto_dim;    /* 0 = OFF, 1 = 30s, 2 = 1m, 3 = 5m */
    uint8_t          display_battery_pct; /* 0 = OFF, 1 = ON */
    uint8_t          theme;               /* IMP-06: 0 = ONYX, 1 = HIGH CONTRAST */
    uint8_t          dimmed;              /* IMP-06: backlight currently auto-dimmed */
    uint32_t         last_input_ms;       /* IMP-06: last button/encoder activity */
    uint8_t          settings_dirty;      /* IMP-10: persist after 2 s of no changes */
    uint32_t         settings_dirty_ms;
    uint8_t          batt_pct;            /* IMP-12: 0..100 from LiPo curve */
    uint8_t          batt_warn;           /* IMP-12: 0 none, 1 low (<15 %), 2 critical (<5 %) */
    uint8_t          batt_warn_shown;     /* IMP-12: toast already shown for this level */
    uint8_t          stomp_map[4];        /* IMP-07: slots on VOL-, VOL+, <, > in STOMP mode */
    char             preset_name[24];     /* IMP-04: shown in save toast / list header */
    uint8_t          saved_index;         /* IMP-04: preset number shown on the save toast */
    uint16_t         overload_ms;         /* > 0 while the OVERLOAD popup is shown (a few seconds, set by the audio core when the system is overloaded) */
    uint8_t          toast_kind;          /* the full-screen toast: 0 = a preset was saved, 1 = the factory reset was done, 2 = the user presets were cleaned, 3 = the Bluetooth pairing was forgotten */
    int8_t           eq_bands_db[5];      /* Global EQ: 100, 400, 1k, 3k, 8k (-12..+12 dB) */
    uint8_t          eq_low_cut;          /* Low Cut Hz (default 80) */
    uint8_t          eq_high_cut;         /* High Cut kHz (default 12) */
    uint8_t          eq_enabled;          /* 1 = active, 0 = bypassed */
    int8_t           slot_bands_db[5];    /* EQ Block 5-Band tone stack: 100, 400, 1k, 3k, 8k */
    uint8_t          tuner_cursor;        /* 0 = REF, 1 = MUTE */
    uint8_t          sys_cursor;          /* 0 = CLEAN USER PRESETS, 1 = FACTORY RESET, 2 = FORGET BT PAIRING */
    uint8_t          confirm_kind;        /* the confirmation dialog: 0 = factory reset, 1 = clean user presets, 2 = forget the Bluetooth pairing */
    uint8_t          reset_confirm_cursor;/* 0 = CANCEL, 1 = RESET */
    uint16_t         drum_step;           /* engine step, 0..PA_DRUM_MAX_STEPS-1 (48 per bar) */
    uint8_t          slot_type[PA_SLOT_COUNT]; /* Current model / capture index per slot */
    uint8_t          user_nam_loaded;     /* 1 = user NAM A2 capture loaded at index PA_STOCK_AMP_COUNT */
    char             user_nam_name[24];   /* Name for custom user NAM capture */
    uint8_t          dirty;
    uint8_t          bt_state;            /* what the icon shows: 0 = off (gray), 1 = broadcasting (cyan / gray, slow flash), 2 = connected (solid cyan); the device sets it from the radio every tick */
    uint8_t          bt_mode;             /* the setting: 0 = OFF, 1 = BLE, 2 = BLE + A2DP (A2DP is not implemented: it behaves as BLE) */
    uint8_t          usb_connected;
    uint8_t          play_mode;           /* 0 = PRESET Mode, 1 = STOMP Mode */
    uint8_t          usb_sample_rate;
    uint8_t          output_limit_mode;   /* 0 BRICKWALL, 1 SOFT, 2 OFF */
    uint8_t          out_route;           /* 0 STEREO, 1 MONO, 2 LEFT, 3 RIGHT, 4 SWAP */
    int8_t           out_balance;         /* -50..+50 */
    uint8_t          stereo_width;        /* 0..100 %: the stereo width of the output (0 = mono): the cabinet's, or the spread's when there is no stereo cabinet */
    uint8_t          usb_loopback;        /* 0 OFF, 1 ON */
    uint8_t          usb_mode;            /* 0 AMP, 1 INTERFACE (host audio only, dry guitar recorded), 2 INTERFACE + DIRECT MONITOR (pa_settings.h) */     /* 0 = 48.0 kHz, 1 = 44.1 kHz */
    uint16_t         battery_mv;
    uint16_t         battery_adc;         /* the averaged SARADC channel 9 count the percentage comes from (0 = none yet) */
    uint8_t          charging;            /* 1 = USB power present: the battery icon shows the charge bolt */
    uint16_t         drum_bpm;
    uint8_t          drum_playing;
    uint8_t          drum_drums_on;       /* 1 = DRUMS ON, 0 = METRONOME CLICK */
    uint8_t          drum_pattern;
    uint8_t          drum_volume;         /* 0..100% dedicated Groove Station master volume */
    uint8_t          stomp_assign_key;    /* the key being assigned: 0 VOL-, 1 VOL+, 2 <, 3 > */
    uint8_t          stomp_assign_cursor; /* the effect highlighted in the assign menu (slot id 0..7) */
    uint8_t          fizz_filter_enabled; /* IMP-09: 0 OFF, 1 LOW, 2 MED, 3 HIGH (LPF right after AMP) */
    uint8_t          ai_eq_enabled;       /* legacy alias */
    uint16_t         reset_hold_timer_ms; /* unused (the factory reset is confirmed with a click) */
    int32_t          slot_params[PA_SLOT_COUNT][6]; /* Up to 6 Q21 parameters per slot */
    uint32_t         sram_used_bytes;
    uint32_t         cpu_total_permille;
    pa_tuner_state_t tuner;
} pa_ui_ctx_t;

/* Onyx 240x240 RGB565 Palette Tokens (matching tokens.py) */
#define ONYX_COL_BG        0x0000u  /* #000000 Screen background */
#define ONYX_COL_PANEL     0x18C3u  /* #181818 Cards, tiles, selected row */
#define ONYX_COL_PANEL_HI  0x2104u  /* #222222 Pressed / highlighted panel */
/* IMP-06: the three grey tokens come from a runtime palette so THEME = HIGH CONTRAST can lift
 * them for outdoor use. Index 0 LINE, 1 DIM, 2 MUTE. */
extern uint16_t g_onyx_greys[3];
#define ONYX_COL_LINE      (g_onyx_greys[0])  /* #292929 Dividers, tracks, inactive outlines */
#define ONYX_COL_DIM       (g_onyx_greys[1])  /* #525252 Signal-path lines, disabled outlines */
#define ONYX_COL_TEXT      0xEF7Du  /* #efefef Values, names */
#define ONYX_COL_MUTE      (g_onyx_greys[2])  /* #8a8a8a Labels, captions */
#define ONYX_COL_SELECT    0xF628u  /* #f5c542 Cursor / focus + preset number (Yellow) */
#define ONYX_COL_OK        0x4ECCu  /* #4cd964 In tune, connected (Green) */
#define ONYX_COL_WARN      0xFE60u  /* #ffcc00 Amber warning */
#define ONYX_COL_BAD       0xFA27u  /* #ff453a Red warning / clipping / low battery */
#define ONYX_COL_BLUETOOTH 0x2E16u  /* #2fc2b0 Bluetooth teal */

/* Block Identity Colors (tokens.py BLOCK_COLORS) */
#define ONYX_COL_DYN       0x4E2Bu  /* #4cc35a Dynamics (Green) */
#define ONYX_COL_DRIVE     0xF447u  /* #f28a3c Drive (Orange) */
#define ONYX_COL_AMP       0xE207u  /* #e5413b Amp (Red) */
#define ONYX_COL_MOD       0x9BDDu  /* #9a7bff Modulation (Purple) */
#define ONYX_COL_DLY       0x2E16u  /* #2fc2b0 Delay (Teal) */
#define ONYX_COL_REV       0x7F19u  /* #7fe0c8 Reverb (Mint) */
#define ONYX_COL_CAB       0x5B7Eu  /* #5b6cf0 Cabinet (Blue) */
#define ONYX_COL_EQ        0xF356u  /* #f06bb0 EQ (Pink) */

void pa_tuner_init(pa_tuner_state_t *t);
void pa_tuner_process_block(pa_tuner_state_t *t, int32_t *buf, int n);
void pa_tuner_ui_tick(pa_tuner_state_t *t);                   /* once per UI tick: the smoothed needle value */

void pa_ui_init(pa_ui_ctx_t *ui);
/* the editor page's reads (include/pa_sysex.h): the state record, and a page of names; registered by pa_audio_core_bind */
uint32_t pa_ui_state_snapshot(const pa_ui_ctx_t *ui, const pa_fx_chain_t *chain, uint8_t *out, uint32_t cap);
uint32_t pa_ui_name_list(uint8_t kind, uint8_t start, uint8_t *out, uint32_t cap);
void pa_ui_handle_inputs(pa_ui_ctx_t *ui, pa_fx_chain_t *chain,
                         uint32_t buttons_pressed, int32_t enc_delta,
                         const uint16_t adc_pots[4]);
void pa_ui_render_pass(void);       /* continue the frame in progress (main loop, between ticks) */
void pa_ui_render(pa_ui_ctx_t *ui, const pa_fx_chain_t *chain,
                  const pa_cpu_meter_t *meter);

/* Glue used by main.c (IMP-01/02/05/06/10/12) */
struct pa_settings_s;
void    pa_ui_load_preset(pa_ui_ctx_t *ui, pa_fx_chain_t *chain, uint8_t idx);   /* click-free */
void    pa_ui_sync_from_preset(pa_ui_ctx_t *ui, const pa_preset_t *pr);
void    pa_ui_apply_io_settings(pa_ui_ctx_t *ui, pa_fx_chain_t *chain);
void    pa_ui_midi_control(pa_ui_ctx_t *ui, pa_fx_chain_t *chain, const pa_midi_ev_t *e);   /* one MIDI control event (CC / NRPN over USB or BLE, decoded in usb_ble.c) */
void    pa_ui_mark_settings_dirty(pa_ui_ctx_t *ui);
void    pa_ui_set_theme(uint8_t theme);
uint8_t pa_battery_pct_from_mv(uint16_t mv);
uint8_t pa_battery_pct_from_adc(uint16_t adc);
void    pa_ui_drop_user_model(pa_ui_ctx_t *ui, pa_fx_chain_t *chain, uint8_t kind, uint8_t idx);   /* a stored capture / cabinet was deleted: a slot that played it goes back to the first stock model */   /* the stock formula: 0 below 528, 100 above 635, else (adc * 100 - 52646) / 109; 255 = no reading */

#define UI_GLYPH_FLAT '\x01'   /* tuner flat sign (the 5x7 font has no lowercase 'b') */

void ui_draw_char(uint16_t x, uint16_t y, char c, uint16_t fg, uint16_t bg);
void ui_draw_str(uint16_t x, uint16_t y, const char *s, uint16_t fg, uint16_t bg);
void ui_draw_char_scaled(uint16_t x, uint16_t y, char c, uint16_t fg, uint16_t bg, uint8_t scale);
void ui_draw_str_scaled(uint16_t x, uint16_t y, const char *s, uint16_t fg, uint16_t bg, uint8_t scale);

/* ---- the Onyx pixel primitives (the Groove Station redesign) -------------
 *
 * A screen drawn 1:1: the Onyx 5x7 font, boxes with pixel-cut corners,
 * whole-pixel sprites and segmented bars. The rules are the ones in
 * GROOVE_SPEC.md, and the reference renders they were made with are the
 * design kit's. */
#define UI_ALIGN_LEFT   0u
#define UI_ALIGN_CENTER 1u
#define UI_ALIGN_RIGHT  2u

#define UI_SPRITE_CHEV_L 0u
#define UI_SPRITE_CHEV_R 1u
#define UI_SPRITE_PLAY   2u

uint16_t ui_onyx_text_w(const char *s, uint8_t scale);
void     ui_onyx_draw_glyph(uint16_t x, uint16_t y, char c, uint8_t scale, uint16_t fg);
void     ui_onyx_draw_str(uint16_t x, uint16_t y, const char *s, uint8_t scale,
                           uint16_t fg, uint8_t align);
void     ui_onyx_cut_box(uint16_t x0, uint16_t y0, uint16_t x1, uint16_t y1,
                          uint8_t cut, uint16_t fill, uint16_t outline);
void     ui_onyx_draw_sprite(uint16_t x, uint16_t y, uint8_t sprite, uint8_t scale, uint16_t fg);
void     ui_onyx_segments(uint16_t x, uint16_t y, uint8_t n, uint8_t w, uint8_t h,
                           uint8_t gap, uint8_t lit, uint16_t on, uint16_t off);

#endif /* PA_UI_H */

