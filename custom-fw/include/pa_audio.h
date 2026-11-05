/*
 * pa_audio.h — Pocket Amp Full-Duplex ALNK0 I2S Audio Driver (Big Rock 1)
 *
 * Clean-room implementation for Pocket Amp ALNK0 audio hardware:
 *   - ALNK0 hardware init (PA4..PA8, ADR0=0x01C20E20, ADR1=0x01C20A20)
 *   - IRQ 11 (ALNK0) registration in 0x01C7FE00 / 0x1EEF104
 *   - ALNK0 ping-pong DMA ISR (64 frames/block, 24-bit I2S @ 44.1 kHz)
 */
#ifndef PA_AUDIO_H
#define PA_AUDIO_H

#include <stdint.h>
#include "../src/dsp/fixedpoint.h"

/* Audio stream parameters (64 frames per block @ 44.1 kHz) */
#define PA_AUDIO_SAMPLE_RATE      44100u
#define PA_AUDIO_BLOCK_FRAMES     64u     /* 64 stereo frames per half-buffer IRQ */
#define PA_AUDIO_SHN_FRAMES       128u    /* 128 stereo frames total in ping-pong buffer */
#define PA_AUDIO_CHANNELS         2u      /* Stereo I2S slots (L + R) */
#define PA_AUDIO_HALF_BYTES       512u    /* 64 * 2 * sizeof(int32_t) */
#define PA_AUDIO_DMA_BYTES        1024u   /* 2 halves * 512 bytes */

/* ALNK0 channel assignments on Pocket Amp */
#define PA_ALNK_TX_CH             0u      /* CH0 (PA7 DAT0) -> External Stereo DAC */
#define PA_ALNK_RX_CH             1u      /* CH1 (PA8 DAT1) <- External Guitar ADC */

/* ALNK0 register bitmasks (base 0x00012E00) */
#define PA_ALNK_CON0_MCLK_EN      (1u << 7)   /* 0x0080 */
#define PA_ALNK_CON0_SCLK_MASTER  (1u << 8)   /* 0x0100 */
#define PA_ALNK_CON0_EN           (1u << 11)  /* 0x0800 */
#define PA_ALNK_CON0_CH0_BUF      (1u << 12)  /* 0x1000: HW using half 1 on CH0 */
#define PA_ALNK_CON0_CH1_BUF      (1u << 13)  /* 0x2000: HW using half 1 on CH1 */

/* ALNK0_CON1 per-channel 4-bit nibble: bit0=EN, bit2=24-bit, bit3=DIR(1=RX,0=TX) */
#define PA_ALNK_CON1_CH0_TX24     0x0005u     /* CH0: TX, 24-bit, enabled */
#define PA_ALNK_CON1_CH1_RX24     0x00D0u     /* CH1: RX, 24-bit, enabled */
#define PA_ALNK_CON1_FULL_DUPLEX  (PA_ALNK_CON1_CH0_TX24 | PA_ALNK_CON1_CH1_RX24) /* 0x00D5 */

/* Callback signature for custom block processing (in_q28[64] -> out_l_q28[64], out_r_q28[64]) */
typedef void (*pa_audio_block_cb_t)(const q28_t *in_mono,
                                    q28_t *out_l,
                                    q28_t *out_r,
                                    int nframes);

/* Runtime audio statistics & level meters (mirrors stock 0x01C1B220 + 408..428) */
typedef struct {
    uint32_t irq_count;
    uint32_t xrun_count;
    int32_t  input_peak_s24;   /* Peak 24-bit input amplitude [0..8388607] */
    int32_t  output_peak_s24;  /* Peak 24-bit output amplitude [0..8388607] */
    uint16_t input_gain_q15;   /* Input gain scalar (0x8000 = 1.0) */
    uint16_t master_vol_q15;   /* Master volume target scalar (0x8000 = 1.0) */
    uint16_t master_vol_cur_q15; /* Master volume current smoothed scalar */
    uint8_t  running;
    uint8_t  bypass;
} pa_audio_state_t;

extern pa_audio_state_t g_pa_audio;

/* Initialize PA4..PA8, clock mux, ALNK0 DMA buffers, IRQ 11, and start full-duplex I2S */
void pa_audio_init(pa_audio_block_cb_t cb);
void pa_audio_alnk_init(void);

/* Stop ALNK0 DMA engine */
void pa_audio_stop(void);

/* Pure C helper to process one 512-byte RX half-buffer into a 512-byte TX half-buffer.
 * Exposed for host-PC unit testing as well as the on-device ALNK0 ISR. */
void pa_audio_process_half(const int32_t *rx_half, int32_t *tx_half);

#endif /* PA_AUDIO_H */
