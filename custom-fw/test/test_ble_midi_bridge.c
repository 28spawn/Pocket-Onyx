/* SPDX-License-Identifier: GPL-3.0-only */
/* Copyright (C) 2026 The Onyx Authors (@28spawn) */
/* custom-fw/test/test_ble_midi_bridge.c - the BLE-MIDI bridge queues (src/board/ble_midi_bridge.c) on the host. */
#include <stdio.h>
#include <string.h>
#include "pa_ble_midi_bridge.h"

static int fails;
#define CHECK(c) do { if (!(c)) { printf("FAIL line %d: %s\n", __LINE__, #c); fails++; } } while (0)

static uint8_t got[256]; static uint32_t got_n;
static void consumer(const uint8_t p[4]) { memcpy(got + got_n, p, 4); got_n += 4; }

/* drain the TX queue into a flat byte list of the MIDI bytes it carries (the inverse of the encoder: what a BLE-MIDI reader would reassemble) */
static uint32_t drain_tx(uint8_t *out)
{
    uint32_t pkt, t, n = 0;
    static const uint8_t len_of_cin[16] = {0, 0, 2, 3, 3, 1, 2, 3, 3, 3, 3, 3, 2, 2, 3, 1};
    while (pa_ble_bridge_tx_peek(&pkt, &t)) {
        const uint32_t cin = pkt & 15u;
        for (uint32_t i = 0; i < len_of_cin[cin]; i++) out[n++] = (uint8_t)(pkt >> (8 + 8 * i));
        pa_ble_bridge_tx_pop();
    }
    return n;
}

int main(void)
{
    /* the unit's read-only identity reply (34 bytes) round-trips unchanged */
    static const uint8_t ident[34] = {0xF0,0x00,0x32,0x45,0x58,0x50,0x6F,0x63,0x6B,0x65,0x74,0x20,0x41,0x4D,0x50,0x5F,0x31,0x30,0x35,0x00,0x56,0x31,0x30,0x35,0x00,0x41,0x43,0x37,0x39,0x31,0x4E,0x00,0x7F,0xF7};
    uint8_t out[1024];
    uint32_t q = pa_ble_bridge_tx_bytes(ident, sizeof ident, 1234);
    CHECK(q == 12u);                                   /* 11 groups of three + the closing byte group */
    CHECK(drain_tx(out) == sizeof ident);
    CHECK(memcmp(out, ident, sizeof ident) == 0);

    /* a SysEx whose length is a multiple of three, and a two-byte end */
    static const uint8_t s9[9] = {0xF0, 1, 2, 3, 4, 5, 6, 7, 0xF7};
    CHECK(pa_ble_bridge_tx_bytes(s9, sizeof s9, 0) == 3u);
    CHECK(drain_tx(out) == 9u && memcmp(out, s9, 9) == 0);
    static const uint8_t s5[5] = {0xF0, 1, 2, 3, 0xF7};
    CHECK(pa_ble_bridge_tx_bytes(s5, sizeof s5, 0) == 2u);
    CHECK(drain_tx(out) == 5u && memcmp(out, s5, 5) == 0);

    /* channel messages: note on (3 bytes), program change (2 bytes) */
    static const uint8_t ch[5] = {0x90, 60, 100, 0xC1, 7};
    CHECK(pa_ble_bridge_tx_bytes(ch, sizeof ch, 0) == 2u);
    CHECK(drain_tx(out) == 5u && memcmp(out, ch, 5) == 0);

    /* an unterminated SysEx is dropped whole; a stray byte is skipped */
    static const uint8_t bad[4] = {0xF0, 1, 2, 3};
    CHECK(pa_ble_bridge_tx_bytes(bad, sizeof bad, 0) == 0u);
    CHECK(drain_tx(out) == 0u);

    /* the queue is bounded: a message that does not fit is dropped whole and counted, the next small one still goes through */
    uint32_t i;
    for (i = 0; i < 11; i++) pa_ble_bridge_tx_bytes(ident, sizeof ident, 0);      /* 11 x 12 = 132 > 128: the last is dropped whole */
    CHECK(pa_ble_bridge_tx_dropped() >= 1u);
    CHECK(drain_tx(out) == 10u * sizeof ident);

    /* a long reply goes through whole: the editor's name list is 290 bytes on the wire (97 packets), the state record about 150 (50): they were dropped (limit 24 packets) and no BLE editor ever got them */
    {
        static uint8_t big[290]; big[0] = 0xF0; for (i = 1; i < 289; i++) big[i] = (uint8_t)(i & 0x7F); big[289] = 0xF7;
        CHECK(pa_ble_bridge_tx_bytes(big, sizeof big, 0) == 97u);
        CHECK(drain_tx(out) == sizeof big && memcmp(out, big, sizeof big) == 0);
    }
    CHECK(pa_ble_bridge_tx_bytes(ch, 3, 0) == 1u);
    drain_tx(out);

    /* RX: pushed in an interrupt, delivered in order by the poll */
    pa_ble_bridge_set_rx_consumer(consumer);
    pa_ble_bridge_rx_push(0x0C | 0xC0u << 8 | 5u << 16);                            /* program change 5 */
    pa_ble_bridge_rx_push(0x0B | 0xB0u << 8 | 20u << 16 | 64u << 24);              /* CC 20 = 64 */
    pa_ble_bridge_poll();
    CHECK(got_n == 8u && got[0] == 0x0C && got[1] == 0xC0 && got[2] == 5 && got[4] == 0x0B && got[7] == 64);
    for (i = 0; i < 40; i++) pa_ble_bridge_rx_push(0x09);                           /* more than the 32 it holds */
    CHECK(pa_ble_bridge_rx_dropped() >= 8u);

    printf(fails ? "ble midi bridge: %d FAILED\n" : "ble midi bridge: all passed\n", fails);
    return fails != 0;
}
