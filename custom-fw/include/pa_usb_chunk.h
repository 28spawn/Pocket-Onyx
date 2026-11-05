/* custom-fw/include/pa_usb_chunk.h - how the USB-MIDI transmit queue is cut into bulk IN packets, and the one rule that keeps the host from waiting forever.
 *
 * A bulk IN transfer ends on the host side only when a SHORT packet (under 64 bytes, a zero-length one included) arrives or its buffer is full. The transmit queue is drained 64 bytes
 * (16 USB-MIDI event packets) at a time, so a reply whose size is an exact multiple of 64 bytes ended with a FULL packet and nothing after it: the host kept the transfer open, never
 * delivered the reply, and the next message arrived glued behind it. Measured on the unit (pack 149A): RAM-read replies of 8, 16, 24 data bytes came back, 32 (48 SysEx bytes = 16
 * event packets = exactly 64 USB bytes) never did, and neither would 240 (6 x 64). The identity reply (36 bytes = 48 USB bytes) always worked.
 *
 * Rule: after a full 64-byte packet that leaves the queue empty, queue one padding event packet (all zero: cable 0, code index 0, which the USB-MIDI class defines as "reserved,
 * ignored") so the next packet is short and closes the transfer. */
#ifndef PA_USB_CHUNK_H
#define PA_USB_CHUNK_H

#include <stdint.h>

#define PA_USB_BULK_MAXP 64u

/* bytes of the next bulk packet when 'queued_pkts' event packets (4 bytes each) wait */
static inline uint32_t pa_usb_next_len(uint32_t queued_pkts)
{
    const uint32_t b = queued_pkts * 4u;
    return b > PA_USB_BULK_MAXP ? PA_USB_BULK_MAXP : b;
}

/* 1: the packet just loaded was full-size and nothing is left behind it, so a padding event packet must follow */
static inline int pa_usb_needs_pad(uint32_t loaded_bytes, uint32_t left_pkts)
{
    return loaded_bytes == PA_USB_BULK_MAXP && left_pkts == 0u;
}

#endif
