/*
 * custom-fw/bench/bench_main.c - A2-Lite WaveNet cycle benchmark, run from RAM in the
 * JieLi ROM download mode. It never touches the flash.
 *
 * The host (tools/run_bench.py) uploads this image to 0x01C02000, writes g_bench_cfg,
 * jumps to bench_entry, and when it returns reads g_bench_res back over USB. A second
 * jump with g_bench_cfg.lcd = 2 draws those results on the LCD.
 *
 * What it measures, all with Timer4 (24 MHz crystal) and min-of-N repetitions:
 *   - the core clock: 64 dependent integer adds per loop iteration, 1 cycle each;
 *   - float add / multiply latency, and float add throughput (4 independent sums);
 *   - pa_wavenet_step_sample()        (baseline, the firmware's code today)
 *   - pa_wavenet_step_sample_fast()   (bit-exact rewrite, 3 independent accumulators)
 *   - pa_wavenet_step_sample_fast6()  (6 accumulators, reordered additions)
 *   and checks on the device that fast == baseline bit for bit, and fast6 is close;
 *   - fixed point vs float (bench_dsp.c): the amp's 4-biquad tone stack in Q21 vs float,
 *     float biquads on denormals, the 1,024-tap cab (firmware's direct Q18 path vs a float
 *     FFT convolution prototype), and the stock GRU engine - all in software, the
 *     unverified hardware accelerator is never touched;
 *   - the DRV block per model and the MOD block per type (bench_fx.c, the firmware's own
 *     fx_drive.c / fx_mod.c), for include/pa_dsp_cost.h (run_bench.py --update-cost).
 *
 * Everything (code and data) is in SRAM here, so this measures the arithmetic, not XIP
 * flash fetches. The flash-resident firmware will be slower by whatever XIP costs.
 *
 * Timer4 is the firmware's own timer (main.c). Its registers are saved and restored, so
 * the ROM finds it as it left it. Interrupts are not touched.
 */
#include <stdint.h>
#include <string.h>
#include "pa_wavenet.h"
#include "pa_wavenet_fast.h"
#include "bench_dsp.h"
#include "bench_fx.h"

extern uint32_t bench_alu_loop(uint32_t iters);
extern uint32_t bench_fadd_dep_loop(uint32_t iters);
extern uint32_t bench_fmul_dep_loop(uint32_t iters);
extern uint32_t bench_fadd_ind_loop(uint32_t iters);

extern void st7735s_init(void);
extern void ui_draw_str_scaled(uint16_t x, uint16_t y, const char *s, uint16_t fg, uint16_t bg, uint8_t scale);
extern void st7735s_fill_rect(uint16_t x, uint16_t y, uint16_t w, uint16_t h, uint16_t rgb565);

#define T4_CON (*(volatile uint32_t *)0x00010800u)
#define T4_CNT (*(volatile uint32_t *)0x00010804u)
#define T4_PRD (*(volatile uint32_t *)0x00010808u)

#define BENCH_MAX_N   2048
#define NVAR          3

/* ---- shared with the host: in .data so they are part of the uploaded image ---------- */
typedef struct {
    char     magic[8];        /* "PABCFG01" */
    uint32_t samples;         /* samples per timed run (<= BENCH_MAX_N) */
    uint32_t reps;            /* repetitions; the minimum is kept */
    uint32_t loop_iters;      /* iterations of each 64-instruction timing loop */
    uint32_t lcd;             /* 0 = measure, 2 = draw the stored results on the LCD */
    uint32_t seed;
} bench_cfg_t;

typedef struct {
    char     magic[8];        /* "PABENCH3" when done (3: + the DRV / MOD block) */
    uint32_t status;          /* 0 running, 1 done, 0xE00x error */
    uint32_t timer_hz;        /* 24,000,000 */
    uint32_t samples, reps, loop_iters, insns_per_iter;
    uint32_t alu_ticks, fadd_dep_ticks, fmul_dep_ticks, fadd_ind_ticks;   /* min over reps */
    uint32_t wn_ticks_min[NVAR];        /* base, fast, fast6 */
    uint32_t wn_ticks_sum[NVAR];
    uint32_t fast_mismatches;           /* samples where fast != base, bit for bit */
    uint32_t fast6_maxdiff_bits;        /* float, max |fast6 - base| */
    uint32_t out_bits_last[NVAR];       /* last output of each variant (sanity) */
    uint32_t model_bytes;
    uint32_t lcd_drawn;
    uint32_t lcd_spi_timeouts;
    bench_dsp_res_t dsp;                /* fixed point vs float: biquads, cab, GRU */
    bench_fx_res_t  fx;                 /* DRV per model, MOD per type */
} bench_res_t;

__attribute__((used)) bench_cfg_t g_bench_cfg = { "PABCFG01", 256, 3, 2000, 0, 12345u };
__attribute__((used)) bench_res_t g_bench_res = { "PABRUN00", 0 };

/* ---- the platform functions st7735s.c needs ----------------------------------------- */
uint32_t plat_ticks24(void) { return T4_CNT; }
void plat_delay_us(uint32_t us) { uint32_t s = T4_CNT, n = us * 24u; while ((uint32_t)(T4_CNT - s) < n) {} }
void plat_delay_ms(uint32_t ms) { while (ms--) plat_delay_us(1000u); }
uint32_t plat_ms(void) { return T4_CNT / 24000u; }
extern uint32_t st7735s_spi_timeouts(void);

/* ---- state (zeroed by bench_main, not part of the image) ---------------------------- */
static pa_wavenet_model_t s_model;
static float s_packed[PA_WN_FAST_PACKED_FLOATS];
static float s_in[BENCH_MAX_N];
static float s_out[NVAR][BENCH_MAX_N];

static uint32_t s_rng;
static float frand(void) {
    s_rng = s_rng * 1664525u + 1013904223u;
    return (float)((s_rng >> 8) & 0xFFFFFFu) * (2.0f / 16777216.0f) - 1.0f;
}

/* One signature for all three, so the timed loop is the same code for each. */
typedef float (*step_fn)(pa_wavenet_model_t *, const float *, float);
static float step_base(pa_wavenet_model_t *m, const float *p, float x) { (void)p; return pa_wavenet_step_sample(m, x); }

static uint32_t time_loop(uint32_t (*fn)(uint32_t), uint32_t iters, uint32_t reps) {
    uint32_t best = 0xFFFFFFFFu;
    for (uint32_t r = 0; r < reps; r++) {
        uint32_t t0 = T4_CNT;
        (void)fn(iters);
        uint32_t dt = T4_CNT - t0;
        if (dt < best) best = dt;
    }
    return best;
}

static void utoa10(uint32_t v, char *out) {
    char t[12]; int n = 0;
    do { t[n++] = (char)('0' + v % 10u); v /= 10u; } while (v);
    while (n) *out++ = t[--n];
    *out = 0;
}

static void line(uint16_t y, const char *label, uint32_t v, const char *unit) {
    char buf[40], num[12];
    utoa10(v, num);
    size_t i = 0;
    for (const char *p = label; *p && i < 20; p++) buf[i++] = *p;
    for (const char *p = num;   *p && i < 30; p++) buf[i++] = *p;
    for (const char *p = unit;  *p && i < 39; p++) buf[i++] = *p;
    buf[i] = 0;
    ui_draw_str_scaled(8, y, buf, 0xFFFF, 0x0000, 2);
}

/* Draw stored results. Cycles/sample = ticks * (cycles per tick) / samples, with cycles
 * per tick from the integer-add loop (64 one-cycle adds per iteration). */
static void draw_results(const bench_res_t *r) {
    uint64_t cyc_num = (uint64_t)r->loop_iters * 64u;              /* cycles in the alu loop */
    uint32_t alu = r->alu_ticks ? r->alu_ticks : 1u;
    uint32_t mhz = (uint32_t)(cyc_num * 24u / alu);
    static const char *const names[NVAR] = { "BASE  ", "FAST  ", "FAST6 " };
    st7735s_init();
    st7735s_fill_rect(0, 0, 240, 240, 0x0000);
#define CPS(t) ((uint32_t)((uint64_t)(t) * cyc_num / ((uint64_t)alu * r->samples)))
    line(0, r->fast_mismatches ? "EXACT NO SMP " : "EXACT OK SMP ", r->samples, "");
    line(24, "CLK ", mhz, " MHZ");
    for (int v = 0; v < NVAR; v++)
        line((uint16_t)(48 + v * 24), names[v], CPS(r->wn_ticks_min[v]), " CYC");
    line(120, "BQ Q21  ", CPS(r->dsp.bq_q21_ticks), " CYC");
    line(144, "BQ FLT  ", CPS(r->dsp.bq_f_ticks), " CYC");
    line(168, "CAB DIR ", CPS(r->dsp.cab_direct_ticks), " CYC");
    line(192, "CAB FFT ", CPS(r->dsp.cab_fft_ticks), " CYC");
    line(216, "GRU     ", CPS(r->dsp.gru_ticks), " CYC");
#undef CPS
}

/*
 * cfg.lcd selects the mode:
 *   0  measure, store the results in g_bench_res, return (no LCD)
 *   2  draw the results already in g_bench_res on the LCD, return (no measuring)
 * The host runs them as two separate jumps, so a slow LCD cannot push the measuring run
 * past the ROM's 5-second USB command timeout, and a stuck LCD cannot lose the results.
 */
void bench_main(void) {
    extern char _bss_start[], _bss_end[];
    memset(_bss_start, 0, (size_t)(_bss_end - _bss_start));

    bench_cfg_t cfg = g_bench_cfg;
    bench_res_t *r = &g_bench_res;
    const int draw_only = (cfg.lcd == 2u);

    if (!draw_only) {
        memset(r, 0, sizeof *r);
        memcpy(r->magic, "PABRUN00", 8);
        r->timer_hz = 24000000u;
    }
    if (memcmp(cfg.magic, "PABCFG01", 8) != 0) {
        if (draw_only) r->lcd_drawn = 0xE001u; else r->status = 0xE001u;
        return;
    }

    /* Timer4: save, start (24 MHz crystal, free-running), check it counts. */
    const uint32_t con0 = T4_CON, prd0 = T4_PRD, cnt0 = T4_CNT;
    T4_CON = 0; T4_PRD = 0xFFFFFFFFu; T4_CNT = 0;
    T4_CON = (2u << 2) | 1u | (1u << 14);
    {
        uint32_t a = T4_CNT;
        for (volatile int i = 0; i < 20000; i++) {}
        if (T4_CNT == a) {
            if (draw_only) r->lcd_drawn = 0xE002u; else r->status = 0xE002u;
            goto restore;
        }
    }

    if (draw_only) {
        if (r->status == 1u && memcmp(r->magic, "PABENCH3", 8) == 0) {
            draw_results(r);
            r->lcd_drawn = 1u;
            r->lcd_spi_timeouts = st7735s_spi_timeouts();
        } else {
            r->lcd_drawn = 0xE005u;                    /* nothing measured yet */
        }
        goto restore;
    }

    if (cfg.samples == 0 || cfg.samples > BENCH_MAX_N) cfg.samples = 256;
    if (cfg.reps == 0 || cfg.reps > 20) cfg.reps = 3;
    if (cfg.loop_iters == 0) cfg.loop_iters = 2000;
    r->samples = cfg.samples; r->reps = cfg.reps;
    r->loop_iters = cfg.loop_iters; r->insns_per_iter = 64;
    r->model_bytes = sizeof s_model;

    /* 1. The core clock and the FPU. */
    r->alu_ticks      = time_loop(bench_alu_loop,      cfg.loop_iters, cfg.reps);
    r->fadd_dep_ticks = time_loop(bench_fadd_dep_loop, cfg.loop_iters, cfg.reps);
    r->fmul_dep_ticks = time_loop(bench_fmul_dep_loop, cfg.loop_iters, cfg.reps);
    r->fadd_ind_ticks = time_loop(bench_fadd_ind_loop, cfg.loop_iters, cfg.reps);

    /* 2. A2-Lite with a realistic (random, finite, non-saturating) set of weights. */
    {
        static float w[PA_WN_LITE_WEIGHTS];
        s_rng = cfg.seed;
        for (int i = 0; i < PA_WN_LITE_WEIGHTS; i++) w[i] = 0.15f * frand();
        w[PA_WN_LITE_WEIGHTS - 1] = 0.8f;
        pa_wavenet_init_default(&s_model);
        if (pa_wavenet_load_weights(&s_model, w, PA_WN_LITE_WEIGHTS, 0.8f) != 0) { r->status = 0xE003u; goto restore; }
        if (pa_wavenet_fast_pack(&s_model, s_packed) != 0) { r->status = 0xE004u; goto restore; }
        /* a guitar-like input: a sine plus a little noise, no libm */
        float s = 0.0f, c = 0.7f, k = 0.0571f;
        for (uint32_t i = 0; i < cfg.samples; i++) {
            s += k * c; c -= k * s;
            s_in[i] = 0.9f * s + 0.02f * frand();
        }
    }
    static const step_fn fns[NVAR] = { step_base, pa_wavenet_step_sample_fast,
                                       pa_wavenet_step_sample_fast6 };
    for (int v = 0; v < NVAR; v++) {
        uint32_t best = 0xFFFFFFFFu, sum = 0;
        for (uint32_t rep = 0; rep < cfg.reps; rep++) {
            pa_wavenet_reset(&s_model);                       /* same start state every run */
            float *out = s_out[v];
            step_fn f = fns[v];
            uint32_t t0 = T4_CNT;
            for (uint32_t i = 0; i < cfg.samples; i++) out[i] = f(&s_model, s_packed, s_in[i]);
            uint32_t dt = T4_CNT - t0;
            if (dt < best) best = dt;
            sum += dt;
        }
        r->wn_ticks_min[v] = best;
        r->wn_ticks_sum[v] = sum;
        memcpy(&r->out_bits_last[v], &s_out[v][cfg.samples - 1], 4);
    }
    {
        float maxd = 0.0f;
        for (uint32_t i = 0; i < cfg.samples; i++) {
            if (memcmp(&s_out[0][i], &s_out[1][i], 4) != 0) r->fast_mismatches++;
            float d = s_out[2][i] - s_out[0][i];
            if (d < 0) d = -d;
            if (d > maxd) maxd = d;
        }
        memcpy(&r->fast6_maxdiff_bits, &maxd, 4);
    }
    /* 3. Fixed point vs float: biquads, cab, GRU (n must be a multiple of 64). */
    {
        uint32_t n_dsp = cfg.samples & ~63u;
        if (n_dsp == 0) n_dsp = 64;
        int rc = bench_dsp_run(&r->dsp, s_in, n_dsp, cfg.reps);
        if (rc != 0) { r->status = (uint32_t)rc; goto restore; }
        /* 4. The DRV block per model, the MOD block per type (same n). */
        rc = bench_fx_run(&r->fx, s_in, n_dsp, cfg.reps);
        if (rc != 0) { r->status = (uint32_t)rc; goto restore; }
    }
    memcpy(r->magic, "PABENCH3", 8);
    r->status = 1;

restore:
    T4_CON = 0;
    T4_PRD = prd0;
    T4_CNT = cnt0;
    T4_CON = con0;
}
