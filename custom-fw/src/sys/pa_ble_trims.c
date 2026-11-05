/* src/sys/pa_ble_trims.c - the radio calibration read from the unit's own record log (include/pa_ble_trims.h). */
#include <string.h>
#include "../../include/pa_ble_trims.h"
#include "../../include/pa_spi_nor.h"

uint16_t pa_ble_crc16_xmodem(const uint8_t *d, uint32_t n)
{
    uint16_t c = 0u;
    for (uint32_t i = 0; i < n; i++) {
        c ^= (uint16_t)((uint16_t)d[i] << 8);
        for (int k = 0; k < 8; k++) c = (uint16_t)((c & 0x8000u) ? ((c << 1) ^ 0x1021u) : (c << 1));
    }
    return c;
}

uint32_t pa_ble_trims_parse(pa_ble_trims_read_fn rd, pa_ble_trims_t *out)
{
    uint8_t hdr[4], data[96];
    uint32_t found = 0u, off = 4u;
    pa_ble_trims_t t;
    memset(&t, 0, sizeof t);
    if (!rd || !out) return 0u;
    if (rd(0u, hdr, 4u) != 0 || hdr[0] != 0x55u || hdr[1] != 0xAAu || hdr[2] != 0xAAu || hdr[3] != 0x55u) return 0u;
    while (off + 4u <= PA_BLE_TRIM_SIZE) {
        if (rd(off, hdr, 4u) != 0) break;
        const uint32_t id = (uint32_t)hdr[1] | (((uint32_t)hdr[2] & 0x0Fu) << 8);
        const uint32_t ln = ((uint32_t)hdr[2] >> 4) | ((uint32_t)hdr[3] << 4);
        if (ln == 0u || off + 4u + ln > PA_BLE_TRIM_SIZE) break;                  /* the end of the log */
        if (ln <= sizeof data) {
            if (rd(off + 4u, data, ln) != 0) break;
            if ((uint8_t)(pa_ble_crc16_xmodem(data, ln) & 0xFFu) != hdr[0]) break;  /* a record that fails its check ends the log */
            if (id == 106u && ln == 2u)  { memcpy(t.x106, data, 2u);  found |= 1u; }
            if (id == 107u && ln == 7u)  { memcpy(t.x107, data, 7u);  found |= 2u; }
            if (id == 108u && ln == 20u) { memcpy(t.x108, data, 20u); found |= 4u; }
            if (id == 187u && ln == 68u) {
                const uint16_t c = pa_ble_crc16_xmodem(data, 64u);
                if (data[64] == (uint8_t)c && data[65] == (uint8_t)(c >> 8) && data[66] == 0u && data[67] == 0u) { memcpy(t.x187, data, 68u); found |= 8u; }
                else found &= ~8u;                                                  /* the last record of the id wins: a damaged newer one is no calibration */
            }
        } else {
            break;                         /* a record too long to verify through the small buffer (none of the stock's is): the log is not read past it */
        }
        off += 4u + ln;
    }
    if (found == 0xFu) *out = t;
    return found;
}

#if !defined(PA_HOST_BUILD) && !defined(PA_HOST_SIM) && !defined(PA_EMU_DLL) && !defined(__wasm__)
static int flash_rd(uint32_t off, uint8_t *buf, uint32_t n) { return pa_spi_nor_read(PA_BLE_TRIM_AREA + off, buf, n); }
int pa_ble_trims_load_from_flash(pa_ble_trims_t *out) { return pa_ble_trims_parse(flash_rd, out) == 0xFu; }
#else
/* a host or emulator build has no stock log */
__attribute__((weak)) int pa_ble_trims_load_from_flash(pa_ble_trims_t *out) { (void)out; return 0; }
#endif
