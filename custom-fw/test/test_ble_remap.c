/* SPDX-License-Identifier: GPL-3.0-only */
/* Copyright (C) 2026 The Onyx Authors (@28spawn) */
/* custom-fw/test/test_ble_remap.c - the channel remap helper (include/pa_ble_remap.h) on the host, against a straight copy of the Core spec text. */
#include <stdio.h>
#include <string.h>
#include "pa_ble_remap.h"

static int fails;
#define CHECK(c) do { if (!(c)) { printf("FAIL line %d: %s\n", __LINE__, #c); fails++; } } while (0)

int main(void)
{
    /* a Windows map seen on the bench: 363f14001c (little-endian bytes of the 37-bit map) */
    static const uint8_t m[5] = {0x36, 0x3f, 0x14, 0x00, 0x1c};
    uint8_t out[37], used_ch[37];
    uint32_t n = 0, i, u;
    for (i = 0; i < 37u; i++) if ((m[i >> 3] >> (i & 7u)) & 1u) used_ch[n++] = (uint8_t)i;
    CHECK(pa_ble_remap_build(m, out) == n);
    CHECK(n == 15u);                                         /* 4 + 6 + 2 + 0 + 3 bits */
    for (u = 0; u < 37u; u++) {
        const int used = (m[u >> 3] >> (u & 7u)) & 1;
        CHECK(out[u] == (used ? u : used_ch[u % n]));        /* spec 4.5.8.2.1: unused -> remapping index = unmapped mod used count */
        CHECK((m[out[u] >> 3] >> (out[u] & 7u)) & 1);        /* the result is always a used channel */
    }
    /* full map: identity */
    static const uint8_t full[5] = {0xff, 0xff, 0xff, 0xff, 0x1f};
    CHECK(pa_ble_remap_build(full, out) == 37u);
    for (u = 0; u < 37u; u++) CHECK(out[u] == u);
    /* nothing used */
    static const uint8_t none[5] = {0, 0, 0, 0, 0};
    CHECK(pa_ble_remap_build(none, out) == 0u);
    /* CSA #1: the unmapped sequence from event 0 is hop, 2 hop, ... mod 37 */
    CHECK(pa_ble_unmapped(0, 11) == 11u && pa_ble_unmapped(1, 11) == 22u && pa_ble_unmapped(2, 11) == 33u && pa_ble_unmapped(3, 11) == 7u);
    printf(fails ? "ble remap: %d FAILED\n" : "ble remap: all passed\n", fails);
    return fails != 0;
}
