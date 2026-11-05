/* The SysEx read commands, on the host.
 *
 * The firmware's parser and its reply path, with the flash and the preset
 * store stubbed out, so the protocol can be checked without an emulator or a
 * device. The editor identifies the device with these reads before it
 * will push a tone (SysEx protocol specification), so they are the gate.
 *
 *   clang -O1 -Icustom-fw/include -o test_sysex_read.exe test_sysex_read.c \
 *         custom-fw/src/sys/sysex.c
 *   test_sysex_read.exe
 */
#include <stdio.h>
#include <string.h>
#include <stdint.h>

#include "pa_sysex.h"

/* ---- the stubs sysex.c needs --------------------------------------------- */
uint32_t pa_crc32(const void *data, uint32_t len) { (void)data; (void)len; return 0; }
int      pa_preset_save(int slot, const void *p) { (void)slot; (void)p; return 0; }
int      pa_settings_save(const void *s) { (void)s; return 0; }
int      pa_settings_load(void *s) { (void)s; return -1; }
int      pa_spi_nor_erase_sector(uint32_t a) { (void)a; return 0; }
int      pa_spi_nor_write(uint32_t a, const void *d, uint32_t l) { (void)a; (void)d; (void)l; return 0; }
void     pa_spi_nor_sync(void) {}
int      pa_user_nam_add(const char *name, uint8_t type, uint8_t voicing, uint8_t has_cab,
                          float head_scale, const void *blob, uint32_t len, const void *meta) {
    (void)name; (void)type; (void)voicing; (void)has_cab; (void)head_scale;
    (void)blob; (void)len; (void)meta;
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


/* ---- the capture ---------------------------------------------------------- */
static uint8_t g_reply[8][512];
static uint32_t g_reply_len[8];
static uint32_t g_reply_count = 0;

static void sink(const uint8_t *data, uint32_t len) {
    if (!data || len == 0u || len > 512u) return;
    if (g_reply_count >= 8u) return;
    memcpy(g_reply[g_reply_count], data, len);
    g_reply_len[g_reply_count] = len;
    g_reply_count++;
}

static int fails = 0;
static void check(int ok, const char *msg) {
    printf("  %s %s\n", ok ? "ok  " : "FAIL", msg);
    if (!ok) fails++;
}

/* Unpack a reply frame and check it: F0 00 59 <type> <len> <data> <chk> F7. */
static int check_reply(const uint8_t *raw, uint32_t n, uint8_t *type, uint32_t *length,
                       const uint8_t **data) {
    if (n < 12u || raw[0] != 0xF0u || raw[n - 1u] != 0xF7u) return 0;
    /* the inner frame is packed 8-to-7-bit */
    uint8_t inner[512];
    uint32_t bits = 0, nbits = 0, k = 0;
    for (uint32_t i = 1; i + 1u < n; i++) {
        bits |= (uint32_t)(raw[i] & 0x7Fu) << nbits;
        nbits += 7u;
        while (nbits >= 8u) {
            inner[k++] = (uint8_t)(bits & 0xFFu);
            bits >>= 8; nbits -= 8u;
            if (k >= sizeof(inner)) break;
        }
    }
    if (k < 7u || inner[0] != 0x00u || inner[1] != 0x59u) return 0;
    uint32_t l0 = inner[3], l1 = inner[4], l2 = inner[5];
    uint32_t len = l0 | (l1 << 8) | (l2 << 16);   /* a plain u24 little-endian */
    if (k < 6u + len + 1u) return 0;
    uint32_t total = (uint32_t)(inner[2] + l0 + l1 + l2);
    for (uint32_t i = 0; i < len; i++) total += inner[6u + i];
    if (inner[6u + len] != (uint8_t)((~total) & 0xFFu)) return 0;
    *type = inner[2];
    *length = len;
    *data = inner + 6;
    return 1;
}

static void ask(uint8_t cmd, const uint8_t *payload, uint32_t len) {
    uint8_t frame[512];
    int n = pa_sysex_build_frame(cmd, payload, len, frame, sizeof(frame));
    if (n <= 0) { printf("  FAIL the frame for cmd %u did not build\n", cmd); fails++; return; }
    for (int i = 0; i < n; i++) pa_sysex_input_byte(frame[i]);
}

int main(void) {
    pa_sysex_init();
    pa_sysex_set_reply_sink(sink);

    printf("=== the device info (cmd 17) - the editor's handshake ===\n");
    ask(17, NULL, 0);
    check(g_reply_count == 1, "the firmware answered");
    if (g_reply_count == 1) {
        uint8_t type; uint32_t len; const uint8_t *data;
        int ok = check_reply(g_reply[0], g_reply_len[0], &type, &len, &data);
        printf("    the raw reply (%u bytes): ", g_reply_len[0]);
        for (uint32_t z = 0; z < g_reply_len[0] && z < 24u; z++) printf("%02x ", g_reply[0][z]);
        printf("\n    the first inner bytes: ");
        { uint32_t bits=0, nb=0, kk=0;
          for (uint32_t z = 1; z + 1u < g_reply_len[0] && kk < 12u; z++) {
            bits |= (uint32_t)(g_reply[0][z] & 0x7Fu) << nb; nb += 7u;
            while (nb >= 8u && kk < 12u) { printf("%02x ", (uint8_t)(bits & 0xFFu)); bits >>= 8; nb -= 8u; kk++; } } }
        printf("\n");
        check(ok, "the reply is a well-formed frame with a right checksum");
        if (ok) {
            char text[256];
            uint32_t n = len < sizeof(text) - 1u ? len : sizeof(text) - 1u;
            memcpy(text, data, n);
            text[n] = 0;
            printf("    the record: %s\n", text);
            check(strstr(text, "name=") != NULL,        "the record has name=");
            check(strstr(text, "model=") != NULL,       "the record has model=");
            check(strstr(text, "id=") != NULL,          "the record has id=");
            check(strstr(text, "type=") != NULL,        "the record has type=");
            check(strstr(text, "version=") != NULL,    "the record has version=");
            check(strstr(text, "DONNER Pocket AMP") != NULL,
                  "the name is one the editor matches (\"DONNER Pocket AMP\")");
            check(strstr(text, "version=105") != NULL, "the version is 105 (PA_FW_VERSION: shown as 1.05, below the stock's 1.05)");
        }
    }

    printf("\n=== the descriptor (cmd 18) ===\n");
    g_reply_count = 0;
    ask(18, NULL, 0);
    check(g_reply_count == 1, "the firmware answered");
    if (g_reply_count == 1) {
        uint8_t type; uint32_t len; const uint8_t *data;
        int ok = check_reply(g_reply[0], g_reply_len[0], &type, &len, &data);
        check(ok, "the reply is well formed");
        check(len == 13u, "the descriptor is 13 bytes");
    }

    printf("\n=== the state record (cmd 23) and the state block (cmd 24) ===\n");
    g_reply_count = 0;
    ask(23, NULL, 0);
    check(g_reply_count == 1, "cmd 23 answered");
    if (g_reply_count == 1) {
        uint8_t type; uint32_t len; const uint8_t *data;
        int ok = check_reply(g_reply[0], g_reply_len[0], &type, &len, &data);
        check(ok && len == 8u, "the record is 8 bytes");
    }
    g_reply_count = 0;
    ask(24, NULL, 0);
    check(g_reply_count == 1, "cmd 24 answered");
    if (g_reply_count == 1) {
        uint8_t type; uint32_t len; const uint8_t *data;
        int ok = check_reply(g_reply[0], g_reply_len[0], &type, &len, &data);
        check(ok && len == 16u, "the block is 16 bytes");
    }

    printf("\n=== the drain (cmd 21) and the read (cmd 35) ===\n");
    g_reply_count = 0;
    ask(21, NULL, 0);
    check(g_reply_count == 1, "cmd 21 answered");
    g_reply_count = 0;
    ask(35, NULL, 0);
    check(g_reply_count == 1, "cmd 35 answered");

    printf("\n=== a command we do not implement gets no reply ===\n");
    g_reply_count = 0;
    ask(19, NULL, 0);
    check(g_reply_count == 0, "nothing was sent");

    printf("\n%s\n", fails == 0 ? "THE READ COMMANDS WORK" : "SOME CHECKS FAILED");
    return fails == 0 ? 0 : 1;
}
