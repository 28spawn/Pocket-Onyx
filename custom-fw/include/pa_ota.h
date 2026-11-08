/* SPDX-License-Identifier: GPL-3.0-only */
/* Copyright (C) 2026 The Onyx Authors (@28spawn) */
/* custom-fw/include/pa_ota.h - the app side of the JieLi OTA handshake
 *
 * The firmware updater reaches the Pocket Amp over USB MIDI - the
 * Windows "KS" kernel-streaming transport - and speaks two frames:
 *
 *   F0 00 32 45 00 00 00 40 7F F7   query: "who are you?"
 *     -> the device answers with a 34-byte ID block on the header 00 32 45 58
 *
 *   F0 22 24 35 7F F7              "enter upgrade"
 *     -> the device switches to the OTA mode and re-enumerates as 4d4a:4155
 *
 * After that the *downloader* does the work: the device requests the image bytes from
 * the host and the host serves them, then the host answers "success\0" for the verify
 * and finish steps.  So the app's job ends at the reboot - it never sees the image.
 * [JieLi AC791N OTA protocol]
 *
 * The same frames are what the recovery route needs: the stock .fwsc is flashed back
 * through this path, so being able to answer the query and honour the upgrade frame is
 * the app's half of "get the original firmware back".
 */
#ifndef PA_OTA_H
#define PA_OTA_H

#include <stdint.h>

/* The ID block the query is answered with (the report's "34-byte ID block"). */
#define PA_OTA_ID_REPLY_LEN 41u   /* F0 + 39 packed bytes (34 decoded: 00 59 11 len24 + 27-byte identity + checksum) + F7 */

/* The frame lengths, so the assembler can tell a real frame from noise. */
#define PA_OTA_FRAME_QUERY_LEN   10u   /* F0 00 32 45 00 00 00 40 7F F7 */
#define PA_OTA_FRAME_UPGRADE_LEN  6u   /* F0 22 24 35 7F F7 */

/* The frames the app recognises. */
typedef enum {
    PA_OTA_FRAME_NONE = 0,
    PA_OTA_FRAME_QUERY,       /* "who are you?" -> answer with the ID block */
    PA_OTA_FRAME_UPGRADE,     /* F0 22 24 35 7F F7: the vendor's MIDI update session (step 1): pa_ota_session.h */
    PA_OTA_FRAME_SOFTKEY      /* F0 22 24 35 7D F7: the stock's soft key -> reboot into the chip's ROM download mode */
} pa_ota_frame_t;

/* Classify a complete frame (the bytes between F0 and F7 inclusive).  Anything that is
 * not one of the two frames returns PA_OTA_FRAME_NONE. */
pa_ota_frame_t pa_ota_classify(const uint8_t *frame, uint32_t len);

/* Build the ID block: the header 00 32 45 58, the device's identity, the F7. */
uint32_t pa_ota_build_id_reply(uint8_t *out, uint32_t cap);

/* The identity the ID block carries, and the device-info record the editor asks for
 * (SysEx cmd 17) - the same facts, so the two cannot drift apart. */
const char *pa_ota_product_name(void);   /* "Pocket AMP"        */
const char *pa_ota_device_id(void);      /* "Pocket AMP_105"    */
const char *pa_ota_chip_name(void);      /* "AC791N"            */
const char *pa_ota_version_string(void); /* "V105"              */
const char *pa_ota_serial(void);         /* the device's serial */

#endif /* PA_OTA_H */
