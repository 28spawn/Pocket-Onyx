/*
 * pa_cab.h — Pocket Amp Partitioned Hardware-Accelerated IR Cabinet Convolver (Big Rock 2)
 *
 * Clean-room implementation of:
 *   - Container parser (@M2, @M3, @M4, NAM magic & float sanity validator)
 *   - Partitioned Block-FIR Convolver using 0x01EE0400 (0x0A82 / 0x8A82)
 */
#ifndef PA_CAB_H
#define PA_CAB_H

#include <stdint.h>
#include "../src/dsp/fixedpoint.h"

#define PA_CAB_BLOCK_SIZE       64
#define PA_CAB_MAX_PARTITIONS   16   /* 16 * 64 = 1024 taps (configurable up to 32 = 2048 taps) */
#define PA_CAB_MAX_TAPS         (PA_CAB_BLOCK_SIZE * PA_CAB_MAX_PARTITIONS)
#define PA_CAB_Q_SHIFT          18   /* Q18 fixed-point format */

/* Blob magic constants */
#define PA_BLOB_MAGIC_M2        0x00324D40u  /* "@M2\0" */
#define PA_BLOB_MAGIC_M3        0x00334D40u  /* "@M3\0" */
#define PA_BLOB_MAGIC_M4        0x00344D40u  /* "@M4\0" */
#define PA_BLOB_MAGIC_NAM       0x004D414Eu  /* "NAM\0" */

typedef struct {
    int32_t  coeffs_q18[PA_CAB_MAX_PARTITIONS][PA_CAB_BLOCK_SIZE];
    int32_t  hist_q18[PA_CAB_MAX_PARTITIONS][PA_CAB_BLOCK_SIZE * 2];
    int32_t  acc_buf[PA_CAB_BLOCK_SIZE];
    int32_t  prev_tail[PA_CAB_BLOCK_SIZE];
    uint8_t  num_partitions;   /* 8 (512 taps), 16 (1024 taps), etc. */
    uint8_t  curr_part;        /* Circular partition index [0..num_partitions-1] */
    uint8_t  bypass;
} pa_cab_ir_t;

/* Initialize IR cabinet convolver with a default speaker impulse response */
void pa_cab_ir_init(pa_cab_ir_t *cab, int num_taps);

/* Load float IR taps [-1.0..+1.0] into partitioned Q18 coefficient tables */
int  pa_cab_ir_load_floats(pa_cab_ir_t *cab, const float *taps, int num_taps);

/* Validate an @M2/@M3/@M4/NAM blob (no product-ID lock) */
int  pa_cab_validate_blob(const uint8_t *blob, uint32_t len);

/* Process a 64-sample block through the partitioned hardware FIR convolver */
void pa_cab_ir_process_block(pa_cab_ir_t *cab, q28_t *samples, int nframes);

#endif /* PA_CAB_H */
