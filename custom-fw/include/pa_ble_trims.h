/* SPDX-License-Identifier: GPL-3.0-only */
/* Copyright (C) 2026 The Onyx Authors (@28spawn) */
/*
 * pa_ble_trims.h - the radio calibration of THIS unit, read from the stock firmware's record log at boot.
 *
 * The Bluetooth radio will only start with four records of the SDK's "VM" store, which the stock firmware wrote at the factory: id 106 (crystal trims, 2 bytes), 107 (PA, 7), 108 (PA digital gain,
 * 20) and 187 (RF table, 68: 64 bytes, a CRC-16/XMODEM of them little-endian, then 00 00). The store is the stock's record log at flash 0xB5000 (include/pa_flash_map.h: read only, never
 * written): 55 AA AA 55, then records from +4: [check = low byte of the CRC-16/XMODEM of the data][id 7:0][id 11:8 | length 3:0 << 4][length 11:4][data]; the log ends at the first record whose
 * check fails, and the last record of an id wins (third_party/optimist_ble/src/ble_vm.h, tools/gen_ble_trims.py).
 *
 * Until pack 192 the calibration was compiled in from ONE unit's backup (ble_trims_unit.h), so every unit built from it ran with that unit's calibration and the same Bluetooth address. Now the
 * firmware reads its own: the parser below is pure (a reader callback), so a host test feeds it the real log, damaged logs and a log with two records of an id.
 */
#ifndef PA_BLE_TRIMS_H
#define PA_BLE_TRIMS_H

#include <stdint.h>

#define PA_BLE_TRIM_AREA  0x000B5000u
#define PA_BLE_TRIM_SIZE  0x1000u            /* the stock's log is one 4 KB sector */

typedef struct {
    uint8_t x106[2], x107[7], x108[20], x187[68];
} pa_ble_trims_t;

/* reads n bytes at offset off of the log; returns 0 when it did */
typedef int (*pa_ble_trims_read_fn)(uint32_t off, uint8_t *buf, uint32_t n);

/* Parse the log through `rd`. Returns a bit mask of the records found AND valid (bit 0: 106, 1: 107, 2: 108, 3: 187); 0xF = all four, the only complete calibration. */
uint32_t pa_ble_trims_parse(pa_ble_trims_read_fn rd, pa_ble_trims_t *out);

/* Read the unit's own log from the flash (0xB5000) into *out. Returns 1 when all four records are there and valid. */
int pa_ble_trims_load_from_flash(pa_ble_trims_t *out);

uint16_t pa_ble_crc16_xmodem(const uint8_t *d, uint32_t n);

/* ---- 1.16: the mirror. The stock's log at 0xB5000 is written by nobody but the stock firmware, and a flash tool run erased it once (the radio then refused to start: "no calibration"). So once the log
 * has been read, a copy of the four records is kept in this firmware's own part of the flash (include/pa_flash_map.h PA_FLASH_TRIMS_MIRROR_ADDR, inside the shared sector, carried across every
 * rewrite of that sector). At boot the order is: the stock's log, then the mirror, then (a build made for one unit) the copy compiled in. */
#define PA_BLE_TRIMS_MIRROR_REC 128u                                  /* the record: magic, version, the 97 bytes, pad, CRC-32 */
int  pa_ble_trims_mirror_load(pa_ble_trims_t *out);                   /* 1 = a valid mirror was read into *out */
void pa_ble_trims_mirror_store(const pa_ble_trims_t *t);              /* call from the main loop: may write the flash, only when the mirror is missing or differs */
int  pa_ble_trims_mirror_snapshot(uint8_t rec[PA_BLE_TRIMS_MIRROR_REC]); /* for settings.c: the record the erased shared sector must get back (1 = there is one) */
/* pure, for the host test: build and check a record */
void pa_ble_trims_mirror_build(const pa_ble_trims_t *t, uint8_t rec[PA_BLE_TRIMS_MIRROR_REC]);
int  pa_ble_trims_mirror_parse(const uint8_t rec[PA_BLE_TRIMS_MIRROR_REC], pa_ble_trims_t *out);

#endif
