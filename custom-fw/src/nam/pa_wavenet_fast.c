/*
 * pa_wavenet_fast.c - faster A2-Lite (3-channel) WaveNet step, for the AC791N's
 * hardware single-precision FPU (the pi32v2 r3 core: fmul / fadd are single instructions).
 *
 * Two variants of pa_wavenet_step_sample(), same model, same state:
 *
 *   pa_wavenet_step_sample_fast()   BIT-EXACT with pa_wavenet_step_sample().
 *     - The baseline loops output -> tap -> input channel, recomputes each ring column
 *       (with a while loop) once per output channel, and reads the conv weights with a
 *       stride of k. Here every tap's column is computed once per layer, each history
 *       column is loaded once, and the three output channels are accumulated side by side:
 *       three independent accumulators, so an fadd does not wait for the previous one.
 *     - The conv weights are read from a PACKED copy (pa_wavenet_fast_pack): per tap the
 *       9 weights [o][j] are adjacent, so the inner loop walks one pointer instead of nine
 *       (with nine, the compiler ran out of registers and spilled them to the stack).
 *     - For each output the additions happen in exactly the baseline's order (bias, then
 *       tap 0..k-1, input channel 0..2), so the result is identical to the last bit.
 *
 *   pa_wavenet_step_sample_fast6()  six accumulators (even and odd taps summed apart, then
 *       added). More independent fadds in flight, but the order of additions differs, so
 *       the result differs from the baseline by float rounding (test/test_wavenet_fast.c
 *       bounds it).
 *
 * Both handle 3 channels only (A2-Lite, the only model the firmware loads) and pass the
 * input through otherwise. (The baseline also accepts channels == 8, but its buffers are
 * sized for 3 channels, so an 8-channel model would read past them; load_weights never
 * sets 8, so that path is unreachable today.)
 *
 * Neither is wired into the audio path yet: the device benchmark (custom-fw/bench/)
 * measures them against the baseline first.
 *
 * PA_RAMFUNC puts the step functions in .ram_text (SRAM) instead of XIP flash when the
 * build defines PA_ENABLE_RAMFUNC. Do NOT enable it in the firmware build until build.ps1
 * extracts .ram_text into build_app.bin: today it copies only .text and .data, so a
 * non-empty .ram_text would be missing from the image and .data would load from the wrong
 * place.
 */
#include "pa_wavenet_fast.h"
#include <string.h>
#include <math.h>

#if defined(PA_ENABLE_RAMFUNC) && !defined(PA_HOST_SIM) && !defined(PA_HOST_BUILD)
#define PA_RAMFUNC __attribute__((section(".ram_text"), noinline))
#else
#define PA_RAMFUNC
#endif

#define C3   3
#define KMAX 15
/* per layer after the conv weights: conv bias 3, mixin 3, 1x1 weights 9, 1x1 bias 3 */
#define LAYER_TAIL (C3 + C3 + C3 * C3 + C3)

int pa_wavenet_fast_pack(const pa_wavenet_model_t *m, float *packed)
{
    if (!m || !packed || m->channels != C3) return -1;
    const float *W = m->weights;
    uint32_t off = 0;
    packed[0] = W[0]; packed[1] = W[1]; packed[2] = W[2];
    off = C3;
    for (int l = 0; l < PA_WN_NUM_LAYERS; l++) {
        const int k = g_pa_wn_specs[l].k;
        float *dst = packed + off;
        const float *src = W + off;
        for (int ki = 0; ki < k; ki++)                  /* [o][j][ki]  ->  [ki][o][j] */
            for (int o = 0; o < C3; o++)
                for (int j = 0; j < C3; j++)
                    *dst++ = src[(o * C3 + j) * k + ki];
        off += (uint32_t)(C3 * C3 * k);
        memcpy(packed + off, W + off, LAYER_TAIL * sizeof(float));
        off += LAYER_TAIL;
    }
    /* head weights, head bias and head scale: unchanged */
    memcpy(packed + off, W + off, (PA_WN_LITE_WEIGHTS - off) * sizeof(float));
    return 0;
}

/* Everything after the convolution stack: identical to the baseline. */
static float wn_head_and_finish(pa_wavenet_model_t *m, const float *P, float h0, float h1,
                                float h2, uint32_t off)
{
    int hpos = (int)m->head_pos + 1;
    if (hpos >= PA_WN_HEAD_TAPS) hpos = 0;
    m->head_pos = (uint16_t)hpos;
    float *hr = &m->head_ring[(uint32_t)hpos * C3];
    hr[0] = h0; hr[1] = h1; hr[2] = h2;

    const float *w = P + off;                      /* [j][tap], as in the baseline */
    float out = P[off + C3 * PA_WN_HEAD_TAPS];     /* head bias */
    for (int j = 0; j < C3; j++) {
        const float *wj = w + j * PA_WN_HEAD_TAPS;
        int col = hpos + 1;                        /* the oldest column, hpos - 15 wrapped */
        if (col >= PA_WN_HEAD_TAPS) col -= PA_WN_HEAD_TAPS;
        for (int ki = 0; ki < PA_WN_HEAD_TAPS; ki++) {
            out += wj[ki] * m->head_ring[(uint32_t)col * C3 + (uint32_t)j];
            if (++col == PA_WN_HEAD_TAPS) col = 0;
        }
    }

    float y = out * m->head_scale;
    if (!isfinite(y)) {
        pa_wavenet_reset(m);
        y = 0.0f;
    }
    if (y > 2.0f)  y = 2.0f;
    if (y < -2.0f) y = -2.0f;
    return y;
}

static inline float wn_input(const pa_wavenet_model_t *m, float x_in)
{
    if (m->input_trim > 0.001f) x_in *= m->input_trim;
    if (x_in > 4.0f)  x_in = 4.0f;
    if (x_in < -4.0f) x_in = -4.0f;
    return x_in;
}

/* Advance a layer's ring, store the new column, and list its taps oldest first. */
static inline float *wn_layer_start(pa_wavenet_model_t *m, int l, float c0, float c1, float c2,
                                    const float **cols, int *kout)
{
    const pa_wavenet_layer_spec_t *s = &g_pa_wn_specs[l];
    const int k = s->k, d = s->d, rlen = s->ring_len;
    float *ring = &m->ring_buf[(uint32_t)s->col_offset * C3];
    int pos = (int)m->ring_pos[l] + 1;
    if (pos >= rlen) pos = 0;
    m->ring_pos[l] = (uint16_t)pos;
    float *wr = ring + (uint32_t)pos * C3;
    wr[0] = c0; wr[1] = c1; wr[2] = c2;
    int col = pos - (k - 1) * d;
    if (col < 0) col += rlen;                      /* (k-1)*d < rlen: one wrap is enough */
    for (int ki = 0; ki < k; ki++) {
        cols[ki] = ring + (uint32_t)col * C3;
        col += d;
        if (col >= rlen) col -= rlen;
    }
    *kout = k;
    return ring;
}

PA_RAMFUNC float pa_wavenet_step_sample_fast(pa_wavenet_model_t *m, const float *P, float x_in)
{
    if (!m || !m->active) return x_in;
    if (m->channels != C3 || !P) return x_in;    /* bypass: the buffers only hold 3 channels */

    x_in = wn_input(m, x_in);
    float c0 = P[0] * x_in, c1 = P[1] * x_in, c2 = P[2] * x_in;
    float h0 = 0.0f, h1 = 0.0f, h2 = 0.0f;
    uint32_t off = C3;
    const float *cols[KMAX];

    for (int l = 0; l < PA_WN_NUM_LAYERS; l++) {
        int k;
        (void)wn_layer_start(m, l, c0, c1, c2, cols, &k);
        const float *wp = P + off;                     /* packed conv weights [ki][o][j] */
        const float *cb = wp + C3 * C3 * k;
        const float *mx = cb + C3, *lw = mx + C3, *lb = lw + C3 * C3;
        off += (uint32_t)(C3 * C3 * k) + LAYER_TAIL;

        float a0 = cb[0], a1 = cb[1], a2 = cb[2];
        const float *const *cp = cols;
        for (int ki = k; ki > 0; ki--, wp += 9) {
            const float *h = *cp++;
            const float x0 = h[0], x1 = h[1], x2 = h[2];
            a0 += wp[0] * x0; a1 += wp[3] * x0; a2 += wp[6] * x0;
            a0 += wp[1] * x1; a1 += wp[4] * x1; a2 += wp[7] * x1;
            a0 += wp[2] * x2; a1 += wp[5] * x2; a2 += wp[8] * x2;
        }

        float z0 = a0 + mx[0] * x_in, z1 = a1 + mx[1] * x_in, z2 = a2 + mx[2] * x_in;
        z0 = (z0 >= 0.0f) ? z0 : 0.01f * z0;
        z1 = (z1 >= 0.0f) ? z1 : 0.01f * z1;
        z2 = (z2 >= 0.0f) ? z2 : 0.01f * z2;
        h0 += z0; h1 += z1; h2 += z2;

        float r0 = lb[0], r1 = lb[1], r2 = lb[2];
        r0 += lw[0] * z0; r0 += lw[1] * z1; r0 += lw[2] * z2;
        r1 += lw[3] * z0; r1 += lw[4] * z1; r1 += lw[5] * z2;
        r2 += lw[6] * z0; r2 += lw[7] * z1; r2 += lw[8] * z2;
        c0 += r0; c1 += r1; c2 += r2;
    }
    return wn_head_and_finish(m, P, h0, h1, h2, off);
}

PA_RAMFUNC float pa_wavenet_step_sample_fast6(pa_wavenet_model_t *m, const float *P, float x_in)
{
    if (!m || !m->active) return x_in;
    if (m->channels != C3 || !P) return x_in;

    x_in = wn_input(m, x_in);
    float c0 = P[0] * x_in, c1 = P[1] * x_in, c2 = P[2] * x_in;
    float h0 = 0.0f, h1 = 0.0f, h2 = 0.0f;
    uint32_t off = C3;
    const float *cols[KMAX];

    for (int l = 0; l < PA_WN_NUM_LAYERS; l++) {
        int k;
        (void)wn_layer_start(m, l, c0, c1, c2, cols, &k);
        const float *wp = P + off;
        const float *cb = wp + C3 * C3 * k;
        const float *mx = cb + C3, *lw = mx + C3, *lb = lw + C3 * C3;
        off += (uint32_t)(C3 * C3 * k) + LAYER_TAIL;

        float e0 = cb[0], e1 = cb[1], e2 = cb[2];      /* even taps */
        float o0 = 0.0f,  o1 = 0.0f,  o2 = 0.0f;       /* odd taps  */
        const float *const *cp = cols;
        int ki = k;
        for (; ki >= 2; ki -= 2, wp += 18) {
            const float *h = cp[0], *g = cp[1];
            cp += 2;
            const float x0 = h[0], x1 = h[1], x2 = h[2];
            const float y0 = g[0], y1 = g[1], y2 = g[2];
            e0 += wp[0] * x0; o0 += wp[9]  * y0;
            e1 += wp[3] * x0; o1 += wp[12] * y0;
            e2 += wp[6] * x0; o2 += wp[15] * y0;
            e0 += wp[1] * x1; o0 += wp[10] * y1;
            e1 += wp[4] * x1; o1 += wp[13] * y1;
            e2 += wp[7] * x1; o2 += wp[16] * y1;
            e0 += wp[2] * x2; o0 += wp[11] * y2;
            e1 += wp[5] * x2; o1 += wp[14] * y2;
            e2 += wp[8] * x2; o2 += wp[17] * y2;
        }
        if (ki) {                                       /* k = 15: one tap left over */
            const float *h = cp[0];
            e0 += wp[0] * h[0]; e0 += wp[1] * h[1]; e0 += wp[2] * h[2];
            e1 += wp[3] * h[0]; e1 += wp[4] * h[1]; e1 += wp[5] * h[2];
            e2 += wp[6] * h[0]; e2 += wp[7] * h[1]; e2 += wp[8] * h[2];
        }
        float z0 = (e0 + o0) + mx[0] * x_in;
        float z1 = (e1 + o1) + mx[1] * x_in;
        float z2 = (e2 + o2) + mx[2] * x_in;
        z0 = (z0 >= 0.0f) ? z0 : 0.01f * z0;
        z1 = (z1 >= 0.0f) ? z1 : 0.01f * z1;
        z2 = (z2 >= 0.0f) ? z2 : 0.01f * z2;
        h0 += z0; h1 += z1; h2 += z2;

        float r0 = lb[0], r1 = lb[1], r2 = lb[2];
        r0 += lw[0] * z0; r0 += lw[1] * z1; r0 += lw[2] * z2;
        r1 += lw[3] * z0; r1 += lw[4] * z1; r1 += lw[5] * z2;
        r2 += lw[6] * z0; r2 += lw[7] * z1; r2 += lw[8] * z2;
        c0 += r0; c1 += r1; c2 += r2;
    }
    return wn_head_and_finish(m, P, h0, h1, h2, off);
}
