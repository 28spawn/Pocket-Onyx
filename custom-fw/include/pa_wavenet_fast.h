/*
 * pa_wavenet_fast.h - faster A2-Lite (3-channel) WaveNet step for the AC791N's hardware
 * single-precision FPU. See src/nam/pa_wavenet_fast.c.
 *
 * Use:
 *   static float packed[PA_WN_FAST_PACKED_FLOATS];
 *   pa_wavenet_fast_pack(&model, packed);              // once, after the weights load
 *   y = pa_wavenet_step_sample_fast(&model, packed, x); // per sample, bit-exact
 *
 * The packed array is a reordered copy of model->weights. Re-pack whenever the weights
 * change; a stale pack gives the old model's output.
 */
#ifndef PA_WAVENET_FAST_H
#define PA_WAVENET_FAST_H

#include "pa_wavenet.h"

#define PA_WN_FAST_PACKED_FLOATS PA_WN_LITE_WEIGHTS

/* Reorder model->weights for the fast steps. Returns 0, or -1 if the model is not 3-channel. */
int   pa_wavenet_fast_pack(const pa_wavenet_model_t *m, float *packed);

/* Bit-exact with pa_wavenet_step_sample(): same outputs, same model state. */
float pa_wavenet_step_sample_fast(pa_wavenet_model_t *m, const float *packed, float x_in);

/* Six accumulators: reordered additions, within float rounding of the baseline. */
float pa_wavenet_step_sample_fast6(pa_wavenet_model_t *m, const float *packed, float x_in);

#endif /* PA_WAVENET_FAST_H */
