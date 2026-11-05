/* custom-fw/test/test_usb_chunk.c - the bulk IN packetising rule (include/pa_usb_chunk.h): over every reply size from 1 to 600 event packets, a burst never ends on a full 64-byte
 * packet once the padding rule is applied, and the old behaviour fails on exactly the sizes the bench showed (RAM-read replies of 32 and 240 data bytes). */
#include <math.h>
#include <stdio.h>
#include "pa_usb_chunk.h"

static int fails;
#define CHECK(c) do { if (!(c)) { printf("FAIL line %d: %s\n", __LINE__, #c); fails++; } } while (0)

/* drain a burst of 'pkts' event packets through the same loop the driver uses; returns the length of the LAST bulk packet; 'pad' applies the rule */
static uint32_t drain(uint32_t pkts, int pad, uint32_t *n_packets)
{
    uint32_t left = pkts, last = 0u;
    *n_packets = 0u;
    while (left) {
        const uint32_t n = pa_usb_next_len(left);
        left -= n / 4u;
        last = n;
        (*n_packets)++;
        if (pad && pa_usb_needs_pad(n, left)) left++;       /* the padding event packet joins the queue */
    }
    return last;
}

/* the RAM-read reply size model of pa_diag.c: 8 header bytes + L data, packed 8 to 7, plus F0 and F7, 3 SysEx bytes per event packet */
static uint32_t reply_pkts(uint32_t L)
{
    const uint32_t inner = 8u + L, packed = (inner * 8u + 6u) / 7u, frame = packed + 2u;
    return (frame + 2u) / 3u;
}

int main(void)
{
    uint32_t p, np, old_bad = 0u;
    for (p = 1u; p <= 600u; p++) {
        const uint32_t last_old = drain(p, 0, &np), last_new = drain(p, 1, &np);
        CHECK(last_new < PA_USB_BULK_MAXP);                 /* with the rule every burst ends on a short packet */
        CHECK(last_old == PA_USB_BULK_MAXP ? (p % 16u == 0u) : (p % 16u != 0u));
        if (last_old == PA_USB_BULK_MAXP) old_bad++;
    }
    CHECK(old_bad == 600u / 16u);                           /* the old behaviour fails on every multiple of 16 event packets */
    /* the bench: replies of 8, 16, 24 data bytes were fine, 32 was not; 240 would not have been */
    CHECK(reply_pkts(8) == 7u && reply_pkts(16) == 10u && reply_pkts(24) == 13u && reply_pkts(32) == 16u && reply_pkts(240) == 96u);
    CHECK(drain(reply_pkts(8), 0, &np) != PA_USB_BULK_MAXP && drain(reply_pkts(16), 0, &np) != PA_USB_BULK_MAXP && drain(reply_pkts(24), 0, &np) != PA_USB_BULK_MAXP);
    CHECK(drain(reply_pkts(32), 0, &np) == PA_USB_BULK_MAXP && drain(reply_pkts(240), 0, &np) == PA_USB_BULK_MAXP);
    CHECK(drain(reply_pkts(32), 1, &np) == 4u && np == 2u);  /* with the rule: the 64-byte packet, then one 4-byte padding packet */
    CHECK(drain(12u, 1, &np) == 48u && np == 1u);           /* the 34-byte identity reply (12 event packets): unchanged, no padding */
    printf(fails ? "usb chunk: %d FAILED\n" : "usb chunk: all passed\n", fails);
    return fails != 0;
}
