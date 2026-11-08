/* SPDX-License-Identifier: GPL-3.0-only */
/* Copyright (C) 2026 The Onyx Authors (@28spawn) */
/*
 * pa_model.h — Pocket Amp Exact NAM A2 16-Unit Q21 GRU Model Structure
 *
 * 3,852-byte NAM A2 GRU work structure.
 */
#ifndef PA_MODEL_H
#define PA_MODEL_H

#include <stdint.h>
#include "pa_config.h"

#ifndef PA_NAM_HIDDEN
#define PA_NAM_HIDDEN       16
#endif
#define PA_NAM_INPUT_EXT    17   /* h[0..15] + x_in */
#define PA_NAM_HW_ROWS      9    /* Rows 0..8 computed on 0x01EE0400 */
#define PA_NAM_CPU_ROWS     7    /* Rows 9..15 computed in parallel on pi32v2 CPU */
#define PA_NAM_BLOB_BYTES   3852u /* serialized weights+biases; runtime fields follow */
#ifndef PA_DC_HOLD_SAMPLES
#define PA_DC_HOLD_SAMPLES 448u   /* the output DC blocker only tracks for this many samples after a model reset (7 blocks, 10 ms: the output is held at gain 0 for as long, see PA_XF_WARM in pa_audio_core.c) */
#endif
#ifndef PA_NAM_QSHIFT
#define PA_NAM_QSHIFT       21
#endif
#define PA_NAM_Q_SHIFT      PA_NAM_QSHIFT
#define PA_Q21_ONE          (1 << PA_NAM_QSHIFT)

typedef int32_t q28_t;

/*
 * 3,852-byte NAM A2 GRU runtime struct layout:
 *   +0      : w_r_hw[9][17]   (612 B)
 *   +612    : w_r_cpu[7][17]  (476 B)
 *   +1088   : w_z_hw[9][17]   (612 B)
 *   +1700   : w_z_cpu[7][17]  (476 B)
 *   +2176   : w_n_hw[9][16]   (576 B)
 *   +2752   : w_n_cpu[7][16]  (448 B)
 *   +3200   : w_in[16]        (64 B)
 *   +3264   : w_out[16]       (64 B)
 *   +3328   : r_gate[16]      (64 B)
 *   +3392   : z_gate[16]      (64 B)
 *   +3456   : n_rec[16]       (64 B)
 *   +3520   : h_state[16]     (64 B)
 *   +3584   : x_in_q21        (4 B)   <-- contiguous with h_state[0..15]!
 *   +3588   : pad_3588        (4 B)
 *   +3592   : b_r[16]         (64 B)
 *   +3656   : b_z[16]         (64 B)
 *   +3720   : b_ni[16]        (64 B)
 *   +3784   : b_nh[16]        (64 B)
 *   +3848   : b_out           (4 B)
 */
typedef struct {
    int32_t w_r_hw[PA_NAM_HW_ROWS][PA_NAM_INPUT_EXT];   /* +0    (612 B) */
    int32_t w_r_cpu[PA_NAM_CPU_ROWS][PA_NAM_INPUT_EXT]; /* +612  (476 B) */
    int32_t w_z_hw[PA_NAM_HW_ROWS][PA_NAM_INPUT_EXT];   /* +1088 (612 B) */
    int32_t w_z_cpu[PA_NAM_CPU_ROWS][PA_NAM_INPUT_EXT]; /* +1700 (476 B) */
    int32_t w_n_hw[PA_NAM_HW_ROWS][PA_NAM_HIDDEN];      /* +2176 (576 B) */
    int32_t w_n_cpu[PA_NAM_CPU_ROWS][PA_NAM_HIDDEN];    /* +2752 (448 B) */
    int32_t w_in[PA_NAM_HIDDEN];                        /* +3200 (64 B)  */
    int32_t w_out[PA_NAM_HIDDEN];                       /* +3264 (64 B)  */

    /* Working gate & state buffers (+3328 .. +3587) */
    int32_t r_gate[PA_NAM_HIDDEN];                      /* +3328 (64 B)  */
    int32_t z_gate[PA_NAM_HIDDEN];                      /* +3392 (64 B)  */
    int32_t n_rec[PA_NAM_HIDDEN];                       /* +3456 (64 B)  */
    int32_t h_state[PA_NAM_INPUT_EXT];                  /* +3520 (68 B: [0..15]=h, [16]=x_in_q21 at +3584) */
    int32_t reserved_3588;                              /* +3588 (4 B)   */

    /* Biases (+3592 .. +3851) */
    int32_t b_r[PA_NAM_HIDDEN];                         /* +3592 (64 B)  */
    int32_t b_z[PA_NAM_HIDDEN];                         /* +3656 (64 B)  */
    int32_t b_ni[PA_NAM_HIDDEN];                        /* +3720 (64 B)  */
    int32_t b_nh[PA_NAM_HIDDEN];                        /* +3784 (64 B)  */
    int32_t b_out;                                      /* +3848 (4 B)   */

    /* Runtime metadata */
    int32_t scale_q21;
    uint8_t active;
    int32_t defizz_lp1;
    int32_t defizz_lp2;
    int32_t dc_x1;
    int32_t dc_y1;
    uint16_t dc_hold;      /* samples left after a reset in which the output DC blocker only TRACKS (x1 = the model's output, y1 = 0, the output 0): a blocker that starts at zero passed the model's resting level, 0.2-1.0 of full scale after a reset, as a step that decayed over 50 ms (pack 186). Last, so no offset moves */
} pa_nam_model_t;

void    pa_nam_reset(pa_nam_model_t *m);
void    pa_nam_init_default(pa_nam_model_t *m);
void    pa_nam_prewarm(const pa_nam_model_t *m);
q28_t   pa_nam_step_sample(pa_nam_model_t *m, q28_t x_in_q28);
void    pa_nam_process_block(pa_nam_model_t *m, q28_t *samples, int nframes);

/* Q21 in-place block wrappers used by main.c / fx_graph */
void    nam_model_init(pa_nam_model_t *m);
int32_t nam_step_q21(pa_nam_model_t *m, int32_t x_in_q21);
void    nam_gru_run_block(pa_nam_model_t *m, int32_t *buf_q21, int nframes);

#endif /* PA_MODEL_H */
