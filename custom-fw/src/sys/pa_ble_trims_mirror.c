/* SPDX-License-Identifier: GPL-3.0-only */
/* Copyright (C) 2026 The Onyx Authors (@28spawn) */
/* src/sys/pa_ble_trims_mirror.c - a copy of the radio calibration in this firmware's own flash (include/pa_ble_trims.h, "the mirror").
 *
 * One 128-byte record at PA_FLASH_TRIMS_MIRROR_ADDR: magic 'TRMR', version, the 97 bytes of the four records, zero padding, CRC-32 of everything before it. It is only ever PROGRAMMED into a blank
 * area (flash bits go 1 -> 0 without an erase); a different or damaged record is replaced through pa_flash_shared_rewrite() (settings.c), which erases the shared sector and puts the settings, the
 * Bluetooth bond and this record back. Nothing here touches the stock's log at 0xB5000. */
#include <string.h>
#include "../../include/pa_ble_trims.h"
#include "../../include/pa_flash_map.h"
#include "../../include/pa_spi_nor.h"
#include "../../include/pa_preset.h"          /* pa_crc32 */

extern int pa_flash_shared_rewrite(const void *reg, uint32_t reg_len);   /* settings.c */

#define MIRROR_MAGIC 0x524D5254u              /* 'TRMR' */
#define MIRROR_VER   1u
_Static_assert(sizeof(pa_ble_trims_t) == 97u, "the four records are 97 bytes");
_Static_assert(PA_BLE_TRIMS_MIRROR_REC <= PA_FLASH_TRIMS_MIRROR_SIZE, "the record fits its area");

static uint8_t s_want[PA_BLE_TRIMS_MIRROR_REC];     /* the record a rewrite in progress must put back instead of the old one */
static uint8_t s_want_valid;

void pa_ble_trims_mirror_build(const pa_ble_trims_t *t, uint8_t rec[PA_BLE_TRIMS_MIRROR_REC])
{
    memset(rec, 0, PA_BLE_TRIMS_MIRROR_REC);
    const uint32_t magic = MIRROR_MAGIC;
    memcpy(rec, &magic, 4);
    rec[4] = (uint8_t)MIRROR_VER;
    memcpy(rec + 8, t, sizeof *t);
    const uint32_t crc = pa_crc32(rec, PA_BLE_TRIMS_MIRROR_REC - 4u);
    memcpy(rec + PA_BLE_TRIMS_MIRROR_REC - 4u, &crc, 4);
}

int pa_ble_trims_mirror_parse(const uint8_t rec[PA_BLE_TRIMS_MIRROR_REC], pa_ble_trims_t *out)
{
    uint32_t magic, crc;
    memcpy(&magic, rec, 4); memcpy(&crc, rec + PA_BLE_TRIMS_MIRROR_REC - 4u, 4);
    if (magic != MIRROR_MAGIC || rec[4] != MIRROR_VER || crc != pa_crc32(rec, PA_BLE_TRIMS_MIRROR_REC - 4u)) return 0;
    if (out) memcpy(out, rec + 8, sizeof *out);
    return 1;
}

static int read_rec(uint8_t rec[PA_BLE_TRIMS_MIRROR_REC])
{
    return pa_spi_nor_read(PA_FLASH_TRIMS_MIRROR_ADDR, rec, PA_BLE_TRIMS_MIRROR_REC) == 0;
}

int pa_ble_trims_mirror_load(pa_ble_trims_t *out)
{
    uint8_t rec[PA_BLE_TRIMS_MIRROR_REC];
    return read_rec(rec) && pa_ble_trims_mirror_parse(rec, out);
}

int pa_ble_trims_mirror_snapshot(uint8_t rec[PA_BLE_TRIMS_MIRROR_REC])
{
    if (s_want_valid) { memcpy(rec, s_want, PA_BLE_TRIMS_MIRROR_REC); return 1; }
    uint8_t cur[PA_BLE_TRIMS_MIRROR_REC];
    if (!read_rec(cur) || !pa_ble_trims_mirror_parse(cur, 0)) return 0;
    memcpy(rec, cur, PA_BLE_TRIMS_MIRROR_REC);
    return 1;
}

void pa_ble_trims_mirror_store(const pa_ble_trims_t *t)
{
    uint8_t want[PA_BLE_TRIMS_MIRROR_REC], cur[PA_BLE_TRIMS_MIRROR_REC];
    pa_ble_trims_mirror_build(t, want);
    if (!read_rec(cur)) return;
    if (memcmp(cur, want, PA_BLE_TRIMS_MIRROR_REC) == 0) return;               /* already there */
    int blank = 1;
    for (uint32_t i = 0; i < PA_BLE_TRIMS_MIRROR_REC; i++) if (cur[i] != 0xFFu) { blank = 0; break; }
    if (blank) { (void)pa_spi_nor_write(PA_FLASH_TRIMS_MIRROR_ADDR, want, PA_BLE_TRIMS_MIRROR_REC); pa_spi_nor_sync(); return; }
    memcpy(s_want, want, PA_BLE_TRIMS_MIRROR_REC); s_want_valid = 1u;          /* a different or damaged record: the sector is rewritten with the new one */
    (void)pa_flash_shared_rewrite(0, 0u);
    s_want_valid = 0u;
}
