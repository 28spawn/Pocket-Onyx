/*
 * src/board/flash_probe.c - PA_FLASH_PROBE: a READ-ONLY survey of the peripheral register space, to find the SPI flash
 * controller (see custom-fw/FLASH-ACCESS.md, section 4). OFF by default.
 *
 * What it does, once, from the main loop (called a few seconds after boot, "<" held at power-on skips it):
 *   - copies the first 64 bytes at 0x02000000 and at 0x02000120 (twice each) into the probe block. Our own image is known, so
 *     tools/jl_ram_read.py show_flash_probe checks them against build_app.bin: this settles
 *     whether the XIP window maps flash 0x4000 or 0x5000 with no guessing;
 *   - walks a table of 72 windows of 10 words (40 B) over the peripheral space 0x10000..0x13FFF (stride 0x100) and a few
 *     extra spots, with plain 32-bit loads, and stores them.
 * It NEVER stores to a peripheral, to flash or to anything outside the probe block.
 *
 * A load from an address that is not decoded may raise a bus fault. Before every window the block says which window is in
 * progress; the fault handler (main.c, PA_FAULT_HANDLER) records the crash and resets into the ROM download mode with the SRAM
 * intact. When the app is started again (warm, no power-off) the probe sees the window that was in progress, marks it
 * FAULT and carries on with the next one, so a few runs sweep everything. A build id in the header restarts the sweep after a
 * new flash. Windows the app itself uses with side effects on reads (the LCD SPI1 FIFO, 0x11D00) are never read.
 *
 * Block: SRAM 0x01C7F000, 3,152 bytes. That is the result area of the retired wn_op_probe (nam_engine.c, "not run any more"), inside the NOINIT region; the
 * pool now runs to 0x01C73EE0 and the stack fill rewrites 0x01C74100.. on every boot, so no other room exists. Re-check the neighbours (grep 0x01C7F) when the map changes.
 */
#include <stdint.h>
#include "../../include/pa_plat.h"
#include "../../include/pa_build_id.h"

#ifndef PA_FLASH_PROBE
#define PA_FLASH_PROBE 0
#endif

void pa_flash_probe_run(void);

#if PA_FLASH_PROBE && !defined(PA_HOST_SIM) && !defined(PA_HOST_BUILD) && !defined(PA_EMU_DLL)

#define FP_BASE      0x01C7F000u
#define FP_MAGIC     0x42525046u        /* "FPRB" */
#define FP_NWIN      72u
#define FP_WWORDS    10u
#define FP_ST_PEND   0u
#define FP_ST_PROG   0x50u              /* "P": in progress (a fault leaves this behind) */
#define FP_ST_DONE   1u
#define FP_ST_FAULT  2u
#define FP_ST_SKIP   3u
#define FP_DONE_ALL  0x444F4E45u        /* "DONE" */

/* Block layout (mirrored by tools/jl_ram_read.py FP_*): words 0 magic, 1 version, 2 build id, 3 state, 4 current window, 5 runs (boots that ran the probe),
 * 6 windows faulted, 7 XIP words that differed between two reads, 8 ticks the sweep took, 9..15 spare; 16..31 XIP[0x02000000], 32..47 XIP[0x02000120]
 * (first 16 words each); 48..67 the status byte of each window (80 bytes); 68.. the data: window i at 68 + 10*i. 788 words. */
typedef struct {
    uint32_t magic, version, build, state, cur, runs, nfault, xip_diff, ticks;
    uint32_t pad0[7];
    uint32_t xip0[16], xip1[16];
    uint8_t  status[FP_NWIN + 8];
    uint32_t data[FP_NWIN][FP_WWORDS];
} fp_block_t;
typedef char fp_size_check[(sizeof(fp_block_t) <= 0xD00u) ? 1 : -1];     /* must stay below 0x01C7FD00 (0x01C7FD6A holds the SARADC trim byte) */
#define FP ((volatile fp_block_t *)(uintptr_t)FP_BASE)

static uint32_t window_base(uint32_t i) {
    static const uint32_t extra[8] = { 0x00016A00u, 0x00040000u, 0x00040200u, 0x00040400u, 0x000119A0u, 0x00011C80u, 0x00013100u, 0x00010800u };
    return (i < 64u) ? 0x00010000u + 0x100u * i : extra[i - 64u];
}
static int window_forbidden(uint32_t base) {
    return base == 0x00011D00u;         /* LCD SPI1 (CON/BAUD/BUF/ADR/CNT): the display driver uses it, BUF is a FIFO */
}

void pa_flash_probe_run(void) {
    const uint32_t build = PA_BUILD_ID;
    if (FP->magic != FP_MAGIC || FP->version != 2u || FP->build != build) {
        for (uint32_t i = 0; i < sizeof(fp_block_t) / 4u; i++) ((volatile uint32_t *)(uintptr_t)FP_BASE)[i] = 0u;
        FP->version = 2u; FP->build = build; FP->runs = 0u; FP->magic = FP_MAGIC;
    }
    if (FP->state == FP_DONE_ALL) return;                           /* the sweep is complete: leave the data for the readout */
    FP->runs++;
    const uint32_t t0 = plat_ticks24();
    for (uint32_t i = 0; i < FP_NWIN; i++) {
        if (FP->status[i] == FP_ST_PROG) { FP->status[i] = FP_ST_FAULT; FP->nfault++; }     /* the last run died here */
    }
    /* the XIP window (our own image, known): two separate reads of each, so a flaky read shows as a difference */
    {
        const volatile uint32_t *x0 = (const volatile uint32_t *)(uintptr_t)0x02000000u;
        const volatile uint32_t *x1 = (const volatile uint32_t *)(uintptr_t)0x02000120u;
        uint32_t diff = 0u;
        for (uint32_t i = 0; i < 16u; i++) { FP->xip0[i] = x0[i]; FP->xip1[i] = x1[i]; }
        for (uint32_t i = 0; i < 16u; i++) diff += (x0[i] != FP->xip0[i]) + (x1[i] != FP->xip1[i]);
        FP->xip_diff = diff;
    }
    for (uint32_t i = 0; i < FP_NWIN; i++) {
        const uint32_t b = window_base(i);
        if (FP->status[i] != FP_ST_PEND) continue;                  /* done, faulted or skipped in an earlier run */
        if (window_forbidden(b)) { FP->status[i] = FP_ST_SKIP; continue; }
        FP->cur = i;
        FP->status[i] = FP_ST_PROG;                                 /* if the next loads fault, this mark stays */
        const volatile uint32_t *p = (const volatile uint32_t *)(uintptr_t)b;
        for (uint32_t k = 0; k < FP_WWORDS; k++) FP->data[i][k] = p[k];
        FP->status[i] = FP_ST_DONE;
    }
    FP->ticks = plat_ticks24() - t0;
    FP->state = FP_DONE_ALL;
}

#else
void pa_flash_probe_run(void) {}
#endif
