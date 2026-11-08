/* SPDX-License-Identifier: GPL-3.0-only */
/* Copyright (C) 2026 The Onyx Authors (@28spawn) */
/* pa_fx.h — common effect-node interface & V105 Customizable 6+1 Stereo/Tail Chain Manager.
 * Matches Pocket Amp User Manual (Pages 4, 6, 7–13) and V105 Release Notes. */
#ifndef PA_FX_H
#define PA_FX_H
#include <stdint.h>
#include "pa_cpu_meter.h"

typedef struct fx_node {
    struct fx_node *next;                              /* [node+0]  scheduler linked list */
    uint32_t        active;                            /* [node+4]  enable flag */
    uint32_t        frame;                             /* [node+12] block size */
    void           *state;                             /* heap: delay lines / comb banks / LFO / coeffs */
    const char     *tag;                               /* rodata id */
    void (*run)       (struct fx_node*, int32_t *buf, int n);
    void (*run_stereo)(struct fx_node*, int32_t *buf_L, int32_t *buf_R, int n);
    void (*create)    (struct fx_node*);
    void (*destroy)   (struct fx_node*);
    void (*set_param) (struct fx_node*, int id, int32_t val);
    void (*reset)     (struct fx_node*);
    uint8_t         tail_enabled;                      /* V105 DLY/REV Tail setting */
    uint8_t         tail_draining;                     /* 1 = draining tail after bypass */
    /* IMP-07 / Blk-02: click-free bypass. ISR crossfades wet<->dry over one block. */
    int8_t          fade_dir;                          /* +1 fading in, -1 fading out, 0 idle */
    uint8_t         pending_destroy;                   /* ISR finished fade-out; UI frees state */
    int32_t         wet_q21;                           /* current wet amount 0..1.0 */
    /* Optional stereo (the cabinet and the EQ): the chain calls run_stereo only while it is wanted, and the node stays mono otherwise,
     * bit for bit. 0 = run_stereo (if any) always runs; 1 = runs while stereo_on is set; 2 = runs while a stereo cabinet feeds it. */
    uint8_t         stereo_opt;
    volatile uint8_t stereo_on;
} fx_node_t;

/* The stereo load governor (audio_alnk.c) clears this when the audio interrupt runs too long: optional-stereo nodes then run mono. */
extern volatile uint8_t g_pa_stereo_allow;
/* Set by the cabinet in a block where it produced two different channels, cleared at the top of every chain run. */
extern volatile uint8_t g_pa_cab_stereo;

/* Effect Slot IDs (Manual Page 4 & Page 7: "6+1 effects simultaneously, customizable order"):
 *   - PA_SLOT_DYNAMICS (AI Noise Gate++ / AI Gate MS + Boost / Compress)
 *   - PA_SLOT_DRIVE    (6x V105 MW-* pedals inside FX module)
 *   - PA_SLOT_AMP      (20 Amp Sims: 1-15 Guitar, 16-20 Bass + NAM A2)
 *   - PA_SLOT_EQ       (3-Band / 5-Band Equalizer)
 *   - PA_SLOT_MOD      (7 Modulation types, Stereo)
 *   - PA_SLOT_DELAY    (5 Delay types, Stereo + Delay Tail)
 *   - PA_SLOT_REVERB   (6 Reverb types, Stereo + Reverb Tail)
 *   - PA_SLOT_CAB      (20 IR Cabinets: 1-15 Guitar, 16-20 Bass + 1024/2048-tap User IR)
 */
typedef enum {
    PA_SLOT_DYNAMICS = 0,
    PA_SLOT_DRIVE    = 1,
    PA_SLOT_AMP      = 2,
    PA_SLOT_CAB      = 3,
    PA_SLOT_EQ       = 4,
    PA_SLOT_MOD      = 5,
    PA_SLOT_DELAY    = 6,
    PA_SLOT_REVERB   = 7,
    PA_SLOT_COUNT    = 8
} pa_fx_slot_id_t;

typedef enum {
    PA_SPK_MODE_FRFR          = 0,
    PA_SPK_MODE_AMP_RETURN    = 1,
    PA_SPK_MODE_HEADPHONE_AIR = 2
} pa_speaker_mode_t;

typedef enum {
    PA_LMT_MODE_OFF       = 0,
    PA_LMT_MODE_SOFT      = 1,
    PA_LMT_MODE_BRICKWALL = 2
} pa_lmt_mode_t;

/* The highest parameter id any block uses: 0..5 are the knobs, 10 is the
 * "explicit model type" id (3 for the DRIVE, whose type uses a different id
 * again - see pa_fx_chain_set_slot_type). */
#define PA_FX_PARAM_IDS 11

typedef struct {
    fx_node_t        *head;
    fx_node_t        *slots[PA_SLOT_COUNT];
    uint8_t           slot_order[PA_SLOT_COUNT]; /* Customizable chain order (Manual Page 4 & 6) */
    int32_t           input_gain_q21;            /* V105 Input Gain */
    int32_t           preset_vol_q21;            /* Manual Page 6: Preset Volume (0..100, step 5 via VOL-/VOL+) */
    pa_speaker_mode_t speaker_mode;
    pa_lmt_mode_t     output_limit_mode;
    int32_t           lmt_env_q21;
    int32_t           spk_lp_L, spk_lp_R;
    int32_t           in_dc_x1, in_dc_y1;        /* Pre-model DC blocker (15 Hz) */
    uint8_t           in_dc_primed;              /* 0 until the first input sample: the blocker then starts AT the input level (x1 = it, y1 = 0), not at zero, so the codec's DC offset is not a step at boot */
    uint8_t           fizz_filter_enabled;       /* IMP-09: 0 OFF, 1 LOW (6.5 kHz), 2 MED (5 kHz), 3 HIGH (4 kHz); runs right after AMP */
    /* IMP-05: global input / output settings (Settings > Input / Output) */
    int32_t           global_in_gain_q21;        /* INPUT GAIN, multiplies the per-preset trim */
    int32_t           gate_thresh_q21;           /* NOISE GATE threshold, 0 = off */
    int32_t           gate_env_q21, gate_gain_q21;
    uint32_t          gate_hold;
    uint8_t           force_cab_off;             /* LINE out with CAB ON LINE OUT = OFF */
    int32_t           preset_vol_cur_q21;        /* smoothed preset volume (zipper-free VOL+/-) */
    /* Optional per-stage cycle meter. The chain adds a calibrated per-slot cost
     * (include/pa_dsp_cost.h) so the device, the host emulator and the WASM
     * build all report the same DSP load. */
    pa_cpu_meter_t      *meter;
    int32_t           fizz_s1_L[2], fizz_s2_L[2];/* 4th-order cascaded biquad states (Left) */
    int32_t           fizz_s1_R[2], fizz_s2_R[2];/* 4th-order cascaded biquad states (Right) */
    /* --- the parameter shadow ---------------------------------------------
     *
     * Bypassing a block FREES its state (C7 SRAM reclaim) and enabling it
     * again calls create(), which restores the FACTORY defaults. Nothing
     * re-applied what the user had set, so a block that was switched off and
     * on again came back with the default knobs - and with the default
     * model, because the type is a parameter too (id 3 for the DRIVE, id 10
     * for the DYN / MOD / DLY / REV).
     *
     * The chain therefore remembers every value that was pushed into a block
     * and replays it when the block's state is created again. The mask says
     * which ids have ever been set, so a value of 0 is a real setting and not
     * "never touched". */
    int32_t           slot_param_q21[PA_SLOT_COUNT][PA_FX_PARAM_IDS];
    uint16_t          slot_param_mask[PA_SLOT_COUNT];
} pa_fx_chain_t;

void fx_graph_run(fx_node_t *head, int32_t *buf, int n);
void fx_set_enabled(fx_node_t **head, fx_node_t *node, int on);

void pa_fx_chain_init(pa_fx_chain_t *chain, fx_node_t *amp_node, fx_node_t *cab_node);
void pa_fx_chain_set_slot_enabled(pa_fx_chain_t *chain, pa_fx_slot_id_t slot, int on);          /* crossfaded */
void pa_fx_chain_set_slot_enabled_now(pa_fx_chain_t *chain, pa_fx_slot_id_t slot, int on);      /* only while muted */
void pa_fx_chain_collect_garbage(pa_fx_chain_t *chain);                                           /* UI thread, each tick */
void pa_fx_chain_set_global_input(pa_fx_chain_t *chain, int8_t gain_db, int8_t gate_db);
void pa_fx_chain_set_output_routing(pa_fx_chain_t *chain, uint8_t line_out, uint8_t cab_on_line);
void pa_fx_chain_swap_order(pa_fx_chain_t *chain, uint8_t pos_a, uint8_t pos_b);
void pa_fx_chain_set_param(pa_fx_chain_t *chain, pa_fx_slot_id_t slot, int param_id, int32_t val);
/* Replay the remembered parameters into a block whose state was just
 * created. Called by the enable path after create(). */
void pa_fx_chain_restore_slot_params(pa_fx_chain_t *chain, pa_fx_slot_id_t slot);
void pa_fx_chain_set_input_gain(pa_fx_chain_t *chain, int32_t gain_q21);
void pa_fx_chain_set_preset_vol(pa_fx_chain_t *chain, uint8_t vol_0_to_100);
void pa_fx_chain_set_speaker_mode(pa_fx_chain_t *chain, pa_speaker_mode_t mode);
void pa_fx_chain_set_output_limit_mode(pa_fx_chain_t *chain, pa_lmt_mode_t mode);
void pa_fx_chain_set_fizz_filter(pa_fx_chain_t *chain, uint8_t enabled);
void pa_fx_chain_relink_ordered(pa_fx_chain_t *chain);
void pa_fx_chain_reset_states(pa_fx_chain_t *chain);
void pa_fx_chain_run(pa_fx_chain_t *chain, int32_t *buf, int n);
void pa_fx_chain_run_stereo(pa_fx_chain_t *chain, int32_t *buf_L, int32_t *buf_R, int n);

/* The second-core hook. When a hook is installed (src/sys/smp_cpu1.c does it once core 1 is up), pa_fx_chain_run_stereo gives the
 * block to the hook, which runs the FRONT of the chain (input conditioning and every slot up to and including the AMP: the heavy
 * part) on core 1 and returns the PREVIOUS block's amp output in the same buffers (one block of latency). This core then runs the
 * slots after the amp and the tail. No hook = the plain single-core chain, bit for bit as before. `to` is the first slot_order
 * index that stays on this core; `stereo_split` is reserved (the amp is mono, so it is 0). */
typedef void (*pa_fx_post_handoff_fn)(pa_fx_chain_t *chain, int32_t *buf_L, int32_t *buf_R, int n, int to, int lo);
void pa_fx_chain_set_post_handoff(pa_fx_post_handoff_fn fn);
/* Input conditioning, then slot_order[0..to), with the caller's own dry-copy scratch (two cores never share it). */
void pa_fx_chain_run_front(pa_fx_chain_t *chain, int32_t *buf_L, int32_t *buf_R, int n, int lo, int to, int32_t *dry_L, int32_t *dry_R);
/* How much of the front core 1 may take, in microseconds of its block (default 1300). The slots before the amp that do not fit run on
 * core 0 before the hand-over (a drive pedal in front of the amp would otherwise push core 1 past its block). 1 = the amp alone. */
void pa_fx_chain_set_core1_budget_us(uint32_t us);

fx_node_t *fx_drive_new(void);
fx_node_t *fx_eq_new(void);
fx_node_t *fx_dynamics_new(void);
fx_node_t *fx_delay_new(void);
fx_node_t *fx_reverb_new(void);
fx_node_t *fx_mod_new(void);
fx_node_t *fx_cab_ir_new(void);
void fx_cab_ir_load_q18(fx_node_t *nd, const int32_t *taps_q18, uint32_t num_taps);
void fx_cab_ir_set_stock_trim(fx_node_t *nd, uint8_t stock_idx);       /* IMP-02 */
void fx_cab_ir_set_trim_db_x10(fx_node_t *nd, int16_t db_x10);
/* The stereo cabinet's width, 0 (mono: no extra work, no extra memory) .. 100 (the two channels fully decorrelated). Main context only. */
void fx_cab_ir_set_stereo_width(fx_node_t *nd, uint8_t pct);
void pa_fx_chain_set_stereo_width(pa_fx_chain_t *chain, uint8_t pct);

/* ---- reverb diagnostics ---------------------------------------------------
 * Read-only hooks for custom-fw/test/test_reverb.c. They expose the tank
 * layout and the decay gain of the REAL node, so the regression test checks the
 * shipped arithmetic instead of a second copy of it. Not used by the firmware. */
uint32_t pa_rev_tank_samples(void);
int32_t *pa_rev_tank_base(const fx_node_t *nd);
uint32_t pa_rev_regions(int type, uint32_t *off, uint32_t *len, uint32_t max);
uint32_t pa_rev_state_regions(const fx_node_t *nd, uint32_t *off, uint32_t *len,
                              uint32_t max);
int32_t  pa_rev_decay_gain(const fx_node_t *nd);
/* What the TIME / DECAY knobs mean in real units, for the screen. They use the same
 * law as the DSP's set_param, so the display cannot drift from the sound. */
uint32_t pa_dly_time_ms(int type, int32_t knob_q21);       /* delay TIME -> ms (per type) */
uint32_t pa_eq_freq_hz(int type, int p, int32_t knob_q21);  /* an EQ frequency knob -> Hz, or 0 */
int32_t  pa_eq_gain_tenth_db(int type, int p, int32_t knob_q21); /* an EQ gain knob -> 0.1 dB,
                                                                 * or INT32_MIN */
uint32_t pa_rev_decay_ms(int type, int32_t knob_q21);      /* reverb DECAY -> RT60 in ms */
uint32_t pa_mod_rate_mhz(int32_t knob_q21);                /* mod RATE -> LFO rate in mHz */
enum { PA_DYN_U_NONE = 0, PA_DYN_U_DB, PA_DYN_U_RATIO, PA_DYN_U_MS, PA_DYN_U_OFF };
int32_t  pa_dyn_display(int type, int p, int32_t knob_q21, int *unit); /* a DYN knob -> 0.1 units */
float    pa_dyn_comp_gr_db(const fx_node_t *nd);           /* test hook: compressor reduction, dB */
void     pa_dyn_set_expander_range_db(fx_node_t *nd, float range_db);
float    pa_dyn_get_expander_range_db(const fx_node_t *nd);
void     pa_dyn_set_manual_makeup_db(fx_node_t *nd, float makeup_db, int auto_makeup);
float    pa_dyn_get_makeup_gain_db(const fx_node_t *nd);
int      pa_dyn_is_auto_makeup(const fx_node_t *nd);

/* IMP-13: one entry point for model/type selection (each node uses a different param id) */
void pa_fx_chain_set_slot_type(pa_fx_chain_t *chain, pa_fx_slot_id_t slot, uint8_t type);
uint8_t pa_fx_slot_type_count(pa_fx_slot_id_t slot);   /* DSP-side count, AMP/CAB excluded */

/* Blk-01: AMP voicing / loudness profile follows the selected model (main.c) */
void pa_amp_set_model_profile(uint8_t amp_idx);

/* Blk-02: ISR handshake for click-free state changes (main.c) */
void pa_audio_request_mute(void);
int  pa_audio_is_muted(void);
void pa_audio_release_mute(void);

#endif /* PA_FX_H */
