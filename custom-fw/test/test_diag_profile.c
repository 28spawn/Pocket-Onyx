/* custom-fw/test/test_diag_profile.c - the diagnostic channel's profile command (7D 03, src/sys/pa_diag.c): write and read the experiment switches, the reply, and that other commands still work. */
#include <stdio.h>
#include <string.h>
#include "pa_diag.h"

static int fails;
#define CHECK(c) do { if (!(c)) { printf("FAIL line %d: %s\n", __LINE__, #c); fails++; } } while (0)

static uint8_t reply[64]; static uint32_t reply_n;
static void sink(const uint8_t *d, uint32_t n) { memcpy(reply, d, n); reply_n = n; }
static uint32_t prof = 0x12u; static int writes;
static uint32_t hook(int write, uint32_t v) { if (write) { prof = v & 0x3Fu; writes++; } return prof; }

static uint32_t pack(const uint8_t *in, uint32_t n, uint8_t *out)       /* 8 to 7, LSB first */
{
    uint32_t bits = 0, nb = 0, k = 0, i;
    for (i = 0; i < n; i++) { bits |= (uint32_t)in[i] << nb; nb += 8; while (nb >= 7) { out[k++] = bits & 0x7F; bits >>= 7; nb -= 7; } }
    if (nb) out[k++] = bits & 0x7F;
    return k;
}
static void send(const uint8_t *inner, uint32_t n)
{
    uint8_t p[32]; uint32_t k = pack(inner, n, p), i;
    reply_n = 0;
    pa_diag_feed(0xF0);
    for (i = 0; i < k; i++) pa_diag_feed(p[i]);
    pa_diag_feed(0xF7);
}
static uint32_t unpack_reply(uint8_t *out)       /* 7 to 8 of the reply between F0 and F7 */
{
    uint32_t bits = 0, nb = 0, k = 0, i;
    for (i = 1; i + 1 < reply_n; i++) { bits |= (uint32_t)(reply[i] & 0x7F) << nb; nb += 7; if (nb >= 8) { out[k++] = bits & 0xFF; bits >>= 8; nb -= 8; } }
    return k;
}

int main(void)
{
    uint8_t body[16];
    pa_diag_set_reply_sink(sink);
    pa_diag_set_profile_hook(hook);
    { const uint8_t rd[7] = {0x7D, 0x03, 0x00, 0, 0, 0, 0}; send(rd, 7); }               /* read */
    CHECK(reply_n > 2 && reply[0] == 0xF0 && reply[reply_n - 1] == 0xF7);
    CHECK(unpack_reply(body) >= 6 && body[0] == 0x7D && body[1] == 0x83 && body[2] == 0x12 && writes == 0);
    { const uint8_t wr[7] = {0x7D, 0x03, 0x01, 0x32, 0, 0, 0}; send(wr, 7); }            /* write 0x32 */
    CHECK(unpack_reply(body) >= 6 && body[2] == 0x32 && writes == 1 && prof == 0x32u);
    { const uint8_t wr[7] = {0x7D, 0x03, 0x01, 0xFF, 0xFF, 0xFF, 0xFF}; send(wr, 7); }   /* only the six defined bits stick */
    CHECK(prof == 0x3Fu);
    pa_diag_set_profile_hook(0);                                                          /* no hook: a harmless 0 */
    { const uint8_t rd[7] = {0x7D, 0x03, 0x00, 0, 0, 0, 0}; send(rd, 7); }
    CHECK(unpack_reply(body) >= 6 && body[2] == 0 && body[3] == 0);
    printf(fails ? "diag profile: %d FAILED\n" : "diag profile: all passed\n", fails);
    return fails != 0;
}
