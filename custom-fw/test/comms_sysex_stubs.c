/* SPDX-License-Identifier: GPL-3.0-only */
/* Copyright (C) 2026 The Onyx Authors (@28spawn) */
/* test/comms_sysex_stubs.c - the storage and settings hooks src/sys/sysex.c calls, stubbed for the host communications tests.
 * Only the flash, preset and settings boundary is stubbed here; the SysEx parser and its dispatch (sysex.c) are the production code.
 * The same stubs are inlined in test_sysex_read.c; this file lets the communications suite link sysex.c without copying them again. */
#include <stdint.h>
#include <stddef.h>

uint32_t pa_crc32(const void *data, uint32_t len) { (void)data; (void)len; return 0; }
int      pa_preset_save(int slot, const void *p) { (void)slot; (void)p; return 0; }
int      pa_preset_restore(int slot, const void *p) { (void)slot; (void)p; return 0; }
int      pa_spi_nor_read(uint32_t addr, void *dst, uint32_t n) { for (uint32_t i = 0; i < n; i++) ((uint8_t *)dst)[i] = (uint8_t)(addr + i); return 0; }   /* the flash-read command (cmd 84): a pattern */
int      pa_preset_load(int slot, void *p) { (void)slot; (void)p; return -1; }
int      pa_settings_save(const void *s) { (void)s; return 0; }
int      pa_settings_load(void *s) { (void)s; return -1; }
int      pa_spi_nor_erase_sector(uint32_t a) { (void)a; return 0; }
int      pa_spi_nor_write(uint32_t a, const void *d, uint32_t l) { (void)a; (void)d; (void)l; return 0; }
void     pa_spi_nor_sync(void) {}
int      pa_user_nam_add(const char *name, uint8_t type, uint8_t voicing, uint8_t has_cab,
                         float head_scale, const void *blob, uint32_t len, const void *meta) {
    (void)name; (void)type; (void)voicing; (void)has_cab; (void)head_scale; (void)blob; (void)len; (void)meta;
    return 0;
}
int      pa_user_nam_get_count(void) { return 0; }
int      pa_user_nam_is_active(uint8_t i) { (void)i; return 0; }
int      pa_user_nam_delete(uint8_t i) { (void)i; return -1; }
int      pa_user_nam_set_name(uint8_t i, const char *n) { (void)i; (void)n; return -1; }
int      pa_user_cab_add(const char *n, int slot, const int32_t *t, uint32_t c) { (void)n; (void)slot; (void)t; (void)c; return -5; }
int      pa_user_cab_delete(uint8_t i) { (void)i; return -1; }
int      pa_user_nam_clear(void) { return -1; }
int      pa_user_cab_clear(void) { return -1; }
int      pa_user_cab_rename(uint8_t i, const char *n) { (void)i; (void)n; return -1; }

