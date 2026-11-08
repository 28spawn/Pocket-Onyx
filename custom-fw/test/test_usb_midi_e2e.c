/* SPDX-License-Identifier: GPL-3.0-only */
/* Copyright (C) 2026 The Onyx Authors (@28spawn) */
/* test/test_usb_midi_e2e.c - the USB-MIDI receive-to-response pipeline, end to end, on the PRODUCTION code.
 *
 *   pa_usb_midi_rx_packet() -> midi_rx_ring -> pa_usb_ble_poll() -> pa_diag_feed() / pa_sysex_input_byte() (+ the OTA scanner)
 *   -> command dispatch -> reply construction -> pa_usb_midi_tx_bytes() -> pa_usb_dev_tx()
 *
 * Production code under test: src/board/usb_ble.c (included below, so the host build's discarding stub of pa_usb_dev_tx can be replaced),
 * src/sys/pa_diag.c, src/sys/sysex.c, src/sys/pa_ota.c, src/board/ble_midi.c.
 * The ONLY things stubbed are the hardware boundary (pa_usb_dev_tx: it captures the bulk-IN event packets the driver would queue) and the flash
 * reader the diagnostic channel reads through, plus the storage hooks sysex.c calls (comms_sysex_stubs.c).
 *
 * What this validates: the firmware LOGIC for the packets that reach it and the packets it hands to the USB driver - decoding, the ring, the parsers, the
 * replies, their exact bytes, event packets and CIN values. What it CANNOT validate: the USB controller, the SIE registers, the FIFO, DMA or the host stack.
 * A pass here is a host-verified result, never a hardware-validated one.
 *
 * Expected values are written independently of the firmware: the request/reply frames and the USB-MIDI event packets come from the 8-to-7 packing rule and
 * USB-MIDI class spec 4.3.2 (system exclusive), re-implemented below; the golden bytes come from the 2026-11-03 investigation (the seven identical requests).
 *
 * Output: one machine-readable line per test,  COMMS-TEST|usb_midi_e2e|<name>|PASS|FAIL|KNOWN|<ms>|<detail>,  then a COMMS-SUITE summary line.
 * KNOWN = a defect that is documented and still present; it does not fail the suite, and flips to FAIL if its current behaviour changes (so a fix is noticed).
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <time.h>

/* The hardware boundary: keep the host build's discarding stub of pa_usb_dev_tx out of the way, and provide the real symbol below. */
#define pa_usb_dev_tx pa_usb_dev_tx_hostnull
#include "pa_usb_dev.h"
#undef pa_usb_dev_tx
int pa_usb_dev_tx(const uint8_t *pkts, uint32_t len);
#include "../src/board/usb_ble.c"

/* ---- the hardware boundary: capture of the bulk IN event packets ---------------------------------------------------------------------------------- */
#define MAX_CALLS 8192
static struct { uint32_t len; uint8_t b[64]; } g_calls[MAX_CALLS];
static uint32_t g_ncalls;
static int g_accept_events = -1;       /* -1: accept everything; k >= 0: accept only k more event packets, then return -1 (the driver's queue-full answer) */

/* Same contract as src/board/usb_dev.c pa_usb_dev_tx (packet by packet, -1 when the queue is full, the packets that fitted stay queued). That function is
 * exercised on its own by the driver state-machine tests; here it is only the capture point. */
int pa_usb_dev_tx(const uint8_t *pkts, uint32_t len)
{
    uint32_t ev = len / 4u, take = ev;
    if (g_accept_events >= 0 && (uint32_t)g_accept_events < ev) take = (uint32_t)g_accept_events;
    if (g_ncalls < MAX_CALLS && take) {
        g_calls[g_ncalls].len = take * 4u;
        memcpy(g_calls[g_ncalls].b, pkts, take * 4u);
        g_ncalls++;
    }
    if (g_accept_events >= 0) g_accept_events -= (int)take;
    return take < ev ? -1 : 0;
}

/* ---- the flash reader the diagnostic channel reads through (hardware boundary) --------------------------------------------------------------------- */
static const uint8_t k_build_id[8] = { 'B', 'L', 'D', 'I', 0x12, 0x0A, 0xA1, 0x9B };      /* "BLDI" + build id 2611022354, as the unit stores it */
static uint8_t fake_byte(uint32_t addr, uint32_t i) { return (uint8_t)((addr + i) * 7u + 3u); }
static int fake_reader(uint32_t addr, void *dst, uint32_t len)
{
    uint8_t *d = (uint8_t *)dst;
    if (addr == 0xFFFFFFF0u) return -1;                                  /* a read the hardware refuses */
    for (uint32_t i = 0; i < len; i++) d[i] = (addr == 0x01C7D5B0u && i < 8u) ? k_build_id[i] : fake_byte(addr, i);
    return 0;
}

/* ---- the reference implementation of the wire formats (independent of the firmware) ----------------------------------------------------------------- */
static uint32_t pack_8to7(const uint8_t *in, uint32_t n, uint8_t *out)
{
    uint32_t bits = 0, nb = 0, k = 0;
    for (uint32_t i = 0; i < n; i++) { bits |= (uint32_t)in[i] << nb; nb += 8; while (nb >= 7) { out[k++] = (uint8_t)(bits & 0x7F); bits >>= 7; nb -= 7; } }
    if (nb) out[k++] = (uint8_t)(bits & 0x7F);
    return k;
}
static uint32_t unpack_7to8(const uint8_t *in, uint32_t n, uint8_t *out)
{
    uint32_t bits = 0, nb = 0, k = 0;
    for (uint32_t i = 0; i < n; i++) { bits |= (uint32_t)(in[i] & 0x7F) << nb; nb += 7; if (nb >= 8) { out[k++] = (uint8_t)bits; bits >>= 8; nb -= 8; } }
    return k;
}
static uint32_t frame_of(const uint8_t *inner, uint32_t n, uint8_t *out)      /* F0 <packed> F7 */
{
    out[0] = 0xF0; uint32_t k = pack_8to7(inner, n, out + 1); out[1 + k] = 0xF7; return k + 2u;
}
/* USB-MIDI class spec 4.3.2, cable 0: a SysEx byte stream as event packets (3 data bytes -> CIN 4, the final 1/2/3 bytes with F7 -> CIN 5/6/7) */
static uint32_t events_of(const uint8_t *f, uint32_t n, uint8_t *ev)
{
    uint32_t i = 0, k = 0;
    while (i < n) {
        uint32_t r = n - i;
        if (r > 3u) { ev[k] = 4; memcpy(ev + k + 1, f + i, 3); i += 3; }
        else { ev[k] = (uint8_t)(4u + r); memset(ev + k + 1, 0, 3); memcpy(ev + k + 1, f + i, r); i += r; }
        k += 4u;
    }
    return k;
}
/* Reassemble the event packets in g_calls[from..] into SysEx messages, checking the class rules. Returns the number of messages, -1 on a malformed stream. */
#define MAX_MSG 64
static uint8_t g_msg[MAX_MSG][700];
static uint32_t g_msg_len[MAX_MSG];
static int g_msg_events[MAX_MSG];
static char g_why[160];
static int collect(uint32_t from)
{
    int nm = 0, in = 0; uint32_t len = 0; int evs = 0;
    for (uint32_t c = from; c < g_ncalls; c++) {
        if (g_calls[c].len % 4u || g_calls[c].len > 64u) { snprintf(g_why, sizeof g_why, "call %u has %u bytes", (unsigned)c, (unsigned)g_calls[c].len); return -1; }
        for (uint32_t o = 0; o < g_calls[c].len; o += 4u) {
            const uint8_t *e = g_calls[c].b + o; uint32_t cin = e[0] & 0x0Fu, cable = e[0] >> 4, nb = 0; int end = 0;
            if (cable) { snprintf(g_why, sizeof g_why, "cable %u", (unsigned)cable); return -1; }
            if (cin == 4) nb = 3; else if (cin == 5) { nb = 1; end = 1; } else if (cin == 6) { nb = 2; end = 1; } else if (cin == 7) { nb = 3; end = 1; }
            else { snprintf(g_why, sizeof g_why, "unexpected CIN %X", (unsigned)cin); return -1; }
            if (!in) { if (e[1] != 0xF0) { snprintf(g_why, sizeof g_why, "message does not start with F0 (%02X)", e[1]); return -1; } in = 1; len = 0; evs = 0; }
            if (len + nb > sizeof g_msg[0] || nm >= MAX_MSG) { snprintf(g_why, sizeof g_why, "message too long"); return -1; }
            memcpy(g_msg[nm] + len, e + 1, nb); len += nb; evs++;
            for (uint32_t z = nb; z < 3u; z++) if (e[1 + z]) { snprintf(g_why, sizeof g_why, "non-zero padding in an end event"); return -1; }
            if (end) {
                if (g_msg[nm][len - 1] != 0xF7) { snprintf(g_why, sizeof g_why, "end event without F7"); return -1; }
                for (uint32_t z = 1; z + 1u < len; z++) if (g_msg[nm][z] & 0x80u) { snprintf(g_why, sizeof g_why, "data byte with the top bit set"); return -1; }
                g_msg_len[nm] = len; g_msg_events[nm] = evs; nm++; in = 0;
            }
        }
    }
    if (in) { snprintf(g_why, sizeof g_why, "unterminated message (%u bytes, no F7)", (unsigned)len); return -1; }
    return nm;
}

/* ---- the host side: deliver a SysEx byte stream as the host would, through the real receive entry point ---------------------------------------------- */
static pa_usb_ble_ctx_t g_ctx;
static void deliver_events(const uint8_t *ev, uint32_t n, uint32_t events_per_packet)
{
    for (uint32_t o = 0; o < n; ) {
        uint32_t take = n - o; if (take > events_per_packet * 4u) take = events_per_packet * 4u;
        pa_usb_midi_rx_packet(&g_ctx, ev + o, take);                 /* what ep1_rx() calls with one bulk OUT packet */
        o += take;
    }
}
static void deliver_frame(const uint8_t *frame, uint32_t n, uint32_t events_per_packet)
{
    uint8_t ev[1400]; uint32_t k = events_of(frame, n, ev); deliver_events(ev, k, events_per_packet);
}
static uint32_t diag_request(uint8_t cmd, uint32_t addr, uint32_t len, uint8_t *frame)
{
    uint8_t in[8] = { 0x7D, cmd, (uint8_t)addr, (uint8_t)(addr >> 8), (uint8_t)(addr >> 16), (uint8_t)(addr >> 24), (uint8_t)len, (uint8_t)(len >> 8) };
    return frame_of(in, 8, frame);
}
/* what the diagnostic channel must answer for a read */
static uint32_t diag_expected(uint8_t rep, uint32_t addr, uint32_t len, int refused, uint8_t *frame)
{
    uint8_t in[16 + 300]; uint32_t n = 0;
    if (len > 256u) len = 256u;
    in[n++] = 0x7D; in[n++] = rep;
    if (refused) { in[n++] = 0xFF; in[n++] = 0xFF; }
    else {
        in[n++] = (uint8_t)len; in[n++] = (uint8_t)(len >> 8);
        in[n++] = (uint8_t)addr; in[n++] = (uint8_t)(addr >> 8); in[n++] = (uint8_t)(addr >> 16); in[n++] = (uint8_t)(addr >> 24);
        for (uint32_t i = 0; i < len; i++) in[n++] = (addr == 0x01C7D5B0u && i < 8u) ? k_build_id[i] : fake_byte(addr, i);
    }
    return frame_of(in, n, frame);
}
static uint32_t editor_frame(uint8_t cmd, const uint8_t *pl, uint32_t len, uint8_t *frame)   /* 00 59 cmd len24le payload chk, packed */
{
    uint8_t in[300]; uint32_t n = 0, sum = cmd + (len & 0xFF) + ((len >> 8) & 0xFF) + ((len >> 16) & 0xFF);
    in[n++] = 0x00; in[n++] = 0x59; in[n++] = cmd; in[n++] = (uint8_t)len; in[n++] = (uint8_t)(len >> 8); in[n++] = (uint8_t)(len >> 16);
    for (uint32_t i = 0; i < len; i++) { in[n++] = pl[i]; sum += pl[i]; }
    in[n++] = (uint8_t)(~sum);
    return frame_of(in, n, frame);
}
static void poll(void) { pa_usb_ble_poll(&g_ctx); }
static void reset_all(void)
{
    memset(&g_ctx, 0, sizeof g_ctx); g_ncalls = 0; g_accept_events = -1;
    pa_usb_ble_init(&g_ctx);                       /* the production wiring: reply sinks, ring indices */
    pa_diag_set_flash_reader(fake_reader);
    /* drain any parser state a previous test left behind with a clean frame boundary */
    pa_diag_feed(0xF0); pa_diag_feed(0xF7); pa_sysex_input_byte(0xF0); pa_sysex_input_byte(0xF7);
    g_ncalls = 0;
}

/* ---- the test harness ------------------------------------------------------------------------------------------------------------------------------ */
static int n_pass, n_fail, n_known;
static const char *t_name; static int t_failed, t_known; static clock_t t_t0; static char t_detail[300];
static void T_BEGIN(const char *name) { t_name = name; t_failed = 0; t_known = 0; t_detail[0] = 0; t_t0 = clock(); reset_all(); }
#define EXPECT(cond, ...) do { if (!(cond) && !t_failed) { t_failed = 1; snprintf(t_detail, sizeof t_detail, __VA_ARGS__); } } while (0)
static void T_END(void)
{
    long ms = (long)((clock() - t_t0) * 1000L / CLOCKS_PER_SEC);
    const char *r = t_failed ? "FAIL" : t_known ? "KNOWN" : "PASS";
    if (t_failed) n_fail++; else if (t_known) n_known++; else n_pass++;
    printf("COMMS-TEST|usb_midi_e2e|%s|%s|%ld|%s\n", t_name, r, ms, t_failed ? t_detail : (t_known ? t_detail : "ok"));
}

/* the golden vectors from the 2026-11-03 investigation: the seven identical requests (flash-read form of the build-id read) and their reply */
/* RAM-read form (7D 02 -> 7D 82), exactly as the PC tool sends it and the device answers it (the 2026-11-03 investigation) */
static const uint8_t G_RREQ_EVENTS[16] = { 0x04,0xf0,0x7d,0x04, 0x04,0x40,0x2d,0x7d, 0x04,0x38,0x00,0x04, 0x07,0x00,0x00,0xf7 };
static const uint8_t G_RREP_EVENTS[28] = { 0x04,0xf0,0x7d,0x04, 0x04,0x22,0x00,0x00, 0x04,0x36,0x75,0x63, 0x04,0x01,0x04,0x31, 0x04,0x22,0x14,0x49, 0x04,0x04,0x05,0x21, 0x07,0x37,0x02,0xf7 };
static const uint8_t G_RREP_FRAME[21] = { 0xf0,0x7d,0x04,0x22,0x00,0x00,0x36,0x75,0x63,0x01,0x04,0x31,0x22,0x14,0x49,0x04,0x05,0x21,0x37,0x02,0xf7 };
/* flash-read form (7D 01 -> 7D 81): the same pipeline and payload, answered by the host build (which has no RAM window) */
static const uint8_t G_FREQ_EVENTS[16] = { 0x04,0xf0,0x7d,0x02, 0x04,0x40,0x2d,0x7d, 0x04,0x38,0x00,0x04, 0x07,0x00,0x00,0xf7 };
static const uint8_t G_FREP_EVENTS[28] = { 0x04,0xf0,0x7d,0x02, 0x04,0x22,0x00,0x00, 0x04,0x36,0x75,0x63, 0x04,0x01,0x04,0x31, 0x04,0x22,0x14,0x49, 0x04,0x04,0x05,0x21, 0x07,0x37,0x02,0xf7 };
static const uint8_t G_FREP_FRAME[21] = { 0xf0,0x7d,0x02,0x22,0x00,0x00,0x36,0x75,0x63,0x01,0x04,0x31,0x22,0x14,0x49,0x04,0x05,0x21,0x37,0x02,0xf7 };
#define BUILD_ADDR 0x01C7D5B0u

int main(void)
{
    uint8_t frame[1400], want[1400], ev[1400];

    /* ===== 1. the seven identical requests (the regression from the USB investigation) ===== */
    T_BEGIN("seven_identical_requests_exact_replies");
    {
        /* the golden bytes are for the RAM-read command 7D 02 sent by the PC tool; on the host the RAM window is refused, so the same pipeline is driven with the
         * flash-read command 7D 01 and its reply is the golden payload with the reply byte 81. The request/reply bytes below are computed by the reference packer. */
        uint32_t nreq = diag_request(0x01, BUILD_ADDR, 8, frame);
        uint8_t rqev[64]; uint32_t rqn = events_of(frame, nreq, rqev);
        uint32_t nrep = diag_expected(0x81, BUILD_ADDR, 8, 0, want);
        EXPECT(nreq == 12u && rqn == 16u, "request is %u MIDI bytes / %u event bytes (expect 12 / 16)", (unsigned)nreq, (unsigned)rqn);
        EXPECT(nrep == 21u && !memcmp(want, G_FREP_FRAME, 21), "the reference reply frame does not match the golden literal");
        EXPECT(!memcmp(rqev, G_FREQ_EVENTS, 16), "the reference request events do not match the golden literal");
        memcpy(want, G_FREP_FRAME, 21);                       /* from here on the expectation is the golden literal, not the reference */
        for (int k = 0; k < 7; k++) {
            uint32_t before = g_ncalls;
            deliver_events(G_FREQ_EVENTS, 16, 16);          /* the golden request, one 16-byte bulk OUT packet */
            EXPECT(g_ncalls == before, "reply %d produced before the poll", k + 1);
            poll();
            EXPECT(g_ncalls == before + 1u, "request %d: %u transfers queued (expect exactly 1)", k + 1, (unsigned)(g_ncalls - before));
            if (g_ncalls == before + 1u) {
                EXPECT(g_calls[before].len == 28u && !memcmp(g_calls[before].b, G_FREP_EVENTS, 28), "request %d: reply event packets differ from the golden literal (len %u)", k + 1, (unsigned)g_calls[before].len);
                int nm = collect(before);
                EXPECT(nm == 1, "request %d: %d messages (%s)", k + 1, nm, g_why);
                if (nm == 1) {
                    EXPECT(g_msg_len[0] == 21u && !memcmp(g_msg[0], want, 21), "request %d: reply frame differs from the reference", k + 1);
                    EXPECT(g_msg_events[0] == 7, "request %d: %d event packets (expect 7)", k + 1, g_msg_events[0]);
                }
                for (uint32_t i = 0; i < 6u; i++) EXPECT((g_calls[before].b[4u * i] & 0xF) == 4u, "request %d: event %u has CIN %X (expect 4)", k + 1, (unsigned)i, g_calls[before].b[4u * i] & 0xF);
                EXPECT((g_calls[before].b[24] & 0xF) == 7u, "request %d: last event has CIN %X (expect 7)", k + 1, g_calls[before].b[24] & 0xF);
            }
        }
        EXPECT(g_ncalls == 7u, "total transfers %u (expect 7: no missing, duplicated or extra replies)", (unsigned)g_ncalls);
        EXPECT(g_ctx.midi_rx_rd == g_ctx.midi_rx_wr, "ring not empty after the polls");
        /* all seven replies byte-identical to each other */
        for (uint32_t i = 1; i < g_ncalls; i++) EXPECT(g_calls[i].len == g_calls[0].len && !memcmp(g_calls[i].b, g_calls[0].b, g_calls[0].len), "reply %u differs from reply 1", (unsigned)i + 1u);
    }
    T_END();

    /* ===== 1b. the RAM-read command exactly as the PC tool sends it: refused on the host, answered 7D 82 FF FF (the device answers with the data) ===== */
    T_BEGIN("ram_read_request_refused_on_host_build");
    {
        deliver_events(G_RREQ_EVENTS, 16, 16); poll();
        int nm = collect(0);
        EXPECT(nm == 1, "%d messages (%s)", nm, g_why);
        if (nm == 1) { uint32_t nw = diag_expected(0x82, BUILD_ADDR, 8, 1, want); EXPECT(g_msg_len[0] == nw && !memcmp(g_msg[0], want, nw), "refusal frame differs"); }
    }
    T_END();

    /* ===== 1c. the golden reply for the device build (the 21-byte frame from the investigation) ===== */
    T_BEGIN("golden_reply_frame_and_events");
    {
        /* the golden frame is the RAM-read reply 7D 82 ...; this build answers the flash form 7D 81 ..., so only the third byte of the packed stream differs */
        uint32_t nw = diag_expected(0x82, BUILD_ADDR, 8, 0, want);
        EXPECT(nw == sizeof G_RREP_FRAME && !memcmp(want, G_RREP_FRAME, nw), "reference reply does not match the golden frame from the investigation");
        uint32_t ne = events_of(want, nw, ev);
        EXPECT(ne == sizeof G_RREP_EVENTS && !memcmp(ev, G_RREP_EVENTS, ne), "reference events do not match the golden event packets");
        uint32_t nr = diag_request(0x02, BUILD_ADDR, 8, frame); uint8_t re[64]; uint32_t rk = events_of(frame, nr, re);
        EXPECT(rk == sizeof G_RREQ_EVENTS && !memcmp(re, G_RREQ_EVENTS, rk), "reference RAM-read request events do not match the golden literal");
    }
    T_END();

    /* ===== 2. delivery granularity: one event per packet, two events per packet, byte-for-byte the same reply ===== */
    for (int per = 1; per <= 3; per++) {
        char nm_[64]; snprintf(nm_, sizeof nm_, "request_split_%d_event_per_packet", per);
        T_BEGIN(nm_);
        uint32_t nreq = diag_request(0x01, BUILD_ADDR, 8, frame); uint8_t rqev[64]; uint32_t rqn = events_of(frame, nreq, rqev);
        for (uint32_t o = 0; o < rqn; o += 4u * (uint32_t)per) {
            deliver_events(rqev + o, (rqn - o) < 4u * (uint32_t)per ? rqn - o : 4u * (uint32_t)per, (uint32_t)per);
            poll();
            EXPECT(g_ncalls == 0u || o + 4u * (uint32_t)per >= rqn, "a reply appeared before the final F7 event");
        }
        EXPECT(g_ncalls == 1u, "%u replies (expect 1)", (unsigned)g_ncalls);
        T_END();
    }

    /* ===== 3. malformed input, then a valid request: the parser must recover, with exactly one reply ===== */
    {
        uint8_t good[64]; uint32_t ng = diag_request(0x01, BUILD_ADDR, 8, good);
        uint8_t gev[64]; uint32_t gn = events_of(good, ng, gev);
        struct { const char *name; uint8_t ev[160]; uint32_t n; uint32_t junk_replies; } cases[8]; int nc = 0;
        /* a: F0 and data but no F7, then the valid request */
        memset(cases, 0, sizeof cases);
        cases[nc].name = "recover_after_missing_F7"; memcpy(cases[nc].ev, gev, 12); cases[nc].n = 12; nc++;
        cases[nc].name = "recover_after_stray_data_without_F0"; { uint8_t d[8] = { 0x04,0x40,0x2d,0x7d, 0x04,0x38,0x00,0x04 }; memcpy(cases[nc].ev, d, 8); cases[nc].n = 8; } nc++;
        cases[nc].name = "recover_after_lone_F7"; { uint8_t d[4] = { 0x05,0xf7,0x00,0x00 }; memcpy(cases[nc].ev, d, 4); cases[nc].n = 4; } nc++;
        cases[nc].name = "recover_after_wrong_manufacturer_id"; {
            uint8_t in[8] = { 0x7E, 0x01, 0xB0, 0xD5, 0xC7, 0x01, 8, 0 }, f[64]; uint32_t n = frame_of(in, 8, f); cases[nc].n = events_of(f, n, cases[nc].ev); } nc++;
        cases[nc].name = "recover_after_short_request_7_bytes"; {
            uint8_t in[7] = { 0x7D, 0x01, 0xB0, 0xD5, 0xC7, 0x01, 8 }, f[64]; uint32_t n = frame_of(in, 7, f); cases[nc].n = events_of(f, n, cases[nc].ev); } nc++;
        cases[nc].name = "recover_after_overlong_garbage_frame"; {
            uint8_t f[700]; uint32_t n = 0; f[n++] = 0xF0; for (int i = 0; i < 600; i++) f[n++] = 0x55; f[n++] = 0xF7;
            /* too long for the cases[] buffer: delivered separately below */ (void)f; cases[nc].n = 0; } nc++;
        for (int c = 0; c < nc; c++) {
            T_BEGIN(cases[c].name);
            if (!strcmp(cases[c].name, "recover_after_overlong_garbage_frame")) {
                uint8_t f[700]; uint32_t n = 0; f[n++] = 0xF0; for (int i = 0; i < 600; i++) f[n++] = 0x55; f[n++] = 0xF7;
                uint8_t big[1000]; uint32_t bn = events_of(f, n, big);
                for (uint32_t o = 0; o < bn; o += 64u) { deliver_events(big + o, (bn - o) < 64u ? bn - o : 64u, 16); poll(); }
            } else if (cases[c].n) { deliver_events(cases[c].ev, cases[c].n, 16); poll(); }
            EXPECT(g_ncalls == 0u, "malformed input produced %u replies (expect 0)", (unsigned)g_ncalls);
            deliver_events(gev, gn, 16); poll();
            EXPECT(g_ncalls == 1u, "after the malformed input the valid request produced %u replies (expect 1)", (unsigned)g_ncalls);
            if (g_ncalls == 1u) { int m = collect(0); diag_expected(0x81, BUILD_ADDR, 8, 0, want); EXPECT(m == 1 && g_msg_len[0] == 21u && !memcmp(g_msg[0], want, 21), "recovered reply is wrong (%s)", g_why); }
            T_END();
        }
    }

    /* ===== 3b. two frames in one bulk packet, and non-SysEx traffic around a request ===== */
    T_BEGIN("two_requests_in_one_packet");
    {
        uint8_t a[64], b[64], pk[64]; uint32_t na = diag_request(0x01, BUILD_ADDR, 8, a), nb = diag_request(0x01, 0x100u, 4, b);
        uint8_t ea[64], eb[64]; uint32_t ka = events_of(a, na, ea), kb = events_of(b, nb, eb);
        EXPECT(ka + kb <= 64u, "test setup: both requests do not fit one packet");
        memcpy(pk, ea, ka); memcpy(pk + ka, eb, kb);
        deliver_events(pk, ka + kb, 16); poll();
        EXPECT(g_ncalls == 2u, "%u replies (expect 2)", (unsigned)g_ncalls);
        int nm = collect(0); EXPECT(nm == 2, "%d messages (%s)", nm, g_why);
        if (nm == 2) {
            uint32_t w1 = diag_expected(0x81, BUILD_ADDR, 8, 0, want);
            EXPECT(g_msg_len[0] == w1 && !memcmp(g_msg[0], want, w1), "first reply wrong");
            uint32_t w2 = diag_expected(0x81, 0x100u, 4, 0, want);
            EXPECT(g_msg_len[1] == w2 && !memcmp(g_msg[1], want, w2), "second reply wrong");
        }
    }
    T_END();

    T_BEGIN("cc_and_program_change_between_requests");
    {
        const uint8_t cc[4] = { 0x0B, 0xB0, 20, 127 }, pc[4] = { 0x0C, 0xC0, 5, 0 };
        deliver_events(cc, 4, 1); poll(); deliver_events(G_FREQ_EVENTS, 16, 16); poll(); deliver_events(pc, 4, 1); poll(); deliver_events(G_FREQ_EVENTS, 16, 16); poll();
        EXPECT(g_ncalls == 2u, "%u replies (expect 2: CC / PC between frames must not disturb them)", (unsigned)g_ncalls);
        EXPECT(g_ctx.midi_pedal_toggle_slot == 0, "CC 20 value 127 did not toggle slot 0 (slot=%d)", g_ctx.midi_pedal_toggle_slot);
        EXPECT(g_ctx.midi_pedal_preset_req == 5, "PC 5 did not request preset 5 (req=%d)", g_ctx.midi_pedal_preset_req);
        int nm = collect(0); EXPECT(nm == 2 && !memcmp(g_msg[0], G_FREP_FRAME, 21) && !memcmp(g_msg[1], G_FREP_FRAME, 21), "reply content wrong (%s)", g_why);
    }
    T_END();

    /* KNOWN DEFECT D3 (src/sys/pa_diag.c pa_diag_feed, src/sys/sysex.c pa_sysex_input_byte): both parsers take EVERY byte between F0 and F7 as frame data.
     * MIDI 1.0: a real-time byte (F8..FF) may appear anywhere, inside a SysEx too, and must be ignored by the receiver. Found by this gate on 2026-11-03: a MIDI clock
     * byte in the middle of a request corrupts it. The test pins today's behaviour; when the defect is fixed it fails and must be updated into a plain PASS check. */
    T_BEGIN("realtime_byte_inside_a_sysex_frame");
    {
        const uint8_t rt[4] = { 0x0F, 0xF8, 0, 0 };
        deliver_events(G_FREQ_EVENTS, 8, 2); deliver_events(rt, 4, 1); deliver_events(G_FREQ_EVENTS + 8, 8, 2); poll();
        int nm = collect(0);
        int correct = (g_ncalls == 1u && nm == 1 && g_msg_len[0] == 21u && !memcmp(g_msg[0], G_FREP_FRAME, 21));
        if (correct) EXPECT(0, "a real-time byte inside a frame no longer corrupts it: D3 is fixed, make this a plain PASS test");
        else { t_known = 1; snprintf(t_detail, sizeof t_detail, "KNOWN D3: a real-time byte inside a SysEx frame corrupts it (%u transfers, %d messages)", (unsigned)g_ncalls, nm); }
    }
    T_END();

    /* KNOWN DEFECT D4 (same functions): MIDI 1.0 says any non-real-time status byte inside a SysEx ends it (the message is abandoned). Here a program change inside a
     * request is swallowed as frame data: the corrupted frame still looked like a read command and was ANSWERED (a 256-byte read at a garbage address). Expected by
     * the spec: no reply. */
    T_BEGIN("status_byte_inside_a_sysex_frame");
    {
        const uint8_t pc[4] = { 0x0C, 0xC0, 5, 0 };
        deliver_events(G_FREQ_EVENTS, 8, 2); deliver_events(pc, 4, 1); deliver_events(G_FREQ_EVENTS + 8, 8, 2); poll();
        if (g_ncalls == 0u) EXPECT(0, "a status byte inside a frame now abandons it: D4 is fixed, make this a plain PASS test");
        else { t_known = 1; snprintf(t_detail, sizeof t_detail, "KNOWN D4: a status byte inside a SysEx frame is taken as data; the garbled frame produced %u transfers", (unsigned)g_ncalls); }
    }
    T_END();

    T_BEGIN("cin0_padding_between_packets_and_inside_a_packet");
    {
        uint8_t a[64], ea[64]; uint32_t na = diag_request(0x01, BUILD_ADDR, 8, a), ka = events_of(a, na, ea);
        const uint8_t pad[4] = { 0, 0, 0, 0 };
        deliver_events(ea, 8, 2); deliver_events(pad, 4, 1); deliver_events(ea + 8, ka - 8, 2); poll();   /* padding as its own packet between two parts of a frame */
        EXPECT(g_ncalls == 1u, "padding between packets: %u replies (expect 1)", (unsigned)g_ncalls);
        /* characterisation: a CIN 0 event INSIDE a packet ends the decoding of that packet (usb_ble.c: if (cin == 0) break;), so the rest of the packet is ignored */
        g_ncalls = 0;
        uint8_t mix[64]; memcpy(mix, ea, 8); memcpy(mix + 8, pad, 4); memcpy(mix + 12, ea + 8, ka - 8);
        deliver_events(mix, 12u + (ka - 8u), 16); poll();
        EXPECT(g_ncalls == 0u, "a CIN 0 event inside a packet no longer ends the packet (%u replies): update this characterisation", (unsigned)g_ncalls);
        deliver_events(ea, ka, 16); poll();       /* and the pipeline still works afterwards */
        EXPECT(g_ncalls == 1u, "no reply to the request after the truncated frame (%u)", (unsigned)g_ncalls);
    }
    T_END();

    /* ===== 4. the receive ring: capacity, overflow, wraparound ===== */
    T_BEGIN("ring_wraparound_500_requests");
    {
        uint8_t a[64], ea[64]; uint32_t na = diag_request(0x01, BUILD_ADDR, 8, a), ka = events_of(a, na, ea);
        uint32_t good = 0; diag_expected(0x81, BUILD_ADDR, 8, 0, want);
        for (int k = 0; k < 500; k++) {
            uint32_t before = g_ncalls;
            deliver_events(ea, ka, 16); poll();
            if (g_ncalls == before + 1u && g_calls[before].len == 28u && collect(before) == 1 && !memcmp(g_msg[0], want, 21)) good++;
        }
        EXPECT(good == 500u, "%u of 500 requests answered correctly across ring wraparounds", (unsigned)good);
        EXPECT(g_ncalls == 500u, "%u transfers (expect 500)", (unsigned)g_ncalls);
        EXPECT(g_ctx.midi_rx_rd == g_ctx.midi_rx_wr, "ring not empty");
    }
    T_END();

    T_BEGIN("ring_frame_straddling_the_wrap_point");
    {
        uint8_t a[64], ea[64]; uint32_t na = diag_request(0x01, BUILD_ADDR, 8, a), ka = events_of(a, na, ea);
        for (uint16_t start = 500; start < 512; start++) {
            g_ctx.midi_rx_rd = g_ctx.midi_rx_wr = start;          /* an empty ring whose indices sit near the end of the buffer */
            uint32_t before = g_ncalls;
            deliver_events(ea, ka, 16); poll();
            EXPECT(g_ncalls == before + 1u, "start index %u: %u replies (expect 1)", start, (unsigned)(g_ncalls - before));
        }
    }
    T_END();

    T_BEGIN("ring_capacity_511_and_overflow_recovery");
    {
        /* fill the ring with data bytes outside any frame (ignored by both parsers), without polling: it holds 511 (one slot is the full/empty marker) */
        uint8_t junk[4] = { 0x04, 0x11, 0x22, 0x33 };
        for (int i = 0; i < 200; i++) pa_usb_midi_rx_packet(&g_ctx, junk, 4);     /* 600 bytes offered */
        uint32_t held = (uint32_t)((g_ctx.midi_rx_wr - g_ctx.midi_rx_rd) & 511u);
        EXPECT(held == 511u, "ring holds %u bytes (expect 511)", (unsigned)held);
        poll();
        EXPECT(g_ctx.midi_rx_rd == g_ctx.midi_rx_wr && g_ncalls == 0u, "ring not drained / spurious reply");
        /* overflow in the MIDDLE of a request: bytes are dropped silently (characterised), the damaged frame must not produce a reply, the next valid one must */
        uint8_t a[64], ea[64]; uint32_t na = diag_request(0x01, BUILD_ADDR, 8, a), ka = events_of(a, na, ea);
        for (int i = 0; i < 168; i++) pa_usb_midi_rx_packet(&g_ctx, junk, 4);     /* 504 bytes: 7 slots left */
        deliver_events(ea, ka, 16);                                              /* 12 bytes offered, 7 fit: the frame loses its tail */
        poll();
        EXPECT(g_ncalls == 0u, "a request truncated by ring overflow produced %u replies (expect 0)", (unsigned)g_ncalls);
        deliver_events(ea, ka, 16); poll();
        EXPECT(g_ncalls == 1u, "the request after the overflow produced %u replies (expect 1)", (unsigned)g_ncalls);
    }
    T_END();

    /* ===== 5. maximum sizes: the biggest reply, clamping, zero length, an exact 64-byte transfer ===== */
    T_BEGIN("max_reply_256_bytes_multi_transfer");
    {
        uint32_t nreq = diag_request(0x01, 0x2000u, 256, frame); uint8_t rqev[64]; uint32_t rqn = events_of(frame, nreq, rqev);
        deliver_events(rqev, rqn, 16); poll();
        uint32_t nw = diag_expected(0x81, 0x2000u, 256, 0, want);
        int nm = collect(0);
        EXPECT(nm == 1, "%d messages (%s)", nm, g_why);
        EXPECT(nm == 1 && g_msg_len[0] == nw && !memcmp(g_msg[0], want, nw), "the 256-byte reply differs from the reference (len %u vs %u)", nm == 1 ? (unsigned)g_msg_len[0] : 0u, (unsigned)nw);
        uint32_t total = 0; for (uint32_t i = 0; i < g_ncalls; i++) { total += g_calls[i].len; EXPECT(g_calls[i].len <= 64u, "transfer %u is %u bytes (max 64)", (unsigned)i, (unsigned)g_calls[i].len); }
        EXPECT(g_ncalls == (total + 63u) / 64u, "%u transfers for %u event bytes (expect %u)", (unsigned)g_ncalls, (unsigned)total, (unsigned)((total + 63u) / 64u));
    }
    T_END();

    T_BEGIN("request_longer_than_256_is_clamped");
    {
        uint32_t nreq = diag_request(0x01, 0x2000u, 300, frame); uint8_t rqev[64]; uint32_t rqn = events_of(frame, nreq, rqev);
        deliver_events(rqev, rqn, 16); poll();
        uint32_t nw = diag_expected(0x81, 0x2000u, 300, 0, want);       /* the reference clamps to 256 as well */
        int nm = collect(0);
        EXPECT(nm == 1 && g_msg_len[0] == nw && !memcmp(g_msg[0], want, nw), "clamped reply differs (%s)", g_why);
    }
    T_END();

    T_BEGIN("zero_length_and_failed_reads");
    {
        uint32_t nreq = diag_request(0x01, 0x3000u, 0, frame); uint8_t rqev[64]; uint32_t rqn = events_of(frame, nreq, rqev);
        deliver_events(rqev, rqn, 16); poll();
        uint32_t nw = diag_expected(0x81, 0x3000u, 0, 0, want); int nm = collect(0);
        EXPECT(nm == 1 && g_msg_len[0] == nw && !memcmp(g_msg[0], want, nw), "zero-length reply differs (%s)", g_why);
        uint32_t mark = g_ncalls;
        nreq = diag_request(0x01, 0xFFFFFFF0u, 8, frame); rqn = events_of(frame, nreq, rqev); deliver_events(rqev, rqn, 16); poll();
        nw = diag_expected(0x81, 0xFFFFFFF0u, 8, 1, want); nm = collect(mark);
        EXPECT(nm == 1 && g_msg_len[0] == nw && !memcmp(g_msg[0], want, nw), "failed-read reply differs (%s)", g_why);
    }
    T_END();

    T_BEGIN("reply_of_exactly_64_event_bytes_is_one_full_transfer");
    {
        /* 32 data bytes -> 48-byte frame -> 16 events = exactly 64 bytes. usb_ble.c hands it over as ONE full transfer; closing it with a short packet is the
         * USB driver's job (the padding rule in usb_dev.c), which the driver state-machine tests cover. This pins the hand-over. */
        uint32_t nreq = diag_request(0x01, 0x4000u, 32, frame); uint8_t rqev[64]; uint32_t rqn = events_of(frame, nreq, rqev);
        deliver_events(rqev, rqn, 16); poll();
        EXPECT(g_ncalls == 1u && g_calls[0].len == 64u, "hand-over is %u transfers, first %u bytes (expect 1 x 64)", (unsigned)g_ncalls, g_ncalls ? (unsigned)g_calls[0].len : 0u);
    }
    T_END();

    /* ===== 6. every CIN on the receive side ===== */
    T_BEGIN("rx_cin_table_all_16_values");
    {
        static const uint8_t spec_len[16] = { 0, 0, 2, 3, 3, 1, 2, 3, 3, 3, 3, 3, 2, 2, 3, 1 };   /* USB-MIDI class spec table 4-1: data bytes per CIN (0 and 1 are reserved) */
        for (int cin = 0; cin < 16; cin++) {
            for (int cable = 0; cable < 2; cable++) {                                            /* the cable number must not change the decoding */
                g_ctx.midi_rx_rd = g_ctx.midi_rx_wr = 0;
                uint8_t e[4] = { (uint8_t)((cable << 4) | cin), 0x11, 0x22, 0x33 };
                pa_usb_midi_rx_packet(&g_ctx, e, 4);
                uint32_t n = (uint32_t)((g_ctx.midi_rx_wr - g_ctx.midi_rx_rd) & 511u);
                EXPECT(n == spec_len[cin], "CIN %X cable %d: %u bytes pushed (spec %u)", cin, cable, (unsigned)n, (unsigned)spec_len[cin]);
            }
        }
        g_ctx.midi_rx_rd = g_ctx.midi_rx_wr = 0;
    }
    T_END();

    T_BEGIN("rx_pedal_messages_and_their_limits");
    {
        struct { uint8_t cin, st, d1, d2; } t[] = {
            { 0x0B, 0xB0, 64, 127 }, { 0x0B, 0xB0, 65, 127 }, { 0x0B, 0xB0, 66, 64 }, { 0x0B, 0xB0, 67, 127 },       /* HOME, PARA click, encoder - / + */
            { 0x0B, 0xB0, 20, 63 },                                                                                  /* value < 64: ignored */
            { 0x0C, 0xC0, 79, 0 }, { 0x0C, 0xC0, 80, 0 },                                                            /* preset 79 accepted, 80 ignored */
        };
        pa_usb_ble_ctx_t *c = &g_ctx; (void)c;
        for (unsigned i = 0; i < sizeof t / sizeof t[0]; i++) { uint8_t e[4] = { t[i].cin, t[i].st, t[i].d1, t[i].d2 }; pa_usb_midi_rx_packet(&g_ctx, e, 4); }
        EXPECT(g_ctx.midi_pedal_btn_events == (PA_BTN_HOME | PA_BTN_PARA_CLICK), "button events %#x", (unsigned)g_ctx.midi_pedal_btn_events);
        EXPECT(g_ctx.midi_pedal_enc_delta == 0, "encoder delta %d (expect -1 + 1 = 0)", (int)g_ctx.midi_pedal_enc_delta);
        EXPECT(g_ctx.midi_pedal_toggle_slot == -1, "CC 20 value 63 toggled a slot");
        EXPECT(g_ctx.midi_pedal_preset_req == 79, "preset request %d (expect 79; PC 80 must be ignored)", (int)g_ctx.midi_pedal_preset_req);
    }
    T_END();

    /* The stock map's module CCs (POCKET_AMP_MIDI_ADDRESS_MAP.md 2.2; MIDI-MAP-CUSTOM.md 2.2): CC 20 FX, 21 AMP, 22 MOD, 23 DLY, 24 REV, 25 CAB; ours continue with 26 DRIVE and
     * 27 EQ. The firmware's own slot order is DYN, DRIVE, AMP, CAB, EQ, MOD, DLY, REV, so the CC number must be translated, not subtracted from 20 (defect D1). */
    T_BEGIN("rx_pedal_module_ccs_follow_the_stock_module_order");
    {
        static const struct { uint8_t cc; int slot; const char *name; } k[] = {
            { 20, PA_SLOT_DYNAMICS, "FX" }, { 21, PA_SLOT_AMP, "AMP" }, { 22, PA_SLOT_MOD, "MOD" }, { 23, PA_SLOT_DELAY, "DLY" },
            { 24, PA_SLOT_REVERB, "REV" }, { 25, PA_SLOT_CAB, "CAB" }, { 26, PA_SLOT_DRIVE, "DRIVE" }, { 27, PA_SLOT_EQ, "EQ" },
        };
        for (unsigned i = 0; i < sizeof k / sizeof k[0]; i++) {
            g_ctx.midi_pedal_toggle_slot = -1;
            uint8_t e[4] = { 0x0B, 0xB0, k[i].cc, 127 };
            pa_usb_midi_rx_packet(&g_ctx, e, 4);
            EXPECT(g_ctx.midi_pedal_toggle_slot == k[i].slot, "CC %u (%s) toggles firmware slot %d (expect %d)", k[i].cc, k[i].name, g_ctx.midi_pedal_toggle_slot, k[i].slot);
        }
        for (uint8_t cc = 28; cc <= 31; cc++) {                /* not a toggle: nothing may happen */
            g_ctx.midi_pedal_toggle_slot = -1;
            uint8_t e[4] = { 0x0B, 0xB0, cc, 127 };
            pa_usb_midi_rx_packet(&g_ctx, e, 4);
            EXPECT(g_ctx.midi_pedal_toggle_slot == -1, "CC %u toggled slot %d (expect none)", cc, g_ctx.midi_pedal_toggle_slot);
        }
        g_ctx.midi_pedal_toggle_slot = -1;
    }
    T_END();

    /* ===== 7. TX saturation: the driver's queue answers -1 mid-reply ===== */
    T_BEGIN("tx_queue_full_returns_error_and_recovers");
    {
        uint8_t a[64], ea[64]; uint32_t na = diag_request(0x01, BUILD_ADDR, 8, a), ka = events_of(a, na, ea);
        g_accept_events = 3;                                  /* the queue has room for 3 of the 7 event packets */
        deliver_events(ea, ka, 16); poll();
        const uint32_t partial = g_ncalls ? g_calls[0].len : 0u;
        g_accept_events = -1;
        uint32_t mark = g_ncalls;
        deliver_events(ea, ka, 16); poll();
        EXPECT(g_ncalls == mark + 1u, "after the queue emptied the next request produced %u transfers (expect 1)", (unsigned)(g_ncalls - mark));
        int nm = collect(mark); diag_expected(0x81, BUILD_ADDR, 8, 0, want);
        EXPECT(nm == 1 && g_msg_len[0] == 21u && !memcmp(g_msg[0], want, 21), "recovered reply wrong (%s)", g_why);
        /* KNOWN DEFECT D2 (custom-fw usb_dev.c pa_usb_dev_tx / usb_ble.c:188): a reply that does not fit is delivered PARTIALLY - the packets that fitted stay queued
         * without their F7. The host sees an unterminated SysEx. Expected today: 12 bytes (3 events) queued. If this changes, the defect was fixed: update. */
        if (partial == 12u) { t_known = 1; snprintf(t_detail, sizeof t_detail, "KNOWN D2: a reply that does not fit leaves %u bytes queued without F7", (unsigned)partial); }
        else EXPECT(0, "partial-reply behaviour changed (%u bytes queued); update the test and retire D2", (unsigned)partial);
    }
    T_END();

    /* ===== 8. the editor SysEx protocol and the OTA handshake through the same pipeline ===== */
    T_BEGIN("editor_descriptor_request_cmd18");
    {
        uint32_t nreq = editor_frame(PA_CMD_DESCRIPTOR_18, 0, 0, frame); uint8_t rqev[64]; uint32_t rqn = events_of(frame, nreq, rqev);
        deliver_events(rqev, rqn, 16); poll();
        int nm = collect(0);
        EXPECT(g_ncalls >= 1u && nm == 1, "%d messages / %u transfers (%s)", nm, (unsigned)g_ncalls, g_why);
        if (nm == 1) {
            uint8_t inner[64]; uint32_t k = unpack_7to8(g_msg[0] + 1, g_msg_len[0] - 2u, inner);
            const uint8_t pl[13] = { 'P','D','0','1', 1, 0, 8, 16, 0, 0, 0, 0, 0 };
            uint32_t sum = PA_REPLY_BLOCK + 13u; for (int i = 0; i < 13; i++) sum += pl[i];
            EXPECT(k >= 20u && inner[0] == 0x00 && inner[1] == 0x59 && inner[2] == PA_REPLY_BLOCK && inner[3] == 13 && inner[4] == 0 && inner[5] == 0, "reply header wrong");
            EXPECT(k >= 20u && !memcmp(inner + 6, pl, 13), "reply payload differs from the descriptor");
            EXPECT(k >= 20u && inner[19] == (uint8_t)~sum, "reply checksum %02X (expect %02X)", k >= 20u ? inner[19] : 0u, (unsigned)(uint8_t)~sum);
        }
    }
    T_END();

    T_BEGIN("editor_device_info_cmd17_and_diag_interleaved");
    {
        uint8_t f1[200], f2[64], e1[400], e2[64];
        uint32_t n1 = editor_frame(PA_CMD_DEVICE_INFO_17, 0, 0, f1), k1 = events_of(f1, n1, e1);
        uint32_t n2 = diag_request(0x01, BUILD_ADDR, 8, f2), k2 = events_of(f2, n2, e2);
        for (int r = 0; r < 5; r++) { deliver_events(e1, k1, 16); poll(); deliver_events(e2, k2, 16); poll(); }
        int nm = collect(0);
        EXPECT(nm == 10, "%d replies for 10 interleaved requests (%s)", nm, g_why);
        int info = 0, diag = 0;
        for (int i = 0; i < nm; i++) {
            uint8_t inner[300]; uint32_t k = unpack_7to8(g_msg[i] + 1, g_msg_len[i] - 2u, inner);
            if (k >= 7u && inner[0] == 0x00 && inner[1] == 0x59 && inner[2] == PA_REPLY_DEVICE_INFO) info++;
            else if (k >= 2u && inner[0] == 0x7D && inner[1] == 0x81) diag++;
        }
        EXPECT(info == 5 && diag == 5, "%d device-info and %d diagnostic replies (expect 5 and 5)", info, diag);
    }
    T_END();

    T_BEGIN("ota_query_answered_and_upgrade_frame_arms_reboot");
    {
        static const uint8_t q[10] = { 0xF0, 0x00, 0x32, 0x45, 0x00, 0x00, 0x00, 0x40, 0x7F, 0xF7 };
        deliver_frame(q, 10, 16); poll();
        int nm = collect(0);
        EXPECT(nm == 1, "OTA query: %d replies (expect 1; the editor parser must stay silent) (%s)", nm, g_why);
        if (nm == 1) {
            EXPECT(g_msg_len[0] == PA_OTA_ID_REPLY_LEN, "ID reply is %u bytes (expect %u)", (unsigned)g_msg_len[0], (unsigned)PA_OTA_ID_REPLY_LEN);
            EXPECT(g_msg[0][0] == 0xF0 && g_msg[0][1] == 0x00 && g_msg[0][2] == 0x32 && g_msg[0][3] == 0x45 && g_msg[0][4] == 0x58, "ID reply header wrong");
            uint8_t d[64]; uint32_t k = 0, acc = 0, nb = 0;       /* decode the 7-bit packing: 34 bytes, 00 59 11 ... "Pocket AMP_105" */
            for (uint32_t i = 1; i + 1 < g_msg_len[0]; i++) { acc |= (uint32_t)(g_msg[0][i] & 0x7Fu) << nb; nb += 7u; if (nb >= 8u) { d[k++] = (uint8_t)acc; acc >>= 8; nb -= 8u; } }
            EXPECT(k == 34u && d[0] == 0 && d[1] == 0x59 && d[2] == 0x11 && !memcmp(d + 6, "Pocket AMP_105", 14), "ID reply does not decode to the identity (%u bytes)", (unsigned)k);
        }
        EXPECT(g_ctx.ota_reboot_pending == 0u, "the query armed a reboot");
        static const uint8_t up[6] = { 0xF0, 0x22, 0x24, 0x35, 0x7D, 0xF7 };      /* the soft key: the ROM download mode */
        uint32_t mark = g_ncalls; deliver_frame(up, 6, 16); poll();
        EXPECT(g_ctx.ota_reboot_pending == PA_OTA_REBOOT_MAGIC, "the soft-key frame did not arm the reboot (%#x)", (unsigned)g_ctx.ota_reboot_pending);
        EXPECT(g_ctx.ota_session_pending == 0u, "the soft key started an update session");
        EXPECT(g_ncalls == mark, "the soft-key frame produced a reply");
        g_ctx.ota_reboot_pending = 0u;
        static const uint8_t sess[6] = { 0xF0, 0x22, 0x24, 0x35, 0x7F, 0xF7 };    /* the update session: the main loop runs it, no reboot */
        deliver_frame(sess, 6, 16); poll();
        EXPECT(g_ctx.ota_session_pending == 1u && g_ctx.ota_reboot_pending == 0u, "the 0x7F frame arms the update session and not the reboot (%u, %#x)", (unsigned)g_ctx.ota_session_pending, (unsigned)g_ctx.ota_reboot_pending);
    }
    T_END();

    printf("COMMS-SUITE|usb_midi_e2e|passed=%d|failed=%d|known=%d\n", n_pass, n_fail, n_known);
    return n_fail ? 1 : 0;
}
