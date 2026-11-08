/* SPDX-License-Identifier: GPL-3.0-only */
/* Copyright (C) 2026 The Onyx Authors (@28spawn) */
/* custom-fw/src/sys/pa_diag.c - the private diagnostic channel (see include/pa_diag.h)
 *
 * USB MIDI carries 7-bit data bytes, so a frame with binary content (the flash dump)
 * has to be packed 8-to-7 and unpacked again - the same bit stream the editor's frames
 * use, and the same scheme as src/sys/sysex.c's unpacker.
 */
#include "pa_diag.h"

#include <string.h>

static pa_diag_reply_fn   s_reply;
static pa_diag_flash_read_fn s_flash_read;

static uint8_t s_packed[PA_DIAG_MAX_REPLY * 2 + 32];
static uint32_t s_packed_len;
static uint8_t s_inner[PA_DIAG_MAX_REPLY * 2 + 32];
static uint32_t s_inner_len;
static int      s_in_frame;

void pa_diag_set_reply_sink(pa_diag_reply_fn fn) { s_reply = fn; }
void pa_diag_set_flash_reader(pa_diag_flash_read_fn fn) { s_flash_read = fn; }
static pa_diag_profile_fn s_profile;
void pa_diag_set_profile_hook(pa_diag_profile_fn fn) { s_profile = fn; }

/* ---- the 8-to-7 bit stream (LSB first, 7 bits per byte) ------------------- */

static void unpack_7to8(const uint8_t *in, uint32_t in_len, uint8_t *out, uint32_t *out_len) {
    uint32_t bits = 0u, nbits = 0u, n = 0u;
    for (uint32_t i = 0; i < in_len; i++) {
        bits |= (uint32_t)(in[i] & 0x7Fu) << nbits;
        nbits += 7u;
        if (nbits >= 8u && n < PA_DIAG_MAX_REPLY * 2u + 32u) {
            out[n++] = (uint8_t)(bits & 0xFFu);
            bits >>= 8u;
            nbits -= 8u;
        }
    }
    *out_len = n;
}

static uint32_t pack_8to7(const uint8_t *in, uint32_t in_len, uint8_t *out, uint32_t out_cap) {
    uint32_t bits = 0u, nbits = 0u, n = 0u;
    for (uint32_t i = 0; i < in_len; i++) {
        bits |= (uint32_t)in[i] << nbits;
        nbits += 8u;
        while (nbits >= 7u) {
            if (n >= out_cap) return n;
            out[n++] = (uint8_t)(bits & 0x7Fu);
            bits >>= 7u;
            nbits -= 7u;
        }
    }
    if (nbits && n < out_cap) out[n++] = (uint8_t)(bits & 0x7Fu);
    return n;
}

/* ---- the RAM read ---------------------------------------------------------- */

/* SRAM only, and only on the device: on a host build the "address" would be a host pointer. A byte
 * at a time through a volatile pointer, so an unaligned address or length is fine. */
static int ram_read(uint32_t addr, void *dst, uint32_t len) {
#if !defined(PA_HOST_BUILD) && !defined(PA_HOST_SIM)
    if (len == 0u || addr < PA_DIAG_RAM_LO || addr >= PA_DIAG_RAM_HI || len > PA_DIAG_RAM_HI - addr) return -1;
    const volatile uint8_t *src = (const volatile uint8_t *)(uintptr_t)addr;
    uint8_t *d = (uint8_t *)dst;
    for (uint32_t i = 0; i < len; i++) d[i] = src[i];
    return 0;
#else
    (void)addr; (void)dst; (void)len;
    return -1;
#endif
}

/* ---- the reads -------------------------------------------------------------- */

static void send_read(uint8_t rep, pa_diag_flash_read_fn reader, uint32_t addr, uint32_t len) {
    if (!s_reply || !reader) return;
    if (len > PA_DIAG_MAX_REPLY) len = PA_DIAG_MAX_REPLY;

    uint8_t data[PA_DIAG_MAX_REPLY];
    uint8_t inner[PA_DIAG_MAX_REPLY + 16];
    uint32_t n = 0;

    if (reader(addr, data, len) != 0) {
        inner[n++] = PA_DIAG_MFG_ID;
        inner[n++] = rep;
        inner[n++] = 0xFFu;                  /* the read failed */
        inner[n++] = 0xFFu;
    } else {
        inner[n++] = PA_DIAG_MFG_ID;
        inner[n++] = rep;
        inner[n++] = (uint8_t)(len & 0xFFu);
        inner[n++] = (uint8_t)((len >> 8) & 0xFFu);
        inner[n++] = (uint8_t)(addr & 0xFFu);
        inner[n++] = (uint8_t)((addr >> 8) & 0xFFu);
        inner[n++] = (uint8_t)((addr >> 16) & 0xFFu);
        inner[n++] = (uint8_t)((addr >> 24) & 0xFFu);
        memcpy(&inner[n], data, len);
        n += len;
    }

    /* F0 <packed> F7 */
    uint8_t frame[PA_DIAG_MAX_REPLY * 2 + 40];
    uint32_t f = 0;
    frame[f++] = 0xF0u;
    f += pack_8to7(inner, n, &frame[f], sizeof(frame) - f - 1u);
    frame[f++] = 0xF7u;
    s_reply(frame, f);
}

/* ---- the RX -------------------------------------------------------------- */

void pa_diag_feed(uint8_t b) {
    if (b == 0xF0u) {
        s_packed_len = 0;
        s_inner_len = 0;
        s_in_frame = 1;
        return;
    }
    if (!s_in_frame) return;
    if (s_packed_len < sizeof(s_packed)) s_packed[s_packed_len++] = b;
    if (b != 0xF7u) return;

    s_in_frame = 0;
    unpack_7to8(s_packed, s_packed_len, s_inner, &s_inner_len);
    s_packed_len = 0;

    /* 7D 01 (flash) or 7D 02 (RAM): <addr u32 LE> <len u16 LE> = 8 bytes. A sender may pad one byte. */
    if (s_inner_len >= 8u && s_inner[0] == PA_DIAG_MFG_ID &&
        (s_inner[1] == PA_DIAG_CMD_FLASH_READ || s_inner[1] == PA_DIAG_CMD_RAM_READ)) {
        uint32_t addr = (uint32_t)s_inner[2] | ((uint32_t)s_inner[3] << 8) |
                        ((uint32_t)s_inner[4] << 16) | ((uint32_t)s_inner[5] << 24);
        uint32_t len = (uint32_t)s_inner[6] | ((uint32_t)s_inner[7] << 8);
        if (s_inner[1] == PA_DIAG_CMD_FLASH_READ) send_read(PA_DIAG_REP_FLASH_READ, s_flash_read, addr, len);
        else                                      send_read(PA_DIAG_REP_RAM_READ, ram_read, addr, len);
    }
    if (s_inner_len >= 7u && s_inner[0] == PA_DIAG_MFG_ID && s_inner[1] == PA_DIAG_CMD_PROFILE && s_reply) {
        const uint32_t v = (uint32_t)s_inner[3] | ((uint32_t)s_inner[4] << 8) | ((uint32_t)s_inner[5] << 16) | ((uint32_t)s_inner[6] << 24);
        const uint32_t cur = s_profile ? s_profile(s_inner[2] != 0u, v) : 0u;
        uint8_t inner[6], frame[16];
        uint32_t f = 0u;
        inner[0] = PA_DIAG_MFG_ID; inner[1] = PA_DIAG_REP_PROFILE;
        inner[2] = (uint8_t)cur; inner[3] = (uint8_t)(cur >> 8); inner[4] = (uint8_t)(cur >> 16); inner[5] = (uint8_t)(cur >> 24);
        frame[f++] = 0xF0u;
        f += pack_8to7(inner, 6u, &frame[f], sizeof(frame) - f - 1u);
        frame[f++] = 0xF7u;
        s_reply(frame, f);
    }
    s_inner_len = 0;
}
