/* SPDX-License-Identifier: GPL-3.0-only */
/* Copyright (C) 2026 The Onyx Authors (@28spawn) */
/*
 * src/board/spi_nor.c — Pocket Amp SPI NOR Flash Hardware & Emulator Backing Store
 *
 * Implements 4 KB sector erase, 256 B page programming, and persistent storage.
 * On Host/Emulator: Backed by 'pocket_amp_flash.bin' (1 MB persistent image, the size of the real chip).
 * On Physical SoC:  Hooks JieLi SPI NOR Flash Controller / XIP memory map.
 */
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include "../../include/pa_spi_nor.h"
#include "../../include/pa_hw.h"
#include "../../include/pa_plat.h"

#if defined(PA_HOST_SIM) || defined(PA_EMU_DLL) || defined(_WIN32) || defined(__linux__) || defined(__APPLE__)

static uint8_t *g_sim_flash = NULL;
static FILE    *g_flash_fp  = NULL;
static int      g_flash_inited = 0;

static const char *k_flash_paths[] = {
    "pocket_amp_flash.bin",
    "custom-fw/emu/pocket_amp_flash.bin",
    "../pocket_amp_flash.bin",
    "../../pocket_amp_flash.bin",
    NULL
};

int pa_spi_nor_init(void) {
    if (g_flash_inited && g_sim_flash) return 0;

    if (!g_sim_flash) {
        g_sim_flash = (uint8_t *)malloc(PA_FLASH_SIZE);
        if (!g_sim_flash) return -1;
    }
    memset(g_sim_flash, 0xFF, PA_FLASH_SIZE);

    /* Try to open existing backing file */
    const char *selected_path = NULL;
    for (int i = 0; k_flash_paths[i]; i++) {
        FILE *fp = fopen(k_flash_paths[i], "rb");
        if (fp) {
            selected_path = k_flash_paths[i];
            fclose(fp);
            break;
        }
    }

    if (!selected_path) {
        selected_path = "custom-fw/emu/pocket_amp_flash.bin";
    }
    /* A test points the simulated chip at its own scratch file (PA_FLASH_FILE), so what one test writes can not change another's result (the registry of user
     * captures lives in this file, and the 1 MB chip has room for one flash capture). */
    {
        const char *env_flash = getenv("PA_FLASH_FILE");
        if (env_flash && env_flash[0]) selected_path = env_flash;
    }

    g_flash_fp = fopen(selected_path, "r+b");
    if (!g_flash_fp) {
        g_flash_fp = fopen(selected_path, "w+b");
        if (g_flash_fp) {
            /* Create a 1 MB blank erased flash image */
            fwrite(g_sim_flash, 1, PA_FLASH_SIZE, g_flash_fp);
            fflush(g_flash_fp);
        }
    } else {
        /* Read existing flash contents into memory (an older, larger image keeps only its first 1 MB) */
        size_t n = fread(g_sim_flash, 1, PA_FLASH_SIZE, g_flash_fp);
        (void)n;
    }

    /* Optional local read-only partition mount via PA_STOCK_BUNDLE (in-memory only;
     * never cached or written out to g_flash_fp on disk). */
    if (g_sim_flash[PA_FLASH_FACTORY_CAB_ADDR] == 0xFF) {
        const char *env_bundle = getenv("PA_STOCK_BUNDLE");
        if (env_bundle && env_bundle[0]) {
            FILE *bfp = fopen(env_bundle, "rb");
            if (bfp) {
                size_t br = fread(&g_sim_flash[PA_FLASH_FACTORY_CAB_ADDR], 1, PA_FLASH_FACTORY_CAB_SIZE, bfp);
                (void)br;
                fclose(bfp);
            }
        }
    }

    g_flash_inited = 1;
    return 0;
}

/* Host-only power-loss injection: after N more mutating operations the "power"
 * drops. The op in flight is torn (erase: half the sector cleared; write: first
 * half of the bytes programmed) and every later op fails until reset. */
static int  g_fault_countdown = -1;
static int  g_power_lost      = 0;

void pa_spi_nor_sim_fail_after(int n_ops) { g_fault_countdown = n_ops; g_power_lost = 0; }
void pa_spi_nor_sim_reboot(void)          { g_fault_countdown = -1;   g_power_lost = 0; }

static int fault_tick(void) { /* returns 1 when this op must be torn */
    if (g_power_lost) return 2;
    if (g_fault_countdown < 0) return 0;
    if (g_fault_countdown == 0) { g_power_lost = 1; return 1; }
    g_fault_countdown--;
    return 0;
}

int pa_spi_nor_erase_sector(uint32_t flash_addr) {
    if (!g_flash_inited) pa_spi_nor_init();
    if (flash_addr >= PA_FLASH_SIZE) return -1;
    if (!pa_flash_rw_ok(flash_addr & ~(PA_FLASH_SECTOR_SIZE - 1u), PA_FLASH_SECTOR_SIZE)) return -3;   /* bootloader/app/calibration/factory bundle protected */
    if (plat_battery_is_brownout()) return -5;    /* Brownout lock: battery voltage critical */

    uint32_t aligned_addr = flash_addr & ~(PA_FLASH_SECTOR_SIZE - 1u);
    int f = fault_tick();
    if (f == 2) return -4;
    uint32_t n = (f == 1) ? PA_FLASH_SECTOR_SIZE / 2u : PA_FLASH_SECTOR_SIZE;
    memset(&g_sim_flash[aligned_addr], 0xFF, n);

    if (g_flash_fp) {
        fseek(g_flash_fp, (long)aligned_addr, SEEK_SET);
        fwrite(&g_sim_flash[aligned_addr], 1, PA_FLASH_SECTOR_SIZE, g_flash_fp);
        fflush(g_flash_fp);
    }
    return f ? -4 : 0;
}

int pa_spi_nor_write(uint32_t flash_addr, const void *data, uint32_t len) {
    if (len == 0u) return 0;
    if (!data) return -1;
    if (!g_flash_inited) pa_spi_nor_init();
    if (flash_addr >= PA_FLASH_SIZE || len > PA_FLASH_SIZE - flash_addr) return -1;
    if (!pa_flash_rw_ok(flash_addr, len)) return -3;   /* bootloader/app/calibration/factory bundle protected, or a write that leaves its window */
    if (plat_battery_is_brownout()) return -5;    /* Brownout lock: battery voltage critical */

    int f = fault_tick();
    if (f == 2) return -4;
    uint32_t n = (f == 1) ? len / 2u : len;

    const uint8_t *src = (const uint8_t *)data;
    /* NOR Flash rule: bits can only flip 1 -> 0 without an erase */
    for (uint32_t i = 0; i < n; i++) {
        g_sim_flash[flash_addr + i] &= src[i];
    }

    if (g_flash_fp) {
        fseek(g_flash_fp, (long)flash_addr, SEEK_SET);
        fwrite(&g_sim_flash[flash_addr], 1, len, g_flash_fp);
        fflush(g_flash_fp);
    }
    return f ? -4 : 0;
}

int pa_spi_nor_read(uint32_t flash_addr, void *dst, uint32_t len) {
    if (len == 0u) return 0;
    if (!dst) return -1;
    if (!g_flash_inited) pa_spi_nor_init();
    if (flash_addr >= PA_FLASH_SIZE || len > PA_FLASH_SIZE - flash_addr) return -1;

    memcpy(dst, &g_sim_flash[flash_addr], len);
    return 0;
}

void pa_spi_nor_sync(void) {
    if (g_flash_fp) {
        fflush(g_flash_fp);
    }
}
void pa_spi_nor_begin(void) { }
void pa_spi_nor_end(void) { }

#else

/* Bare-metal JieLi AC791N / pi32v2 Implementation: the SRAM driver in flash_ram.c (hardware-proven, packs 169-171). */
extern void pa_flash_dev_init(void);
extern int  pa_flash_dev_op(uint32_t op, uint32_t off, void *buf, uint32_t n, uint32_t *took_us);
extern void pa_audio_core_flash_begin(void);
extern void pa_audio_core_flash_end(void);
extern void plat_wdt_feed(void);

static int s_batch;
void pa_spi_nor_begin(void) { if (s_batch++ == 0) pa_audio_core_flash_begin(); }
void pa_spi_nor_end(void)   { if (s_batch > 0 && --s_batch == 0) pa_audio_core_flash_end(); }

int pa_spi_nor_init(void) {
    pa_flash_dev_init();
    return 0;
}

/* Errors: -1 bad argument, -3 outside the rewritable windows, -4 the driver or the chip failed (or the read-back did not match), -5 battery critical. */
int pa_spi_nor_erase_sector(uint32_t flash_addr) {
    const uint32_t a = flash_addr & ~(PA_FLASH_SECTOR_SIZE - 1u);
    uint8_t b[256];
    int rc;
    if (!pa_flash_rw_ok(a, PA_FLASH_SECTOR_SIZE)) return -3;   /* bootloader/app/calibration/factory bundle protected */
    if (plat_battery_is_brownout()) return -5;    /* Brownout lock: battery voltage critical */
    pa_flash_dev_init();
    pa_spi_nor_begin();
    plat_wdt_feed();
    rc = pa_flash_dev_op(1u, a, 0, 0u, 0);
    for (uint32_t o = 0; rc == 0 && o < PA_FLASH_SECTOR_SIZE; o += sizeof b) {       /* read it back: blank? */
        if (pa_flash_dev_op(0u, a + o, b, (uint32_t)sizeof b, 0) != 0) { rc = -4; break; }
        for (uint32_t i = 0; i < sizeof b; i++) if (b[i] != 0xFFu) { rc = -4; break; }
    }
    plat_wdt_feed();
    pa_spi_nor_end();
    return rc ? -4 : 0;
}

int pa_spi_nor_write(uint32_t flash_addr, const void *data, uint32_t len) {
    const uint8_t *src = (const uint8_t *)data;
    uint8_t b[256], v[256];
    int rc = 0;
    if (len == 0u) return 0;
    if (!data) return -1;
    if (!pa_flash_rw_ok(flash_addr, len)) return -3;   /* bootloader/app/calibration/factory bundle protected, or a write that leaves its window */
    if (plat_battery_is_brownout()) return -5;    /* Brownout lock: battery voltage critical */
    pa_flash_dev_init();
    pa_spi_nor_begin();
    while (len && rc == 0) {
        uint32_t chunk = 256u - (flash_addr & 0xFFu);
        if (chunk > len) chunk = len;
        memcpy(b, src, chunk);                                   /* the source may be in the flash (a const table): the driver takes SRAM only */
        plat_wdt_feed();
        if (pa_flash_dev_op(2u, flash_addr, b, chunk, 0) != 0) rc = -4;
        else if (pa_flash_dev_op(0u, flash_addr, v, chunk, 0) != 0 || memcmp(b, v, chunk) != 0) rc = -4;     /* read it back */
        flash_addr += chunk; src += chunk; len -= chunk;
    }
    plat_wdt_feed();
    pa_spi_nor_end();
    return rc;
}

/*
 * Read. Inside the app partition the XIP window is read directly (fast, nothing to quiesce). Anywhere else the SRAM driver reads the raw flash (the controller leaves XIP for the
 * duration, so it is bracketed like a write). The bounds are checked without computing flash_addr + len, so a huge len cannot wrap around and slip past the check.
 */
int pa_spi_nor_read(uint32_t flash_addr, void *dst, uint32_t len) {
    uint8_t *out = (uint8_t *)dst;
    uint8_t b[256];
    int rc = 0;
    if (len == 0u) return 0;
    if (!dst) return -1;
    if (flash_addr >= PA_FLASH_SIZE || len > PA_FLASH_SIZE - flash_addr) return -1;
    if (flash_addr >= PA_FLASH_APP_WINDOW_START && flash_addr <= PA_FLASH_APP_WINDOW_END && len <= PA_FLASH_APP_WINDOW_END - flash_addr + 1u) {
        const uint8_t *xip_ptr = (const uint8_t *)(0x02000000u + (flash_addr - PA_FLASH_APP_WINDOW_START));
        memcpy(dst, xip_ptr, len);
        return 0;
    }
    pa_flash_dev_init();
    pa_spi_nor_begin();
    while (len && rc == 0) {
        const uint32_t chunk = len > sizeof b ? (uint32_t)sizeof b : len;
        if (pa_flash_dev_op(0u, flash_addr, b, chunk, 0) != 0) rc = -4;
        else memcpy(out, b, chunk);
        flash_addr += chunk; out += chunk; len -= chunk;
        if ((flash_addr & 0xFFFu) == 0u) plat_wdt_feed();
    }
    pa_spi_nor_end();
    return rc;
}

void pa_spi_nor_sync(void) {
}

#endif
