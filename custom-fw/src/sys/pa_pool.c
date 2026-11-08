/* SPDX-License-Identifier: GPL-3.0-only */
/* Copyright (C) 2026 The Onyx Authors (@28spawn) */
/*
 * src/sys/pa_pool.c - deterministic static-pool allocator for effect state (see notes below).
 */
#include <stdint.h>
#include <string.h>
#include "pa_config.h"
#include "pa_evlog.h"
#ifdef PA_POOL_TEST
#define PA_POOL_ATTR __attribute__((aligned(8)))
#else
#define PA_POOL_ATTR __attribute__((section(".pool"), aligned(8)))
#endif

#if !defined(PA_HOST_SIM) || defined(PA_POOL_TEST)
/* Deterministic first-fit pool allocator with coalescing (no libc heap).
 * The old bump allocator's pa_free() was a no-op, so every effect enable/disable
 * leaked pool memory and a long session would eventually fail to create effects
 * (silent bypass). Blocks are 8-byte aligned; header is 8 bytes. Only called from
 * the UI/main context, never from the audio ISR. */
typedef struct pa_blk { uint32_t size; uint32_t used; } pa_blk_t; /* size includes header */
#define PA_POOL_LARGE 4096u     /* from this size (bytes, aligned) a block is placed at the top of the pool, see pa_alloc() */

static uint8_t g_dsp_pool[PA_DSP_POOL_BYTES] PA_POOL_ATTR;
static uint8_t g_pool_ready = 0;
static uint32_t g_pool_used = 0;
#define PA_POOL_HOMES 8u
static struct { uint32_t need, off; } g_homes[PA_POOL_HOMES];
uint32_t pa_pool_used_bytes(void) { return g_pool_used; }

/* For the readout (SRAM 0x01C7E780, device only): [0] "POOL", [1] bytes in use, [2] largest free block, [3] requests that failed, [4] size of the last failed request, [5] peak in use */
#if !defined(PA_HOST_BUILD) && !defined(PA_HOST_SIM) && !defined(PA_POOL_TEST)
#define POOL_STAT(i) (((volatile uint32_t *)(uintptr_t)0x01C7E780u)[(i)])
#else
static uint32_t s_pool_stat[8];
#define POOL_STAT(i) (s_pool_stat[(i)])
#endif

static void pool_init(void) {
    pa_blk_t *b = (pa_blk_t *)g_dsp_pool;
    b->size = sizeof(g_dsp_pool);
    b->used = 0;
    g_pool_ready = 1;
    for (uint32_t h = 0; h < PA_POOL_HOMES; h++) g_homes[h].need = 0u;
    for (int i = 0; i < 8; i++) POOL_STAT(i) = 0u;
    POOL_STAT(0) = 0x4C4F4F50u;                                  /* "POOL" */
}

static void pool_update_stats(void) {
    uint32_t used = 0, off = 0, largest = 0;
    while (off < sizeof(g_dsp_pool)) {
        pa_blk_t *b = (pa_blk_t *)&g_dsp_pool[off];
        if (b->used) used += b->size; else if (b->size > largest) largest = b->size;
        off += b->size;
    }
    g_pool_used = used;
    POOL_STAT(0) = 0x4C4F4F50u;                                  /* "POOL": written on every update (the word at 0x01C7E780 was lost after pool_init, and the readout said "not counted") */
    POOL_STAT(1) = used; POOL_STAT(2) = largest;
    if (used > POOL_STAT(5)) POOL_STAT(5) = used;
}

/* Homes. Each effect asks for the same large sizes every time (the delay line, the reverb tank, the cabinet and its spectra), so the pool remembers where
 * a block of that size last lived and puts it back there when that space is free: with the pool as full as it is (147,256 of 147,456 bytes with
 * every effect on) the layout has to come back exactly as it was, or a big block no longer finds a hole. A home is dropped when a different block takes
 * part of it. */

static pa_blk_t *pool_try_home(uint32_t need) {
    for (uint32_t h = 0; h < PA_POOL_HOMES; h++) {
        if (g_homes[h].need != need) continue;
        const uint32_t off = g_homes[h].off;
        uint32_t o = 0;
        pa_blk_t *c = (pa_blk_t *)g_dsp_pool;
        while (o + c->size <= off) { o += c->size; c = (pa_blk_t *)&g_dsp_pool[o]; }
        if (c->used || off + need > o + c->size) return 0;                    /* the home is not free right now */
        const uint32_t before = off - o;
        uint32_t after = (o + c->size) - (off + need);
        if (before != 0u && before < 16u + (uint32_t)sizeof(pa_blk_t)) return 0;   /* no room for a free block below it */
        uint32_t take = need;
        if (after != 0u && after < 16u + (uint32_t)sizeof(pa_blk_t)) { take += after; after = 0u; }   /* too small to stand alone: stays with the block */
        pa_blk_t *b = c;
        if (before) { c->size = before; b = (pa_blk_t *)&g_dsp_pool[off]; }
        b->size = take; b->used = 1;
        if (after) { pa_blk_t *r = (pa_blk_t *)&g_dsp_pool[off + take]; r->size = after; r->used = 0; }
        return b;
    }
    return 0;
}

static void pool_set_home(uint32_t need, uint32_t off, uint32_t size) {
    uint32_t slot = PA_POOL_HOMES;
    for (uint32_t h = 0; h < PA_POOL_HOMES; h++) {
        if (g_homes[h].need == 0u) { if (slot == PA_POOL_HOMES) slot = h; continue; }
        if (g_homes[h].need == need) { slot = h; continue; }
        /* another home that this block overlaps is dropped */
        const uint32_t ho = g_homes[h].off, he = ho + g_homes[h].need;
        if (off < he && ho < off + size) g_homes[h].need = 0u;
    }
    if (slot == PA_POOL_HOMES) return;
    g_homes[slot].need = need; g_homes[slot].off = off;
}

/* Every request that returns NULL is counted, with the size asked for last (the same words the readout at 0x01C7E780 shows): a slot that stays off because the pool was full is then
 * visible to a diagnostic or a host tool, not only to whoever reads the effect's screen. */
uint32_t pa_pool_alloc_fail_count(void) { return POOL_STAT(3); }
uint32_t pa_pool_alloc_fail_last_bytes(void) { return POOL_STAT(4); }
static void *pool_refuse(uint32_t n) { POOL_STAT(3)++; POOL_STAT(4) = n; return NULL; }

void *pa_alloc(uint32_t n) {
    if (!g_pool_ready) pool_init();
    if (n == 0u) return NULL;                                  /* not a failure: nothing was asked for */
    if (n > UINT32_MAX - 7u) return pool_refuse(n);
    uint32_t aligned_n = (n + 7u) & ~7u;
    if (aligned_n > UINT32_MAX - (uint32_t)sizeof(pa_blk_t)) return pool_refuse(n);
    uint32_t need = aligned_n + (uint32_t)sizeof(pa_blk_t);
    if (need > PA_DSP_POOL_BYTES) return pool_refuse(n);      /* bigger than the whole pool */
    /* Two ends. The pool is nearly full with every effect on (147,256 of 147,456 bytes with the stereo cabinet), so where a block goes decides whether the
     * next one fits. Large blocks (the delay line, the reverb tank, the cabinet and its spectra) are put at the TOP of the last free block that holds them,
     * small ones at the bottom of the first: the small states then never split the hole a large block left, and that hole is still whole when the same
     * effect is switched on again. First-fit for everything let a few small states into the cabinet's hole, and the cabinet (16,792 bytes) then could not
     * come back although the memory was free ("EFFECT MEMORY FULL ... largest free block 13,864"). */
    if (aligned_n >= PA_POOL_LARGE) {
        pa_blk_t *hb = pool_try_home(need);
        if (hb) {
            pool_set_home(need, (uint32_t)((uint8_t *)hb - g_dsp_pool), hb->size);
            pool_update_stats();
            memset((void *)(hb + 1), 0, hb->size - (uint32_t)sizeof(pa_blk_t));
            return (void *)(hb + 1);
        }
        uint32_t best = 0xFFFFFFFFu, o = 0;
        while (o < sizeof(g_dsp_pool)) {
            pa_blk_t *c = (pa_blk_t *)&g_dsp_pool[o];
            if (!c->used && c->size >= need) best = o;
            o += c->size;
        }
        if (best != 0xFFFFFFFFu) {
            pa_blk_t *c = (pa_blk_t *)&g_dsp_pool[best];
            pa_blk_t *b = c;
            if (c->size >= need + 16u + sizeof(pa_blk_t)) {          /* carve the block from the END of the hole, leave the rest free below it */
                const uint32_t rest = c->size - need;
                c->size = rest;
                b = (pa_blk_t *)&g_dsp_pool[best + rest];
                b->size = need;
            }
            b->used = 1;
            pool_set_home(need, (uint32_t)((uint8_t *)b - g_dsp_pool), b->size);
            pool_update_stats();
            memset((void *)(b + 1), 0, b->size - (uint32_t)sizeof(pa_blk_t));
            return (void *)(b + 1);
        }
    }
    uint32_t off = 0;
    while (off < sizeof(g_dsp_pool)) {
        pa_blk_t *b = (pa_blk_t *)&g_dsp_pool[off];
        if (!b->used && b->size >= need) {
            if (b->size >= need + 16u + sizeof(pa_blk_t)) { /* split */
                pa_blk_t *rest = (pa_blk_t *)&g_dsp_pool[off + need];
                rest->size = b->size - need;
                rest->used = 0;
                b->size = need;
            }
            b->used = 1;
            pool_update_stats();
            /* Every block comes back ZEROED. The pool reuses memory that other
             * effects released (a preset change frees and creates blocks), and the
             * effects start their filters from this state: with leftover bytes - a
             * reverb tank's samples, a delay line - a fresh drive started from
             * garbage (even NaN) and played silence after a few preset changes
             * (the "presets 41-80: the drive gives no audio" report). The host and
             * emulator builds use calloc, which is why only the device and the
             * WebAssembly build showed it. */
            memset((void *)(b + 1), 0, b->size - (uint32_t)sizeof(pa_blk_t));
            return (void *)(b + 1);
        }
        off += b->size;
    }
    POOL_STAT(3)++; POOL_STAT(4) = n;                       /* exhausted (or too fragmented): the caller gets NULL and the effect stays off, silently */
    pool_update_stats();
#if !defined(PA_HOST_BUILD) && !defined(PA_HOST_SIM) && !defined(PA_POOL_TEST)
    pa_evlog(PA_EV_POOL_FAIL, n, POOL_STAT(2));
#endif
    return 0; /* exhausted: caller must handle NULL */
}

void pa_free(void *p) {
    if (!p || !g_pool_ready) return;
    uint8_t *bp = (uint8_t *)p - sizeof(pa_blk_t);
    if (bp < g_dsp_pool || bp >= g_dsp_pool + sizeof(g_dsp_pool)) return; /* not ours */
    ((pa_blk_t *)bp)->used = 0;
    /* coalesce all adjacent free blocks */
    uint32_t off = 0;
    while (off < sizeof(g_dsp_pool)) {
        pa_blk_t *b = (pa_blk_t *)&g_dsp_pool[off];
        uint32_t next = off + b->size;
        if (!b->used && next < sizeof(g_dsp_pool)) {
            pa_blk_t *nb = (pa_blk_t *)&g_dsp_pool[next];
            if (!nb->used) { b->size += nb->size; continue; }
        }
        off = next;
    }
    pool_update_stats();
}
#endif

#if defined(PA_HOST_SIM) && !defined(PA_POOL_TEST)
/* the host / emulator allocator is the C heap: it is not the pool and has no counters */
uint32_t pa_pool_alloc_fail_count(void) { return 0u; }
uint32_t pa_pool_alloc_fail_last_bytes(void) { return 0u; }
#endif
