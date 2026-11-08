/* SPDX-License-Identifier: GPL-3.0-only */
/* Copyright (C) 2026 The Onyx Authors (@28spawn) */
/* custom-fw/include/pa_ble_remap.h - channel selection algorithm #1 (Core Vol 6 Part B 4.5.8.2), the part the radio hardware may or may not do for a sparse channel map.
 *
 * Windows centrals send maps with 12..20 of the 37 data channels used (the Wi-Fi band is cut out), so most events land on an unused "unmapped" channel and the used channel
 * is found by remapping: used[unmapped mod count]. The hardware had only been measured with nearly full maps. pa_ble_remap_build() gives, for each unmapped index, the channel
 * to tune: the driver writes those into the engine's frequency table and presents a full map, so the engine never has to remap.
 *
 *   pa_ble_remap_build(map, out): out[u] = u when channel u is used, else the used channel list[u mod count]; returns the count of used channels (0 = an unusable map).
 *   pa_ble_unmapped(event, hop): the unmapped index of connection event 'event' (CSA #1: lastUnmapped starts at 0, + hop mod 37 each event, the first event is number 0). */
#ifndef PA_BLE_REMAP_H
#define PA_BLE_REMAP_H

#include <stdint.h>

static inline uint32_t pa_ble_remap_build(const uint8_t chm[5], uint8_t out[37])
{
    uint8_t list[37];
    uint32_t i, used = 0u;
    for (i = 0; i < 37u; i++)
        if ((chm[i >> 3] >> (i & 7u)) & 1u) list[used++] = (uint8_t)i;
    if (!used) return 0u;
    for (i = 0; i < 37u; i++)
        out[i] = ((chm[i >> 3] >> (i & 7u)) & 1u) ? (uint8_t)i : list[i % used];
    return used;
}

static inline uint8_t pa_ble_unmapped(uint32_t event, uint32_t hop)
{
    return (uint8_t)(((event + 1u) * hop) % 37u);
}

#endif
