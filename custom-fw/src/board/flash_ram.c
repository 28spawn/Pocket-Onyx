/* custom-fw/src/board/flash_ram.c - the SPI NOR flash driver, step 1: a READ-ONLY probe that runs from SRAM.
 *
 * Why: the firmware cannot write its flash yet (pa_spi_nor_erase_sector / _write return -2: FLASH-ACCESS.md says the controller's registers were unknown to this repo). Optimist's open
 * Referencing open hardware documentation for AC791N SPI NOR flash (JEDEC 0x856014). Register map and timing sequences are implemented cleanly in original code. Our own readouts agree with it (SPI0_CON at 0x11C00 reads 0x1028 in the app; the chip answers 0x856014).
 *
 * What it does: with the interrupts off and core 1 not started yet (it runs at boot, before pa_smp_init), put the flash controller (SFC) in command mode, read the JEDEC id (0x9F), both status
 * registers (0x05, 0x35) and 16 bytes at three places (fast read 0x0B), put it back in XIP mode, and leave everything in the array pa_flprobe[] for jl_ram_read.py. Nothing is ever erased or
 * programmed: the commands used are all reads. While the SFC is in command mode NO instruction can be fetched from the flash, so everything between the two SFC_CON writes lives in the section
 * .flashfn (app.ld: SRAM at 0x01C75A00, copied from the flash image first), uses no call, no constant table and no string, and every wait is bounded; the first wait comes BEFORE anything
 * is changed, so a timeout there leaves the controller exactly as it was, and the restore (SFC_CON) is written even after a timeout. It is opt-in (main.c: '<' alone held at power-on).
 *
 * Registers (from the same reading): SFC_CON 0x40200 (bit 31 busy; bits 11:8 the read mode, 6 / 7 = continuous-read XIP: refused), SPI0_CON 0x11C00 (b15 PND, b14 PCLR, b12 DIR (1 = receive),
 * b3 around each transfer; the SDK's value 0x1029), SPI0_BUF 0x11C08, IOMAP_CON0 0x5101C (bit 5: 1 = the SFC owns the flash pins), PORTD 0x500C0 / 0x500C8 (PD0 = flash CS#), CACHE_CON
 * 0x1EEE008 (bit 14: idle). */
#include "pa_config.h"
#include "pa_build_id.h"
#include <stdint.h>
#include "pa_flash_guard.h"

#if !defined(PA_HOST_BUILD) && !defined(PA_HOST_SIM) && !defined(PA_EMU_DLL) && !defined(__wasm__)
#include <stdint.h>

#define FL_SFC_CON   (*(volatile uint32_t *)0x00040200u)
#define FL_SPI0_CON  (*(volatile uint32_t *)0x00011C00u)
#define FL_SPI0_BUF  (*(volatile uint32_t *)0x00011C08u)
#define FL_IOMAP0    (*(volatile uint32_t *)0x0005101Cu)
#define FL_PORTD_OUT (*(volatile uint32_t *)0x000500C0u)
#define FL_PORTD_DIR (*(volatile uint32_t *)0x000500C8u)
#define FL_CACHE_CON (*(volatile uint32_t *)0x01EEE008u)
#define FL_T4        (*(volatile uint32_t *)0x00010804u)       /* TIMER4, 24 MHz */

#define FLASHFN __attribute__((section(".flashfn"), noinline, used))
#define FLINL static inline __attribute__((always_inline))   /* (fl_cs / fl_tx / fl_rx are real functions in the same section: they stay next to the probe, so the copy keeps their relative calls valid) */

/* the results for jl_ram_read.py (the address is published in the BLE status block, word FL_PTR) */
enum { FLP_MAGIC, FLP_RC, FLP_ID, FLP_SR, FLP_SFC0, FLP_SFC1, FLP_SPI0, FLP_SPI1, FLP_IO0, FLP_IO1, FLP_US, FLP_STAGE, FLP_DATA, FLP_WORDS = FLP_DATA + 12 };
volatile uint32_t pa_flprobe[FLP_WORDS];
/* Where the probe got to, kept across a restart: [0] "FLT1", [1] the stage. If the probe hangs, the watchdog restarts the unit, pa_flprobe is cleared with the rest of .bss, and without this nothing would say where it stopped. */
volatile uint32_t pa_flt_log[3] __attribute__((section(".noinit.flt")));   /* [0] "FLT1", [1] stage, [2] the BUILD ID that wrote them: readable after any reset (Gate F0) */
#define STG(n) do { o[FLP_STAGE] = (n); pa_flt_log[1] = (n); } while (0)


extern uint8_t _ff_start[], _ff_end[], _ff_load[];

FLINL void fl_csync(void) { __asm__ volatile("csync" ::: "memory"); }
FLINL void fl_cli(void) { __asm__ volatile("cli" ::: "memory"); }
FLINL int fl_spi_wait(uint32_t *err)                                  /* bounded: 2 ms */
{
    const uint32_t t0 = FL_T4;
    int ok = 1;
    fl_csync();
    while ((FL_SPI0_CON & 0x8000u) == 0u)
        if ((uint32_t)(FL_T4 - t0) > 48000u) { ok = 0; break; }
    FL_SPI0_CON |= 0x4000u;                                          /* PCLR */
    if (!ok) *err = 1u;
    return ok;
}
FLASHFN static void fl_cs(int high)
{
    volatile uint32_t i;
    FL_PORTD_DIR &= ~1u;
    if (high) FL_PORTD_OUT |= 1u; else FL_PORTD_OUT &= ~1u;
    for (i = 10u; i != 0u; i--) { }
}
FLASHFN static void fl_tx(uint32_t b, uint32_t *err)
{
    FL_SPI0_CON |= 0x08u;
    FL_SPI0_CON &= ~0x1000u;
    FL_SPI0_BUF = b;
    fl_spi_wait(err);
    FL_SPI0_CON &= ~0x08u;
}
FLASHFN static uint32_t fl_rx(uint32_t *err)
{
    uint32_t v;
    FL_SPI0_CON |= 0x08u;
    FL_SPI0_CON |= 0x1000u;
    FL_SPI0_BUF = 0xFFu;
    fl_spi_wait(err);
    v = FL_SPI0_BUF & 0xFFu;
    FL_SPI0_CON &= ~0x08u;
    return v;
}

/* o: pa_flprobe. Returns 0 (all read), -1 cache never idle, -2 continuous-read mode (refused), -3 SFC busy never cleared (nothing was changed in -1..-3), -4 a transfer timed out (restored anyway). */
FLASHFN int pa_flashfn_probe(volatile uint32_t *o)
{
    uint32_t sfc, spi, io, err = 0u, t0, k, i, a, id, sr1, sr2;
    fl_cli();
    STG(1u);
    t0 = FL_T4;
    while ((FL_CACHE_CON & 0x4000u) == 0u)
        if ((uint32_t)(FL_T4 - t0) > 480000u) return -1;
    sfc = FL_SFC_CON; spi = FL_SPI0_CON; io = FL_IOMAP0;
    o[FLP_SFC0] = sfc; o[FLP_SPI0] = spi; o[FLP_IO0] = io;
    if ((((sfc >> 8) & 0xFu) == 6u) || (((sfc >> 8) & 0xFu) == 7u)) return -2;
    fl_csync();
    t0 = FL_T4;
    while ((int32_t)FL_SFC_CON < 0)
        if ((uint32_t)(FL_T4 - t0) > 480000u) return -3;             /* 20 ms: nothing has been changed yet */
    STG(2u);
    t0 = FL_T4;
    /* ---- from here to the restore no instruction may come from the flash ---- */
    FL_SFC_CON = 0u;                                                 /* XIP off */
    FL_IOMAP0 &= ~0x20u;                                             /* the pins are ours */
    FL_SPI0_CON = 0u;
    FL_SPI0_CON = 0x1029u;
    fl_csync();
    STG(0x14u);                                                      /* XIP is off from here */
    fl_cs(0); fl_tx(0x9Fu, &err);
    id = fl_rx(&err) << 16; id |= fl_rx(&err) << 8; id |= fl_rx(&err);
    fl_cs(1);
    STG(0x15u);
    fl_cs(0); fl_tx(0x05u, &err); sr1 = fl_rx(&err); fl_cs(1);
    fl_cs(0); fl_tx(0x35u, &err); sr2 = fl_rx(&err); fl_cs(1);
    STG(0x16u);
    for (k = 0; k < 3u; k++) {
        a = (k == 0u) ? 0x4000u : (0xB5000u + ((k - 1u) << 12));
        fl_cs(0);
        fl_tx(0x0Bu, &err);
        fl_tx((a >> 16) & 0xFFu, &err); fl_tx((a >> 8) & 0xFFu, &err); fl_tx(a & 0xFFu, &err);
        fl_tx(0xFFu, &err);                                          /* the dummy byte */
        for (i = 0; i < 4u; i++) {
            uint32_t w = fl_rx(&err);
            w |= fl_rx(&err) << 8; w |= fl_rx(&err) << 16; w |= fl_rx(&err) << 24;
            o[FLP_DATA + k * 4u + i] = w;
        }
        fl_cs(1);
        STG(0x17u + k);
    }
    FL_SPI0_CON = spi;                                               /* restore, in the order of the reading */
    FL_IOMAP0 = (FL_IOMAP0 & ~0x20u) | (io & 0x20u);              /* the bit goes back to what it was (pack 169, first run on the unit: it was 0 before the probe, 0x0F, and the unconditional "|= 0x20" of the FM-1 driver left 0x2F) */
    fl_csync();
    STG(0x1Au);                                                      /* the pins are back with the controller */
    t0 = FL_T4;
    while ((int32_t)FL_SFC_CON < 0)
        if ((uint32_t)(FL_T4 - t0) > 480000u) break;                 /* (restored below whatever happens) */
    FL_SFC_CON = sfc;                                                /* XIP on again */
    fl_csync();
    STG(3u);
    o[FLP_ID] = id; o[FLP_SR] = sr1 | sr2 << 8;
    o[FLP_SFC1] = FL_SFC_CON; o[FLP_SPI1] = FL_SPI0_CON; o[FLP_IO1] = FL_IOMAP0;
    return err ? -4 : 0;
}


FLASHFN static void fl_addr3(uint32_t a, uint32_t *err)
{
    fl_tx((a >> 16) & 0xFFu, err); fl_tx((a >> 8) & 0xFFu, err); fl_tx(a & 0xFFu, err);
}
FLASHFN static uint32_t fl_rdsr1(uint32_t *err)
{
    uint32_t v;
    fl_cs(0); fl_tx(0x05u, err); v = fl_rx(err); fl_cs(1);
    return v;
}
/* wait until the chip's busy bit clears; max_ticks in TIMER4 ticks (24 per us); returns 0 idle, -1 timeout; *took = ticks waited */
FLASHFN static int fl_wait_idle(uint32_t max_ticks, uint32_t *took, uint32_t *err)
{
    const uint32_t t0 = FL_T4;
    uint32_t polls = 0u;
    for (;;) {
        const uint32_t sr = fl_rdsr1(err);
        const uint32_t dt = (uint32_t)(FL_T4 - t0);
        if (*err) { *took = dt; return -1; }
        if ((sr & 1u) == 0u) { *took = dt; return 0; }
        if (dt > max_ticks || ++polls > 4000000u) { *took = dt; return -1; }
    }
}
/* drop the XIP cache lines of the flash range (as the FM-1 driver and our bring-up driver do after an erase or a program) */
FLASHFN static void fl_inval(uint32_t off, uint32_t len)
{
    uint32_t a = (0x02000000u + off - 0x4000u) & ~31u;
    const uint32_t e = 0x02000000u + off - 0x4000u + len;
    uint32_t t0 = FL_T4;
    while ((FL_CACHE_CON & 0x4000u) == 0u)
        if ((uint32_t)(FL_T4 - t0) > 480000u) return;
    for (; a < e; a += 32u) {
        fl_csync();
        __asm__ volatile("flushinv [%0]" :: "r"(a) : "memory");
        fl_csync();
        t0 = FL_T4;
        while ((FL_CACHE_CON & 0x4000u) == 0u)
            if ((uint32_t)(FL_T4 - t0) > 480000u) return;
    }
}

/* ===================================================================================================================================================================================================
 * THE DRIVER (step 3): one generic operation, run from SRAM with the controller in command mode. Hardware-proven on the unit by the step 2 write test (pack 171: program 1.5 ms, erase 17 ms).
 *   op 0  read n bytes at off into buf (any address of the chip)
 *   op 1  erase the 4 KB sector at off
 *   op 2  program n bytes (1..256, inside one 256-byte page) at off from buf
 * An erase or a program is refused (-1) unless it lies completely inside one of the two rewritable windows (the same ranges as pa_flash_map.h: 0xB6000..0xBCFFF and 0xFB000..0xFEFFF; the bootloader,
 * the app, the stock's record log at 0xB5000, BTIF, USER, the factory bundle and 0xFF000 are out of reach), and a buffer must lie in SRAM (the flash cannot be read while the controller is in command
 * mode). Commands sent: 0x06 write enable, 0x04 write disable, 0x02 page program, 0x20 sector erase, 0x05 status, 0x0B fast read. Never 0x01 / 0x31 / 0x11 / 0xC7 / 0x60 / 0xD8 / 0x52.
 * The interrupts are masked by the caller (pa_flash_dev_op) and the audio is already silent; core 1 is parked. Return: 0 ok, -1 refused / bad argument, -2 cache never idle, -3 write enable did not latch,
 * -4 the chip did not finish in time, -5 a transfer timed out, -6 continuous-read XIP mode (refused), -7 SFC busy never cleared. Nothing was changed for -1, -2, -6 and -7. */
FLASHFN int pa_flashfn_op(uint32_t op, uint32_t off, uint8_t *buf, uint32_t n, uint32_t *took)
{
    uint32_t sfc, spi, io, err = 0u, t0, i, sr, dt = 0u, extra;
    int rc = 0;
    fl_cli();
    *took = 0u;
    if (!pa_fld_args_ok(op, off, (uint32_t)(uintptr_t)buf, n)) return -1;       /* pa_flash_guard.h: the windows, the page, the SRAM buffer */
    t0 = FL_T4;
    while ((FL_CACHE_CON & 0x4000u) == 0u)
        if ((uint32_t)(FL_T4 - t0) > 480000u) return -2;
    sfc = FL_SFC_CON; spi = FL_SPI0_CON; io = FL_IOMAP0;
    if ((((sfc >> 8) & 0xFu) == 6u) || (((sfc >> 8) & 0xFu) == 7u)) return -6;
    fl_csync();
    t0 = FL_T4;
    while ((int32_t)FL_SFC_CON < 0)
        if ((uint32_t)(FL_T4 - t0) > 480000u) return -7;
    /* ---- XIP is off from here to the restore ---- */
    FL_SFC_CON = 0u;
    FL_IOMAP0 &= ~0x20u;
    FL_SPI0_CON = 0u;
    FL_SPI0_CON = 0x1029u;
    fl_csync();
    if (op == 0u) {
        fl_cs(0);
        fl_tx(0x0Bu, &err); fl_addr3(off, &err); fl_tx(0xFFu, &err);
        for (i = 0u; i < n; i++) buf[i] = (uint8_t)fl_rx(&err);
        fl_cs(1);
    } else {
        fl_cs(0); fl_tx(0x06u, &err); fl_cs(1);                     /* write enable, and the chip must say so */
        sr = fl_rdsr1(&err);
        if ((sr & 2u) == 0u || err != 0u) {
            fl_cs(0); fl_tx(0x04u, &err); fl_cs(1);
            rc = err ? -5 : -3;
        } else if (op == 1u) {
            fl_cs(0); fl_tx(0x20u, &err); fl_addr3(off, &err); fl_cs(1);
            fl_inval(off, 4096u);
            if (fl_wait_idle(12000000u, &dt, &err) != 0) rc = -4;    /* up to 500 ms */
        } else {
            fl_cs(0); fl_tx(0x02u, &err); fl_addr3(off, &err);
            for (i = 0u; i < n; i++) fl_tx(buf[i], &err);
            fl_cs(1);
            fl_inval(off, n);
            if (fl_wait_idle(240000u, &dt, &err) != 0) rc = -4;      /* up to 10 ms */
        }
        *took = dt;
        (void)fl_wait_idle(48000000u, &extra, &err);                /* never hand the flash back to XIP while the chip is busy: up to 2 s more */
    }
    FL_SPI0_CON = spi;
    FL_IOMAP0 = (FL_IOMAP0 & ~0x20u) | (io & 0x20u);
    fl_csync();
    t0 = FL_T4;
    while ((int32_t)FL_SFC_CON < 0)
        if ((uint32_t)(FL_T4 - t0) > 480000u) break;
    FL_SFC_CON = sfc;
    fl_csync();
    return (err != 0u && rc == 0) ? -5 : rc;
}

/* XIP side: copy the RAM function in, call it through a pointer (XIP -> SRAM is beyond a direct call), and record the result. Interrupts are off at boot and core 1 has not started. */
void pa_flash_probe_boot(void)
{
    uint32_t *d = (uint32_t *)(void *)_ff_start;
    const uint32_t *s = (const uint32_t *)(const void *)_ff_load;
    /* The entry is the probe's own address (its link address is in the SRAM section), NOT the start of the section: the section starts with whichever function the linker placed first (fl_cs in the
     * pack 166 link: _ff_start 0x01C76500 = fl_cs, the probe at 0x01C765BE). Calling _ff_start entered fl_cs with the result block as its argument and never reached the probe: the "stage 0, died before
     * its first line" of the first hardware run (BLE-DEPLOYMENT-PLAN.md, phase 4). */
    int (*volatile fn)(volatile uint32_t *) = &pa_flashfn_probe;
    uint32_t t0, i;
    const int hung = (pa_flt_log[0] == 0x31544C46u) && (pa_flt_log[1] != 3u);   /* "FLT1" and not finished, stage 0 included (the RAM function was never entered): the last run of the probe stopped there. Stage 0 used to count as 'not hung', so a probe that never started was run again on every boot (Gate F0) */
    for (i = 0; i < FLP_WORDS; i++) pa_flprobe[i] = 0u;
    pa_flprobe[FLP_MAGIC] = 0x52504C46u;                              /* "FLPR" */
    if (hung) {                                                       /* do not run it again: report where it stopped and let the caller go to the download mode */
        pa_flprobe[FLP_RC] = 0xFFFFFFF0u;
        pa_flprobe[FLP_STAGE] = pa_flt_log[1];
        pa_flt_log[0] = 0u;                                           /* the next boot may try again */
        return;
    }
    if ((uintptr_t)(void *)&pa_flashfn_probe < (uintptr_t)(void *)_ff_start || (uintptr_t)(void *)&pa_flashfn_probe >= (uintptr_t)(void *)_ff_end) {   /* not inside the SRAM section: never call it */
        pa_flprobe[FLP_RC] = 0xFFFFFFE0u;
        return;
    }
    pa_flt_log[0] = 0x31544C46u; pa_flt_log[1] = 0u; pa_flt_log[2] = PA_BUILD_ID;   /* (the build id and the stage marker are written before the risky call) */
    while (d < (uint32_t *)(void *)_ff_end) *d++ = *s++;
    if (((const volatile uint32_t *)(const void *)_ff_start)[0] == 0u || ((const volatile uint32_t *)(const void *)_ff_start)[0] == 0xFFFFFFFFu) {   /* the image held nothing at the load address (it was all zeros in packs 140-167: build.ps1 did not pack .flashfn): never jump into it */
        pa_flprobe[FLP_RC] = 0xFFFFFFD0u;
        pa_flt_log[0] = 0u;
        return;
    }
    __asm__ volatile("csync" ::: "memory");
    t0 = FL_T4;
    pa_flprobe[FLP_RC] = (uint32_t)fn(pa_flprobe);
    pa_flprobe[FLP_US] = (uint32_t)(FL_T4 - t0) / 24u;
    pa_flprobe[FLP_STAGE] |= 0x100u;                                  /* back in the XIP side */
}

/* XIP side of the driver. pa_flash_dev_init() copies the section into SRAM once (at boot, after the app image is in place) and refuses to arm if the copy is empty (the packaging bug of packs 140-167).
 * pa_flash_dev_op() is the only way in: it calls the RAM function through a pointer (XIP -> SRAM is beyond a direct call). The caller has already quiesced the system (pa_spi_nor.c). */
/* The boot guard. The first flash operations of a power-on run while the Bluetooth radio is already advertising and the interrupts are off; whether the radio and the flash controller coexist is not proven.
 * If such an operation hangs, the watchdog restarts the unit and, without this, the next boot would hang the same way, for ever. pa_fl_guard (kept across a warm reset): [0] "FLG1", [1] 0 fine, 1 a boot-time
 * flash operation is in progress (set at the first one, cleared by pa_flash_dev_boot_done when the main loop starts), 2 disabled: the last boot died inside one, so every flash operation of this power-on
 * session is refused (-8): the presets and the settings are the defaults, the unit works. A power cycle clears it (the RAM is not kept), and the next power-on tries again. */
volatile uint32_t pa_fl_guard[2] __attribute__((section(".noinit.flt")));
extern volatile uint8_t g_pa_irq_on;
static volatile uint8_t s_fl_guard_seen;                          /* this boot's: zero at every boot (.bss), unlike pa_fl_guard */
static int fl_guard_blocked(void) { return pa_flash_guard_blocked(pa_fl_guard, g_pa_irq_on, &s_fl_guard_seen); }
void pa_flash_dev_boot_done(void) { pa_flash_guard_boot_done(pa_fl_guard); }
static int s_dev_ok;
void pa_flash_dev_init(void)
{
    uint32_t *d = (uint32_t *)(void *)_ff_start;
    const uint32_t *s = (const uint32_t *)(const void *)_ff_load;
    if (s_dev_ok) return;
    while (d < (uint32_t *)(void *)_ff_end) *d++ = *s++;
    __asm__ volatile("csync" ::: "memory");
    if (((const volatile uint32_t *)(const void *)_ff_start)[0] == 0u || ((const volatile uint32_t *)(const void *)_ff_start)[0] == 0xFFFFFFFFu) return;
    if ((uintptr_t)(void *)&pa_flashfn_op < (uintptr_t)(void *)_ff_start || (uintptr_t)(void *)&pa_flashfn_op >= (uintptr_t)(void *)_ff_end) return;
    s_dev_ok = 1;
}
int pa_flash_dev_op(uint32_t op, uint32_t off, void *buf, uint32_t n, uint32_t *took_us)
{
    int (*volatile fn)(uint32_t, uint32_t, uint8_t *, uint32_t, uint32_t *) = &pa_flashfn_op;
    uint32_t ticks = 0u;
    int rc;
    if (!s_dev_ok) return -8;                                         /* not armed */
    if (fl_guard_blocked()) return -8;                                /* a boot-time flash operation hung the last boot: no flash I/O this session */
    rc = fn(op, off, (uint8_t *)buf, n, &ticks);
    if (took_us) *took_us = ticks / 24u;
    return rc;
}

#else
void pa_flash_probe_boot(void) { }
void pa_flash_dev_init(void) { }
void pa_flash_dev_boot_done(void) { }
int pa_flash_dev_op(uint32_t op, uint32_t off, void *buf, uint32_t n, uint32_t *took_us) { (void)op; (void)off; (void)buf; (void)n; (void)took_us; return -8; }
#endif
