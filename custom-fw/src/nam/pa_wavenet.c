/* SPDX-License-Identifier: GPL-3.0-only */
/* Copyright (C) 2026 The Onyx Authors (@28spawn) */
/*
 * pa_wavenet.c — Pocket Amp Embedded 23-Layer Dilated WaveNet Inference Engine
 *
 * Implements full-fidelity, real-time inference for Neural Amp Modeler (NAM)
 * and ToneHunt A2 captures:
 *   - A2-Full (Channels = 8, 12,146 weights): full-resolution studio tube amp modeling
 *   - A2-Lite (Channels = 3, 1,871 weights): embedded microcontroller mode
 *
 * Zero dynamic memory allocations during audio processing.
 */

#include "pa_wavenet.h"
#include "pa_hw_accel.h"
#include <string.h>
#include <math.h>

/* Static 23-layer specifications: kernel sizes, dilations, and column ring lengths */
const pa_wavenet_layer_spec_t g_pa_wn_specs[PA_WN_NUM_LAYERS] = {
    {  6,   1,    6,     0 },
    {  6,   3,   16,     6 },
    {  6,   7,   36,    22 },
    {  6,  17,   86,    58 },
    {  6,  41,  206,   144 },
    {  6, 101,  506,   350 },
    {  6, 239, 1196,   856 },
    {  6,   1,    6,  2052 },
    {  6,   3,   16,  2058 },
    {  6,   7,   36,  2074 },
    {  6,  17,   86,  2110 },
    {  6,  41,  206,  2196 },
    {  6, 101,  506,  2402 },
    {  6, 239, 1196,  2908 },
    { 15,   1,   15,  4104 },
    { 15,  13,  183,  4119 },
    {  6,   1,    6,  4302 },
    {  6,   3,   16,  4308 },
    {  6,   7,   36,  4324 },
    {  6,  17,   86,  4360 },
    {  6,  41,  206,  4446 },
    {  6, 101,  506,  4652 },
    {  6, 239, 1196,  5158 }
};

/* Diagnostic GPIO Profiling Pin: PB0 (gpio 16)
 * When running on AC791N hardware, PB0 is pulled HIGH at the start of
 * pa_wavenet_step_sample() and LOW at exit.
 * Probe PB0 on an oscilloscope / logic analyzer to directly measure execution time (t_high)
 * against the 22.68 us per-sample period or 1.45 ms 64-sample buffer window.
 *
 * It is OFF by default.  The release build must not drive a pin whose job on this board
 * is unknown, and toggling it inside the audio interrupt can corrupt the display's
 * control pins.  Define PA_PROFILE_PIN to turn it on for a profiling build. */
#if !defined(PA_HOST_BUILD) && !defined(PA_HOST_SIM) && defined(PA_PROFILE_PIN)
#define PA_DBG_PIN_INIT()  do { (*(volatile uint32_t *)0x00050048u) &= ~(1u << 0); } while(0)
#define PA_DBG_PIN_HIGH()  do { (*(volatile uint32_t *)0x00050040u) |=  (1u << 0); } while(0)
#define PA_DBG_PIN_LOW()   do { (*(volatile uint32_t *)0x00050040u) &= ~(1u << 0); } while(0)
extern uint32_t plat_ticks24(void);
#else
#define PA_DBG_PIN_INIT()  ((void)0)
#define PA_DBG_PIN_HIGH()  ((void)0)
#define PA_DBG_PIN_LOW()   ((void)0)
#if !defined(PA_HOST_BUILD) && !defined(PA_HOST_SIM)
extern uint32_t plat_ticks24(void);
#else
static inline uint32_t plat_ticks24(void) { return 0; }
#endif
#endif

static void wn_blk_build(pa_wavenet_model_t *m);

void pa_wavenet_reset(pa_wavenet_model_t *m) {
    if (!m) return;
    PA_DBG_PIN_INIT();
    memset(m->ring_q, 0, sizeof(m->ring_q));                 /* the union: covers the float ring too */
    memset(m->ring_pos, 0, sizeof(m->ring_pos));
    memset(m->head_ring, 0, sizeof(m->head_ring));
    memset(m->head_q, 0, sizeof(m->head_q));
    m->head_pos = 0;
    m->in_dc_x1 = m->in_dc_y1 = 0.0f;
    m->hpf_x1 = m->hpf_x2 = m->hpf_y1 = m->hpf_y2 = 0.0f;
    m->lpf_x1 = m->lpf_x2 = m->lpf_y1 = m->lpf_y2 = 0.0f;
    m->dc_x1 = m->dc_y1 = 0.0f; m->dc_hold = PA_DC_HOLD_SAMPLES;
    m->peak_sample_cycles = 0;
    m->last_block_cycles = 0;
}

void pa_wavenet_init_default(pa_wavenet_model_t *m) {
    if (!m) return;
    memset(m, 0, sizeof(pa_wavenet_model_t));
    m->channels   = 3;
    m->head_scale = 1.0f;
    m->input_trim = 1.0f;

    /* Rechannel weights: input mono -> 3 channels */
    m->weights[0] = 0.50f;
    m->weights[1] = 0.35f;
    m->weights[2] = 0.20f;

    uint32_t off = 3;
    for (int l = 0; l < PA_WN_NUM_LAYERS; l++) {
        int k = g_pa_wn_specs[l].k;
        /* Center conv tap */
        for (int c = 0; c < 3; c++) {
            m->weights[off + (c * 3 + c) * k + (k - 1)] = 0.40f;
        }
        off += 3 * 3 * k;
        off += 3; /* conv bias */
        for (int c = 0; c < 3; c++) m->weights[off + c] = 0.15f; /* mixin */
        off += 3;
        for (int c = 0; c < 3; c++) m->weights[off + c * 3 + c] = 0.10f; /* l1x1 */
        off += 3 * 3;
        off += 3; /* l1x1 bias */
    }

    /* Head weights */
    for (int c = 0; c < 3; c++) {
        m->weights[off + c * 16 + 15] = 0.33f;
    }
    off += 3 * 16;
    m->weights[off] = 0.0f; /* head_b */
    off += 1;
    m->weights[off] = 1.0f; /* head_scale */

    pa_wavenet_reset(m);
    m->initialized = 1;
    m->active      = 1;
}

int pa_wavenet_load_weights(pa_wavenet_model_t *m, const float *w, uint32_t count, float head_scale) {
    if (!m || !w) return -1;

    /* A2-Lite only, everywhere, so every build runs the same model with the
     * same quantization. PA_WN_MAX_WEIGHTS == 1871 == PA_WN_LITE_WEIGHTS here,
     * so an A2-Full count MUST be rejected explicitly - otherwise it would
     * alias the A2-Lite branch and tag a 3-channel model as 8-channel (a read
     * past the end of the ring buffer). */
    if (count == PA_WN_LITE_WEIGHTS || count == (PA_WN_LITE_WEIGHTS - 1)) {
        m->channels = 3;
    } else {
        return -2;   /* A2-Full (8ch) does not fit the device RAM budget */
    }

    /* NaN protection: reject weights, head_scale, or input_trim that are not finite.
     * A single bad weight currently makes the output NaN until reboot, and converting
     * NaN to an integer is undefined - in practice that can mean a full-scale burst
     * into headphones. */
    for (uint32_t i = 0; i < count; i++) {
        if (!isfinite(w[i])) return -3;
    }
    if (!isfinite(head_scale)) return -3;
    if (!isfinite(m->input_trim)) m->input_trim = 1.0f;

    memcpy(m->weights, w, count * sizeof(float));

    if (count == PA_WN_MAX_WEIGHTS || count == PA_WN_LITE_WEIGHTS) {
        m->head_scale = m->weights[count - 1];
        if (!isfinite(m->head_scale)) m->head_scale = 1.0f;
    } else {
        m->head_scale = head_scale;
    }
    if (m->input_trim < 0.001f) {
        m->input_trim = 1.0f;
    }

    wn_blk_build(m);
    pa_wavenet_reset(m);
    m->initialized = 1;
    m->active      = 1;
    return 0;
}

void pa_wavenet_set_input_trim(pa_wavenet_model_t *m, float trim) {
    if (!m) return;
    if (trim < 0.01f) trim = 0.01f;
    if (trim > 20.0f) trim = 20.0f;
    m->input_trim = trim;
}

float pa_wavenet_get_input_trim(const pa_wavenet_model_t *m) {
    return (m && m->input_trim > 0.001f) ? m->input_trim : 1.0f;
}

float pa_wavenet_step_sample(pa_wavenet_model_t *m, float x_in) {
    if (!m || !m->active) return x_in;

    const int C = (int)m->channels;
    if (C != 3 && C != 8) return x_in;

    PA_DBG_PIN_HIGH();

    /* Apply calibrated analog input trim scalar multiplier before non-linear core */
    if (m->input_trim > 0.001f) {
        x_in *= m->input_trim;
    }

    /* Clamp extreme input spikes */
    if (x_in > 4.0f)  x_in = 4.0f;
    if (x_in < -4.0f) x_in = -4.0f;

    /* 1. Input Rechannel Conv (1 -> C) */
    float cur[PA_WN_MAX_CHANNELS];
    for (int c = 0; c < C; c++) {
        cur[c] = m->weights[c] * x_in;
    }

    float head_accum[PA_WN_MAX_CHANNELS];
    memset(head_accum, 0, C * sizeof(float));

    uint32_t w_off = (uint32_t)C;

    /* 2. Dilated Convolution Layer Stack (23 layers) */
    for (int l = 0; l < PA_WN_NUM_LAYERS; l++) {
        const pa_wavenet_layer_spec_t *spec = &g_pa_wn_specs[l];
        const int k    = spec->k;
        const int d    = spec->d;
        const int rlen = spec->ring_len;
        const uint32_t roff = (uint32_t)spec->col_offset * (uint32_t)C;

        /* Advance layer ring write index */
        int pos = (int)m->ring_pos[l] + 1;
        if (pos >= rlen) pos = 0;
        m->ring_pos[l] = (uint16_t)pos;

        /* Store current input column into layer history */
        uint32_t base = roff + (uint32_t)pos * (uint32_t)C;
        for (int c = 0; c < C; c++) {
            m->ring_buf[base + c] = cur[c];
        }

        const uint32_t conv_w_off = w_off;
        const uint32_t conv_b_off = conv_w_off + (uint32_t)(C * C * k);
        const uint32_t mix_w_off  = conv_b_off + (uint32_t)C;
        const uint32_t l1x1_w_off = mix_w_off  + (uint32_t)C;
        const uint32_t l1x1_b_off = l1x1_w_off + (uint32_t)(C * C);
        w_off = l1x1_b_off + (uint32_t)C;

        float act[PA_WN_MAX_CHANNELS];
        float nxt[PA_WN_MAX_CHANNELS];

        /* Convolution + Bias + Mixin + LeakyReLU
         * NOTE: Operates in pure 32-bit float without internal dithering.
         * Dither is strictly isolated to the final physical DAC output quantization stage
         * to prevent noise amplification by high-gain non-linear activations. */
        for (int o = 0; o < C; o++) {
            float acc = m->weights[conv_b_off + o];

            for (int ki = 0; ki < k; ki++) {
                int taps_back = (k - 1 - ki) * d;
                int col = pos - taps_back;
                while (col < 0) col += rlen;

                uint32_t rbase = roff + (uint32_t)col * (uint32_t)C;
                const float *hist_col = &m->ring_buf[rbase];

                for (int j = 0; j < C; j++) {
                    uint32_t w_tap = conv_w_off + (uint32_t)((o * C + j) * k + ki);
                    acc += m->weights[w_tap] * hist_col[j];
                }
            }

            /* Conditioning Mixin */
            float z = acc + m->weights[mix_w_off + o] * x_in;

            /* LeakyReLU (0.01 negative slope) */
            float a = (z >= 0.0f) ? z : (0.01f * z);
            act[o] = a;
            head_accum[o] += a;
        }

        /* 1x1 Residual mixing */
        for (int o = 0; o < C; o++) {
            float res = m->weights[l1x1_b_off + o];
            for (int j = 0; j < C; j++) {
                res += m->weights[l1x1_w_off + o * C + j] * act[j];
            }
            nxt[o] = cur[o] + res;
        }

        for (int c = 0; c < C; c++) {
            cur[c] = nxt[c];
        }
    }

    /* 3. Head Output FIR Rechannel (16-tap FIR on accumulated head) */
    const uint32_t head_w_off = w_off;
    const uint32_t head_b_off = head_w_off + (uint32_t)(C * PA_WN_HEAD_TAPS);

    int hpos = (int)m->head_pos + 1;
    if (hpos >= PA_WN_HEAD_TAPS) hpos = 0;
    m->head_pos = (uint16_t)hpos;

    uint32_t hbase = (uint32_t)hpos * (uint32_t)C;
    for (int c = 0; c < C; c++) {
        m->head_ring[hbase + c] = head_accum[c];
    }

    float out = m->weights[head_b_off];
    for (int j = 0; j < C; j++) {
        for (int ki = 0; ki < PA_WN_HEAD_TAPS; ki++) {
            int taps_back = (PA_WN_HEAD_TAPS - 1 - ki);
            int col = hpos - taps_back;
            while (col < 0) col += PA_WN_HEAD_TAPS;

            uint32_t cbase = (uint32_t)col * (uint32_t)C;
            uint32_t w_tap = head_w_off + (uint32_t)(j * PA_WN_HEAD_TAPS + ki);
            out += m->weights[w_tap] * m->head_ring[cbase + j];
        }
    }

    float y = out * m->head_scale;
    /* NaN protection: if output is not finite, output silence and reset model state */
    if (!isfinite(y)) {
        memset(m->ring_buf, 0, sizeof(m->ring_buf));
        memset(m->ring_pos, 0, sizeof(m->ring_pos));
        memset(m->head_ring, 0, sizeof(m->head_ring));
        m->head_pos = 0;
        m->in_dc_x1 = m->in_dc_y1 = 0.0f;
        m->hpf_x1 = m->hpf_x2 = m->hpf_y1 = m->hpf_y2 = 0.0f;
        m->lpf_x1 = m->lpf_x2 = m->lpf_y1 = m->lpf_y2 = 0.0f;
        m->dc_x1 = m->dc_y1 = 0.0f; m->dc_hold = PA_DC_HOLD_SAMPLES;
        m->peak_sample_cycles = 0;
        y = 0.0f;
    }

    /* Output safety clip at +-2.0f */
    if (y > 2.0f)  y = 2.0f;
    if (y < -2.0f) y = -2.0f;

    PA_DBG_PIN_LOW();
    return y;
}

typedef struct {
    float b0, b1, b2, a1, a2;
} biquad_coeffs_t;

/* Exact 2nd-order Butterworth HPF and LPF tables for the 5 Voicing Profiles @ 44.1 kHz */
static const biquad_coeffs_t k_hpf_profiles[PA_VOICE_COUNT] = {
    /* CLEAN: 20 Hz HPF (preserves down to 30 Hz sub-bass chime) */
    { 0.99798712f, -1.99597423f, 0.99798712f, -1.99597018f, 0.99597828f },
    /* CRUNCH: 30 Hz HPF */
    { 0.99698219f, -1.99396439f, 0.99698219f, -1.99395528f, 0.99397349f },
    /* HIGHGAIN: 45 Hz HPF (tightens high-gain palm-mutes, removes flubby mud) */
    { 0.99547671f, -1.99095341f, 0.99547671f, -1.99093295f, 0.99097387f },
    /* FUZZ: 35 Hz HPF */
    { 0.99648011f, -1.99296022f, 0.99648011f, -1.99294783f, 0.99297261f },
    /* BASS: 20 Hz HPF (preserves 30.8 Hz Low B & 41.2 Hz Low E fundamentals) */
    { 0.99798712f, -1.99597423f, 0.99798712f, -1.99597018f, 0.99597828f }
};

static const biquad_coeffs_t k_lpf_profiles[PA_VOICE_COUNT] = {
    /* CLEAN: 18.5 kHz LPF (pristine harmonic chime & pick attack) */
    { 0.69819890f, 1.39639781f, 0.69819890f, 1.30313964f, 0.48965598f },
    /* CRUNCH: 16.0 kHz LPF */
    { 0.53713881f, 1.07427763f, 0.53713881f, 0.84713918f, 0.30141607f },
    /* HIGHGAIN: 16.0 kHz LPF (preserves pick attack and full harmonic bandwidth) */
    { 0.53713881f, 1.07427763f, 0.53713881f, 0.84713918f, 0.30141607f },
    /* FUZZ: 16.0 kHz LPF (preserves fuzz harmonic bite) */
    { 0.53713881f, 1.07427763f, 0.53713881f, 0.84713918f, 0.30141607f },
    /* BASS: 14.0 kHz LPF */
    { 0.42908473f, 0.85816945f, 0.42908473f, 0.50018742f, 0.21615149f }
};

static const char * const k_voice_names[PA_VOICE_COUNT] = {
    "Clean", "Crunch", "High Gain", "Fuzz", "Bass"
};

void pa_wavenet_set_voicing(pa_wavenet_model_t *m, pa_voicing_profile_t profile) {
    if (!m) return;
    if ((int)profile >= PA_VOICE_COUNT) profile = PA_VOICE_CLEAN;
    m->voicing_profile = (uint8_t)profile;
}

const char *pa_wavenet_get_voicing_name(pa_voicing_profile_t profile) {
    if ((int)profile >= PA_VOICE_COUNT) return "Clean";
    return k_voice_names[profile];
}

static inline float wavenet_condition_input(pa_wavenet_model_t *m, float x_raw) {
    /* 0. 1st-order DC-blocking high-pass filter (fc ≈ 15 Hz @ 44.1 kHz, R = 0.9979f):
     *    y[n] = x[n] - x[n-1] + R * y[n-1]
     * Strips static ADC front-end and converter DC offsets before audio enters
     * the non-linear LeakyReLU activation stack, preventing idle offset biasing. */
    float x = x_raw - m->in_dc_x1 + 0.9979f * m->in_dc_y1;
    m->in_dc_x1 = x_raw;
    m->in_dc_y1 = x;

    uint8_t vp = (m->voicing_profile < PA_VOICE_COUNT) ? m->voicing_profile : 0;
    const biquad_coeffs_t *hp_c = &k_hpf_profiles[vp];
    const biquad_coeffs_t *lp_c = &k_lpf_profiles[vp];

    /* 1. Profile-specific HPF */
    float hp = hp_c->b0 * x + hp_c->b1 * m->hpf_x1 + hp_c->b2 * m->hpf_x2
             - hp_c->a1 * m->hpf_y1 - hp_c->a2 * m->hpf_y2;
    m->hpf_x2 = m->hpf_x1; m->hpf_x1 = x;
    m->hpf_y2 = m->hpf_y1; m->hpf_y1 = hp;

    /* 2. Profile-specific LPF */
    float lp = lp_c->b0 * hp + lp_c->b1 * m->lpf_x1 + lp_c->b2 * m->lpf_x2
             - lp_c->a1 * m->lpf_y1 - lp_c->a2 * m->lpf_y2;
    m->lpf_x2 = m->lpf_x1; m->lpf_x1 = hp;
    m->lpf_y2 = m->lpf_y1; m->lpf_y1 = lp;

    return lp;
}

static inline float pa_fast_tanhf(float x) {
    if (x >= 3.0f)  return 1.0f;
    if (x <= -3.0f) return -1.0f;
    float x2 = x * x;
    return x * (27.0f + x2) / (27.0f + 9.0f * x2);
}

static inline float wavenet_dc_block_output(pa_wavenet_model_t *m, float y_raw) {
    /* 1-pole 5 Hz DC blocker: removes persistent layer bias */
    float y = m->dc_hold ? (m->dc_hold--, 0.0f) : y_raw - m->dc_x1 + 0.9993f * m->dc_y1;      /* tracking right after a reset: no step */
    m->dc_x1 = y_raw;
    m->dc_y1 = y;
    return y;
}


/* ====================================================================== block engine, stock style */

#define Q21F 2097152.0f
/* One block in 256 is profiled (it was one in 32). The profile's timer reads make that block about 100 us longer on core 1, which has under 100 us to spare with this engine:
 * core 0 then waits for it at the next hand-over, inside the audio interrupt. That is a hiccup of the measuring, not of the amp, and it now comes every 370 ms instead of every
 * 46 ms; the readout (tools/jl_ram_read.py) still gets a fresh profile about three times a second. */
#define WN_PROF_MASK 255u
static int32_t s_wn_hb;                                 /* the head bias, for the core 0 output stage (see wn_block_run) */
#ifndef PA_WN_LOWMEM
#define PA_WN_LOWMEM 1                                    /* the matrices and job results in the low SRAM (see wn_lowmem_selftest) */
#endif

/* the 12-byte (3 channel) copy of the stock's gather: one load-multiple and one store-multiple */
#if !defined(PA_HOST_BUILD) && !defined(PA_HOST_SIM)
#define WN_COPY12(p, d) __asm__ volatile("{r6-r4} = [%0++]\n\t[%1++] = {r6-r4}" : "+r"(p), "+r"(d) :: "r4", "r5", "r6", "memory")
/* two taps with independent register triples: the second load can issue while the first is in flight */
#define WN_COPY12X2(p, q, d) __asm__ volatile("{r6-r4} = [%0++]\n\t{r9-r7} = [%1++]\n\t[%2++] = {r6-r4}\n\t[%2++] = {r9-r7}" \
                                              : "+r"(p), "+r"(q), "+r"(d) :: "r4", "r5", "r6", "r7", "r8", "r9", "memory")
#else
#define WN_COPY12(p, d) do { (d)[0] = (p)[0]; (d)[1] = (p)[1]; (d)[2] = (p)[2]; (p) += 3; (d) += 3; } while (0)
#define WN_COPY12X2(p, q, d) do { WN_COPY12(p, d); WN_COPY12(q, d); } while (0)
#endif

/* wq layout: per layer l, 9k + 21 words: 3 conv vectors of 3k + 2 words (taps oldest..newest x 3 channels, then the mixin weight and the conv bias, Q24: they meet the
 * matrix row's x and 1.0 columns, as in the stock), 1x1 weights x9 (Q24, column-major: input j, then output o), 1x1 bias x3 (64-bit, Q45, low word first). After the layers: the head, 48 words in the order the head job reads its window (tap oldest..newest x 3
 * channels, Q24), then the head bias (Q21). blk_ok stays 0 when a weight does not fit (the float engine then keeps running on the host). */
#if !defined(PA_HOST_BUILD) && !defined(PA_HOST_SIM)
static int wn_lowmem_selftest(void);
#endif
static int32_t wn_blk_q(float v, int *bad, int frac) {
    const float x = v * (float)(1 << frac);
    if (!(x > -2147483000.0f && x < 2147483000.0f)) { *bad = 1; return 0; }
    return (int32_t)lrintf(x);
}

static int wn_asm_selftest(void);
static uint8_t s_wn_asm;                                  /* 1: the assembly CPU stage passed its self-test and is used */

static void wn_blk_build(pa_wavenet_model_t *m) {
    int bad = 0;
    uint32_t w = 3u, o = 0u;
    m->blk_ok = 0; m->blk_bad = 0; m->blk_slow = 0;
    for (int c = 0; c < 3; c++) m->rech[c] = m->weights[c];
    for (int l = 0; l < PA_WN_NUM_LAYERS; l++) {
        const int k = g_pa_wn_specs[l].k;
        const uint32_t conv_w = w, conv_b = conv_w + (uint32_t)(9 * k), mix_w = conv_b + 3u, l1_w = mix_w + 3u, l1_b = l1_w + 9u;
        w = l1_b + 3u;
        for (int oc = 0; oc < 3; oc++) {                                   /* the stock's matrix has 3k + 2 columns: the taps, then x (the mixin) and 1.0 (the bias) */
            for (int ki = 0; ki < k; ki++)
                for (int j = 0; j < 3; j++) m->wq[o++] = wn_blk_q(m->weights[conv_w + (uint32_t)((oc * 3 + j) * k + ki)], &bad, 24);
            m->wq[o++] = wn_blk_q(m->weights[mix_w + (uint32_t)oc], &bad, 24);
            m->wq[o++] = wn_blk_q(m->weights[conv_b + (uint32_t)oc], &bad, 24);
        }
        for (int j = 0; j < 3; j++)                                        /* column-major: one input's three weights are adjacent (the CPU stage walks them with one pointer) */
            for (int oc = 0; oc < 3; oc++) m->wq[o++] = wn_blk_q(m->weights[l1_w + (uint32_t)(oc * 3 + j)], &bad, 24);
        for (int oc = 0; oc < 3; oc++) {                                   /* the 1x1 bias as the start value of a 64-bit accumulator: Q21 << 24, low word first */
            const int64_t b64 = (int64_t)wn_blk_q(m->weights[l1_b + (uint32_t)oc], &bad, 21) * 16777216LL;
            m->wq[o++] = (int32_t)(uint32_t)(uint64_t)b64;
            m->wq[o++] = (int32_t)(b64 >> 32);
        }
    }
    const uint32_t head_w = w, head_b = head_w + 3u * PA_WN_HEAD_TAPS;
    for (int ki = 0; ki < PA_WN_HEAD_TAPS; ki++)
        for (int j = 0; j < 3; j++) m->wq[o++] = wn_blk_q(m->weights[head_w + (uint32_t)(j * PA_WN_HEAD_TAPS + ki)], &bad, 24);
    m->wq[o++] = wn_blk_q(m->weights[head_b], &bad, 21);
    if (!bad && o <= PA_WN_WQ_WORDS) m->blk_ok = 1;
    m->blk_low = 0;
#if !defined(PA_HOST_BUILD) && !defined(PA_HOST_SIM)
    if (m->blk_ok) m->blk_low = (uint8_t)wn_lowmem_selftest();
    if (!m->blk_low) m->blk_ok = 0;                                        /* the device keeps no other place for the matrices */
    s_wn_asm = (uint8_t)wn_asm_selftest();
#endif
}

/* PLACEMENT EXPERIMENT. The CPU phases did not get faster with a better gather or a leaner driver, which smells like the CPU and the accelerator
 * sharing one SRAM bus (the accelerator reads two words and does one multiply-add every 4.3 cycles). The two chunk matrices and the job results are the
 * data the accelerator streams: put them in the low SRAM (0x01C02000.., the RAMTEXT region, unused since .ram_text is empty), away from the rings and
 * the residual stream the CPU reads, and each matrix in a different 8 KB. Self-tested at load: one job with every buffer there must come out exact. */
#if !defined(PA_HOST_BUILD) && !defined(PA_HOST_SIM)
#define WN_LOW_MA ((int32_t *)0x01C02000u)
#define WN_LOW_MB ((int32_t *)0x01C04000u)
#define WN_LOW_ZA ((int32_t *)0x01C05800u)
#define WN_LOW_ZB ((int32_t *)0x01C05A00u)
static int wn_lowmem_selftest(void) {
    int32_t *A = WN_LOW_MA, *B = WN_LOW_MB, *D = WN_LOW_ZA;
    for (int i = 0; i < 12; i++) A[i] = (i + 1) * 65536;
    for (int j = 0; j < 3; j++) B[j] = (j + 1) << 20;                  /* Q24 */
    for (int i = 0; i < 8; i++) D[i] = 0x7FEEDBAC;
    pa_hw_vec_cmd_t c;
    c.config = PA_HW_CFG_MATVEC; c.src_a = A; c.src_b = B; c.dst = D;
    c.ctrl = (24u << 24) | (4u << 12) | 3u; c.stride = (4u << 20) | 12u;
    pa_hw_accel_submit_lean(&c);
    pa_hw_accel_wait_lean();
    if (pa_hw_accel_broken()) return 0;
    for (int r = 0; r < 4; r++) {
        int64_t acc = 0;
        for (int j = 0; j < 3; j++) acc += (int64_t)A[r * 3 + j] * B[j];
        if (D[r] != (int32_t)((acc + (1LL << 23)) >> 24)) return 0;
    }
    return D[4] == 0x7FEEDBAC;                                         /* and it wrote nothing past its rows */
}
#endif

int pa_wavenet_block_ready(const pa_wavenet_model_t *m) { return m && m->blk_ok && !m->blk_bad && !pa_hw_accel_broken(); }

/* One accelerator job, asynchronous: pa_hw_accel_submit() first waits for the job before it, so the time it takes is the time the CPU stalled on the accelerator. */
static inline void wn_submit(uint32_t *stall, int prof, const int32_t *mat, uint32_t step_bytes, const int32_t *vec, int32_t *dst, uint32_t dst_step_bytes,
                             uint32_t rows, uint32_t cols) {
    /* bits 23:20 of the stride = the output step in bytes (4 or 12), bits 15:0 = the matrix row step in bytes */
    const uint32_t ctrl = (24u << 24) | (rows << 12) | cols, stride = (dst_step_bytes << 20) | step_bytes;
    if (prof) { const uint32_t t0 = plat_ticks24(); pa_hw_accel_job_lean(PA_HW_CFG_MATVEC, mat, vec, dst, ctrl, stride); *stall += plat_ticks24() - t0; }
    else      pa_hw_accel_job_lean(PA_HW_CFG_MATVEC, mat, vec, dst, ctrl, stride);
}

/* Ring geometry. A layer's ring holds R = (k - 1) d + 64 columns of 3 words, then a copy of its first 32 columns, so a run of rows never wraps. */
static inline uint32_t wn_R(int l) { return ((uint32_t)g_pa_wn_specs[l].k - 1u) * g_pa_wn_specs[l].d + PA_WN_BLK_N; }
static inline uint32_t wn_MIR(int l) { (void)l; return PA_WN_MIRROR; }
#define WN_ONE_Q21 0x200000                                       /* 1.0 in Q21: the matrix's bias column */

static inline void wn_put(int32_t *ring, uint32_t R, uint32_t mir, uint32_t c, int32_t v0, int32_t v1, int32_t v2) {
    int32_t *q = &ring[3u * c];
    q[0] = v0; q[1] = v1; q[2] = v2;
    if (c < mir) { int32_t *q2 = &ring[3u * (R + c)]; q2[0] = v0; q2[1] = v1; q2[2] = v2; }
}

/* The gather of one chunk's rows of one layer: the matrix rows from the ring, which the post stage of the layer before wrote. Every layer is gathered, as in the stock
 * (dilation 1 included): a row is the k taps (12 bytes each), then x, the block's input sample, and 1.0, so the accelerator also does the mixin and the bias. */
typedef struct {
    const int32_t *ring;
    int32_t *M;
    const int32_t *xp;                                              /* the input sample of the next row */
    uint32_t R, k, cols;
    uint32_t row;                                                   /* next matrix row */
    uint32_t s[15];                                                 /* ring column of the next row's tap ki (oldest .. newest) */
} wn_g_t;

static void wn_g_init(wn_g_t *g, pa_wavenet_model_t *m, int l, uint32_t col_base, uint32_t first_row, int32_t *M, const int32_t *xp) {
    const uint32_t k = g_pa_wn_specs[l].k, d = g_pa_wn_specs[l].d;
    g->k = k; g->cols = 3u * k + 2u; g->R = wn_R(l);
    g->ring = &m->ring_q[3u * col_base]; g->M = M; g->row = 0u; g->xp = xp;
    uint32_t c = (uint32_t)m->ring_pos[l] + 1u + first_row;           /* the column of the first row of this chunk */
    if (c >= g->R) c -= g->R;
    for (uint32_t ki = 0; ki < k; ki++) {
        uint32_t sc = c + g->R - (k - 1u - ki) * d;                  /* c < R and back < R: one subtraction is enough */
        if (sc >= g->R) sc -= g->R;
        g->s[ki] = sc;
    }
}

static inline void wn_gather6(int32_t *d, const int32_t *ring, const uint32_t *s, uint32_t rows, const int32_t *xp) {
    const int32_t *p0 = ring + 3u * s[0], *p1 = ring + 3u * s[1], *p2 = ring + 3u * s[2], *p3 = ring + 3u * s[3], *p4 = ring + 3u * s[4], *p5 = ring + 3u * s[5];
    for (uint32_t r = 0; r < rows; r++) {
        WN_COPY12X2(p0, p1, d); WN_COPY12X2(p2, p3, d); WN_COPY12X2(p4, p5, d);
        d[0] = xp[r]; d[1] = WN_ONE_Q21; d += 2;
    }
}

static void wn_gather_gen(int32_t *M, uint32_t cols, uint32_t k, const int32_t *ring, const uint32_t *s, uint32_t rows, const int32_t *xp) {
    uint32_t ki = 0;
    for (; ki + 1u < k; ki += 2u) {                                 /* two taps at a time: they are adjacent in a matrix row */
        const int32_t *p = ring + 3u * s[ki], *q = ring + 3u * s[ki + 1u];
        int32_t *d = M + 3u * ki;
        for (uint32_t r = 0; r < rows; r++) { WN_COPY12X2(p, q, d); d += cols - 6u; }
    }
    if (ki < k) {
        const int32_t *p = ring + 3u * s[ki];
        int32_t *d = M + 3u * ki;
        for (uint32_t r = 0; r < rows; r++) { WN_COPY12(p, d); d += cols - 3u; }
    }
    int32_t *d = M + 3u * k;
    for (uint32_t r = 0; r < rows; r++) { d[0] = xp[r]; d[1] = WN_ONE_Q21; d += cols; }
}

static void wn_g_run(wn_g_t *g, uint32_t rows) {
    int32_t *dst = g->M + g->row * g->cols;
    if (g->k == 6u) wn_gather6(dst, g->ring, g->s, rows, g->xp);
    else            wn_gather_gen(dst, g->cols, g->k, g->ring, g->s, rows, g->xp);
    for (uint32_t ki = 0; ki < g->k; ki++) { uint32_t sc = g->s[ki] + rows; if (sc >= g->R) sc -= g->R; g->s[ki] = sc; }
    g->row += rows; g->xp += rows;
}

/* The CPU stage's view of the rings: it reads the residual stream (the layer's own input) from the layer's ring and writes the next layer's input straight into
 * the next layer's ring (and its mirror), so no separate residual array and no copy into the ring are left. dst is NULL after the last layer. */
typedef struct {
    const int32_t *src; uint32_t Rs, cs;
    int32_t *dst; uint32_t Rd, cd, mird;
} wn_pr_t;

static void wn_pr_init(wn_pr_t *p, pa_wavenet_model_t *m, int l, const uint32_t *colb, uint32_t first_row) {
    p->Rs = wn_R(l); p->src = &m->ring_q[3u * colb[l]];
    uint32_t c = (uint32_t)m->ring_pos[l] + 1u + first_row;
    if (c >= p->Rs) c -= p->Rs;
    p->cs = c;
    if (l + 1 < PA_WN_NUM_LAYERS) {
        p->Rd = wn_R(l + 1); p->dst = &m->ring_q[3u * colb[l + 1]]; p->mird = wn_MIR(l + 1);
        c = (uint32_t)m->ring_pos[l + 1] + 1u + first_row;
        if (c >= p->Rd) c -= p->Rd;
        p->cd = c;
    } else { p->dst = 0; p->Rd = 0u; p->cd = 0u; p->mird = 0u; }
}

/* THE CPU STAGE (wn_asm_mix / wn_asm_head in pa_wavenet_asm.c), per row: a = leaky ReLU of the job's result z (which already holds the conv, the mixin and the bias),
 * h = ssat(h + a), then the 1x1 mix of the three activations on three 64-bit accumulators started at the 1x1 bias, >> 24, plus the residual, saturating, written straight
 * into the next layer's ring. The C versions below define the arithmetic; on the device the assembly runs when its self-test against them passed. */
#define WN_KLEAKY 42949632                                          /* 0.01 * 2^32 (0x028F5C00) */

static inline int32_t wn_sadd(int32_t a, int32_t b) {
    const int64_t s = (int64_t)a + (int64_t)b;
    return s > 2147483647LL ? 2147483647 : (s < -2147483648LL ? (int32_t)0x80000000 : (int32_t)s);
}

static void wn_post_head_c(const int32_t *z, int32_t *h, uint32_t rows) {
    for (uint32_t r = 0; r < rows; r++)
        for (uint32_t o = 0; o < 3u; o++) {
            int32_t a = z[3u * r + o];
            if (a < 0) a = (int32_t)(((int64_t)a * WN_KLEAKY) >> 32);
            h[3u * r + o] = wn_sadd(h[3u * r + o], a);
        }
}

static void wn_post_mix_c(const int32_t *z, int32_t *h, const int32_t *w, const int32_t *b64, const int32_t *src, int32_t *dst, uint32_t rows) {
    for (uint32_t r = 0; r < rows; r++) {
        int32_t a[3];
        for (uint32_t o = 0; o < 3u; o++) {
            a[o] = z[3u * r + o];
            if (a[o] < 0) a[o] = (int32_t)(((int64_t)a[o] * WN_KLEAKY) >> 32);
            h[3u * r + o] = wn_sadd(h[3u * r + o], a[o]);
        }
        for (uint32_t o = 0; o < 3u; o++) {
            int64_t acc = (int64_t)(((uint64_t)(uint32_t)b64[2u * o + 1u] << 32) | (uint32_t)b64[2u * o]);
            for (uint32_t j = 0; j < 3u; j++) acc += (int64_t)w[3u * j + o] * a[j];
            dst[3u * r + o] = wn_sadd((int32_t)(acc >> 24), src[3u * r + o]);
        }
    }
}

#if !defined(PA_HOST_BUILD) && !defined(PA_HOST_SIM)
/* weak: the assembly kernels (pa_wavenet_asm.c) exist only in the device build; where they are absent the C version runs */
extern void wn_asm_mix(const int32_t *z, int32_t *h, const int32_t *w, const int32_t *b64, const int32_t *src, int32_t *dst, uint32_t rows) __attribute__((weak));
extern void wn_asm_head(const int32_t *z, int32_t *h, uint32_t rows) __attribute__((weak));
/* Reached through pointers: a direct call to an undefined weak symbol cannot be linked (the call instruction is relative), a pointer to one is simply NULL. */
typedef void (*wn_mix_fn_t)(const int32_t *, int32_t *, const int32_t *, const int32_t *, const int32_t *, int32_t *, uint32_t);
typedef void (*wn_head_fn_t)(const int32_t *, int32_t *, uint32_t);
/* The empty asm hides the value from the optimizer: seeing a constant address it would turn the call back into a direct (unlinkable) one. */
static inline wn_mix_fn_t  wn_fn_mix(void)  { wn_mix_fn_t f = wn_asm_mix;   __asm__ volatile("" : "+r"(f)); return f; }
static inline wn_head_fn_t wn_fn_head(void) { wn_head_fn_t f = wn_asm_head; __asm__ volatile("" : "+r"(f)); return f; }
static inline void wn_post_mix(const int32_t *z, int32_t *h, const int32_t *w, const int32_t *b64, const int32_t *src, int32_t *dst, uint32_t rows) {
    if (s_wn_asm) wn_fn_mix()(z, h, w, b64, src, dst, rows); else wn_post_mix_c(z, h, w, b64, src, dst, rows);
}
static inline void wn_post_head(const int32_t *z, int32_t *h, uint32_t rows) {
    if (s_wn_asm) wn_fn_head()(z, h, rows); else wn_post_head_c(z, h, rows);
}

/* Both CPU stages on the same pseudo-random rows (negative and positive, two head sums at the saturation edge): the assembly must agree word for word. */
static int wn_asm_selftest(void) {
    if (!wn_fn_mix() || !wn_fn_head()) return 0;                       /* not assembled in: the C version runs */
    enum { N = 11 };
    int32_t z[3 * N], h1[3 * N], h2[3 * N], src[3 * N + 3], d1[3 * N], d2[3 * N], w[9], b[6];
    uint32_t x = 0x2545F491u;
#define RND() (x = x * 1664525u + 1013904223u, (int32_t)(x >> 8) - (1 << 23))
    for (int i = 0; i < 3 * N; i++) { z[i] = RND() * 2; h1[i] = h2[i] = RND() * 64; src[i] = RND() * 32; d1[i] = d2[i] = 0x5A5A5A5A; }
    h1[0] = h2[0] = 2147483000; z[0] = 1 << 22;
    h1[4] = h2[4] = -2147483000; z[4] = -(1 << 23);
    for (int i = 0; i < 9; i++) w[i] = RND() * 2;
    for (int o = 0; o < 3; o++) { const int64_t v = (int64_t)(RND() * 8) * 16777216LL; b[2 * o] = (int32_t)(uint32_t)(uint64_t)v; b[2 * o + 1] = (int32_t)(v >> 32); }
#undef RND
    wn_post_mix_c(z, h1, w, b, src, d1, N);
    wn_fn_mix()(z, h2, w, b, src, d2, N);
    for (int i = 0; i < 3 * N; i++) if (h1[i] != h2[i] || d1[i] != d2[i]) return 0;
    int32_t g1[3 * N], g2[3 * N];
    for (int i = 0; i < 3 * N; i++) g1[i] = g2[i] = h1[i];
    wn_post_head_c(z, g1, N);
    wn_fn_head()(z, g2, N);
    for (int i = 0; i < 3 * N; i++) if (g1[i] != g2[i]) return 0;
    return 1;
}
#else
#define wn_post_mix wn_post_mix_c
#define wn_post_head wn_post_head_c
#endif

/* The CPU stage of one layer for rows ra..rb-1 of a chunk: runs of rows up to the end of the next layer's ring, the mirror columns copied after each run. */
static void wn_P(const int32_t *lw, const int32_t *zc, uint32_t r0c, int32_t *hacc, wn_pr_t *pr, uint32_t ra, uint32_t rb) {
    const uint32_t rows = rb - ra;
    const int32_t *z = &zc[3u * (ra - r0c)];
    int32_t *h = &hacc[3u * ra];
    if (!pr->dst) { wn_post_head(z, h, rows); return; }                 /* the last layer: only the activation and the head sum */
    const int32_t *sp = pr->src + 3u * pr->cs;                          /* linear: the mirror makes a read past the end of the ring valid (rows <= 11 here) */
    uint32_t cd = pr->cd, done = 0;
    const uint32_t Rd = pr->Rd, mird = pr->mird;
    while (done < rows) {
        uint32_t run = Rd - cd;                                         /* rows before the destination ring wraps */
        if (run > rows - done) run = rows - done;
        int32_t *dp = pr->dst + 3u * cd;
        wn_post_mix(z + 3u * done, h + 3u * done, lw, lw + 9, sp + 3u * done, dp, run);
        if (cd < mird) {                                                /* the rows whose column is mirrored: copy them to the end of the ring */
            uint32_t mc = mird - cd;
            if (mc > run) mc = run;
            const int32_t *q = dp;
            int32_t *mp = dp + 3u * Rd;
            for (uint32_t i = 0; i < mc; i++) WN_COPY12(q, mp);
        }
        done += run; cd += run;
        if (cd >= Rd) cd = 0;
    }
    pr->cd = cd;
    uint32_t cs = pr->cs + rows;
    if (cs >= pr->Rs) cs -= pr->Rs;
    pr->cs = cs;
}

#define WN_TIMED2(acc, acc2, sel, call) do { if (prof) { tq = plat_ticks24(); call; const uint32_t dq_ = plat_ticks24() - tq; (acc) += dq_; if (sel) (acc2) += dq_; } else { call; } } while (0)
#define WN_TIMED(acc, call) do { if (prof) { tq = plat_ticks24(); call; (acc) += plat_ticks24() - tq; } else { call; } } while (0)

/* One block of n <= 64 samples through the 23 layers, software-pipelined like the stock:
 *   phase A of layer l: the accelerator runs chunk A's three conv jobs; the CPU post-processes chunk B of layer l - 1 (writing layer l's ring) and gathers chunk B of layer l
 *   phase B of layer l: the accelerator runs chunk B's three jobs;       the CPU post-processes chunk A of layer l (writing layer l + 1's ring) and gathers chunk A of layer l + 1
 * The CPU's share of each phase is cut in thirds, one third behind each job. A dilation-1 layer needs no gather: its jobs read a window of the ring.
 * x_in_f: the conditioned, trimmed, clamped input; y_raw: the head output before the head scale and the post filters. */
static void wn_block_run(pa_wavenet_model_t *m, const float *x_in_f, float *y_raw, uint32_t n) {
    const uint32_t t0 = plat_ticks24();
    uint32_t stall = 0, jobs = 0, t_p = 0, t_g = 0, tq, t_g1 = 0, t_p1 = 0;
    const int prof = ((m->blk_n++ & WN_PROF_MASK) == 0u);            /* the timer reads cost about 100 us a block: profile one block in WN_PROF_MASK + 1 */
    int32_t *xq = m->blk_x, *H = m->head_q, *hacc = H + 3u * (PA_WN_HEAD_TAPS - 1u), *yq = m->blk_y;
#if !defined(PA_HOST_BUILD) && !defined(PA_HOST_SIM)
    int32_t *MA = WN_LOW_MA, *MB = WN_LOW_MB, *zA = WN_LOW_ZA, *zB = WN_LOW_ZB;          /* the matrices and the job results live in the low SRAM (blk_low) */
#else
    int32_t *MA = m->blk_m[0], *MB = m->blk_m[1], *zA = m->blk_z[0], *zB = m->blk_z[1];
#endif
    const int32_t *wq = m->wq;
    const uint32_t nA = n > PA_WN_CHUNK ? PA_WN_CHUNK : n, nB = n - nA;
    const uint32_t thA[4] = { 0u, nA / 3u, (2u * nA) / 3u, nA }, thB[4] = { nA, nA + nB / 3u, nA + (2u * nB) / 3u, n };

    static uint32_t colb[PA_WN_NUM_LAYERS], wo[PA_WN_NUM_LAYERS], whw, lay_ok;      /* the layout is a constant of the spec table: computed once */
    if (!lay_ok) {
        uint32_t cb = 0u, w = 0u;
        for (int l = 0; l < PA_WN_NUM_LAYERS; l++) {
            colb[l] = cb; cb += wn_R(l) + wn_MIR(l);
            wo[l] = w; w += 9u * g_pa_wn_specs[l].k + 21u;
        }
        whw = w;                                                    /* the head's offset: the rechannel weights are in m->rech, not in wq */
        lay_ok = 1u;
    }
    const int32_t *whead = &wq[whw];

    {   /* layer 0's input, straight into its ring */
        const uint32_t R0 = wn_R(0), mir0 = wn_MIR(0);
        int32_t *ring0 = &m->ring_q[3u * colb[0]];
        uint32_t c = (uint32_t)m->ring_pos[0] + 1u;
        if (c >= R0) c -= R0;
        for (uint32_t t = 0; t < n; t++) {
            const float x = x_in_f[t];
            xq[t] = (int32_t)(x * Q21F);
            wn_put(ring0, R0, mir0, c, (int32_t)(m->rech[0] * x * Q21F), (int32_t)(m->rech[1] * x * Q21F), (int32_t)(m->rech[2] * x * Q21F));
            if (++c >= R0) c = 0;
            hacc[3u * t] = hacc[3u * t + 1u] = hacc[3u * t + 2u] = 0;
        }
    }

    wn_g_t gA, gB;
    wn_pr_t prA, prB;
    wn_g_init(&gA, m, 0, colb[0], 0u, MA, xq);
    for (uint32_t i = 0; i < 3u; i++) WN_TIMED(t_g, wn_g_run(&gA, thA[i + 1u] - thA[i]));           /* the one serial gather: layer 0, chunk A */

    for (int l = 0; l < PA_WN_NUM_LAYERS; l++) {
        const uint32_t k = g_pa_wn_specs[l].k, cols = 3u * k + 2u, step = cols * 4u;
        const int32_t *L = &wq[wo[l]], *lw = L + 3u * cols;
        const uint32_t kp = g_pa_wn_specs[l > 0 ? l - 1 : 0].k;
        const int32_t *lwp = &wq[wo[l > 0 ? l - 1 : 0]] + 3u * (3u * kp + 2u);
        /* phase A: chunk A's three jobs; the CPU post-processes chunk B of the layer before and gathers chunk B of this one */
        if (nB) {
            wn_g_init(&gB, m, l, colb[l], nA, MB, xq + nA);
            if (l > 0) wn_pr_init(&prB, m, l - 1, colb, nA);
        }
        for (uint32_t i = 0; i < 3u; i++) {
            wn_submit(&stall, prof, MA, step, L + i * cols, zA + i, 12u, nA, cols); jobs++;
            if (nB) {
                if (l > 0) WN_TIMED(t_p, wn_P(lwp, zB, nA, hacc, &prB, thB[i], thB[i + 1u]));
                WN_TIMED2(t_g, t_g1, l == 1, wn_g_run(&gB, thB[i + 1u] - thB[i]));
            }
        }
        /* phase B: chunk B's three jobs; the CPU post-processes chunk A of this layer and gathers chunk A of the next */
        wn_pr_init(&prA, m, l, colb, 0u);
        if (l + 1 < PA_WN_NUM_LAYERS) wn_g_init(&gA, m, l + 1, colb[l + 1], 0u, MA, xq);
        if (nB) {
            for (uint32_t i = 0; i < 3u; i++) {
                wn_submit(&stall, prof, MB, step, L + i * cols, zB + i, 12u, nB, cols); jobs++;
                WN_TIMED2(t_p, t_p1, l == 1, wn_P(lw, zA, 0u, hacc, &prA, thA[i], thA[i + 1u]));
                if (l + 1 < PA_WN_NUM_LAYERS) WN_TIMED(t_g, wn_g_run(&gA, thA[i + 1u] - thA[i]));
            }
        } else {                                                       /* a block of at most 32 samples: nothing to overlap */
            pa_hw_accel_wait_lean();
            for (uint32_t i = 0; i < 3u; i++) {
                WN_TIMED(t_p, wn_P(lw, zA, 0u, hacc, &prA, thA[i], thA[i + 1u]));
                if (l + 1 < PA_WN_NUM_LAYERS) WN_TIMED(t_g, wn_g_run(&gA, thA[i + 1u] - thA[i]));
            }
        }
    }
    /* the head: a sliding 48-word window over the accumulated activations, in two jobs; chunk B's last layer is post-processed behind the first */
    wn_submit(&stall, prof, H, 12u, whead, yq, 4u, nA, 3u * PA_WN_HEAD_TAPS); jobs++;
    if (nB) {
        const int32_t *lwl = &wq[wo[PA_WN_NUM_LAYERS - 1]];                  /* (the last layer has no 1x1: its stage only activates and sums) */
        wn_pr_init(&prB, m, PA_WN_NUM_LAYERS - 1, colb, nA);
        for (uint32_t i = 0; i < 3u; i++) WN_TIMED(t_p, wn_P(lwl, zB, nA, hacc, &prB, thB[i], thB[i + 1u]));
        wn_submit(&stall, prof, H + 3u * nA, 12u, whead, yq + nA, 4u, nB, 3u * PA_WN_HEAD_TAPS); jobs++;
    }
    /* Behind the last job (its submit waited for the one before): chunk A's output samples to float, and the carry of the newest 15 head steps. The carry reads
     * rows the last layer's stage wrote and writes words the head jobs have read already. */
    if (!nB) pa_hw_accel_wait_lean();                               /* a block of at most 32 samples: the only head job is the last one */
    const int32_t hb = whead[3u * PA_WN_HEAD_TAPS];
    s_wn_hb = hb;
    if (y_raw) for (uint32_t t = 0; t < nA; t++) y_raw[t] = (float)(yq[t] + hb) * (1.0f / Q21F);
    for (uint32_t i = 0; i < 3u * (PA_WN_HEAD_TAPS - 1u); i++) H[i] = H[3u * n + i];          /* carry the newest 15 steps */
    for (int l = 0; l < PA_WN_NUM_LAYERS; l++) {
        uint32_t p = (uint32_t)m->ring_pos[l] + n;                  /* n <= 64 <= R: one subtraction */
        const uint32_t R = wn_R(l);
        if (p >= R) p -= R;
        m->ring_pos[l] = (uint16_t)p;
    }
    pa_hw_accel_wait_lean();
    if (y_raw) for (uint32_t t = nA; t < n; t++) y_raw[t] = (float)(yq[t] + hb) * (1.0f / Q21F);
    if (prof) { m->blk_t[0] = stall; m->blk_t[2] = t_p; m->blk_t[3] = t_g; m->blk_t[6] = plat_ticks24() - t0; m->blk_t[7] = t_g1; m->blk_t[9] = t_p1; }
    else      m->blk_t[5] = plat_ticks24() - t0;
    m->blk_t[1] = jobs;
}

void pa_wavenet_process_block_float(pa_wavenet_model_t *m, float *samples, int nframes) {
    if (!m || !samples) return;
    uint32_t t_start = plat_ticks24();
    float makeup = (m->makeup_gain > 0.05f) ? m->makeup_gain : (m->is_amp_cab ? 1.40f : 1.00f);

    for (int i = 0; i < nframes; i++) {
        float x_clean = wavenet_condition_input(m, samples[i]);
        float y_raw = pa_wavenet_step_sample(m, x_clean);
        float y_f = wavenet_dc_block_output(m, y_raw);
        float y_out = y_f * makeup;
        if (m->voicing_profile == PA_VOICE_CLEAN) {
            /* High-headroom transparent clean limiter */
            if (y_out > 1.40f)  y_out = 1.40f + 0.10f * pa_fast_tanhf((y_out - 1.40f) * 2.0f);
            if (y_out < -1.40f) y_out = -1.40f + 0.10f * pa_fast_tanhf((y_out + 1.40f) * 2.0f);
        } else {
            /* Warm tube-sag soft saturation */
            if (y_out > 1.20f)  y_out = 1.20f + 0.15f * pa_fast_tanhf((y_out - 1.20f) * 2.0f);
            if (y_out < -1.20f) y_out = -1.20f + 0.15f * pa_fast_tanhf((y_out + 1.20f) * 2.0f);
        }
        /* NaN protection: if output is not finite, output silence and reset model state */
        if (!isfinite(y_out)) {
            memset(m->ring_buf, 0, sizeof(m->ring_buf));
            memset(m->ring_pos, 0, sizeof(m->ring_pos));
            memset(m->head_ring, 0, sizeof(m->head_ring));
            m->head_pos = 0;
            m->in_dc_x1 = m->in_dc_y1 = 0.0f;
            m->hpf_x1 = m->hpf_x2 = m->hpf_y1 = m->hpf_y2 = 0.0f;
            m->lpf_x1 = m->lpf_x2 = m->lpf_y1 = m->lpf_y2 = 0.0f;
            m->dc_x1 = m->dc_y1 = 0.0f; m->dc_hold = PA_DC_HOLD_SAMPLES;
            m->peak_sample_cycles = 0;
            y_out = 0.0f;
        }
        samples[i] = y_out;
    }

    uint32_t dt_ticks = plat_ticks24() - t_start;
    if (dt_ticks > 0 && nframes > 0) {
        uint32_t block_cyc = dt_ticks * 15u;
        m->last_block_cycles = block_cyc;
        uint32_t sample_cyc = block_cyc / (uint32_t)nframes;
        if (sample_cyc > m->peak_sample_cycles) m->peak_sample_cycles = sample_cyc;
    }
}

#if !defined(PA_HOST_BUILD) && !defined(PA_HOST_SIM)
static uint32_t wn_dg_nan;
static float    wn_dg_out, wn_dg_in;
#endif

/* The input conditioning for a whole block, the state and the profile's coefficients in locals (the per-sample version reloaded and stored a dozen state floats
 * and looked up the profile for every sample). Same arithmetic, same order. */
static void wn_condition_block(pa_wavenet_model_t *m, const int32_t *in_q21, float *xb, uint32_t n, float trim) {
    const float inv_q21 = 1.0f / (float)(1 << PA_NAM_QSHIFT);
    const uint8_t vp = (m->voicing_profile < PA_VOICE_COUNT) ? m->voicing_profile : 0;
    const biquad_coeffs_t h = k_hpf_profiles[vp], l = k_lpf_profiles[vp];
    float ix1 = m->in_dc_x1, iy1 = m->in_dc_y1;
    float hx1 = m->hpf_x1, hx2 = m->hpf_x2, hy1 = m->hpf_y1, hy2 = m->hpf_y2;
    float lx1 = m->lpf_x1, lx2 = m->lpf_x2, ly1 = m->lpf_y1, ly2 = m->lpf_y2;
    for (uint32_t i = 0; i < n; i++) {
        const float xr = (float)in_q21[i] * inv_q21;
        const float x = xr - ix1 + 0.9979f * iy1;
        ix1 = xr; iy1 = x;
        const float hp = h.b0 * x + h.b1 * hx1 + h.b2 * hx2 - h.a1 * hy1 - h.a2 * hy2;
        hx2 = hx1; hx1 = x; hy2 = hy1; hy1 = hp;
        const float lp = l.b0 * hp + l.b1 * lx1 + l.b2 * lx2 - l.a1 * ly1 - l.a2 * ly2;
        lx2 = lx1; lx1 = hp; ly2 = ly1; ly1 = lp;
        float xo = lp * trim;
        if (xo > 4.0f) xo = 4.0f;
        if (xo < -4.0f) xo = -4.0f;
        xb[i] = xo;
    }
    m->in_dc_x1 = ix1; m->in_dc_y1 = iy1;
    m->hpf_x1 = hx1; m->hpf_x2 = hx2; m->hpf_y1 = hy1; m->hpf_y2 = hy2;
    m->lpf_x1 = lx1; m->lpf_x2 = lx2; m->lpf_y1 = ly1; m->lpf_y2 = ly2;
}

/* The output side for a whole block: head scale and clip, the 5 Hz DC blocker, makeup, the soft clip and the conversion back to Q21. The block engine cannot make a NaN
 * (it is integer arithmetic), so the NaN check is a comparison; the per-sample diagnostics maxima became per-block. */
static void wn_post_block(pa_wavenet_model_t *m, const float *yb, int32_t *out_q21, uint32_t n, float makeup, float *peak_out, float *peak_raw) {
    const float f_q21 = (float)(1 << PA_NAM_QSHIFT);
    const float hs = m->head_scale;
    const int clean = (m->voicing_profile == PA_VOICE_CLEAN);
    float dx1 = m->dc_x1, dy1 = m->dc_y1, po = 0.0f, pr = 0.0f;
    uint32_t hold = m->dc_hold;
    for (uint32_t i = 0; i < n; i++) {
        float y_raw = yb[i] * hs;
        if (y_raw > 2.0f) y_raw = 2.0f;
        if (y_raw < -2.0f) y_raw = -2.0f;
        const float y_f = hold ? (hold--, 0.0f) : y_raw - dx1 + 0.9993f * dy1;                  /* tracking right after a reset: no step */
        dx1 = y_raw; dy1 = y_f;
        float y_out = y_f * makeup;
        if (clean) {
            if (y_out > 1.40f)  y_out = 1.40f + 0.10f * pa_fast_tanhf((y_out - 1.40f) * 2.0f);
            if (y_out < -1.40f) y_out = -1.40f + 0.10f * pa_fast_tanhf((y_out + 1.40f) * 2.0f);
        } else {
            if (y_out > 1.20f)  y_out = 1.20f + 0.15f * pa_fast_tanhf((y_out - 1.20f) * 2.0f);
            if (y_out < -1.20f) y_out = -1.20f + 0.15f * pa_fast_tanhf((y_out + 1.20f) * 2.0f);
        }
        if (y_out != y_out) y_out = 0.0f;                              /* NaN: silence this sample */
        float scaled = y_out * f_q21;
        if (scaled < -2147483520.0f) scaled = -2147483520.0f;
        if (scaled >  2147483520.0f) scaled =  2147483520.0f;
        out_q21[i] = (int32_t)scaled;
        const float ay = y_out < 0.0f ? -y_out : y_out, ar = y_raw < 0.0f ? -y_raw : y_raw;
        if (ay > po) po = ay;
        if (ar > pr) pr = ar;
    }
    m->dc_x1 = dx1; m->dc_y1 = dy1; m->dc_hold = (uint16_t)hold;
    *peak_out = po; *peak_raw = pr;
}

/* THE CONDITIONING AND THE OUTPUT STAGE ON CORE 0. They cost core 1 about 40 us of the 1451 us a block (software floats), which the block engine can not spare;
 * core 0 (351 us a block on average) can. Core 0 conditions the input of the block it is about to hand over (pa_wavenet_pre_core0), core 1 runs the engine on it and
 * leaves the head output as floats (s_wn_yb), core 0 finishes the block that comes back (pa_wavenet_post_core0). The model's filter states are then only touched by core 0.
 * When core 0 does not defer (core 1 off, a block over 64 samples, the GRU) everything runs inside pa_wavenet_process_block_q21 as before. */
#if !defined(PA_HOST_BUILD) && !defined(PA_HOST_SIM)
#define s_wn_xb ((float *)0x01C05C00u)          /* the low SRAM past the job buffers (the RAM image has no room for 256 bytes more): one block, read at the start of the job */
#else
static float s_wn_xb[PA_WN_BLK_N];
#endif
static volatile uint8_t s_wn_pre_valid, s_wn_defer_ok, s_wn_post_pending;
static float s_wn_post_makeup;


void pa_wavenet_set_defer(int on) { s_wn_defer_ok = on ? 1u : 0u; if (!on) { s_wn_pre_valid = 0u; s_wn_post_pending = 0u; } }

/* core 0, before the hand-over: m = the WaveNet the amp slot will run, or NULL when it will run something else */
int pa_wavenet_pre_valid(void) { return s_wn_pre_valid; }

/* gain: the amp slot's pre-amp gain (linear), which core 1 would have applied to the samples first: the filters are linear, so it joins the trim. */
void pa_wavenet_pre_core0(pa_wavenet_model_t *m, const int32_t *buf, int n, float gain) {
    if (!s_wn_defer_ok || !m || !m->active || n <= 0 || n > (int)PA_WN_BLK_N || !pa_wavenet_block_ready(m)) { s_wn_pre_valid = 0u; return; }
    wn_condition_block(m, buf, s_wn_xb, (uint32_t)n, ((m->input_trim > 0.001f) ? m->input_trim : 1.0f) * gain);
    s_wn_pre_valid = 1u;
}

/* core 0, after the hand-over: the output stage of the block that came back, if core 1 left it for us */
void pa_wavenet_post_core0(pa_wavenet_model_t *m, int32_t *buf, int n) {
    if (!s_wn_post_pending) return;
    s_wn_post_pending = 0u;
    if (!m) return;
    float yb[PA_WN_BLK_N], po, pr;
    for (int t = 0; t < n; t++) yb[t] = (float)(m->blk_y[t] + s_wn_hb) * (1.0f / Q21F);          /* the head output, as the engine left it in integers */
    wn_post_block(m, yb, buf, (uint32_t)n, s_wn_post_makeup, &po, &pr);
#if !defined(PA_HOST_BUILD) && !defined(PA_HOST_SIM)
    if (po > wn_dg_out) wn_dg_out = po;
    if (pr > wn_dg_in) wn_dg_in = pr;
#endif
}

/* 1 while the block engine runs the amp: the debug level scans on core 1 (fx_graph.c) then run on one block in 8 only. */
__attribute__((weak)) volatile uint8_t g_pa_wn_lean;
/* Set by the amp slot (core 1) while the slot is fading in or out: the output stage then runs inside the engine call, so the wet/dry crossfade that follows sees the finished wet signal. */
volatile uint8_t g_pa_wn_inline;

void pa_wavenet_process_block_q21(pa_wavenet_model_t *m, int32_t *samples_q21, int nframes) {
    if (!m || !samples_q21) return;
    uint32_t t_start = plat_ticks24();
    const float inv_q21 = 1.0f / (float)(1 << PA_NAM_QSHIFT);
    const float f_q21   = (float)(1 << PA_NAM_QSHIFT);
    float makeup = (m->makeup_gain > 0.05f) ? m->makeup_gain : (m->is_amp_cab ? 1.40f : 1.00f);

    /* The block engine (accelerator conv mode) when it is ready; the per-sample float engine otherwise (accelerator marked broken, a weight that
     * does not fit Q24, or a block longer than 64 samples). */
    float yb_local[PA_WN_BLK_N];
    const int use_blk = (nframes > 0 && nframes <= (int)PA_WN_BLK_N && pa_wavenet_block_ready(m));
    const int defer = use_blk && s_wn_defer_ok && !g_pa_wn_inline;   /* core 0 finishes the block (not while the slot fades: the crossfade needs the finished wet signal) */
    const int pre = use_blk && s_wn_pre_valid;                        /* core 0 has conditioned its input */
    float *yb = defer ? (float *)0 : yb_local;                         /* deferred: core 0 converts the integers itself */
    g_pa_wn_lean = (uint8_t)use_blk;
    if (use_blk) {
        float xb_local[PA_WN_BLK_N];
        const float *xb = xb_local;
        if (pre) xb = s_wn_xb;
        else wn_condition_block(m, samples_q21, xb_local, (uint32_t)nframes, (m->input_trim > 0.001f) ? m->input_trim : 1.0f);
        wn_block_run(m, xb, yb, (uint32_t)nframes);
        /* The run-time guard: core 1 has to finish a block (the whole job: conditioning, engine, output) in 1.451 ms on average, or core 0 waits a little longer for it
         * every block and the hang detector (6 ms) switches core 1 off. A job over 1.458 ms (35,000 ticks) counts up, one under 1.40 ms (33,600) counts down; at 16
         * this engine is given up (blk_bad): the amp slot then runs the GRU loaded before and core 1 keeps its deadline. An engine that averages under budget never
         * trips it. */
        /* A profiled block is NOT judged: its timer reads add about 100 us, so it is always over 1.458 ms. The test here used to be "blk_t[5] != 0", which is true from the
         * second block on, so every profiled block was judged and counted as a slow one. An engine whose normal blocks sit in the neutral band (1.40 - 1.458 ms: neither up
         * nor down) then only ever counted up, one step per profiled block, and was given up after 16 of them (0.74 s at one in 32) although it never missed its block. */
        if (((m->blk_n - 1u) & WN_PROF_MASK) != 0u) {                 /* blk_n was counted up by wn_block_run: this tests the block it just ran */
            const uint32_t tj = plat_ticks24() - t_start;
            m->blk_t[5] = tj;
            if (tj > PA_US_SCALE(35000u)) { if (++m->blk_slow >= 16u) { m->blk_bad = 1u; m->active = 0u; } }
            else if (tj < PA_US_SCALE(33600u) && m->blk_slow) m->blk_slow--;
        }
        if (defer) { s_wn_post_makeup = makeup; s_wn_post_pending = 1u; }
        else {
            float po, pr;
            wn_post_block(m, yb, samples_q21, (uint32_t)nframes, makeup, &po, &pr);
#if !defined(PA_HOST_BUILD) && !defined(PA_HOST_SIM)
            if (po > wn_dg_out) wn_dg_out = po;
            if (pr > wn_dg_in) wn_dg_in = pr;
#endif
        }
    }

    for (int i = 0; !use_blk && i < nframes; i++) {
        float y_raw;
        {
            float x_f = (float)samples_q21[i] * inv_q21;
            float x_clean = wavenet_condition_input(m, x_f);
            y_raw = pa_wavenet_step_sample(m, x_clean);
        }
        float y_f = wavenet_dc_block_output(m, y_raw);
        float y_out = y_f * makeup;
        if (m->voicing_profile == PA_VOICE_CLEAN) {
            if (y_out > 1.40f)  y_out = 1.40f + 0.10f * pa_fast_tanhf((y_out - 1.40f) * 2.0f);
            if (y_out < -1.40f) y_out = -1.40f + 0.10f * pa_fast_tanhf((y_out + 1.40f) * 2.0f);
        } else {
            if (y_out > 1.20f)  y_out = 1.20f + 0.15f * pa_fast_tanhf((y_out - 1.20f) * 2.0f);
            if (y_out < -1.20f) y_out = -1.20f + 0.15f * pa_fast_tanhf((y_out + 1.20f) * 2.0f);
        }
        /* NaN protection: if output is not finite, output silence and reset model state */
        if (!isfinite(y_out)) {
            memset(m->ring_buf, 0, sizeof(m->ring_buf));
            memset(m->ring_pos, 0, sizeof(m->ring_pos));
            memset(m->head_ring, 0, sizeof(m->head_ring));
            m->head_pos = 0;
            m->in_dc_x1 = m->in_dc_y1 = 0.0f;
            m->hpf_x1 = m->hpf_x2 = m->hpf_y1 = m->hpf_y2 = 0.0f;
            m->lpf_x1 = m->lpf_x2 = m->lpf_y1 = m->lpf_y2 = 0.0f;
            m->dc_x1 = m->dc_y1 = 0.0f; m->dc_hold = PA_DC_HOLD_SAMPLES;
            m->peak_sample_cycles = 0;
            y_out = 0.0f;
#if !defined(PA_HOST_BUILD) && !defined(PA_HOST_SIM)
            wn_dg_nan++;
#endif
        }
        float scaled = y_out * f_q21;
        /* Q21 clamp: 2147483520.0f is the largest float below 2^31 that can be exactly represented, preventing float->int undefined behaviour. */
        if (scaled < -2147483520.0f) scaled = -2147483520.0f;
        if (scaled >  2147483520.0f) scaled =  2147483520.0f;
        samples_q21[i] = (int32_t)scaled;
#if !defined(PA_HOST_BUILD) && !defined(PA_HOST_SIM)
        { const float ay = y_out < 0.0f ? -y_out : y_out, ax = y_raw < 0.0f ? -y_raw : y_raw;
          if (ay > wn_dg_out) wn_dg_out = ay; if (ax > wn_dg_in) wn_dg_in = ax; }
#endif
    }

    uint32_t dt_ticks = plat_ticks24() - t_start;
#if !defined(PA_HOST_BUILD) && !defined(PA_HOST_SIM)
    {   /* WaveNet diagnostics at SRAM 0x01C7EF00 ("WNDG"): last / longest block (TIMER4 ticks, 24 per us), blocks, NaN resets, the largest output
         * and the head output before the makeup (x 1e6), makeup x 1000, voicing, is_amp_cab, input_trim x 1000 */
        volatile uint32_t *D = (volatile uint32_t *)(uintptr_t)0x01C7EF00u;
        static uint8_t dg_init;
        if (!dg_init) { dg_init = 1; for (int q = 0; q < 32; q++) D[q] = 0u; }            /* SRAM is not cleared at boot: the counters started from whatever was there */
        D[1] = dt_ticks; if (dt_ticks > D[2]) D[2] = dt_ticks; D[3] = D[3] + 1u; D[4] = wn_dg_nan;
        D[5] = (uint32_t)(wn_dg_out * 1.0e6f); D[6] = (uint32_t)(wn_dg_in * 1.0e6f);
        D[7] = (uint32_t)(makeup * 1000.0f); D[8] = m->voicing_profile; D[9] = m->is_amp_cab; D[10] = (uint32_t)(m->input_trim * 1000.0f);
        D[11] = m->blk_t[0]; D[12] = m->blk_t[1]; D[13] = m->blk_t[5]; D[14] = (uint32_t)m->blk_bad; D[15] = (uint32_t)m->blk_slow; D[16] = (uint32_t)use_blk; D[17] = m->blk_t[2]; D[18] = m->blk_t[3]; D[19] = m->blk_t[6]; D[20] = m->blk_t[7]; D[21] = m->blk_t[8]; D[22] = m->blk_t[9]; D[23] = m->blk_t[10]; D[24] = (uint32_t)m->blk_low | ((uint32_t)s_wn_asm << 1);
        D[0] = 0x474E4457u;                                     /* "WDNG" little-endian bytes W D N G */
    }
#endif
    if (dt_ticks > 0 && nframes > 0) {
        uint32_t block_cyc = dt_ticks * 15u;
        m->last_block_cycles = block_cyc;
        uint32_t sample_cyc = block_cyc / (uint32_t)nframes;
        if (sample_cyc > m->peak_sample_cycles) m->peak_sample_cycles = sample_cyc;
    }
}

