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

#endif
