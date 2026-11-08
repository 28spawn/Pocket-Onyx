/* SPDX-License-Identifier: GPL-3.0-only */
/* Copyright (C) 2026 The Onyx Authors (@28spawn) */
/* test/test_flash_guard.c - the SRAM flash driver's argument check against the map, for every address, and the boot guard's state machine.
 *
 * The RAM function cannot include pa_flash_map.h, so include/pa_flash_guard.h repeats the three rewritable windows. This test makes the repetition safe: for every offset of the chip (every 16 bytes) and
 * for the edges of the 32-bit range, an erase or a program is allowed by the driver's check if and only if pa_flash_rw_ok() allows it. It also pins what the stock's live data needs (0xB5000, 0xFF000, BTIF,
 * USER, the factory bundle, the app and the bootloader are never allowed) and walks the boot guard through every transition. */
#include "../include/pa_ota_session.h"
#include <stdio.h>
#include <string.h>
#include "../include/pa_flash_map.h"
#include "../include/pa_flash_guard.h"

static int fails;
#define CHECK(ok, ...) do { if (!(ok)) { fails++; printf("  FAIL: " __VA_ARGS__); printf("\n"); } } while (0)

int main(void)
{
    const uint32_t BUF = 0x01C10000u;
    uint32_t bad_erase = 0, bad_prog = 0, allowed = 0;
    /* every 16 bytes of the chip, plus the edges of the 32-bit range */
    for (uint64_t o64 = 0; o64 <= 0x100000u + 0x2000u; o64 += 16u) {
        const uint32_t o = (uint32_t)o64;
        for (uint32_t n = 1; n <= 256u; n = (n < 16u) ? n + 5u : n * 2u) {
            const int drv = pa_fld_args_ok(2u, o, BUF, n), map = (((o & 0xFFu) + n) <= 256u) && pa_flash_rw_ok(o, n);
            if (drv != map) bad_prog++;
            allowed += (uint32_t)drv;
        }
        if ((o & 0xFFFu) == 0u) {
            const int drv = pa_fld_args_ok(1u, o, 0u, 0u), map = pa_flash_rw_ok(o, 4096u);
            if (drv != map) bad_erase++;
        }
    }
    const uint32_t edge[] = { 0xFFFFF000u, 0xFFFFFF00u, 0xFFFFFFF0u, 0xFFF00000u, 0xFFFB6000u, 0xFFFFB000u, 0x7FFFF000u, 0x80000000u, 0xFFFFFFFFu, 0x00100000u, 0x000FF000u, 0x000BD000u, 0x000B5000u };
    for (unsigned i = 0; i < sizeof edge / sizeof edge[0]; i++) {
        CHECK(!pa_fld_args_ok(1u, edge[i] & ~0xFFFu, 0u, 0u) || pa_flash_rw_ok(edge[i] & ~0xFFFu, 4096u), "erase at %#x allowed by the driver but not by the map", edge[i]);
        for (uint32_t n = 1; n <= 256u; n += 51u)
            CHECK(!pa_fld_args_ok(2u, edge[i], BUF, n) || pa_flash_rw_ok(edge[i], n), "program at %#x (%u) allowed by the driver but not by the map", edge[i], n);
    }
    CHECK(bad_prog == 0, "%u program checks differ between the driver and pa_flash_rw_ok", bad_prog);
    CHECK(bad_erase == 0, "%u erase checks differ between the driver and pa_flash_rw_ok", bad_erase);
    CHECK(allowed > 0, "the driver allowed nothing at all (the test is blind)");
    /* our window (0x86000..0xB3FFF) is writable, sector by sector, and its edges are exact */
    for (uint32_t a = 0x86000u; a < 0xB4000u; a += 0x1000u) {
        CHECK(pa_fld_args_ok(1u, a, 0u, 0u), "the driver refuses a user-store sector it should allow (%#x)", a);
        CHECK(pa_fld_args_ok(2u, a, BUF, 256u) && pa_flash_rw_ok(a, 256u), "the driver or the map refuses a page of the user store (%#x)", a);
    }
    CHECK(!pa_fld_args_ok(1u, 0x85000u, 0u, 0u) && !pa_fld_args_ok(2u, 0x85F00u, BUF, 256u), "the last sector of the app image stays closed");
    CHECK(!pa_fld_args_ok(2u, 0xB3F80u, BUF, 257u) && !pa_fld_args_ok(1u, 0xB4000u, 0u, 0u) && !pa_fld_args_ok(2u, 0xB4F00u, BUF, 256u) && !pa_fld_args_ok(1u, 0xB5000u, 0u, 0u), "the sector with the V1.05 trailer (0xB4000, the 6 bytes at 0xB4FF8) and the stock's record log behind it stay closed");
    /* what must never be writable */
    {
        const struct { uint32_t a; const char *what; } never[] = {
            { 0x00000000u, "the boot record" }, { 0x00001000u, "the bootloader" }, { 0x00003000u, "the bootloader's last sector" }, { 0x00004000u, "the app area" }, { 0x00085000u, "the app image's last sector" }, { 0x00085FFFu, "the app image's last byte" }, { 0x000B4000u, "the V1.05 trailer's sector" }, { 0x000B4FF8u, "the V1.05 trailer" },
            { 0x000B5000u, "the stock's record log" }, { 0x000BD000u, "BTIF" }, { 0x000BE000u, "USER (factory calibration)" }, { 0x000BF000u, "the factory bundle" }, { 0x000FA000u, "the factory bundle's last sector" },
            { 0x000FF000u, "the stock's save record" } };
        for (unsigned i = 0; i < sizeof never / sizeof never[0]; i++) {
            CHECK(!pa_fld_args_ok(1u, never[i].a, 0u, 0u), "the driver would erase %s (%#x)", never[i].what, never[i].a);
            CHECK(!pa_fld_args_ok(2u, never[i].a, BUF, 16u), "the driver would program %s (%#x)", never[i].what, never[i].a);
            CHECK(!pa_flash_rw_ok(never[i].a, 16u), "the map allows a write to %s (%#x)", never[i].what, never[i].a);
        }
        /* the stock's VM (0xB5000..0xBCFFF) and its saved presets (0xFB000..0xFEFFF): since pack 194 they are the stock's alone, whole, sector by sector */
        for (uint32_t a = 0xB5000u; a < 0xB8000u; a += 0x1000u) { CHECK(!pa_fld_args_ok(1u, a, 0u, 0u) && !pa_fld_args_ok(2u, a, BUF, 16u) && !pa_flash_rw_ok(a, 16u), "the stock's VM sector %#x is writable", a); }
        CHECK(PA_OTA_AREA == 0xB8000u && PA_OTA_AREA + PA_OTA_AREA_LEN == 0xBD000u && PA_OTA_RES >= 0xBCF00u && PA_OTA_RES + 112u <= 0xBD000u, "the update session's staging area and record are exactly the map's staging window");
        /* the five update staging sectors (0xB8000..0xBCFFF) ARE writable - the update session erases, programs and erases them again (found on the unit: without them the web install stopped after step 1's checks) - and only they,
         * with the record at 0xBCF00 and the staging head at 0xB8000 inside; BTIF right above stays closed */
        for (uint32_t a = 0xB8000u; a < 0xBD000u; a += 0x1000u) { CHECK(pa_fld_args_ok(1u, a, 0u, 0u) && pa_flash_rw_ok(a, 4096u), "the staging sector %#x is refused", a); }
        CHECK(pa_fld_args_ok(2u, 0xB8000u, BUF, 32u) && pa_fld_args_ok(2u, 0xBCF00u, BUF, 112u) && pa_fld_args_ok(2u, 0xB8020u, BUF, 256u - 0x20u), "the staging head, the loader body and the update record can be programmed");
        CHECK(!pa_fld_args_ok(1u, 0xBD000u, 0u, 0u) && !pa_fld_args_ok(2u, 0xBCFF0u, BUF, 64u + 256u) && !pa_flash_rw_ok(0xBCFF0u, 0x20u), "BTIF above the staging area stays closed, and a program cannot run over its edge");
        for (uint32_t a = 0xFB000u; a < 0xFF000u; a += 0x1000u) { CHECK(!pa_fld_args_ok(1u, a, 0u, 0u) && !pa_fld_args_ok(2u, a, BUF, 16u) && !pa_flash_rw_ok(a, 16u), "the stock's saved-presets sector %#x is writable", a); }
    }
    /* the other arguments */
    CHECK(!pa_fld_args_ok(3u, 0x89000u, BUF, 16u), "an unknown op is refused");
    CHECK(!pa_fld_args_ok(2u, 0x89000u, 0x02000000u, 16u), "a program from a buffer in the flash is refused");
    CHECK(!pa_fld_args_ok(0u, 0x89000u, 0x02000000u, 16u), "a read into a buffer in the flash is refused");
    CHECK(!pa_fld_args_ok(2u, 0x89000u, 0x01C7FFF0u, 64u), "a buffer that runs past the end of the SRAM is refused");
    CHECK(!pa_fld_args_ok(2u, 0x890F0u, BUF, 32u), "a program that crosses a 256-byte page is refused");
    CHECK(!pa_fld_args_ok(2u, 0x89000u, BUF, 257u), "a program longer than a page is refused");
    CHECK(!pa_fld_args_ok(1u, 0x89010u, 0u, 0u), "an erase that is not sector aligned is refused");
    CHECK(!pa_fld_args_ok(0u, 0x000FFFF0u, BUF, 64u), "a read past the end of the chip is refused");
    CHECK(pa_fld_args_ok(0u, 0x00004000u, BUF, 256u), "a read of the app area into SRAM is allowed");
    CHECK(pa_fld_args_ok(0u, 0x000FF000u, BUF, 256u), "a read of the stock's save record is allowed (only writes are fenced)");
    /* the boot guard */
    {
        volatile uint32_t g[2];
        volatile uint8_t seen;
        g[0] = 0x12345678u; g[1] = 7u; seen = 0u;                                            /* power-on: the RAM holds garbage */
        CHECK(!pa_flash_guard_blocked(g, 0, &seen), "a power-on boot with garbage in the guard must not be blocked");
        CHECK(g[0] == PA_FLG_MAGIC && g[1] == 1u, "the first boot-time operation marks the guard in progress");
        CHECK(!pa_flash_guard_blocked(g, 0, &seen) && !pa_flash_guard_blocked(g, 0, &seen), "the later boot-time operations of the SAME boot are not blocked by the first one's mark");
        CHECK(g[1] == 1u, "still in progress");
        pa_flash_guard_boot_done(g);
        CHECK(g[1] == 0u, "the main loop start clears the in-progress mark");
        CHECK(!pa_flash_guard_blocked(g, 1, &seen), "a runtime operation is not blocked when the guard is clear");
        /* a boot that dies inside a boot-time operation: the mark stays 1 across the watchdog restart, and the next boot starts with seen = 0 */
        seen = 0u; (void)pa_flash_guard_blocked(g, 0, &seen);
        CHECK(g[1] == 1u, "in progress again");
        seen = 0u;                                                                           /* the watchdog restart: this boot's variable is zero again, the noinit state is kept */
        CHECK(pa_flash_guard_blocked(g, 0, &seen), "the boot after a hung one must be blocked");
        CHECK(g[1] == 2u, "...and disabled for the session");
        CHECK(pa_flash_guard_blocked(g, 0, &seen), "...for every boot-time operation of that boot");
        pa_flash_guard_boot_done(g);
        CHECK(g[1] == 2u, "the main loop start must not clear a disabled guard");
        CHECK(pa_flash_guard_blocked(g, 1, &seen), "runtime operations are blocked while disabled");
        seen = 0u;
        CHECK(pa_flash_guard_blocked(g, 0, &seen), "a further warm boot stays blocked");
        g[0] = 0xDEADBEEFu; seen = 0u;                                                       /* a power cycle: the RAM is not kept */
        CHECK(!pa_flash_guard_blocked(g, 0, &seen) && g[1] == 1u, "after a power cycle the guard lets the first boot-time operation run again");
    }
    printf("[flash guard] driver windows == map windows for %u allowed program shapes; boot guard transitions walked\n", allowed);
    printf(fails ? "RESULT: %d FAILURE(S)\n" : "RESULT: ALL PASSED\n", fails);
    return fails ? 1 : 0;
}
