/* SPDX-License-Identifier: GPL-3.0-only */
/* Copyright (C) 2026 The Onyx Authors (@28spawn) */
/*
 * pa_spi_nor.h — Pocket Amp SPI NOR Flash Hardware & Emulator Abstraction
 *
 * Provides physical / simulated NOR flash operations:
 *   - 4 KB sector erase (0x20)
 *   - 256 B page write (0x02)
 *   - Fast read (0x0B / direct memory pointer)
 *   - Persistent backing file (pocket_amp_flash.bin) in simulation/host builds
 */
#ifndef PA_SPI_NOR_H
#define PA_SPI_NOR_H

#include <stdint.h>
#include "pa_flash_map.h"

int  pa_spi_nor_init(void);
int  pa_spi_nor_erase_sector(uint32_t flash_addr);
int  pa_spi_nor_write(uint32_t flash_addr, const void *data, uint32_t len);
int  pa_spi_nor_read(uint32_t flash_addr, void *dst, uint32_t len);
void pa_spi_nor_sync(void);
/* A batch of writes (a preset bank, a settings record) is bracketed by begin / end. On the device begin mutes the output, waits for the fade, parks core 1 and masks the interrupts (the flash cannot be
 * fetched from while the controller is in command mode), end undoes it; a single erase / write / read outside the app window does the same by itself. They nest. No-ops on the host. */
void pa_spi_nor_begin(void);
void pa_spi_nor_end(void);

#endif /* PA_SPI_NOR_H */
