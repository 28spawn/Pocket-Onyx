/* custom-fw/src/fx/fx_cab_ir.c — Partitioned Q18 Hardware FIR Cabinet Convolver + Low/High Cut
 *
 * Implements:
 *   1. Partitioned 64-Tap Block Convolver on 0x01EE0400 HW
 *      in Q18 fixed-point (262144.0 = 0x48800000, >> 18) for 1024/2048-tap Cabinet IRs.
 *   2. User Manual Page 13 IR CAB Module parameters:
 *      - Param 0 (`CAB_PARAM_LEVEL`):    Output Level in Q21
 *      - Param 1 (`CAB_PARAM_LOW_CUT`):  Low-cut HPF adjustable 20 Hz .. 300 Hz
 *      - Param 2 (`CAB_PARAM_HIGH_CUT`): High-cut LPF adjustable 18000 Hz .. 5000 Hz
 */

#include <string.h>
#include "pa_fx.h"
#include "pa_cab.h"
#include "pa_stock_assets.h"   /* PA_STOCK_CAB_TAPS only - no link dependency */
#include "pa_hw_accel.h"
#include "pa_config.h"
#include "../dsp/fixedpoint.h"
#include <math.h>

/* PA_CAB_FFT 1: the cabinet convolution runs on the CPU as a float FFT convolution (fft_conv.c). The
 * accelerator's FIR job writes 129 words for a 64-sample block and the Q18 output was being read as 64
 * consecutive results, so the cab was most likely wrong; the FFT path is correct by construction (it
 * matched the direct convolution to 3.8e-6 in bench/). 0 = the accelerator FIR path with its probe. */
#ifndef PA_CAB_FFT
#define PA_CAB_FFT 1
#endif
#include "fft_conv.h"

#define CAB_PART_LEN   64u
#define CAB_MAX_PARTS  (PA_IR_MAX_TAPS / CAB_PART_LEN)

#if !defined(PA_HOST_BUILD) && !defined(PA_HOST_SIM)
#include "pa_plat.h"                    /* PA_ISR_CP */
/* BENCH: the accelerator's FIR output goes to a big static buffer (not the stack) with a guard region
 * before and after it, all pre-filled with a unique value per word (so real output cannot match the
 * fill). After the FIRST job the write extent is stored in SRAM, so a stall does not lose it:
 *   0x01C7C3E0 = one past the last word the engine changed (in the work area)
 *   0x01C7C3E4 = how many work-area words changed
 *   0x01C7C3E8 = the first work-area word that changed (0xFFFF = none)
 *   0x01C7C3EC = guard words changed: low 16 bits before the buffer, high 16 bits after it
 * 65 = the engine writes n+1 words; far more, or a guard change, = it writes outside its output block.
 * Unchanged words do not prove the engine never wrote them (it can write the same value). */
#if !PA_CAB_FFT
#define CAB_PROBE 1
#define CAB_PROBE_WORDS 2048u
#define CAB_PROBE_GUARD 64u
static int32_t  s_fir_mem[CAB_PROBE_GUARD + CAB_PROBE_WORDS + CAB_PROBE_GUARD] __attribute__((aligned(8)));
static uint32_t s_fir_probe_state;       /* 0 = not armed, 1 = armed (first job pending), 2 = measured */
static inline uint32_t cab_probe_fill(uint32_t i) { return 0xC0DE0000u ^ (i * 0x9E3779B1u); }
#endif /* !PA_CAB_FFT */
#endif
#ifndef PA_ISR_CP
#define PA_ISR_CP(n) ((void)0)
#endif

enum {
    CAB_PARAM_LEVEL    = 0, /* Output volume in Q21 */
    CAB_PARAM_LOW_CUT  = 1, /* 20..300 Hz low-cut in Q21 [0..PA_Q21_ONE] */
    CAB_PARAM_HIGH_CUT = 2  /* 18000..5000 Hz high-cut in Q21 [0..PA_Q21_ONE] */
};

/* A Direct Form II Transposed biquad (12 dB/octave).
 *
 * The low-cut and the high-cut used to be first-order one-pole filters
 * (6 dB/octave). That is too gentle for a guitar cabinet: a 6 dB/octave
 * low-cut set to tame the rumble also thins out the 100-150 Hz body, and a
 * 6 dB/octave high-cut pulls down the upper midrange instead of isolating the
 * harsh edge. A second-order Butterworth (Q = 0.707) is what a real cab (and
 * the mic in front of it) actually does, and it is the same kind of filtering
 * the post-AMP fizz filter already uses. */
typedef struct {
    int32_t b0, b1, b2;   /* Q21 */
    int32_t a1, a2;       /* Q21 */
    float   fb0, fb1, fb2, fa1, fa2;   /* the same coefficients as floats (the filter runs on the FPU) */
    float   z1, z2;       /* the state */
} cab_biquad_t;

typedef struct {
    int32_t  ir_q18[PA_IR_MAX_TAPS];              /* Quantized Q18 IR taps */
    /* IMP-13: one linear time-ordered history instead of 32 copies of a 128-sample window.
     * hist_x[p] of the old layout == hist[L-128-p*64 .. L-1-p*64], L = (num_parts+1)*64.
     * 8.4 KB instead of 16 KB, which is what lets CAB + MOD + DLY + REV fit the DSP pool. */
    int32_t  hist[(CAB_MAX_PARTS + 1u) * CAB_PART_LEN];
    uint32_t num_taps;
    uint32_t num_parts;
    int32_t  level_q21;                           /* Manual Page 13: Level */
    int32_t  trim_q21;                            /* IMP-02: per-IR loudness normalisation */
    cab_biquad_t hp;                              /* the low cut  (20..300 Hz)  */
    cab_biquad_t lp;                              /* the high cut (18k..5k Hz)  */
    /* The stereo cabinet (PA_CAB_FFT only). All of it stays idle - and h2 stays NULL, so no pool memory is used - until the width is
     * first set above 0. The right channel is the cabinet convolved with a phase-dispersed copy of the same IR (same magnitude
     * response, so the same tone; different phase, so the two ears get a different signal), mixed back with the left by the width. */
    fc_cpx (*h2)[FC_BINS];                        /* the right IR's partition spectra, from the pool */
    volatile uint8_t h2_ready;                    /* the spectra match the loaded IR */
    float    rz[4];                               /* the right channel's low cut / high cut filter states */
    volatile float mix_c_tgt, mix_s_tgt;          /* the width as a rotation: L = c A + s B, R = c A - s B (target; set from the main loop) */
    float    mix_c, mix_s;                        /* where the audio is now (ramped over a block) */
} cab_ir_state_t;

extern void *pa_alloc(uint32_t);
extern void  pa_free(void *);

static void cab_ir_process_block(cab_ir_state_t *s, int32_t *buf, int blk);

/* ---- the second-order filters ---------------------------------------------
 * A Direct Form II Transposed biquad, and the RBJ cookbook coefficients for a
 * 2nd-order Butterworth (Q = 0.707) high-pass or low-pass. */
static inline int32_t cab_biquad_run(cab_biquad_t *f, int32_t x) {
    const float fx = (float)x;
    float y = f->fb0 * fx + f->z1;
    f->z1 = f->fb1 * fx - f->fa1 * y + f->z2;
    f->z2 = f->fb2 * fx - f->fa2 * y;
    if (!(f->z1 > -4.0e9f && f->z1 < 4.0e9f)) f->z1 = 0.0f;      /* NaN / infinity / wild: reset the state */
    if (!(f->z2 > -4.0e9f && f->z2 < 4.0e9f)) f->z2 = 0.0f;
    if (y > 2.0e9f) y = 2.0e9f; else if (y < -2.0e9f) y = -2.0e9f;
    return (int32_t)y;
}

static void cab_biquad_set(cab_biquad_t *f, int high_pass, float f0_hz, float q) {
    const float w  = 2.0f * 3.14159265f * f0_hz / (float)PA_SAMPLE_RATE;
    const float c  = cosf(w), sn = sinf(w);
    const float al = sn / (2.0f * q);
    const float a0 = 1.0f + al;
    float b0, b1, b2;
    if (high_pass) {
        b0 = (1.0f + c) * 0.5f;  b1 = -(1.0f + c);      b2 = (1.0f + c) * 0.5f;
    } else {
        b0 = (1.0f - c) * 0.5f;  b1 =  (1.0f - c);      b2 = (1.0f - c) * 0.5f;
    }
    f->b0 = (int32_t)lrintf(b0 / a0 * (float)PA_Q21_ONE);
    f->b1 = (int32_t)lrintf(b1 / a0 * (float)PA_Q21_ONE);
    f->b2 = (int32_t)lrintf(b2 / a0 * (float)PA_Q21_ONE);
    f->a1 = (int32_t)lrintf(-2.0f * c / a0 * (float)PA_Q21_ONE);
    f->a2 = (int32_t)lrintf((1.0f - al) / a0 * (float)PA_Q21_ONE);
    const float k = 1.0f / (float)PA_Q21_ONE;
    f->fb0 = (float)f->b0 * k; f->fb1 = (float)f->b1 * k; f->fb2 = (float)f->b2 * k;
    f->fa1 = (float)f->a1 * k; f->fa2 = (float)f->a2 * k;
    f->z1 = 0.0f; f->z2 = 0.0f;
}

#if PA_CAB_FFT
/* The convolver's state: the IR spectra and the frequency-domain delay line (about 17 KB, static, so it is
 * not part of the node state the pool frees on bypass). Loaded whenever the IR changes. The gain folds in
 * everything the old path did after the FIR: the Q18 IR, and the x280000/2^18 loudness calibration, so
 * feeding the Q21 samples straight in gives calibrated Q21 out. */
#if !defined(PA_HOST_BUILD) && !defined(PA_HOST_SIM) && !defined(PA_EMU_DLL) && !defined(__wasm__)
#define CAB_FFT_ATTR __attribute__((section(".pool"), aligned(8)))     /* the 96 KB data region is full; the pool has room */
#else
#define CAB_FFT_ATTR __attribute__((aligned(8)))
#endif
static fc_state_t s_fc CAB_FFT_ATTR;
static uint8_t    s_fc_tables;

/* How many of the cabinet's 64-tap partitions run (16 = all 1,024 taps); the load governor in audio_alnk.c lowers it when the audio interrupt runs too long. */
volatile uint8_t g_pa_cab_parts_limit = 16;

static uint8_t s_width_pct;                     /* the stereo width, remembered across a bypass like the selected IR */

/* The IR as floats while its spectra are built: in the state's `hist`, which the FFT convolver never uses (it is the accelerator FIR
 * path's history, 4,352 B in the node state) - so this needs no static buffer of its own, and the .bss, which is full, gets 4 KB back. */
#define CAB_IR_F(s) ((float *)(void *)(s)->hist)
_Static_assert(sizeof(((cab_ir_state_t *)0)->hist) >= sizeof(float) * FC_MAXP * FC_B, "the IR scratch must fit in hist");

static uint32_t cab_ir_float(cab_ir_state_t *s) {
    const float g = 280000.0f / (2097152.0f * 262144.0f);
    uint32_t n = s->num_parts * CAB_PART_LEN;
    if (n > (uint32_t)(FC_MAXP * FC_B)) n = (uint32_t)(FC_MAXP * FC_B);
    if (!s_fc_tables) { fc_tables_init(); s_fc_tables = 1u; }
    for (uint32_t i = 0; i < n; i++) CAB_IR_F(s)[i] = (float)s->ir_q18[i] * g;
    return n;
}

/* The right channel's IR: the left one through four Schroeder all-pass sections (y[i] = -g x[i] + x[i-D] + g y[i-D]). An all-pass has a flat
 * magnitude response, so the cabinet keeps its tone; the phase it adds differs from one frequency to the next, so the two ears no longer
 * get the same waveform. The delays are short and mutually prime (0.25 .. 1.1 ms), so the smear stays well inside the cabinet's own decay. */
static void cab_disperse(float *a, int n) {
    static const uint8_t D[4] = { 11u, 19u, 31u, 47u };
    const float g = 0.45f;
    for (int k = 0; k < 4; k++) {
        const int d = D[k];
        float xr[47];
        memset(xr, 0, sizeof xr);
        int r = 0;
        for (int i = 0; i < n; i++) {
            const float x = a[i];
            const float yo = (i >= d) ? a[i - d] : 0.0f;
            a[i] = -g * x + xr[r] + g * yo;
            xr[r] = x;
            if (++r == d) r = 0;
        }
    }
}

/* The scratch holds the left IR (n taps): build the right IR's spectra from it. */
static void cab_h2_fill(cab_ir_state_t *s, uint32_t n) {
    s->h2_ready = 0u;
    cab_disperse(CAB_IR_F(s), (int)n);
    (void)fc_ir_spectra(s->h2, CAB_IR_F(s), (int)n);
    s->h2_ready = 1u;
}

static void cab_fft_load(cab_ir_state_t *s) {
    const uint32_t n = cab_ir_float(s);
    (void)fc_init(&s_fc, CAB_IR_F(s), (int)n);
    if (s->h2) cab_h2_fill(s, n);                /* the stereo right IR follows the left one */
}
#endif

PA_HOT static void cab_ir_run(fx_node_t *nd, int32_t *buf, int n) {
    cab_ir_state_t *s = (cab_ir_state_t *)nd->state;
    if (!s || s->num_parts == 0) return;

    /* The partitioned convolution below is defined for exactly CAB_PART_LEN (64)
     * samples: the history ring and the partition offsets assume it. If the block
     * were longer, the old code clamped blk to 64 and left samples 64..n-1 as RAW,
     * UNCABBED guitar - a hard discontinuity every block. pa_config.h allows
     * PA_BLOCK_N up to 128, so process the block in 64-sample sub-blocks. */
    for (int off = 0; off < n; off += CAB_PART_LEN) {
        int chunk = n - off;
        if (chunk > (int)CAB_PART_LEN) chunk = (int)CAB_PART_LEN;
        cab_ir_process_block(s, buf + off, chunk);
        PA_ISR_CP(0x73);                           /* a 64-sample sub-block is finished (back in cab_ir_run) */
    }
}

#if PA_CAB_FFT
/* One 64-sample block of the stereo cabinet. A = the block through the left IR, B = through the right (phase-dispersed) IR; one forward
 * FFT serves both. The width is a rotation of the pair: L = c A + s B, R = c A - s B with c = cos t, s = sin t, t = width * 45 deg, so the
 * power of each channel stays that of the mono cabinet and the channels' correlation falls from 1 to 0 as the width goes from 0 to 100.
 * The two filters and the level then run on each channel (the right channel has its own filter states). */
PA_HOT static void cab_ir_process_block_st(cab_ir_state_t *s, int32_t *bl, int32_t *br, int blk) {
    float in[CAB_PART_LEN], oa[CAB_PART_LEN], ob[CAB_PART_LEN];
    for (uint32_t k = 0; k < CAB_PART_LEN; k++) in[k] = (k < (uint32_t)blk) ? (float)bl[k] : 0.0f;
    fc_set_limit(&s_fc, (int)g_pa_cab_parts_limit);
    fc_block2(&s_fc, (const fc_cpx (*)[FC_BINS])s->h2, (int)s->num_parts, in, oa, ob);
    PA_ISR_CP(0x74);
    const float g = ((float)s->level_q21 * (1.0f / 2097152.0f)) * ((float)s->trim_q21 * (1.0f / 2097152.0f));
    const float hb0 = s->hp.fb0, hb1 = s->hp.fb1, hb2 = s->hp.fb2, ha1 = s->hp.fa1, ha2 = s->hp.fa2;
    const float lb0 = s->lp.fb0, lb1 = s->lp.fb1, lb2 = s->lp.fb2, la1 = s->lp.fa1, la2 = s->lp.fa2;
    float hz1 = s->hp.z1, hz2 = s->hp.z2, lz1 = s->lp.z1, lz2 = s->lp.z2;
    float rh1 = s->rz[0], rh2 = s->rz[1], rl1 = s->rz[2], rl2 = s->rz[3];
    float c = s->mix_c, sn = s->mix_s;
    const float dc = (s->mix_c_tgt - c) * (1.0f / (float)CAB_PART_LEN), ds = (s->mix_s_tgt - sn) * (1.0f / (float)CAB_PART_LEN);
    for (int i = 0; i < blk; i++) {
        float a = oa[i], b = ob[i];
        if (!(a > -1.0e9f && a < 1.0e9f)) a = 0.0f;
        if (!(b > -1.0e9f && b < 1.0e9f)) b = 0.0f;
        c += dc; sn += ds;                                           /* the width moves over one block, so turning it does not click */
        const float ca = c * a, sb = sn * b;
        const float xl = ca + sb, xr = ca - sb;
        float yh = hb0 * xl + hz1;
        hz1 = hb1 * xl - ha1 * yh + hz2;
        hz2 = hb2 * xl - ha2 * yh;
        float yl = lb0 * yh + lz1;
        lz1 = lb1 * yh - la1 * yl + lz2;
        lz2 = lb2 * yh - la2 * yl;
        float v = yl * g;
        if (v > 2.1474836e9f) v = 2.1474836e9f; else if (v < -2.1474836e9f) v = -2.1474836e9f;
        bl[i] = (int32_t)v;
        yh = hb0 * xr + rh1;
        rh1 = hb1 * xr - ha1 * yh + rh2;
        rh2 = hb2 * xr - ha2 * yh;
        yl = lb0 * yh + rl1;
        rl1 = lb1 * yh - la1 * yl + rl2;
        rl2 = lb2 * yh - la2 * yl;
        v = yl * g;
        if (v > 2.1474836e9f) v = 2.1474836e9f; else if (v < -2.1474836e9f) v = -2.1474836e9f;
        br[i] = (int32_t)v;
    }
    if (!(hz1 > -4.0e9f && hz1 < 4.0e9f)) hz1 = 0.0f;
    if (!(hz2 > -4.0e9f && hz2 < 4.0e9f)) hz2 = 0.0f;
    if (!(lz1 > -4.0e9f && lz1 < 4.0e9f)) lz1 = 0.0f;
    if (!(lz2 > -4.0e9f && lz2 < 4.0e9f)) lz2 = 0.0f;
    if (!(rh1 > -4.0e9f && rh1 < 4.0e9f)) rh1 = 0.0f;
    if (!(rh2 > -4.0e9f && rh2 < 4.0e9f)) rh2 = 0.0f;
    if (!(rl1 > -4.0e9f && rl1 < 4.0e9f)) rl1 = 0.0f;
    if (!(rl2 > -4.0e9f && rl2 < 4.0e9f)) rl2 = 0.0f;
    s->hp.z1 = hz1; s->hp.z2 = hz2; s->lp.z1 = lz1; s->lp.z2 = lz2;
    s->rz[0] = rh1; s->rz[1] = rh2; s->rz[2] = rl1; s->rz[3] = rl2;
    s->mix_c = c; s->mix_s = sn;
}

PA_HOT static void cab_ir_run_stereo(fx_node_t *nd, int32_t *bl, int32_t *br, int n) {
    cab_ir_state_t *s = (cab_ir_state_t *)nd->state;
    if (!s || s->num_parts == 0) { for (int i = 0; i < n; i++) br[i] = bl[i]; return; }
    if (!s->h2 || !s->h2_ready) {                    /* the right IR is being rebuilt: this block in mono */
        cab_ir_run(nd, bl, n);
        for (int i = 0; i < n; i++) br[i] = bl[i];
        return;
    }
    for (int off = 0; off < n; off += CAB_PART_LEN) {
        int chunk = n - off;
        if (chunk > (int)CAB_PART_LEN) chunk = (int)CAB_PART_LEN;
        cab_ir_process_block_st(s, bl + off, br + off, chunk);
        PA_ISR_CP(0x73);
    }
    g_pa_cab_stereo = 1;
}
#endif

PA_HOT static void cab_ir_process_block(cab_ir_state_t *s, int32_t *buf, int blk) {
#if PA_CAB_FFT
    {
        float in[CAB_PART_LEN], out[CAB_PART_LEN];
        for (uint32_t k = 0; k < CAB_PART_LEN; k++) in[k] = (k < (uint32_t)blk) ? (float)buf[k] : 0.0f;
        fc_set_limit(&s_fc, (int)g_pa_cab_parts_limit);
        fc_block(&s_fc, in, out);
        PA_ISR_CP(0x74);
        /* The two filters and the level, all in float for the whole block: the old loop converted to integer between the stages, ran two
         * guards per filter per sample and two 64-bit multiplies (about 47 cycles each) per sample, 81 us a block on the unit. The state is
         * checked once per block instead of once per sample; the output differs from the old path by less than one Q21 step. */
        const float g = ((float)s->level_q21 * (1.0f / 2097152.0f)) * ((float)s->trim_q21 * (1.0f / 2097152.0f));
        const float hb0 = s->hp.fb0, hb1 = s->hp.fb1, hb2 = s->hp.fb2, ha1 = s->hp.fa1, ha2 = s->hp.fa2;
        const float lb0 = s->lp.fb0, lb1 = s->lp.fb1, lb2 = s->lp.fb2, la1 = s->lp.fa1, la2 = s->lp.fa2;
        float hz1 = s->hp.z1, hz2 = s->hp.z2, lz1 = s->lp.z1, lz2 = s->lp.z2;
        for (int i = 0; i < blk; i++) {
            float o = out[i];
            if (!(o > -1.0e9f && o < 1.0e9f)) o = 0.0f;              /* NaN / infinity / wild: never let it into the chain */
            const float yh = hb0 * o + hz1;
            hz1 = hb1 * o - ha1 * yh + hz2;
            hz2 = hb2 * o - ha2 * yh;
            const float yl = lb0 * yh + lz1;
            lz1 = lb1 * yh - la1 * yl + lz2;
            lz2 = lb2 * yh - la2 * yl;
            float v = yl * g;
            if (v > 2.1474836e9f) v = 2.1474836e9f; else if (v < -2.1474836e9f) v = -2.1474836e9f;
            buf[i] = (int32_t)v;
        }
        if (!(hz1 > -4.0e9f && hz1 < 4.0e9f)) hz1 = 0.0f;           /* NaN / infinity / wild: reset the state */
        if (!(hz2 > -4.0e9f && hz2 < 4.0e9f)) hz2 = 0.0f;
        if (!(lz1 > -4.0e9f && lz1 < 4.0e9f)) lz1 = 0.0f;
        if (!(lz2 > -4.0e9f && lz2 < 4.0e9f)) lz2 = 0.0f;
        s->hp.z1 = hz1; s->hp.z2 = hz2; s->lp.z1 = lz1; s->lp.z2 = lz2;
        return;
    }
#endif
    /* The accelerator's FIR job is started with a count of n+1 (PA_HW_CTRL_FIR_Q18), the way stock does,
     * and it writes its results to this buffer. A bare [CAB_PART_LEN] array on the stack let a 65th word
     * land on the saved registers of this frame: the job finished and the function then returned into
     * garbage (the 2026-10-31 stall, last breadcrumb 0x6C). Pad it: a few words of slack, 8-byte aligned. */
#if defined(CAB_PROBE)
    int32_t *const acc_out = &s_fir_mem[CAB_PROBE_GUARD];
    if (s_fir_probe_state == 0u) {
        for (uint32_t i = 0; i < CAB_PROBE_GUARD + CAB_PROBE_WORDS + CAB_PROBE_GUARD; i++)
            s_fir_mem[i] = (int32_t)cab_probe_fill(i);
        s_fir_probe_state = 1u;
    }
#else
    int32_t acc_out[CAB_PART_LEN + 16] __attribute__((aligned(8)));
#endif

    const uint32_t L = (s->num_parts + 1u) * CAB_PART_LEN;
    /* Age the history by one block and append the current block (Q21 -> Q18) */
    memmove(&s->hist[0], &s->hist[CAB_PART_LEN], (L - CAB_PART_LEN) * sizeof(int32_t));
    for (uint32_t k = 0; k < CAB_PART_LEN; k++) {
        s->hist[L - CAB_PART_LEN + k] = (k < (uint32_t)blk) ? ((buf[k] + 4) >> 3) : 0;
    }

    /* Execute partitioned Q18 FIR convolution on 0x01EE0400 hardware accelerator */
    for (uint32_t p = 0; p < s->num_parts; p++) {
        const int32_t *part_coeffs = &s->ir_q18[p * CAB_PART_LEN];
        const int32_t *part_src    = &s->hist[L - 2u * CAB_PART_LEN - p * CAB_PART_LEN + 1u];
        pa_hw_accel_fir_part64(part_coeffs, part_src, acc_out, (int)CAB_PART_LEN, (p > 0) ? 1 : 0);
        PA_ISR_CP(0x6E);                           /* back in the cabinet loop after a job */
#if defined(CAB_PROBE)
        if (s_fir_probe_state == 1u) {             /* measure the first job's write extent */
            uint32_t last = 0, changed = 0, first = 0xFFFFu, pre = 0, post = 0;
            for (uint32_t i = 0; i < CAB_PROBE_GUARD; i++) {
                if (s_fir_mem[i] != (int32_t)cab_probe_fill(i)) pre++;
            }
            for (uint32_t i = 0; i < CAB_PROBE_WORDS; i++) {
                if (s_fir_mem[CAB_PROBE_GUARD + i] != (int32_t)cab_probe_fill(CAB_PROBE_GUARD + i)) {
                    if (first == 0xFFFFu) first = i;
                    last = i + 1u; changed++;
                }
            }
            for (uint32_t i = 0; i < CAB_PROBE_GUARD; i++) {
                const uint32_t k = CAB_PROBE_GUARD + CAB_PROBE_WORDS + i;
                if (s_fir_mem[k] != (int32_t)cab_probe_fill(k)) post++;
            }
            *(volatile uint32_t *)0x01C7C3E0u = last;
            *(volatile uint32_t *)0x01C7C3E4u = changed;
            *(volatile uint32_t *)0x01C7C3E8u = first;
            *(volatile uint32_t *)0x01C7C3ECu = pre | (post << 16);
            s_fir_probe_state = 2u;
            PA_ISR_CP(0x71);                       /* the output has been inspected */
        }
#endif
    }
    PA_ISR_CP(0x6F);                               /* all partitions done */
    PA_ISR_CP(0x74);                               /* about to convert and filter the output */

    /* Convert Q18 output to calibrated Q21 and apply Manual Page 13 Low-Cut, High-Cut, and Level
     * Calibrated to provide equal perceived acoustic loudness between cab-engaged and cab-bypassed states */
    for (int i = 0; i < blk; i++) {
        int32_t x = (int32_t)(((int64_t)acc_out[i] * 280000LL + (1LL << 17)) >> 18);

        /* Low-cut HPF (20 Hz .. 300 Hz) and high-cut LPF (18 kHz .. 5 kHz),
         * both second order now (12 dB/octave). */
        int32_t hp = cab_biquad_run(&s->hp, x);
        int32_t lp = cab_biquad_run(&s->lp, hp);

        buf[i] = sat32(q21_mul(q21_mul(lp, s->level_q21), s->trim_q21));
        if (i == 0) PA_ISR_CP(0x75);               /* the first output sample was computed */
    }
    PA_ISR_CP(0x72);                               /* immediately before returning from cab_ir_process_block */
}

/* The default cabinet: a synthesised closed-back 4x12 (tools/gen_test_audio.py),
 * 512 taps in Q18, scaled to CAB_REF_L2_Q18. */
static const int32_t k_default_cab_ir[512] = {
#include "cab_default_table.inc"
};

/* The L2 norm of the reference cabinet, in Q18 - the energy every user IR is
 * normalised to. The default cab is generated at exactly this norm, so the stock
 * cabs keep exactly the level they have always had. */
#define CAB_REF_L2_Q18 400150.0

/* ---- Remembered cabinet -------------------------------------------------------
 * The node state is FREED on bypass (PA_FEAT_FREE_ON_BYPASS) and re-created on the
 * next enable. Previously create() always reloaded the default IR, so every
 * cab off/on cycle silently replaced the user's cabinet with the default while the UI
 * kept showing the selected name - and fx_cab_ir_load_q18() early-returned when
 * the state was gone, so changing the model while bypassed was dropped entirely.
 * The selected IR and its trim are therefore remembered OUTSIDE the node state and
 * restored on create. */
static int32_t  s_sel_taps[PA_STOCK_CAB_TAPS];   /* 4 KB, the IR the user selected */
static uint32_t s_sel_num_taps = 0;
static int32_t  s_sel_trim_q21 = PA_Q21_ONE;

/* How many samples at the end of a TRUNCATED impulse response are faded out.
 * A hard cut leaves a step discontinuity, which rings in the frequency domain
 * (comb filtering, a metallic edge). A half-Hann over the last 64 samples
 * makes the truncation land on zero instead. */
#define CAB_TAIL_WINDOW 64u

static void cab_ir_apply_taps(cab_ir_state_t *s, const int32_t *taps_q18, uint32_t num_taps) {
    const uint32_t max_active_taps = CAB_MAX_PARTS * CAB_PART_LEN;
    const int truncated = (num_taps > max_active_taps);
    if (num_taps > max_active_taps) num_taps = max_active_taps;

    for (uint32_t i = 0; i < PA_IR_MAX_TAPS; i++) {
        s->ir_q18[i] = (i < num_taps) ? taps_q18[i] : 0;
    }

    /* --- drop the pre-delay -------------------------------------------------
     * A mic capture starts with a few tenths of a millisecond to two
     * milliseconds of air travel before the first transient. Those zeros eat
     * into the tap budget and delay the whole tail, so find the onset and
     * start there. The threshold is 40 dB below the peak, and at most a
     * quarter of the response is ever dropped. */
    if (num_taps > 8u) {
        int32_t peak = 0;
        for (uint32_t i = 0; i < num_taps; i++) {
            int32_t a = taps_q18[i] < 0 ? -taps_q18[i] : taps_q18[i];
            if (a > peak) peak = a;
        }
        if (peak > 0) {
            const int32_t thresh = peak / 100;      /* -40 dB */
            uint32_t onset = 0;
            while (onset < num_taps) {
                int32_t a = taps_q18[onset] < 0 ? -taps_q18[onset] : taps_q18[onset];
                if (a >= thresh) break;
                onset++;
            }
            const uint32_t max_trim = num_taps / 4u;
            if (onset > max_trim) onset = max_trim;
            /* Preserve 8 samples (~0.18 ms) of pre-roll so the transient wavefront
             * rises smoothly from zero without an abrupt step discontinuity at tap 0 */
            if (onset > 8u) onset -= 8u;
            else onset = 0u;
            if (onset > 0u) {
                for (uint32_t i = 0; i + onset < num_taps; i++) {
                    s->ir_q18[i] = taps_q18[i + onset];
                }
                for (uint32_t i = num_taps - onset; i < PA_IR_MAX_TAPS; i++) {
                    s->ir_q18[i] = 0;
                }
                num_taps -= onset;
            }
        }
    }

    /* --- fade the tail of a TRUNCATED response ------------------------------
     * Only when the response did not fit: a complete capture already ends
     * where it ends, and fading it would just make it shorter. */
    if (truncated && num_taps > CAB_TAIL_WINDOW) {
        const uint32_t start = num_taps - CAB_TAIL_WINDOW;
        for (uint32_t i = 0; i < CAB_TAIL_WINDOW; i++) {
            /* a half-Hann: 1 -> 0 across the window */
            const float w = 0.5f * (1.0f + cosf(3.14159265f * (float)i / (float)CAB_TAIL_WINDOW));
            s->ir_q18[start + i] = (int32_t)lrintf((float)s->ir_q18[start + i] * w);
        }
    }

    uint32_t parts = (num_taps + CAB_PART_LEN - 1u) / CAB_PART_LEN;
    if (parts == 0u) parts = 1u;
    if (parts > CAB_MAX_PARTS) parts = CAB_MAX_PARTS;
    s->num_taps  = parts * CAB_PART_LEN;
    s->num_parts = parts;
#if PA_CAB_FFT
    cab_fft_load(s);
#endif
}

/* Make the node's stereo state match the remembered width (main context: it may take pool memory and build the spectra). */
static void cab_stereo_apply(fx_node_t *nd, cab_ir_state_t *s) {
#if PA_CAB_FFT
    if (s_width_pct == 0u) { nd->stereo_on = 0u; return; }       /* mono: nothing is allocated, nothing is built, the audio path is the old one */
    if (!s->h2) {
        s->h2 = (fc_cpx (*)[FC_BINS])pa_alloc((uint32_t)sizeof(fc_cpx) * FC_MAXP * FC_BINS);
        if (!s->h2) { nd->stereo_on = 0u; return; }              /* the pool is full (pa_alloc logged it): stay mono */
    }
    if (!s->h2_ready) { const uint32_t n = cab_ir_float(s); cab_h2_fill(s, n); }
    const float t = (float)s_width_pct * (1.0f / 100.0f) * 0.78539816f;     /* 0 .. 45 degrees */
    s->mix_c_tgt = cosf(t);
    s->mix_s_tgt = sinf(t);
    if (!nd->stereo_on) { s->mix_c = 1.0f; s->mix_s = 0.0f; }    /* start from mono and let the first block ramp to the width */
    nd->stereo_on = 1u;
#else
    (void)nd; (void)s;
#endif
}

void fx_cab_ir_set_stereo_width(fx_node_t *nd, uint8_t pct) {
#if !PA_CAB_FFT
    (void)nd; (void)pct;
    return;
#else
    s_width_pct = (pct > 100u) ? 100u : pct;
    if (!nd) return;
    cab_ir_state_t *s = (cab_ir_state_t *)nd->state;
    if (!s) { nd->stereo_on = 0u; return; }                      /* bypassed: applied on the next create() */
    cab_stereo_apply(nd, s);
#endif
}

static void cab_ir_create(fx_node_t *nd) {
    cab_ir_state_t *s = (cab_ir_state_t *)pa_alloc(sizeof(cab_ir_state_t));
    if (s) {
        /* Default speaker cabinet: the synthesised closed 4x12 (cab_default_table.inc) */
        s->num_taps  = 1024u;
        s->num_parts = 16u;      /* 16 x 64 = 1024 taps */
        for (uint32_t i = 0; i < 512u; i++) {
            s->ir_q18[i] = k_default_cab_ir[i];
        }
        for (uint32_t i = 512u; i < PA_IR_MAX_TAPS; i++) {
            s->ir_q18[i] = 0;
        }

        memset(s->hist, 0, sizeof(s->hist));
        s->level_q21    = PA_Q21_ONE;
        s->trim_q21     = PA_Q21_ONE;
        cab_biquad_set(&s->hp, 1, 20.0f, 0.7071f);     /* ~20 Hz low cut   */
        cab_biquad_set(&s->lp, 0, 18000.0f, 0.7071f);  /* ~18 kHz high cut */

        /* Restore the cabinet (and its trim) the user had selected before the
         * bypass, so toggling the cab off and on does not change the sound. */
        if (s_sel_num_taps > 0u) {
            cab_ir_apply_taps(s, s_sel_taps, s_sel_num_taps);
        }
        s->trim_q21 = s_sel_trim_q21;
#if PA_CAB_FFT
        cab_fft_load(s);
#endif
    }
    nd->state = s;
    nd->stereo_on = 0u;
    if (s) cab_stereo_apply(nd, s);
}

static void cab_ir_destroy(fx_node_t *nd) {
    if (nd->state) {
        nd->stereo_on = 0u;
#if PA_CAB_FFT
        if (((cab_ir_state_t *)nd->state)->h2) pa_free(((cab_ir_state_t *)nd->state)->h2);
#endif
        pa_free(nd->state);
        nd->state = 0;
    }
}

static void cab_ir_set_param(fx_node_t *nd, int id, int32_t val) {
    cab_ir_state_t *s = (cab_ir_state_t *)nd->state;
    if (!s) return;
    switch (id) {
        case CAB_PARAM_LEVEL:
            s->level_q21 = val;
            break;
        case CAB_PARAM_LOW_CUT:
            /* Q21 [0..1] -> 20 Hz .. 300 Hz, second order */
            {
                float f0 = 20.0f + (float)val / (float)PA_Q21_ONE * 280.0f;
                cab_biquad_set(&s->hp, 1, f0, 0.7071f);
            }
            break;
        case CAB_PARAM_HIGH_CUT:
            /* Q21 [0..1] -> 18 kHz .. 5 kHz, second order */
            {
                float f0 = 18000.0f - (float)val / (float)PA_Q21_ONE * 13000.0f;
                cab_biquad_set(&s->lp, 0, f0, 0.7071f);
            }
            break;
        default: break;
    }
}

static void cab_ir_reset(fx_node_t *nd) {
    cab_ir_state_t *s = (cab_ir_state_t *)nd->state;
    if (s) {
        memset(s->hist, 0, sizeof(s->hist));
        s->hp.z1 = 0; s->hp.z2 = 0;
        s->lp.z1 = 0; s->lp.z2 = 0;
        for (int i = 0; i < 4; i++) s->rz[i] = 0.0f;
    }
}

fx_node_t *fx_cab_ir_new(void) {
    static fx_node_t n;
    n.run          = cab_ir_run;
#if PA_CAB_FFT
    n.run_stereo   = cab_ir_run_stereo;
    n.stereo_opt   = 1;                   /* the chain runs it in stereo only while the width is set (and the load governor allows it) */
#else
    n.run_stereo   = 0;
#endif
    n.stereo_on    = 0;
    n.create       = cab_ir_create;
    n.destroy      = cab_ir_destroy;
    n.set_param    = cab_ir_set_param;
    n.reset        = cab_ir_reset;
    n.active       = 1;
    n.frame        = PA_BLOCK_N;
    n.tag          = "cab";
    n.tail_enabled = 0;
    n.tail_draining= 0;
    return &n;
}

/* IMP-02: the 20 stock IRs differ in loudness (pink-weighted 100 Hz-5 kHz). Trims are
 * relative to the group mean (guitar 0-14, bass 15-19) so the overall level does not move.
 * Up to 5.8 dB apart - Cab 11 (US Twin 2x12) is 2.8 dB under the guitar-cab
 * average, Cab 4 (Hot-Rod 4x12) 3.0 dB over. */
static const int8_t k_cab_trim_db_x10[20] = {
    -19,   2,  -3, -30,  12, -10,   9,  -6,  -7,   9,  28,   2,   9,   4,  -1,
    -18,   2,  -9,   8,  17
};
void fx_cab_ir_set_trim_db_x10(fx_node_t *nd, int16_t db_x10) {
    int32_t trim = (int32_t)(powf(10.0f, (float)db_x10 / 200.0f) * (float)PA_Q21_ONE);
    s_sel_trim_q21 = trim;          /* remember across a bypass */
    if (!nd || !nd->state) return;
    ((cab_ir_state_t *)nd->state)->trim_q21 = trim;
}
void fx_cab_ir_set_stock_trim(fx_node_t *nd, uint8_t stock_idx) {
    fx_cab_ir_set_trim_db_x10(nd, (stock_idx < 20u) ? k_cab_trim_db_x10[stock_idx] : 0);
}

/* Normalise an impulse response to a fixed L2 energy.
 *
 * The convolution scales the accumulator by a fixed factor, so a user-loaded
 * IR comes out at whatever energy it happened to be captured with: a hot IR
 * slams the output limiter, a quiet one is inaudible. The 20 stock cabs have
 * a measured trim table for exactly that reason, but a third-party IR has no
 * entry in it.
 *
 * This scales the taps so the response has the same L2 norm as the reference
 * cabinet, which is the same "one cab sounds as loud as another" rule the trim
 * table applies by hand. */
void fx_cab_ir_normalise_q18(int32_t *taps_q18, uint32_t num_taps) {
    if (!taps_q18 || num_taps == 0u) return;
    double acc = 0.0;
    for (uint32_t i = 0; i < num_taps; i++) {
        acc += (double)taps_q18[i] * (double)taps_q18[i];
    }
    if (acc <= 0.0) return;
    const double norm = sqrt(acc);
    const double target = (double)CAB_REF_L2_Q18;
    if (norm <= 0.0) return;
    const double k = target / norm;
    for (uint32_t i = 0; i < num_taps; i++) {
        taps_q18[i] = (int32_t)lrintf((double)taps_q18[i] * k);
    }
}

void fx_cab_ir_load_q18(fx_node_t *nd, const int32_t *taps_q18, uint32_t num_taps) {
    if (!nd || !taps_q18) return;
    uint32_t orig_taps = num_taps;
    uint32_t save_n = (num_taps > PA_STOCK_CAB_TAPS) ? PA_STOCK_CAB_TAPS : num_taps;
    /* Remember first, so a selection made while the cab is bypassed is not lost. */
    for (uint32_t i = 0; i < save_n; i++) s_sel_taps[i] = taps_q18[i];
    s_sel_num_taps = save_n;
    cab_ir_state_t *s = (cab_ir_state_t *)nd->state;
    if (!s) return;                 /* bypassed: restored on the next create() */
    cab_ir_apply_taps(s, taps_q18, orig_taps);
    for (uint32_t i = 0; i < s->num_taps && i < PA_STOCK_CAB_TAPS; i++) s_sel_taps[i] = s->ir_q18[i];
    s_sel_num_taps = s->num_taps;
    memset(s->hist, 0, sizeof(s->hist));
    s->hp.z1 = 0; s->hp.z2 = 0;
    s->lp.z1 = 0; s->lp.z2 = 0;
    for (int i = 0; i < 4; i++) s->rz[i] = 0.0f;
}
