/* SPDX-License-Identifier: GPL-3.0-only */
/* Copyright (C) 2026 The Onyx Authors (@28spawn) */
/* src/board/ota_hw.c - the device side of the vendor MIDI update session (src/sys/pa_ota_session.c): USB-MIDI in and out, the flash (the five staging sectors only), the screen and the reset into the staged loader.
 * Device only: the host builds use the refusing stubs in pa_ota_session.c. */
#include <stdint.h>
#include <string.h>
#include "../../include/pa_ota_session.h"
#include "../../include/pa_spi_nor.h"
#include "../../include/pa_usb_dev.h"
#include "../../include/pa_plat.h"
#include "../../include/pa_hw.h"

extern int  pa_usb_ble_rx_pop(uint8_t *b);
extern int  pa_usb_ble_tx_raw(const uint8_t *data, uint32_t len);
extern void plat_wdt_feed(void);
extern void pa_c1_halt_if_running(void);
extern void pa_ui_ota_notice(const char *a, const char *b);

int pa_ota_hw_send(const uint8_t *msg, uint32_t n) {
    for (uint32_t t = plat_ms(); (uint32_t)(plat_ms() - t) < 200u; pa_ota_hw_idle())
        if (pa_usb_ble_tx_raw(msg, n) >= 0) return 0;
    return -1;
}

/* one SysEx at a time: the bytes between F0 and F7 (realtime bytes are skipped, any other status byte drops the frame); the frame is built in the caller's buffer, so a frame may arrive over several calls */
static uint32_t s_fl;
static int s_in;
int pa_ota_hw_frame(uint8_t *buf, uint32_t cap, uint32_t *n) {
    uint8_t b;
    while (pa_usb_ble_rx_pop(&b)) {
        if (b >= 0xF8u) continue;
        if (b == 0xF0u) { s_fl = 0u; s_in = 1; continue; }
        if (!s_in) continue;
        if (b == 0xF7u) { s_in = 0; *n = s_fl; s_fl = 0u; return 1; }
        if (b >= 0x80u) { s_in = 0; continue; }
        if (s_fl < cap) buf[s_fl++] = b; else s_in = 0;
    }
    return 0;
}
extern uint8_t *pa_user_nam_scratch(uint32_t *n);
uint8_t *pa_ota_hw_scratch(uint32_t *n) { return pa_user_nam_scratch(n); }
uint32_t pa_ota_hw_ms(void) { return plat_ms(); }
void pa_ota_hw_idle(void) { pa_usb_dev_poll(); plat_wdt_feed(); }

static int in_area(uint32_t off, uint32_t n) { return off >= PA_OTA_AREA && n <= PA_OTA_AREA_LEN && off - PA_OTA_AREA <= PA_OTA_AREA_LEN - n; }
int pa_ota_hw_erase(uint32_t off) {
    if (!in_area(off, 0x1000u) || (off & 0xFFFu)) return -8;
    pa_spi_nor_begin();
    const int rc = pa_spi_nor_erase_sector(off);
    pa_spi_nor_end();
    return rc;
}
int pa_ota_hw_prog(uint32_t off, const void *p, uint32_t n) {
    if (!in_area(off, n)) return -8;
    pa_spi_nor_begin();
    const int rc = pa_spi_nor_write(off, p, n);
    pa_spi_nor_sync();
    pa_spi_nor_end();
    return rc;
}
int pa_ota_hw_read(uint32_t off, void *p, uint32_t n) { return pa_spi_nor_read(off, p, n); }

void pa_ota_hw_show(uint32_t step, int32_t code) {
    static const char *const STEP[] = { "", "PACKAGE", "CHECK HEAD", "LOADER", "CONFIRM", "RESTART" };
    if (step < 9u) { pa_ui_ota_notice("UPDATE", STEP[step < 6u ? step : 0u]); return; }
    char b[16] = "FAILED  -";
    uint32_t k = 9u, v = (uint32_t)(-code);
    if (v >= 10u) b[k++] = (char)('0' + v / 10u % 10u);
    b[k++] = (char)('0' + v % 10u); b[k] = '\0';
    pa_ui_ota_notice("UPDATE", code == 1 ? "DRY RUN OK" : b);
    for (uint32_t t = plat_ms(); (uint32_t)(plat_ms() - t) < 1500u; ) pa_ota_hw_idle();
}

/* the update is certain: erase what this firmware created in the user store, so no registry names a capture or cabinet the loader is about to erase and nothing of ours is left behind */
extern int pa_user_nam_clear(void);
extern int pa_user_cab_clear(void);
void pa_ota_hw_wipe(void) { (void)pa_user_nam_clear(); (void)pa_user_cab_clear(); }

/* the record into RAM at 0x01C7FD88 and a core reset: the SPL then runs the staged loader (as the Pendel project's fm1_enter_update, which runs on this unit) */
void pa_ota_hw_commit(const uint8_t *parm) {
    pa_c1_halt_if_running();
    __asm__ volatile("cli" ::: "memory");
    if (!(PA_READ32(0x01EEE240u) & 1u)) PA_WRITE32(0x01EEE240u, 0xE7u);          /* drop the CPU0 write limits so the record can be written */
    PA_WRITE32(JL_CPU0_WR_LIMIT_EN, 0u);
    volatile uint8_t *rec = (volatile uint8_t *)(uintptr_t)0x01C7FD88u;
    for (uint32_t i = 0; i < 112u; i++) rec[i] = parm[i];
    PA_WRITE32(0x00010000u, PA_READ32(0x00010000u) | 0x10u);                      /* PWR_CON: core reset */
    for (;;) { }
}
