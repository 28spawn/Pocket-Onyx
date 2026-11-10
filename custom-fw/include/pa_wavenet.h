/* SPDX-License-Identifier: GPL-3.0-only */
/* Copyright (C) 2026 The Onyx Authors (@28spawn) */
/*
 * pa_wavenet.h — Pocket Amp Embedded 23-Layer Dilated WaveNet Inference Engine
 *
 * Implements full-fidelity, real-time inference for Neural Amp Modeler (NAM)
 * and ToneHunt A2 captures:
 *   - A2-Full (Channels = 8, 12,146 weights): full-resolution studio tube amp modeling
 *   - A2-Lite (Channels = 3, 1,871 weights): ultra-low-power embedded microcontroller mode
 *
 * Designed for both:
 *   - JieLi AC791N dual 320 MHz pi32v2 bare-metal SoC (fits within 336 KB POOL RAM, zero dynamic alloc)
 *   - Host Emulator DLL (pocket_amp_emu.dll)
 */
#ifndef PA_WAVENET_H
#define PA_WAVENET_H

#include <stdint.h>
#include "pa_config.h"

#define PA_WN_NUM_LAYERS          23
/* The device has 96 KB RAM + 336 KB POOL. An A2-Full (8-channel) model needs
 * weights[12146] = 48,584 B + ring_buf[50,832] = 203,328 B = 247 KB, which by
 * itself overran the POOL region and broke the link (RAM overflowed by 85,668 B).
 * A2-Lite (3-channel) needs 1,871 weights (7,484 B) + a 19,062-float ring
 * (76,248 B) = 83,732 B.
 *
 * A2-Lite is now the ONLY configuration - on the device, in the host emulator
 * AND in the WebAssembly build - so all three reproduce the same audio: the
 * same channel count, the same ring length, the same Q21 weights and the same
 * 512-entry activation LUTs. Previously the emulator was built A2-Full and
 * happily played A2-Full captures that the device refuses to load. */
#define PA_WN_MAX_CHANNELS        3
#define PA_WN_MAX_WEIGHTS         1871
#define PA_WN_LITE_WEIGHTS        1871
#define PA_WN_HEAD_TAPS           16
#define PA_WN_TOTAL_COLS          6354
#define PA_WN_MAX_RING_LEN        (PA_WN_TOTAL_COLS * PA_WN_MAX_CHANNELS) /* 19,062 floats (3ch) = 76,248 B */

/* BLOCK ENGINE: block processing architecture. Layer routine, job worker, gather, and
 * CPU stage. Every layer runs over the whole 64-sample block in two chunks of 32 rows. The dilated taps are gathered into a (rows) x (3 * taps)
 * matrix, the accelerator's conv-mode matvec job (config 0xAA1: the matrix pointer steps by the low 16 bits of the stride word per row, the vector is the
 * weights of one output channel, the result goes out 12 bytes apart) does the multiply-adds, three jobs per chunk. While a chunk's jobs run, the CPU
 * does the other chunk's post-processing (mixin and bias, leaky ReLU, head accumulation, the 1x1 mix, the residual) and gathers the next layer's
 * matrix, so the accelerator (about 12 ns a multiply-add, measured) is the only thing that costs time. Weights Q24, samples Q21, the jobs' shift 24. */
#define PA_WN_BLK_N            64u
#define PA_WN_CHUNK            32u                        /* rows per chunk */
#define PA_WN_MIRROR           32u                        /* the first columns of every ring are stored again after its end: a gather never wraps */
#define PA_WN_MIRROR_D1        48u                        /* the dilation-1 layers hand the accelerator a window of the ring itself: 32 rows + 14 taps */
#define PA_WN_BLK_COLS         8539u                      /* sum over layers of (k - 1) * d + 64 + mirror (32) */
#define PA_WN_BLK_RING_WORDS   (PA_WN_BLK_COLS * 3u)      /* 25,809 words */
#define PA_WN_WQ_WORDS         1960u                      /* 21 layers x 75 + 2 x 156 + the head 49 = 1,936 */
#define PA_WN_BLK_MAXCOLS      47u                        /* 3 * 15 + 2 (x and 1.0) */
#define PA_WN_BLK_HEAD_STEPS   (PA_WN_HEAD_TAPS - 1u + PA_WN_BLK_N)

#ifndef PA_DC_HOLD_SAMPLES
#define PA_DC_HOLD_SAMPLES 448u   /* the output DC blocker only tracks for this many samples after a model reset (7 blocks, 10 ms: the output is held at gain 0 for as long, see PA_XF_WARM in pa_audio_core.c) */
#endif

typedef struct {
    uint8_t  k;           /* Kernel size: 6 or 15 */
    uint16_t d;           /* Dilation: 1..239 */
    uint16_t ring_len;    /* (k - 1) * d + 1 */
    uint16_t col_offset;  /* Column offset in global ring buffer */
} pa_wavenet_layer_spec_t;

typedef enum {
    PA_VOICE_CLEAN     = 0, /* Wideband 20 Hz HPF, 18.5 kHz LPF, uncompressed dynamic chime */
    PA_VOICE_CRUNCH    = 1, /* 30 Hz HPF, 16.0 kHz LPF, dynamic touch-sensitive sag */
    PA_VOICE_HIGHGAIN  = 2, /* 45 Hz tight HPF, 12.5 kHz anti-fizz LPF, tight metal bite */
    PA_VOICE_FUZZ      = 3, /* 35 Hz HPF, 9.5 kHz anti-aliasing LPF, hard saturation */
    PA_VOICE_BASS      = 4, /* 20 Hz sub-bass HPF, 14.0 kHz LPF, deep 30-41 Hz punch */
    PA_VOICE_COUNT     = 5
} pa_voicing_profile_t;

typedef struct {
    uint8_t  channels;    /* 3 (Lite) or 8 (Full) */
    uint8_t  initialized;
    uint8_t  active;
    uint8_t  is_amp_cab;  /* 1 if model includes cabinet */
    uint8_t  voicing_profile; /* pa_voicing_profile_t */
    float    head_scale;
    float    input_trim;         /* Input level staging trim / scalar multiplier (default 1.0f) */
    float    target_loudness_db; /* e.g. -18.0 dBFS unified target */
    float    makeup_gain;        /* calibrated loudness makeup */

    /* Model Weights (up to 12,146 float32 = 48,584 bytes) */
    float    weights[PA_WN_MAX_WEIGHTS];

    /* Causal History Ring Buffer: the float reference engine (19,062 floats) or the block engine (23,409 Q21 words) - never both at once */
    union {
        float   ring_buf[PA_WN_MAX_RING_LEN];
        int32_t ring_q[PA_WN_BLK_RING_WORDS];
    };
    uint16_t ring_pos[PA_WN_NUM_LAYERS];

    /* Block engine state (blk_ok = 1 once the Q24 tables were built and every weight fitted; blk_bad = 1 once the run-time guard gave up on it) */
    uint8_t  blk_ok, blk_bad, blk_slow;
    uint32_t blk_n;                                       /* blocks run: one in 256 is profiled (WN_PROF_MASK, pa_wavenet.c) */
    int32_t  wq[PA_WN_WQ_WORDS];                          /* per layer: 3 conv vectors (3k words), mixin x3, bias x3 (Q24), 1x1 x9 (Q24), 1x1 bias x3 (Q21); then the head */
    float    rech[3];                                     /* the rechannel weights */
    int32_t  head_q[3u * PA_WN_BLK_HEAD_STEPS];           /* head history: 15 old steps, then the block's accumulated activations (3 words a step) */
#if defined(PA_HOST_BUILD) || defined(PA_HOST_SIM)
    int32_t  blk_m[2][PA_WN_CHUNK * PA_WN_BLK_MAXCOLS];   /* the gathered matrix of each chunk, one row a sample (the device keeps them in the low SRAM) */
    int32_t  blk_z[2][3u * PA_WN_CHUNK];                  /* each chunk's job results */
#endif
    int32_t  blk_x[PA_WN_BLK_N], blk_y[PA_WN_BLK_N];
    uint8_t  blk_low;                                     /* 1: the matrices and job results live in the low SRAM (self-tested at load) */
    uint32_t blk_t[12];                                    /* profile of the last block: ticks waiting for the accelerator, jobs, total (TIMER4 ticks) */

    /* 16-tap Head output FIR Ring (16 * 8 = 128 floats = 512 bytes) */
    float    head_ring[PA_WN_HEAD_TAPS * PA_WN_MAX_CHANNELS];
    uint16_t head_pos;

    /* Input 15 Hz DC Blocker, Adaptive Conditioning (HPF + LPF) & Output 5 Hz DC Blocker */
    float    in_dc_x1, in_dc_y1;
    float    hpf_x1, hpf_x2, hpf_y1, hpf_y2;
    float    lpf_x1, lpf_x2, lpf_y1, lpf_y2;
    float    dc_x1, dc_y1;

    /* Real-Time Profiling Telemetry (Timer4 / Cycle Counter) */
    uint32_t peak_sample_cycles;
    uint32_t last_block_cycles;

    /* Runtime metadata */
    char     name[64];

    uint16_t dc_hold;             /* samples left after a reset in which the output DC blocker only tracks (see pa_nam_model_t::dc_hold). Last, so no offset moves */
} pa_wavenet_model_t;

/* Global layer configuration table */
extern const pa_wavenet_layer_spec_t g_pa_wn_specs[PA_WN_NUM_LAYERS];

/* Core WaveNet API */
void  pa_wavenet_init_default(pa_wavenet_model_t *m);
int   pa_wavenet_load_weights(pa_wavenet_model_t *m, const float *w, uint32_t count, float head_scale);
void  pa_wavenet_set_voicing(pa_wavenet_model_t *m, pa_voicing_profile_t profile);
const char *pa_wavenet_get_voicing_name(pa_voicing_profile_t profile);
void  pa_wavenet_set_input_trim(pa_wavenet_model_t *m, float trim);
float pa_wavenet_get_input_trim(const pa_wavenet_model_t *m);

void  pa_wavenet_reset(pa_wavenet_model_t *m);
float pa_wavenet_step_sample(pa_wavenet_model_t *m, float x_in);
void  pa_wavenet_process_block_float(pa_wavenet_model_t *m, float *samples, int nframes);
void  pa_wavenet_process_block_q21(pa_wavenet_model_t *m, int32_t *samples_q21, int nframes);
/* The input conditioning and the output stage on core 0 (see pa_wavenet.c): set_defer(1) while core 0 hands blocks to core 1, pre_core0 before the hand-over
 * (m = NULL when the amp slot runs something else), post_core0 on the block that comes back. */
void  pa_wavenet_set_defer(int on);
void  pa_wavenet_pre_core0(pa_wavenet_model_t *m, const int32_t *buf, int n, float gain);
int   pa_wavenet_pre_valid(void);
void  pa_wavenet_post_core0(pa_wavenet_model_t *m, int32_t *buf, int n);
int   pa_wavenet_block_ready(const pa_wavenet_model_t *m);   /* 1 when the block engine will run (accelerator conv mode, not given up by the guard) */

#endif /* PA_WAVENET_H */
