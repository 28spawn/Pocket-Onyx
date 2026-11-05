/* cpu_meter.c — per-stage DSP load meter (roadmap E12). Portable C, no device I/O here.
 * On-device: wire cpu_meter_set_clock() to the pi32v2 free-running cycle counter.
 * On host (PC sim): inject a mock clock. Rendering (ST7789) lives in ui_display.c. */
#include "pa_cpu_meter.h"

/* freestanding: no libc on the target */
static void pa_bzero(void *p, unsigned n){ unsigned char *b=(unsigned char*)p; while(n--) *b++=0; }

void cpu_meter_init(pa_cpu_meter_t *m, uint32_t cpu_hz, uint32_t block_n, uint32_t sample_rate) {
    pa_bzero(m, sizeof(*m));
    /* cycles available to process one block of block_n samples at sample_rate */
    m->budget = (uint32_t)(((uint64_t)cpu_hz * block_n) / sample_rate);
    if (m->budget == 0) m->budget = 1;
    m->ema_shift = 3;                 /* EMA ~ 1/8 step: stable but responsive on the display */
}

void cpu_meter_set_clock(pa_cpu_meter_t *m, pa_cycle_fn f) { m->clock = f; }

void cpu_meter_begin(pa_cpu_meter_t *m) { m->t0 = m->clock ? m->clock() : 0; }

void cpu_meter_end(pa_cpu_meter_t *m, enum pa_stage s) {
    if ((int)s >= (int)PA_METER_MAX_STAGES) return;
    uint32_t now = m->clock ? m->clock() : 0;
    m->acc[s] += (uint32_t)(now - m->t0);   /* unsigned delta: correct across counter wrap */
}

void cpu_meter_add(pa_cpu_meter_t *m, enum pa_stage s, uint32_t cycles) {
    if (!m || (int)s >= (int)PA_METER_MAX_STAGES) return;
    m->acc[s] += cycles;
}

void cpu_meter_block_done(pa_cpu_meter_t *m) {
    if (!m) return;
    uint32_t block_cycles = 0;
    for (int i = 0; i < PA_METER_MAX_STAGES; i++) {
        block_cycles += m->acc[i];
        /* EMA: ema += (acc - ema) >> shift  (signed step so it can fall) */
        int32_t d = (int32_t)m->acc[i] - (int32_t)m->ema[i];
        m->ema[i] = (uint32_t)((int32_t)m->ema[i] + (d >> m->ema_shift));
        m->acc[i] = 0;
    }

    if (m->budget > 0) {
        uint32_t cur = (uint32_t)(((uint64_t)block_cycles * 1000u) / m->budget);
        if (cur > 1000u) cur = 1000u;
        if (cur >= m->peak_permille) {
            m->peak_permille = cur;
            m->peak_decay_ctr = 0;
        } else {
            /* Hold peak, then decay ~1 permille per ~10 blocks (~14.5 ms at 64 samples) */
            if (++m->peak_decay_ctr >= 10u) {
                m->peak_decay_ctr = 0;
                if (m->peak_permille > cur) m->peak_permille--;
            }
        }
    }
}

static uint32_t sum_ema(const pa_cpu_meter_t *m) {
    uint32_t t = 0; for (int i=0;i<PA_METER_MAX_STAGES;i++) t += m->ema[i]; return t;
}

uint32_t cpu_meter_total_permille(const pa_cpu_meter_t *m) {
    if (!m || m->budget == 0u) return 0u;   /* the meter was never initialised */
    uint64_t t = sum_ema(m);
    uint32_t p = (uint32_t)((t * 1000u) / m->budget);
    return p > 1000u ? 1000u : p;          /* clamp (overrun = overload) */
}

uint32_t cpu_meter_peak_permille(const pa_cpu_meter_t *m) {
    if (!m || m->budget == 0u) return 0u;
    return m->peak_permille > 1000u ? 1000u : m->peak_permille;
}

uint32_t cpu_meter_stage_permille(const pa_cpu_meter_t *m, enum pa_stage s) {
    if (!m || m->budget == 0u) return 0u;
    if ((int)s >= (int)PA_METER_MAX_STAGES) return 0;
    uint32_t p = (uint32_t)(((uint64_t)m->ema[s] * 1000u) / m->budget);
    return p > 1000u ? 1000u : p;
}

uint32_t cpu_meter_headroom_permille(const pa_cpu_meter_t *m) {
    uint32_t used = cpu_meter_total_permille(m);
    return used >= 1000u ? 0u : (1000u - used);
}
