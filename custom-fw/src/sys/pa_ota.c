/* SPDX-License-Identifier: GPL-3.0-only */
/* Copyright (C) 2026 The Onyx Authors (@28spawn) */
/* custom-fw/src/sys/pa_ota.c - the app side of the JieLi OTA handshake
 *
 * See include/pa_ota.h for the protocol.  Everything here is a pure function of the
 * byte stream, so the host test (custom-fw/test/test_ota.c) can check it without a
 * device.
 */
#include "pa_ota.h"
#include "pa_config.h"

#include <string.h>

/* The identity the ID block carries.
 *
 * The device name and the chip come from the stock's own data: the app's config table
 * holds "Pocket AMP_105" (the name the device presents) and the config partition holds
 * CHIP_NAME=AC791N.  The version is PA_FW_VERSION, the same number the editor's
 * device-info record reports. */
const char *pa_ota_product_name(void) { return "Pocket AMP"; }
const char *pa_ota_device_id(void)    { return "Pocket AMP_105"; }
const char *pa_ota_chip_name(void)    { return "AC791N"; }

const char *pa_ota_version_string(void) {
    static char buf[8];
    /* "V105" - the version the editor and the updater compare against */
    buf[0] = 'V';
    buf[1] = (char)('0' + (PA_FW_VERSION / 100) % 10);
    buf[2] = (char)('0' + (PA_FW_VERSION / 10) % 10);
    buf[3] = (char)('0' + PA_FW_VERSION % 10);
    buf[4] = 0;
    return buf;
}

const char *pa_ota_serial(void) {
    /* The device's serial, as the System Info page shows it. */
    return "PA2410-03F2A";
}

/* ---- the frame classifier ------------------------------------------------ */

pa_ota_frame_t pa_ota_classify(const uint8_t *f, uint32_t n) {
    if (!f) return PA_OTA_FRAME_NONE;
    if (n == PA_OTA_FRAME_QUERY_LEN &&
        f[0] == 0xF0u && f[1] == 0x00u && f[2] == 0x32u && f[3] == 0x45u &&
        f[4] == 0x00u && f[5] == 0x00u && f[6] == 0x00u && f[7] == 0x40u &&
        f[8] == 0x7Fu && f[9] == 0xF7u) {
        return PA_OTA_FRAME_QUERY;
    }
    if (n == PA_OTA_FRAME_UPGRADE_LEN &&
        f[0] == 0xF0u && f[1] == 0x22u && f[2] == 0x24u && f[3] == 0x35u &&
        (f[4] == 0x7Fu || f[4] == 0x7Du) && f[5] == 0xF7u) {
        return f[4] == 0x7Du ? PA_OTA_FRAME_SOFTKEY : PA_OTA_FRAME_UPGRADE;      /* 7D = the soft key into the ROM download mode (what the stock does), 7F = the update session */
    }
    return PA_OTA_FRAME_NONE;
}

/* ---- the ID block -------------------------------------------------------- */

uint32_t pa_ota_build_id_reply(uint8_t *out, uint32_t cap) {
    if (!out || cap < PA_OTA_ID_REPLY_LEN) return 0;

    /* The same message the original firmware answers (and the Pendel installer's parser requires): cmd 0x11 in the 00 59 framing - 00 59 11 | len24 = 27 | the identity "Pocket AMP_105"
     * zero-padded to 27 bytes | ~sum of the 27 bytes - packed 8 to 7 bits between F0 and F7. Its first four wire bytes are the "00 32 45 58" header the notes call the ID block's. Before pack 193
     * the identity text was put on the wire raw behind that header: a host that decodes the 7-bit packing saw 28 bytes of garbage, so no web installer ever recognised the unit. */
    uint8_t inner[6 + 27 + 1];
    const char *id = pa_ota_device_id();
    uint32_t i, sum = 0;
    inner[0] = 0x00u; inner[1] = 0x59u; inner[2] = 0x11u; inner[3] = 27u; inner[4] = 0u; inner[5] = 0u;
    for (i = 0; i < 27u; i++) { inner[6 + i] = (id[i] && (uint32_t)strlen(id) > i) ? (uint8_t)id[i] : 0u; sum += inner[6 + i]; }
    inner[33] = (uint8_t)~sum;
    out[0] = 0xF0u;
    uint32_t n = 1u;
    {   /* pack 8 to 7, LSB first */
        uint32_t acc = 0, nb = 0;
        for (i = 0; i < sizeof inner; i++) {
            acc |= (uint32_t)inner[i] << nb; nb += 8u;
            while (nb >= 7u) { out[n++] = (uint8_t)(acc & 0x7Fu); acc >>= 7; nb -= 7u; }
        }
        if (nb) out[n++] = (uint8_t)(acc & 0x7Fu);
    }
    out[n++] = 0xF7u;
    return n;
}
