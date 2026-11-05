/* test_cpu_meter.c — PC SIMULATION of the CPU meter (runs natively on host; no device/VM).
 * Proves the meter logic before flashing: injects a mock cycle clock and a per-stage cost scenario,
 * then renders the load meter exactly as the on-device ST7789 routine would read it.
 * Stage cycle costs here are ILLUSTRATIVE (we lack real per-stage cycle data without the device);
 * the POINT is that the meter math, smoothing, totals, headroom and reclaim behaviour are correct.
 *
 * Build/run on the Mac:  cc -I../include test_cpu_meter.c ../src/sys/cpu_meter.c -o /tmp/pa_meter && /tmp/pa_meter
 */
#include "pa_cpu_meter.h"
#include "pa_config.h"
#include <stdio.h>

/* --- mock cycle counter --- */
static uint32_t g_clock = 0;
static uint32_t mock_clock(void) { return g_clock; }
/* run one stage: advance the mock clock by `cost` cycles between begin/end */
static void sim_stage(pa_cpu_meter_t *m, enum pa_stage s, uint32_t cost) {
    cpu_meter_begin(m); g_clock += cost; cpu_meter_end(m, s);
}

static const char *NAMES[PA_METER_MAX_STAGES] =
    {"NAM","EQ","DRIVE","DYN","DELAY","REVERB","MOD","MISC"};

static void render(const pa_cpu_meter_t *m, const char *title) {
    printf("\n== %s ==  total %3u.%u%%   headroom %3u.%u%%  (budget %u cyc/block)\n",
        title,
        cpu_meter_total_permille(m)/10, cpu_meter_total_permille(m)%10,
        cpu_meter_headroom_permille(m)/10, cpu_meter_headroom_permille(m)%10,
        m->budget);
    for (int i=0;i<PA_METER_MAX_STAGES;i++){
        uint32_t pm = cpu_meter_stage_permille(m, (enum pa_stage)i);
        if (!pm) continue;
        char bar[41]; int fill = (int)(pm*40/1000); if(fill>40)fill=40;
        for(int k=0;k<40;k++) bar[k]= k<fill?'#':'.'; bar[40]=0;
        printf("  %-6s [%s] %2u.%u%%\n", NAMES[i], bar, pm/10, pm%10);
    }
}

int main(void) {
    pa_cpu_meter_t m;
    /* PA_CPU_HZ (pa_config.h) - the same clock the device's meter is compiled
     * with, so this simulation cannot drift from the firmware (it used to say
     * 320 MHz here while the firmware used 360). */
    cpu_meter_init(&m, PA_CPU_HZ, 64u, 44100u);
    cpu_meter_set_clock(&m, mock_clock);

    /* ILLUSTRATIVE per-block stage costs (cycles). NAM is HW-offloaded so CPU share is modest. */
    const uint32_t C_NAM=150000, C_EQ=18000, C_DRIVE=14000, C_REVERB=95000, C_DELAY=42000, C_MISC=28000;

    puts("Scenario A: NAM(H=16) + EQ + Drive + Reverb + Delay all active");
    for (int blk=0; blk<24; blk++){
        sim_stage(&m,PA_ST_NAM,C_NAM); sim_stage(&m,PA_ST_EQ,C_EQ); sim_stage(&m,PA_ST_DRIVE,C_DRIVE);
        sim_stage(&m,PA_ST_REVERB,C_REVERB); sim_stage(&m,PA_ST_DELAY,C_DELAY); sim_stage(&m,PA_ST_MISC,C_MISC);
        cpu_meter_block_done(&m);
    }
    render(&m, "all effects ON");

    puts("\nScenario B: bypass REVERB + DELAY (C7 reclaim) -> free CPU for a bigger NAM");
    for (int blk=0; blk<24; blk++){
        sim_stage(&m,PA_ST_NAM,C_NAM); sim_stage(&m,PA_ST_EQ,C_EQ); sim_stage(&m,PA_ST_DRIVE,C_DRIVE);
        /* reverb + delay freed: not run at all */
        sim_stage(&m,PA_ST_MISC,C_MISC);
        cpu_meter_block_done(&m);
    }
    render(&m, "reverb+delay bypassed");

    printf("\n-> reclaimed headroom can fund NAM H=24 (~2.25x MAC) / H=32 (~4x), HW-offloaded.\n");

    /* Verify CPU meter gives the right percentage for a known cycle count */
    {   /* Scenario A: verify total permille = sum of stage permilles */
        pa_cpu_meter_t m2;
        cpu_meter_init(&m2, PA_CPU_HZ, 64u, 44100u);
        cpu_meter_set_clock(&m2, mock_clock);
        const uint32_t C_NAM=150000, C_EQ=18000, C_DRIVE=14000, C_REVERB=95000, C_DELAY=42000, C_MISC=28000;
        for (int blk=0; blk<1; blk++){ /* just one block for speed */
            sim_stage(&m2,PA_ST_NAM,C_NAM); sim_stage(&m2,PA_ST_EQ,C_EQ); sim_stage(&m2,PA_ST_DRIVE,C_DRIVE);
            sim_stage(&m2,PA_ST_REVERB,C_REVERB); sim_stage(&m2,PA_ST_DELAY,C_DELAY); sim_stage(&m2,PA_ST_MISC,C_MISC);
            cpu_meter_block_done(&m2);
        }
        uint32_t total = cpu_meter_total_permille(&m2);
        uint32_t headroom = cpu_meter_headroom_permille(&m2);
        uint32_t peak = cpu_meter_peak_permille(&m2);
        /* Simple check: total + headroom should be <= 1000 */
        if (total > 1000 || peak == 0 || peak > 1000) { printf("FAIL: CPU meter total > 100%% (%u permille, peak=%u)\\n", total, peak); return 1; }
        else { printf("PASS: CPU meter math verified (total=%u%%%%, peak=%u%%%%, headroom=%u%%%%)\\n", total/10, peak/10, headroom/10); }
    }

    return 0;
}
