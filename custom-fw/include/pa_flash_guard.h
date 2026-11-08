/* SPDX-License-Identifier: GPL-3.0-only */
/* Copyright (C) 2026 The Onyx Authors (@28spawn) */
/* include/pa_flash_guard.h - the two pieces of the flash driver that decide whether an operation may run, written as pure inline functions so that a host test can check them exhaustively
 * (test/test_flash_guard.c) and the SRAM driver (src/board/flash_ram.c, which cannot call anything) still compiles them in place (always_inline: no call, no table).
 *
 * pa_fld_args_ok: the argument check of the RAM operation. op 0 read, 1 erase a 4 KB sector, 2 program 1..256 bytes inside one 256-byte page. An erase or a program must lie completely inside one of the
 * three rewritable windows of pa_flash_map.h (0xB6000..0xBCFFF, 0xFB000..0xFEFFF and the user store 0x86000..0xB3FFF; the numbers are repeated here because the RAM function cannot read the map, and test_flash_guard.c compares the two
 * for every address); a buffer must lie in SRAM. The window test is written without computing off + len, so a huge offset cannot wrap round into a window.
 *
 * pa_flash_guard_*: the boot guard. g[0] "FLG1" (valid), g[1] 0 fine, 1 a boot-time flash operation is in progress, 2 disabled for this power-on session. */
#ifndef PA_FLASH_GUARD_H
#define PA_FLASH_GUARD_H
#include <stdint.h>

#define PA_FLD_WIN_OK(o, l) ((((o) >= 0x00086000u) && ((o) < 0x000B4000u) && ((l) <= 0x000B4000u - (o))) || (((o) >= 0x000B8000u) && ((o) < 0x000BD000u) && ((l) <= 0x000BD000u - (o))))
#define PA_FLD_SRAM_LO 0x01C00000u
#define PA_FLD_SRAM_HI 0x01C80000u

static inline __attribute__((always_inline)) int pa_fld_args_ok(uint32_t op, uint32_t off, uint32_t buf, uint32_t n)
{
    if (op > 2u) return 0;
    if (op == 0u) {
        if (n == 0u || off >= 0x00100000u || n > 0x00100000u - off) return 0;
    } else if (op == 1u) {
        if ((off & 0xFFFu) != 0u || !PA_FLD_WIN_OK(off, 0x1000u)) return 0;
    } else {
        if (n == 0u || n > 256u || ((off & 0xFFu) + n) > 256u || !PA_FLD_WIN_OK(off, n)) return 0;
    }
    if (op != 1u && (buf < PA_FLD_SRAM_LO || buf >= PA_FLD_SRAM_HI || n > PA_FLD_SRAM_HI - buf)) return 0;
    return 1;
}

#define PA_FLG_MAGIC 0x31474C46u
/* 1 = refuse every flash operation (a boot-time one hung the last boot). The noinit state is judged ONCE per boot, at the first boot-time operation (*seen is a zero-initialised variable of this boot, not
 * kept across a reset): state 1 found there means the LAST boot died inside one, because this boot has not set it yet. irq_on: 0 during the boot init, 1 from the main loop. */
static inline __attribute__((always_inline)) int pa_flash_guard_blocked(volatile uint32_t *g, int irq_on, volatile uint8_t *seen)
{
    if (irq_on) return g[0] == PA_FLG_MAGIC && g[1] == 2u;
    if (!*seen) {
        *seen = 1u;
        if (g[0] != PA_FLG_MAGIC) { g[0] = PA_FLG_MAGIC; g[1] = 0u; }
        else if (g[1] == 1u) g[1] = 2u;                                /* the last boot died inside a boot-time operation */
        if (g[1] != 2u) g[1] = 1u;                                     /* in progress, until the main loop starts */
    }
    return g[1] == 2u;
}
/* the main loop has started: the boot-time operations all finished */
static inline __attribute__((always_inline)) void pa_flash_guard_boot_done(volatile uint32_t *g)
{
    if (g[0] == PA_FLG_MAGIC && g[1] == 1u) g[1] = 0u;
}
#endif
