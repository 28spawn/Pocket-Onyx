/* pa_cpu_meter.h — per-stage DSP load meter (roadmap E12).
 * Portable, I/O-free logic (host-testable). The only device dependency is the cycle source,
 * injected via cpu_meter_set_clock() so the same code runs on-device and in a PC simulation.
 * Budget basis: cycles/block = CPU_HZ * N / SAMPLE_RATE. */
#ifndef PA_CPU_METER_H
#define PA_CPU_METER_H
#include <stdint.h>

#define PA_METER_MAX_STAGES 8
enum pa_stage { PA_ST_NAM=0, PA_ST_EQ, PA_ST_DRIVE, PA_ST_DYN, PA_ST_DELAY, PA_ST_REVERB, PA_ST_MOD, PA_ST_MISC };

typedef uint32_t (*pa_cycle_fn)(void);   /* free-running cycle counter (wraps; deltas are fine) */

typedef struct {
    pa_cycle_fn clock;
    uint32_t    budget;                  /* cycles available per audio block */
    uint32_t    t0;                      /* begin timestamp (current stage) */
    uint32_t    acc[PA_METER_MAX_STAGES];/* cycles this block, per stage */
    uint32_t    ema[PA_METER_MAX_STAGES];/* smoothed cycles (Q0, EMA) for display */
    uint32_t    peak_permille;           /* peak load (permille 0..1000) with slow decay */
    uint16_t    peak_decay_ctr;          /* decay counter for peak hold */
    uint8_t     ema_shift;               /* EMA factor: new = old + (sample-old)>>shift */
} pa_cpu_meter_t;

void     cpu_meter_init(pa_cpu_meter_t*, uint32_t cpu_hz, uint32_t block_n, uint32_t sample_rate);
void     cpu_meter_set_clock(pa_cpu_meter_t*, pa_cycle_fn);
void     cpu_meter_begin(pa_cpu_meter_t*);                 /* call at stage entry */
void     cpu_meter_end  (pa_cpu_meter_t*, enum pa_stage);  /* call at stage exit  */
/* Add a KNOWN cycle count to a stage without needing a clock. Used by the
 * per-slot accounting in fx_graph.c, where the cost is a calibrated model
 * (include/pa_dsp_cost.h) so the device, the host emulator and the WebAssembly
 * build all report the same load. */
void     cpu_meter_add  (pa_cpu_meter_t*, enum pa_stage, uint32_t cycles);
void     cpu_meter_block_done(pa_cpu_meter_t*);            /* once per audio block: fold acc->ema, reset acc */
uint32_t cpu_meter_total_permille(const pa_cpu_meter_t*);  /* overall load, 0..1000 (‰) */
uint32_t cpu_meter_peak_permille(const pa_cpu_meter_t*);   /* instantaneous peak load with decay, 0..1000 (‰) */
uint32_t cpu_meter_stage_permille(const pa_cpu_meter_t*, enum pa_stage);
uint32_t cpu_meter_headroom_permille(const pa_cpu_meter_t*); /* free budget, ‰ (for bigger-NAM sizing) */

#endif /* PA_CPU_METER_H */
