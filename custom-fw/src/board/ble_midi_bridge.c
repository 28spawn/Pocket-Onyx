/* custom-fw/src/board/ble_midi_bridge.c - see include/pa_ble_midi_bridge.h. */
#include "pa_ble_midi_bridge.h"

#define RXQ_N 32u                   /* event packets (a power of two) */
#define TXQ_N 128u                  /* event packets of three bytes: the editor's name list reply is 97 of them (a power of two); with 64 and a limit of 24 per message every reply over 72 bytes was dropped */

static volatile uint32_t s_rxq[RXQ_N];
static volatile uint32_t s_rx_w, s_rx_r, s_rx_drop;
static volatile uint32_t s_txq[TXQ_N];
static volatile uint16_t s_txt[TXQ_N];
static volatile uint32_t s_tx_w, s_tx_r, s_tx_drop, s_tx_n;
static void (*s_consumer)(const uint8_t pkt4[4]);

void pa_ble_bridge_set_rx_consumer(void (*consumer)(const uint8_t pkt4[4])) { s_consumer = consumer; }

void pa_ble_bridge_rx_push(uint32_t pkt)
{
    if ((uint32_t)(s_rx_w - s_rx_r) >= RXQ_N) { s_rx_drop++; return; }
    s_rxq[s_rx_w & (RXQ_N - 1u)] = pkt;
    s_rx_w++;                                       /* published last: the main loop reads only below it */
}

void pa_ble_bridge_poll(void)
{
    uint32_t guard = 0u;
    while (s_rx_r != s_rx_w && guard++ < RXQ_N) {
        const uint32_t p = s_rxq[s_rx_r & (RXQ_N - 1u)];
        uint8_t b[4];
        b[0] = (uint8_t)p; b[1] = (uint8_t)(p >> 8); b[2] = (uint8_t)(p >> 16); b[3] = (uint8_t)(p >> 24);
        s_rx_r++;
        if (s_consumer) s_consumer(b);
    }
}

/* the event packets one message needs, and writes them when 'out' is not 0; returns the count (0: nothing valid at d[0]) and, in *used, the bytes consumed */
static uint32_t encode_one(const uint8_t *d, uint32_t n, uint32_t *out, uint32_t *used)
{
    const uint8_t s = d[0];
    uint32_t k = 0u;
    *used = 1u;
    if (s == 0xF0u) {                               /* SysEx: groups of three bytes, the last one carries F7 */
        uint32_t j = 0u;
        for (;;) {
            uint8_t b[3] = {0u, 0u, 0u};
            uint32_t m = 0u;
            int end = 0;
            while (m < 3u && j < n) { b[m++] = d[j]; if (d[j++] == 0xF7u) { end = 1; break; } }
            if (!end && m < 3u) { *used = n; return 0u; }          /* no F7: drop it whole */
            if (out) out[k] = (end ? (m == 1u ? 5u : (m == 2u ? 6u : 7u)) : 4u) | (uint32_t)b[0] << 8 | (uint32_t)b[1] << 16 | (uint32_t)b[2] << 24;
            k++;
            if (end) break;
        }
        *used = j;
        return k;
    }
    if (s >= 0x80u && s < 0xF0u) {                  /* a channel message: 2 data bytes, 1 for program change and channel pressure */
        const uint32_t len = ((s >> 4) == 0xCu || (s >> 4) == 0xDu) ? 2u : 3u;
        if (n < len) { *used = n; return 0u; }
        if (out) out[0] = (uint32_t)(s >> 4) | (uint32_t)s << 8 | (uint32_t)d[1] << 16 | (len == 3u ? (uint32_t)d[2] << 24 : 0u);
        *used = len;
        return 1u;
    }
    return 0u;                                      /* a stray data byte or a real-time byte: skipped */
}

uint32_t pa_ble_bridge_tx_bytes(const uint8_t *data, uint32_t len, uint32_t t_ms)
{
    uint32_t i = 0u, total = 0u;
    while (i < len) {
        uint32_t used, k, pk[TXQ_N];
        uint32_t need = encode_one(data + i, len - i, 0, &used);
        if (!need) { i += used; continue; }
        if (need > TXQ_N || (uint32_t)(s_tx_w - s_tx_r) + need > TXQ_N) { s_tx_drop++; i += used; continue; }
        encode_one(data + i, len - i, pk, &used);
        for (k = 0; k < need; k++) {
            s_txq[s_tx_w & (TXQ_N - 1u)] = pk[k];
            s_txt[s_tx_w & (TXQ_N - 1u)] = (uint16_t)t_ms;
            s_tx_w++;
        }
        s_tx_n += need;
        total += need;
        i += used;
    }
    return total;
}

int pa_ble_bridge_tx_peek(uint32_t *pkt, uint32_t *t_ms)
{
    if (s_tx_r == s_tx_w) return 0;
    *pkt = s_txq[s_tx_r & (TXQ_N - 1u)];
    *t_ms = s_txt[s_tx_r & (TXQ_N - 1u)];
    return 1;
}

void pa_ble_bridge_tx_pop(void) { if (s_tx_r != s_tx_w) s_tx_r++; }

uint32_t pa_ble_bridge_rx_dropped(void) { return s_rx_drop; }
uint32_t pa_ble_bridge_tx_dropped(void) { return s_tx_drop; }
uint32_t pa_ble_bridge_tx_queued(void) { return s_tx_n; }
