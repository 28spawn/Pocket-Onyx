/* custom-fw/test/test_ota.c - the app side of the JieLi OTA handshake
 *
 * The two frames the updater sends over USB MIDI, and the ID block the query is answered
 * with.  Pure logic, so it is checked here rather than on a device.
 */
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "pa_ota.h"
#include "pa_config.h"

static int fails = 0;

static void check(int ok, const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    printf(ok ? "  ok   " : "  FAIL ");
    vprintf(fmt, ap);
    printf("\n");
    va_end(ap);
    if (!ok) fails++;
}

static const uint8_t k_query[10] =
    { 0xF0u, 0x00u, 0x32u, 0x45u, 0x00u, 0x00u, 0x00u, 0x40u, 0x7Fu, 0xF7u };
static const uint8_t k_upgrade[6] =
    { 0xF0u, 0x22u, 0x24u, 0x35u, 0x7Fu, 0xF7u };

/* does the block contain this text? (memmem is not in C99) */
static int contains(const uint8_t *hay, uint32_t n, const char *needle) {
    uint32_t l = (uint32_t)strlen(needle);
    if (l == 0 || l > n) return 0;
    for (uint32_t i = 0; i + l <= n; i++) {
        if (memcmp(&hay[i], needle, l) == 0) return 1;
    }
    return 0;
}

int main(void) {
    printf("=== the OTA handshake ===\n");

    check(pa_ota_classify(k_query, sizeof(k_query)) == PA_OTA_FRAME_QUERY,
          "the query frame is recognised");
    check(pa_ota_classify(k_upgrade, sizeof(k_upgrade)) == PA_OTA_FRAME_UPGRADE,
          "the 0x7F update-session frame is recognised");
    {
        const uint8_t k_upgrade_7d[6] = { 0xF0u, 0x22u, 0x24u, 0x35u, 0x7Du, 0xF7u };
        check(pa_ota_classify(k_upgrade_7d, sizeof(k_upgrade_7d)) == PA_OTA_FRAME_SOFTKEY,
              "the 0x7D soft key (the ROM download mode) is told apart from the 0x7F update session");
    }

    /* noise must not be mistaken for a frame */
    const uint8_t noise[] = { 0xF0u, 0x7Du, 0x01u, 0x00u, 0xF7u };
    check(pa_ota_classify(noise, sizeof(noise)) == PA_OTA_FRAME_NONE,
          "another SysEx frame is not mistaken for an OTA frame");
    check(pa_ota_classify(k_query, sizeof(k_query) - 1u) == PA_OTA_FRAME_NONE,
          "a truncated frame is not accepted");
    check(pa_ota_classify(k_upgrade, sizeof(k_upgrade) + 1u) == PA_OTA_FRAME_NONE,
          "an over-long frame is not accepted");
    {
        uint8_t wrong[10];
        memcpy(wrong, k_query, 10);
        wrong[7] = 0x41u;                    /* 41 instead of 40 */
        check(pa_ota_classify(wrong, sizeof(wrong)) == PA_OTA_FRAME_NONE,
              "a frame with a wrong byte is not accepted");
    }

    /* the ID block */
    uint8_t id[PA_OTA_ID_REPLY_LEN + 8];
    uint32_t n = pa_ota_build_id_reply(id, sizeof(id));
    check(n == PA_OTA_ID_REPLY_LEN, "the ID block is %u bytes (got %u)",
          (unsigned)PA_OTA_ID_REPLY_LEN, (unsigned)n);
    check(id[0] == 0xF0u && id[1] == 0x00u && id[2] == 0x32u && id[3] == 0x45u &&
          id[4] == 0x58u, "the ID block starts with the header 00 32 45 58");
    check(id[n - 1] == 0xF7u, "the ID block ends with F7");
    {   /* the message inside the 7-bit packing: 00 59 11 | len24 = 27 | "Pocket AMP_105" zero-padded | ~sum - what the original answers and the Pendel installer parses (34 decoded bytes) */
        uint8_t d[64]; uint32_t k = 0, acc = 0, nb = 0;
        for (uint32_t i = 1; i + 1 < n; i++) { acc |= (uint32_t)(id[i] & 0x7Fu) << nb; nb += 7u; if (nb >= 8u) { d[k++] = (uint8_t)acc; acc >>= 8; nb -= 8u; } }
        check(k == 34u && d[0] == 0x00u && d[1] == 0x59u && d[2] == 0x11u && d[3] == 27u && d[4] == 0u && d[5] == 0u, "the ID block decodes to 34 bytes: 00 59 11, length 27 (got %u bytes)", (unsigned)k);
        check(memcmp(d + 6, "Pocket AMP_105", 15) == 0, "the identity in it is Pocket AMP_105");
        uint32_t sum = 0; for (uint32_t i = 6; i < 33; i++) sum += d[i];
        check(d[33] == (uint8_t)~sum, "its checksum is right");
    }
    /* the version must be the same number the editor's device-info record reports */
    check(strstr(pa_ota_version_string(), "V") == pa_ota_version_string(),
          "the version string starts with V");
    check((unsigned)(pa_ota_version_string()[1] - '0') * 100u +
          (unsigned)(pa_ota_version_string()[2] - '0') * 10u +
          (unsigned)(pa_ota_version_string()[3] - '0') == PA_FW_VERSION,
          "the version string matches PA_FW_VERSION (%u)", (unsigned)PA_FW_VERSION);

    printf("\n%s\n", fails == 0 ? "OTA HANDSHAKE OK" : "OTA HANDSHAKE FAILED");
    return fails == 0 ? 0 : 1;
}
