/* SPDX-License-Identifier: GPL-3.0-only */
/* Copyright (C) 2026 The Onyx Authors (@28spawn) */
/* include/pa_ota_session.h - the firmware's half of the USB-MIDI update sequence (step 1) - USB-MIDI only.
 *
 * The host (the web editor's Firmware tab, or standard updater) sends the "upgrade" frame F0 22 24 35 7F F7. The running firmware then pulls parts of the
 * package from the host with cmd 0x30 read requests, checks them, stages the package's own update loader in the five 4 KB sectors 0xB8000..0xBCFFF (the top of the VM area), asks the host for "success"
 * (nothing is committed without it), writes the update record (flash 0xBCF00 and RAM 0x01C7FD88) and resets the core: the boot area's SPL runs the staged loader, which writes the new firmware.
 * The boot area (0..0x3FFF) is never touched by this file. A failure erases the staging area and the firmware carries on.
 *
 * The protocol and the staging layout are those of the Pendel project's chip/ota.c (MPL-2.0, (C) 2026 Leo Kuroshita, Hugelton Instruments), which runs them on this very unit (it proves the SPL side);
 * this is a port to this firmware's own flash, USB and screen code. The other frame, F0 22 24 35 7D F7 (the diagnostic soft key), still resets into the chip's ROM download mode.
 *
 * The hooks below are the platform: src/board/ota_hw.c on the device, the test's own on the host.
 */
#ifndef PA_OTA_SESSION_H
#define PA_OTA_SESSION_H
#include <stdint.h>

#if defined(BOARD_BLACKBOX)
#error "pa_ota_session.h stages the update in the Pocket AMP's sectors: BOARD_BLACKBOX has no staging area yet"
#endif

#define PA_OTA_AREA       0x000B8000u        /* the staged loader: 5 x 4 KB */
#define PA_OTA_AREA_LEN    0x00005000u
#define PA_OTA_RES         (PA_OTA_AREA + PA_OTA_AREA_LEN - 0x100u)   /* the update record: 4 KB boundary - 256, where the SPL looks */
#define PA_OTA_FRAME_MAX   640u               /* a 512-byte response is 605 wire bytes */

/* hooks (return 0 = ok unless said) */
int      pa_ota_hw_send(const uint8_t *msg, uint32_t n);          /* one complete F0..F7 message to the host; 0 ok */
int      pa_ota_hw_frame(uint8_t *buf, uint32_t cap, uint32_t *n); /* the next received SysEx (the 7-bit bytes between F0 and F7) copied into buf; 1 if there is one */
uint8_t *pa_ota_hw_scratch(uint32_t *n);                          /* a RAM scratch of at least 2.5 KB for the session (the device lends a buffer that is idle while the session runs) */
uint32_t pa_ota_hw_ms(void);
void     pa_ota_hw_idle(void);                                    /* called while waiting: USB poll, watchdog */
int      pa_ota_hw_erase(uint32_t off);                           /* 4 KB sector, only inside the staging area */
int      pa_ota_hw_prog(uint32_t off, const void *p, uint32_t n);
int      pa_ota_hw_read(uint32_t off, void *p, uint32_t n);
void     pa_ota_hw_show(uint32_t step, int32_t code);             /* step 1..5 progress; 9 = ended with code (1 = dry run) */
void     pa_ota_hw_wipe(void);                                    /* the update is certain (the loader is staged, its record is in the flash): this firmware erases what it created (stored captures and cabinets), once, just before the reset */
void     pa_ota_hw_commit(const uint8_t *parm);                   /* the record into RAM, a core reset: never returns on the device */

int      pa_ota_session_run(void);       /* 0 never returns when committed; else the failure code (<0) */
int      pa_ota_boot_cleanup(void);      /* a staging area an update left behind is erased: 1 if there was one, 0 none, <0 read error */
uint32_t pa_ota_session_busy(void);      /* 1 while a session runs (the normal SysEx parser must not see the frames) */

/* self-contained helpers, exposed for the host test */
uint32_t pa_ota_pack7(const uint8_t *in, uint32_t n, uint8_t *out);
uint32_t pa_ota_unpack7(const uint8_t *in, uint32_t n, uint8_t *out, uint32_t max);
#endif
