/* SPDX-License-Identifier: GPL-3.0-only */
/* Copyright (C) 2026 The Onyx Authors (@28spawn) */
/* custom-fw/include/pa_diag.h - a private diagnostic channel
 *
 * A manufacturer SysEx (0x7D, the "educational/non-commercial" ID) used for host
 * diagnostic reads. It allows reading regions of SPI NOR flash and RAM for hardware
 * inspection and verification. This is a *read* - it changes nothing.
 *
 *   F0 7D 01 <addr u32 LE> <len u16 LE> F7      ->  F0 7D 81 <len u16 LE> <data> F7
 *
 * The reply is capped at 256 bytes per frame; ask for more and read it in pieces.
 */
#ifndef PA_DIAG_H
#define PA_DIAG_H

#include <stdint.h>

#define PA_DIAG_MFG_ID   0x7Du
#define PA_DIAG_CMD_FLASH_READ 0x01u
#define PA_DIAG_REP_FLASH_READ 0x81u
/* RAM read (read-only, SRAM 0x01C00000-0x01C7FFFF only, device builds only):
 *   F0 7D 02 <addr u32 LE> <len u16 LE> F7  ->  F0 7D 82 <len u16 LE> <addr u32 LE> <data> F7
 * (every frame is 8-to-7 packed, like the flash read). A refused or failed read answers len = 0xFFFF.
 * The PC test runner (tools/hw_test.py) uses it to read the black box, the crash record, the
 * test-state block and any variable by name while the unit runs. */
#define PA_DIAG_CMD_RAM_READ   0x02u
#define PA_DIAG_REP_RAM_READ   0x82u
#define PA_DIAG_CMD_PROFILE    0x03u   /* 7D 03 <op u8: 0 read, 1 write> <value u32 LE>: the Bluetooth experiment switches (pa_ble_diag.h); reply 7D 83 <current u32 LE> */
#define PA_DIAG_REP_PROFILE    0x83u
typedef uint32_t (*pa_diag_profile_fn)(int write, uint32_t value);
void pa_diag_set_profile_hook(pa_diag_profile_fn fn);
#define PA_DIAG_RAM_LO         0x01C00000u
#define PA_DIAG_RAM_HI         0x01C80000u   /* exclusive */
#define PA_DIAG_MAX_REPLY 256u

/* The reply sink: the USB MIDI path. */
typedef void (*pa_diag_reply_fn)(const uint8_t *data, uint32_t len);

/* Set where the replies go (the USB MIDI TX path). */
void pa_diag_set_reply_sink(pa_diag_reply_fn fn);

/* Feed one byte of the MIDI stream. */
void pa_diag_feed(uint8_t b);

/* The flash reader the diagnostic uses.  It is set at init so this module does not
 * depend on the board layer directly (and so the host build can stub it). */
typedef int (*pa_diag_flash_read_fn)(uint32_t addr, void *dst, uint32_t len);
void pa_diag_set_flash_reader(pa_diag_flash_read_fn fn);

#endif /* PA_DIAG_H */
