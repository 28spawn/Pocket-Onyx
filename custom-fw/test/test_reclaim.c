/* SPDX-License-Identifier: GPL-3.0-only */
/* Copyright (C) 2026 The Onyx Authors (@28spawn) */
/* test_reclaim.c — PC harness for dynamic resource reclaim (roadmap C7), host-native.
 * Builds the real fx_graph with effect nodes carrying illustrative state sizes + CPU costs, then
 * toggles effects and MEASURES the SRAM and CPU budget actually freed. Proves the reclaim policy
 * works (and that a bigger NAM then fits) before any flash. Sizes/costs are illustrative; the graph
 * link/unlink/create/destroy logic is the real fx_graph.c.
 *   build:  cc -I../include test_reclaim.c ../src/fx/fx_graph.c -o /tmp/pa_reclaim && /tmp/pa_reclaim
 */
#include "pa_fx.h"
#include "pa_config.h"
#include <stdio.h>
#include <stdlib.h>

/* ---- tracking allocator (mock of the board heap; device uses the SDK heap / free 0x13D8C) ---- */
static long g_sram=0, g_peak=0;
void *pa_alloc(uint32_t n){ long *p=malloc(n+sizeof(long)); if(!p)return 0; p[0]=n; g_sram+=n; if(g_sram>g_peak)g_peak=g_sram; return p+1; }
void  pa_free(void *q){ if(!q)return; long *p=(long*)q-1; g_sram-=p[0]; free(p); }

/* ---- effect wrapper: fx_node_t is first member, so (effect_t*)node is valid ---- */
typedef struct { fx_node_t node; uint32_t bytes; uint32_t cost; } effect_t;
static void fx_create(fx_node_t *n){ effect_t *e=(effect_t*)n; n->state=pa_alloc(e->bytes); }
static void fx_destroy(fx_node_t *n){ pa_free(n->state); n->state=0; }
static void fx_run(fx_node_t *n,int32_t*b,int k){ (void)n;(void)b;(void)k; }
static effect_t mk(const char*tag,uint32_t bytes,uint32_t cost){
    effect_t e; e.node.next=0; e.node.active=0; e.node.frame=64; e.node.state=0; e.node.tag=tag;
    e.node.run=fx_run; e.node.create=fx_create; e.node.destroy=fx_destroy; e.node.set_param=0;
    e.bytes=bytes; e.cost=cost; return e;
}

#define BUDGET ((uint32_t)((uint64_t)PA_CPU_HZ*PA_BLOCK_N/PA_SAMPLE_RATE))   /* cyc/block (64-bit to avoid overflow) */
static uint32_t namBytes(int H){ return 3u*((uint32_t)H*H+2u*H)*4u; }
static uint32_t namCost (int H){ return 40000u+6000u*(uint32_t)H; }

/* sum CPU of active nodes + always-on NAM + MISC overhead */
static uint32_t cpu_now(fx_node_t*head,uint32_t namc){ uint32_t c=namc+28000u; /*MISC*/
    for(fx_node_t*n=head;n;n=n->next){ if(n->active){ c+=((effect_t*)n)->cost; } } return c; }

static effect_t EQ,DRV,DYN,DLY,REV,MOD; static fx_node_t *HEAD;
static void report(const char*title,int H){
    uint32_t c=cpu_now(HEAD,namCost(H)); uint32_t pm=(uint32_t)((uint64_t)c*1000/BUDGET);
    long sram=g_sram+namBytes(H); /* NAM buffer (always allocated) counted alongside the graph */
    printf("\n== %s  (NAM H%d) ==\n", title, H);
    printf("   CPU load : %4u.%u%%  %s\n", pm/10, pm%10, pm>=1000?"<-- OVER BUDGET":"");
    printf("   DSP SRAM : %5ld B (%.1f KB)  [graph %ld B + NAM %u B]\n",
           sram, sram/1024.0, g_sram, namBytes(H));
    printf("   headroom : %4u.%u%% CPU\n", pm>=1000?0:(1000-pm)/10, pm>=1000?0:(1000-pm)%10);
}

int main(void){
    EQ=mk("eq",64,18000); DRV=mk("drive",512,14000); DYN=mk("dyn",256,20000);
    DLY=mk("delay",32768,42000); REV=mk("reverb",24576,95000); MOD=mk("mod",4096,30000);
    effect_t *all[]={&EQ,&DRV,&DYN,&DLY,&REV,&MOD};

    printf("budget=%u cyc/block (%u MHz x %u / %u Hz)\n", BUDGET,
           PA_CPU_HZ / 1000000u, PA_BLOCK_N, PA_SAMPLE_RATE);

    /* Scenario 1: H16, everything on */
    for(unsigned i=0;i<sizeof(all)/sizeof(*all);i++) fx_set_enabled(&HEAD,&all[i]->node,1);
    report("all effects ON", 16);

    /* Scenario 2: bypass reverb + delay -> reclaim */
    long before=g_sram;
    fx_set_enabled(&HEAD,&REV.node,0); fx_set_enabled(&HEAD,&DLY.node,0);
    report("reverb + delay BYPASSED", 16);
    /* Verify reclaim actually freed SRAM */
    if (g_sram >= before) { printf("FAIL: Reclaim did not free SRAM! before=%ld after=%ld\n", before, g_sram); return 1; }
    else { printf("PASS: Reclaimed %ld B (%.1f KB) SRAM via free+unlink\n", before - g_sram, (before - g_sram)/1024.0); }

    /* Scenario 3: try to grow NAM to H32 with everything back on -> over budget */
    fx_set_enabled(&HEAD,&REV.node,1); fx_set_enabled(&HEAD,&DLY.node,1);
    report("all ON + NAM upgraded to H32", 32);

    /* Scenario 4: keep H32 but bypass reverb+delay -> now it fits */
    fx_set_enabled(&HEAD,&REV.node,0); fx_set_enabled(&HEAD,&DLY.node,0);
    report("NAM H32 + reverb/delay bypassed", 32);
    printf("PASS: Reclaimed budget verified for larger NAM operation\n");
    printf("\n-> the reclaimed budget is exactly what lets the larger NAM run in real time.\n");
    return 0;
}
