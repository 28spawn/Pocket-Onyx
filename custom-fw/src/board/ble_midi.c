/* SPDX-License-Identifier: GPL-3.0-only */
/* Copyright (C) 2026 The Onyx Authors (@28spawn) */
/*
 * custom-fw/src/board/ble_midi.c - the BLE-MIDI 1.0 framing.
 *
 * A BLE-MIDI peer does not send bare MIDI bytes. Every packet is
 *
 *     byte 0    the header: 0x80 | (timestamp >> 7); the low 6 bits carry the
 *               timestamp's high bits
 *     byte 1    the timestamp byte: 0x80 | (timestamp & 0x7F), the high bit
 *               set. The byte AFTER it is unconditionally the first byte of a
 *               MIDI message - it may be a status byte, and must not be
 *               mistaken for another timestamp
 *     byte 2..  the message
 *
 * and inside a SysEx the rules change: the data bytes are 7-bit (MIDI SysEx
 * data always is), so a byte with the high bit SET is framing and not
 * payload - it introduces either the closing 0xF7 or a System Real-Time byte.
 * A SysEx CONTINUATION packet carries the header and raw data with NO
 * timestamp byte at all.
 *
 * The device's control protocol is a SysEx frame (the M-VAVE one), so this
 * is the layer that turns a real BLE-MIDI peer into the same bytes the USB
 * MIDI path delivers - the two transports then converge on one assembler.
 *
 * The framing conforms to standard MIDI over Bluetooth Low Energy 1.0 (MMA/AMEI), and the test vectors
 * in custom-fw/test/test_ble_midi.c are the upstream's own.
 *
 * It is kept in its own file (no USB, no firmware dependencies) so it can be
 * tested on the host.
 */
#include <string.h>
#include <stdint.h>

#include "../../include/pa_usb_ble.h"

#define BLE_TS_SYSEX 1u   /* a timestamp was seen inside a SysEx: the next byte
                             is the closing F7 or a System Real-Time byte */
#define BLE_TS_MSG   2u   /* a timestamp was seen: the next byte is the
                             message's first byte */

/* Push one byte of MIDI. */
static void ble_push(uint8_t b, uint8_t *out, uint32_t cap, uint32_t *o,
                     uint8_t *in_sysex) {
    if (*o < cap) out[(*o)++] = b;
    if (b == 0xF0u) *in_sysex = 1u;
    else if (b == 0xF7u) *in_sysex = 0u;
}

uint32_t pa_ble_midi_unframe(const uint8_t *pkt, uint32_t len, uint8_t *out,
                             uint32_t cap, uint8_t *state, uint8_t *in_sysex) {
    if (!pkt || !out || !state || !in_sysex || len < 2u) return 0u;
    uint32_t o = 0u;
    uint32_t i;

    /* A peer that was interrupted and started a NEW SysEx sends
     * [header][timestamp][F0 ...] - and a SysEx's payload can never contain
     * an F0 (the data is 7-bit), so that is unambiguous: the old frame was
     * lost and this one starts fresh. Without this a dropped connection
     * would leave the framer inside a SysEx that never ends. */
    int fresh_sysex = (len >= 3u && (pkt[1] & 0x80u) && pkt[2] == 0xF0u);

    if (*state == BLE_TS_SYSEX) {
        /* The byte after a timestamp that was seen inside a SysEx: the closing
         * F7, or a System Real-Time byte (which is dropped). */
        *state = 0u;
        if (fresh_sysex) {
            *in_sysex = 0u;
            i = 2u;
        } else {
            if (pkt[1] == 0xF7u) {
                ble_push(0xF7u, out, cap, &o, in_sysex);
            } else if (pkt[1] < 0xF8u) {
                /* Neither the F7 nor a real-time byte: the timestamp was
                 * spurious, so take the byte as data rather than lose it. */
                ble_push(pkt[1], out, cap, &o, in_sysex);
            }
            i = 2u;
        }
    } else if (*state == BLE_TS_MSG) {
        /* The byte after a timestamp: unconditionally the message's first
         * byte, even if its high bit is clear - that is running status. */
        *state = 0u;
        ble_push(pkt[1], out, cap, &o, in_sysex);
        i = 2u;
    } else if (*in_sysex) {
        if (fresh_sysex) {
            /* the previous frame was lost: this packet starts a new one */
            *in_sysex = 0u;
            i = 2u;
        } else {
            i = 1u;     /* a SysEx continuation: the header, then raw data */
        }
    } else {
        i = 2u;         /* the header and the timestamp, then the message */
        if (len == 2u) {
            /* The packet carried the header and the timestamp and nothing
             * else: the message's first byte is in the next packet. */
            *state = BLE_TS_MSG;
            return 0u;
        }
    }

    for (; i < len; i++) {
        uint8_t b = pkt[i];
        if (!*in_sysex) {
            /* Outside a SysEx every byte is the message. */
            ble_push(b, out, cap, &o, in_sysex);
            continue;
        }
        if (b == 0xF7u) {
            /* The closing F7. The spec puts a timestamp byte before it, and
             * the upstream's own sender does too - but its packet splitting
             * can also leave the F7 bare at the end of a packet. Inside a
             * SysEx a 0xF7 cannot be data (SysEx data is 7-bit), so it is
             * unambiguous: it ends the frame. */
            ble_push(0xF7u, out, cap, &o, in_sysex);
            continue;
        }
        if (b & 0x80u) {
            /* A timestamp byte: framing, not payload. The byte after it is
             * the closing F7 or a System Real-Time byte. */
            if (i + 1u >= len) { *state = BLE_TS_SYSEX; break; }
            b = pkt[++i];
            if (b == 0xF7u) ble_push(0xF7u, out, cap, &o, in_sysex);
            /* A System Real-Time byte is dropped rather than interleaved: the
             * firmware's SysEx parser is a byte stream, so a stray 0xFE would
             * be taken as payload, and the firmware has no use for active
             * sensing. */
            continue;
        }
        ble_push(b, out, cap, &o, in_sysex);
    }
    return o;
}

/* The TX side: the device's replies leave over the same link, so they have
 * to be framed according to standard BLE-MIDI 1.0 framing:
 *
 *   every packet  : starts with the header byte 0x80 (timestamp 0)
 *   first packet  : 0x80, 0x80, 0xF0, <payload...>
 *   continuation  : 0x80, <raw payload...>
 *   final packet  : ..., 0x80 (a timestamp), 0xF7 - the F7 is ALWAYS preceded
 *                   by a timestamp byte
 *
 * It streams: a packet is handed to `emit` as soon as it is full, so a long
 * reply never needs a buffer bigger than one packet. `mtu` is the
 * negotiated ATT MTU (the BLE minimum is 23), so a write payload is mtu - 3
 * bytes; it is capped at 64, which is far more than a MIDI packet needs.
 */
#define BLE_TX_MAX_PKT 68u

void pa_ble_midi_tx_stream(pa_ble_emit_t emit, const uint8_t *msg, uint32_t len,
                           uint32_t mtu) {
    if (!emit || !msg || len == 0u) return;
    if (mtu < 23u) mtu = 23u;                 /* the BLE minimum ATT MTU */
    uint32_t chunk_max = mtu - 3u;             /* the write payload */
    if (chunk_max < 4u) chunk_max = 4u;
    if (chunk_max > BLE_TX_MAX_PKT - 4u) chunk_max = BLE_TX_MAX_PKT - 4u;

    uint8_t pkt[BLE_TX_MAX_PKT];

    if (msg[0] != 0xF0u || msg[len - 1u] != 0xF7u) {
        /* A plain MIDI message goes out in ONE packet: the timestamp byte
         * introduces it, and the continuation packets - which carry raw data
         * and no timestamp - are a SysEx thing. (The upstream's sender sends
         * one command per packet for the same reason.) */
        uint32_t take = len;
        if (take > chunk_max - 2u) take = chunk_max - 2u;
        uint32_t n = 0u;
        pkt[n++] = 0x80u;                      /* the header */
        pkt[n++] = 0x80u;                      /* the timestamp byte */
        memcpy(&pkt[n], msg, take);
        n += take;
        emit(pkt, n);
        return;
    }

    /* The closing F7 is always preceded by a timestamp byte, so it is held
     * back and emitted with one. */
    int hold_f7 = 1;
    uint32_t total = len - 1u;

    uint32_t pos = 0u;
    int first = 1;
    int f7_sent = 0;
    while (pos < total) {
        uint32_t pkt_len = 0u;
        pkt[pkt_len++] = 0x80u;               /* the header: timestamp 0 */
        if (first) {
            pkt[pkt_len++] = 0x80u;           /* the timestamp byte */
            first = 0;
        }
        uint32_t room = chunk_max - pkt_len;
        uint32_t take = total - pos;
        if (take > room) take = room;
        /* the closing F7 needs two more bytes in this packet */
        if ((pos + take) >= total && (take + 2u) > room) {
            take = (room >= 3u) ? (room - 2u) : 0u;
        }
        if (take) { memcpy(&pkt[pkt_len], &msg[pos], take); pkt_len += take; pos += take; }
        if (pos >= total) {
            pkt[pkt_len++] = 0x80u;           /* the timestamp before the F7 */
            pkt[pkt_len++] = 0xF7u;
            f7_sent = 1;
        }
        emit(pkt, pkt_len);
        if (take == 0u && pos < total) break;  /* cannot make progress */
    }
    if (hold_f7 && !f7_sent) {
        /* the F7 did not fit in the last data packet: it goes out on its own,
         * still behind a timestamp byte */
        pkt[0] = 0x80u;
        pkt[1] = 0x80u;
        pkt[2] = 0xF7u;
        emit(pkt, 3u);
    }
}
