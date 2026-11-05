/*
 * pa_hw_accel.h — JieLi AC791N Hardware Vector/Matrix/FIR Coprocessor (0x01EE0400)
 *
 * Hardware accelerator interface for JieLi AC791N:
 *   - Matrix-Vector MAC mode (config=0x0AA1, ctrl=0x15009011 / 0x1500A010)
 *   - Partitioned Block-FIR mode (config=0x0A82 overwrite / 0x8A82 accumulate,
 *     ctrl=0x12001000 | (nframes + 1))
 *
 * On-device (!PA_HOST_BUILD && !PA_HOST_SIM): Programs 0x01C1046C and launches 0x01EE0400
 *                             asynchronously so the CPU can compute in parallel before pa_hw_accel_wait().
 * On host PC (PA_HOST_SIM):   Bit-accurate C emulation of 0x01EE0400.
 */
#ifndef PA_HW_ACCEL_H
#define PA_HW_ACCEL_H

#include <stdint.h>

/* Command config words (+0x00 in descriptor) */
#define PA_HW_CFG_MATVEC          0x00000AA1u  /* Matrix-Vector multiply (2721) */
#define PA_HW_CFG_FIR_OVERWRITE   0x00000A82u  /* Block-FIR convolution, overwrite dst (2690) */
#define PA_HW_CFG_FIR_ACCUMULATE  0x00008A82u  /* Block-FIR convolution, accumulate dst += (35458) */

/* Control words (+0x10 in descriptor) */
#define PA_HW_CTRL_GRU_9x17_A     0x15009011u  /* shift=21 (0x15), rows=9, cols=17 (0x11) */
#define PA_HW_CTRL_GRU_9x17_B     0x1500A011u  /* shift=21 (0x15), rows=9, cols=17 (0x11) */
#define PA_HW_CTRL_GRU_9x16       0x1500A010u  /* shift=21 (0x15), rows=9, cols=16 (0x10) */
#define PA_HW_CTRL_FIR_Q18(n)     (0x12001000u | ((uint32_t)((n) + 1) & 0xFFFFu))

/* Stride words (+0x14 in descriptor) */
#define PA_HW_STRIDE_17           0x00411000u  /* 9 rows, row stride = 17 words */
#define PA_HW_STRIDE_16           0x00410000u  /* 9 rows, row stride = 16 words */

/* 24-byte hardware command block layout (resides at 0x01C1046C on device) */
typedef struct {
    uint32_t    config;   /* +0x00: PA_HW_CFG_* */
    const void *src_a;    /* +0x04: Input vector (MATVEC) or filter taps (FIR) */
    const void *src_b;    /* +0x08: Weight matrix (MATVEC) or delay history (FIR) */
    void       *dst;      /* +0x0C: Output buffer */
    uint32_t    ctrl;     /* +0x10: Shift [31:24], mode/rows, length */
    uint32_t    stride;   /* +0x14: Row/col stride config */
} pa_hw_vec_cmd_t;

/* Initialize hardware accelerator state */
void pa_hw_accel_init(void);

/* Wait for any in-flight hardware job to complete and ack bit 6.
 *
 * The wait is BOUNDED: it spins at most PA_HW_ACCEL_WAIT_LIMIT times.  A job that does
 * not report READY in that time is counted as an error, the accelerator is marked broken
 * and everything from then on runs in software.  The unbounded version could not only
 * hang: it hung inside the audio interrupt, so the device locked up with no way to see
 * why. */
#define PA_HW_ACCEL_WAIT_LIMIT 2000u
void pa_hw_accel_wait(void);

/* 1 when the accelerator gave up and the software path is in use, 0 when the hardware
 * is still trusted. */
uint8_t pa_hw_accel_broken(void);

/* How many times the accelerator timed out (0 while it works). */
uint32_t pa_hw_accel_errors(void);

/* Submit a 24-byte command block to 0x01EE0400 (asynchronous on device, emulated on host) */
void pa_hw_accel_submit(const pa_hw_vec_cmd_t *cmd);

/* The lean variants for a long run of jobs from one caller (the WaveNet block engine): same register protocol, no timer reads, breadcrumbs or
 * statistics. The caller ends the run with pa_hw_accel_wait_lean(). */
void pa_hw_accel_submit_lean(const pa_hw_vec_cmd_t *cmd);
void pa_hw_accel_wait_lean(void);
void pa_hw_accel_job_lean(uint32_t config, const void *a, const void *b, void *dst, uint32_t ctrl, uint32_t stride);

/* Convenience helper for 64-tap Q18 Partitioned IR Cabinet Block-FIR */
void pa_hw_accel_fir_part64(const int32_t *coeffs_q18,
                            const int32_t *hist_x_q18,
                            int32_t *dst_q18,
                            int nframes,
                            int accumulate);

#endif /* PA_HW_ACCEL_H */
