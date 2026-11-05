/* The BLE-MIDI 1.0 framing, device side.
 *
 * A real BLE-MIDI peer does not send bare MIDI bytes: every packet is
 * [header][timestamp][message] and a SysEx that spans packets is sent as
 * continuation packets with raw data and no timestamp. The device's control
 * protocol is a SysEx frame, so the framing has to be stripped before the
 * bytes reach the parser.
 *
 * The rules conform to standard BLE-MIDI 1.0 (MMA/AMEI) and the examples
 * below are the upstream's own test vectors, from
 * flutter_midi_command_ble/test/ble_midi_parse_test.dart.
 *
 *   clang -O1 -Icustom-fw/include -o test_ble_midi.exe test_ble_midi.c \
 *         custom-fw/src/board/ble_midi.c
 *   test_ble_midi.exe
 */
#include <stdio.h>
#include <string.h>
#include <stdint.h>

/* The device side of the framing, in src/board/ble_midi.c. */
uint32_t pa_ble_midi_unframe(const uint8_t *pkt, uint32_t len, uint8_t *out,
                             uint32_t cap, uint8_t *state, uint8_t *in_sysex);
typedef void (*pa_ble_emit_t)(const uint8_t *pkt, uint32_t len);
void pa_ble_midi_tx_stream(pa_ble_emit_t emit, const uint8_t *msg, uint32_t len,
                           uint32_t mtu);

/* Collect the packets the TX framer emits. */
#define MAX_PKT 64
static uint8_t  g_pkt[MAX_PKT][80];
static uint32_t g_pkt_len[MAX_PKT];
static uint32_t g_pkt_count = 0;

static void collect(const uint8_t *pkt, uint32_t len) {
    if (g_pkt_count >= MAX_PKT || len > 80u) return;
    memcpy(g_pkt[g_pkt_count], pkt, len);
    g_pkt_len[g_pkt_count] = len;
    g_pkt_count++;
}

static int fails = 0;
static void check(int ok, const char *msg) {
    printf("  %s %s\n", ok ? "ok  " : "FAIL", msg);
    if (!ok) fails++;
}

static void hex(const uint8_t *p, uint32_t n) {
    for (uint32_t i = 0; i < n; i++) printf("%02x ", p[i]);
}

static int is_eq(const uint8_t *got, uint32_t n, const uint8_t *want, uint32_t wn) {
    if (n != wn) return 0;
    return memcmp(got, want, wn) == 0;
}

int main(void) {
    uint8_t out[256];
    uint8_t state = 0, in_sysex = 0;

    printf("=== 1. a Note On, ts=0: header, tsLow, 0x90 0x3C 0x64 ===\n");
    {
        const uint8_t pkt[] = {0x80, 0x80, 0x90, 0x3C, 0x64};
        const uint8_t want[] = {0x90, 0x3C, 0x64};
        uint32_t n = pa_ble_midi_unframe(pkt, sizeof(pkt), out, sizeof(out), &state, &in_sysex);
        printf("    got %u bytes: ", n); hex(out, n); printf("\n");
        check(is_eq(out, n, want, sizeof(want)), "the message comes out as 90 3C 64");
    }

    printf("\n=== 2. a short SysEx: header, ts, F0 7E 7F 06 01, ts(0x80), F7 ===\n");
    {
        const uint8_t pkt[] = {0x80, 0x80, 0xF0, 0x7E, 0x7F, 0x06, 0x01, 0x80, 0xF7};
        const uint8_t want[] = {0xF0, 0x7E, 0x7F, 0x06, 0x01, 0xF7};
        uint32_t n = pa_ble_midi_unframe(pkt, sizeof(pkt), out, sizeof(out), &state, &in_sysex);
        printf("    got %u bytes: ", n); hex(out, n); printf("\n");
        check(is_eq(out, n, want, sizeof(want)),
              "the SysEx comes out as F0 7E 7F 06 01 F7");
    }

    printf("\n=== 3. a SysEx split over two packets, with a real-time byte ===\n");
    {
        const uint8_t p1[] = {0x80, 0x80, 0xF0, 0x01, 0x02, 0x80, 0xFE};
        const uint8_t p2[] = {0x80, 0x03, 0x04, 0x80, 0xF7};
        const uint8_t want[] = {0xF0, 0x01, 0x02, 0x03, 0x04, 0xF7};
        uint8_t buf[64];
        uint32_t n = 0;
        n += pa_ble_midi_unframe(p1, sizeof(p1), buf + n, sizeof(buf) - n, &state, &in_sysex);
        n += pa_ble_midi_unframe(p2, sizeof(p2), buf + n, sizeof(buf) - n, &state, &in_sysex);
        printf("    got %u bytes: ", n); hex(buf, n); printf("\n");
        /* the real-time byte is dropped rather than interleaved into the
         * SysEx - the firmware's parser is a byte stream */
        check(is_eq(buf, n, want, sizeof(want)),
              "the SysEx is reassembled without the framing bytes");
    }

    printf("\n=== 4. a SysEx that spans three packets ===\n");
    {
        const uint8_t p1[] = {0x80, 0x80, 0xF0, 0x11};
        const uint8_t p2[] = {0x80, 0x22, 0x33};
        const uint8_t p3[] = {0x80, 0x44, 0x80, 0xF7};
        const uint8_t want[] = {0xF0, 0x11, 0x22, 0x33, 0x44, 0xF7};
        uint8_t buf[64];
        uint32_t n = 0;
        n += pa_ble_midi_unframe(p1, sizeof(p1), buf + n, sizeof(buf) - n, &state, &in_sysex);
        n += pa_ble_midi_unframe(p2, sizeof(p2), buf + n, sizeof(buf) - n, &state, &in_sysex);
        n += pa_ble_midi_unframe(p3, sizeof(p3), buf + n, sizeof(buf) - n, &state, &in_sysex);
        printf("    got %u bytes: ", n); hex(buf, n); printf("\n");
        check(is_eq(buf, n, want, sizeof(want)),
              "the three packets join into one SysEx");
        check(in_sysex == 0, "the SysEx is closed");
    }

    printf("\n=== 5. a message split across packets (running status) ===\n");
    {
        const uint8_t p1[] = {0x80, 0x80, 0x90, 0x3C};
        const uint8_t p2[] = {0x80, 0x80, 0x64};
        const uint8_t want[] = {0x90, 0x3C, 0x64};
        uint8_t buf[64];
        uint32_t n = 0;
        n += pa_ble_midi_unframe(p1, sizeof(p1), buf + n, sizeof(buf) - n, &state, &in_sysex);
        n += pa_ble_midi_unframe(p2, sizeof(p2), buf + n, sizeof(buf) - n, &state, &in_sysex);
        printf("    got %u bytes: ", n); hex(buf, n); printf("\n");
        check(is_eq(buf, n, want, sizeof(want)),
              "the message continues across the packet boundary");
    }

    printf("\n=== 6. a SysEx that ends on a timestamp at the end of a packet ===\n");
    {
        const uint8_t p1[] = {0x80, 0x80, 0xF0, 0x55, 0x80};
        const uint8_t p2[] = {0x80, 0xF7};
        const uint8_t want[] = {0xF0, 0x55, 0xF7};
        uint8_t buf[64];
        uint32_t n = 0;
        n += pa_ble_midi_unframe(p1, sizeof(p1), buf + n, sizeof(buf) - n, &state, &in_sysex);
        n += pa_ble_midi_unframe(p2, sizeof(p2), buf + n, sizeof(buf) - n, &state, &in_sysex);
        printf("    got %u bytes: ", n); hex(buf, n); printf("\n");
        check(is_eq(buf, n, want, sizeof(want)),
              "the closing F7 after a timestamp is delivered");
    }

    printf("\n=== 7. a bare F7 at the end of a packet (the upstream's split) ===\n");
    {
        /* The upstream's sender splits a SysEx into packets of packetSize - 1
         * bytes, and when the remaining data is exactly that long the F7 ends
         * a packet with no timestamp before it. Inside a SysEx a 0xF7 cannot
         * be data, so it still has to end the frame. */
        const uint8_t p1[] = {0x80, 0x80, 0xF0, 0x11, 0x22};
        const uint8_t p2[] = {0x80, 0x33, 0xF7};
        const uint8_t want[] = {0xF0, 0x11, 0x22, 0x33, 0xF7};
        uint8_t buf[64];
        uint32_t n = 0;
        n += pa_ble_midi_unframe(p1, sizeof(p1), buf + n, sizeof(buf) - n, &state, &in_sysex);
        n += pa_ble_midi_unframe(p2, sizeof(p2), buf + n, sizeof(buf) - n, &state, &in_sysex);
        printf("    got %u bytes: ", n); hex(buf, n); printf("\n");
        check(is_eq(buf, n, want, sizeof(want)),
              "a bare F7 still closes the SysEx");
        check(in_sysex == 0, "the SysEx is closed");
    }

    printf("\n=== 8. a peer that was interrupted mid-SysEx starts a new one ===\n");
    {
        /* The old frame never ended (the phone went out of range), so the
         * framer is still "inside a SysEx". The new packet is
         * [header][timestamp][F0 ...] - and a SysEx's payload can never hold
         * an F0, so the framer can tell that this is a new frame.
         *
         * What matters is that the new frame's F0 is DELIVERED: the bytes of
         * the lost frame are already in the stream and cannot be taken back,
         * but the firmware's parser restarts on an F0, so the device resyncs
         * on the new frame instead of waiting for the old one to end. */
        const uint8_t p1[] = {0x80, 0x80, 0xF0, 0x11, 0x22};
        const uint8_t p2[] = {0x80, 0x80, 0xF0, 0x33, 0xF7};
        const uint8_t want[] = {0xF0, 0x11, 0x22, 0xF0, 0x33, 0xF7};
        uint8_t buf[64];
        uint32_t n = 0;
        n += pa_ble_midi_unframe(p1, sizeof(p1), buf + n, sizeof(buf) - n, &state, &in_sysex);
        n += pa_ble_midi_unframe(p2, sizeof(p2), buf + n, sizeof(buf) - n, &state, &in_sysex);
        printf("    got %u bytes: ", n); hex(buf, n); printf("\n");
        check(is_eq(buf, n, want, sizeof(want)),
              "the new frame's F0 is delivered, so the parser can resync");
        check(in_sysex == 0, "the SysEx is closed");
    }

    printf("\n=== 9. a packet that is only a header and a timestamp ===\n");
    {
        const uint8_t p1[] = {0x80, 0x80};
        const uint8_t p2[] = {0x80, 0x90, 0x3C, 0x64};
        const uint8_t want[] = {0x90, 0x3C, 0x64};
        uint8_t buf[64];
        uint32_t n = 0;
        n += pa_ble_midi_unframe(p1, sizeof(p1), buf + n, sizeof(buf) - n, &state, &in_sysex);
        n += pa_ble_midi_unframe(p2, sizeof(p2), buf + n, sizeof(buf) - n, &state, &in_sysex);
        printf("    got %u bytes: ", n); hex(buf, n); printf("\n");
        check(is_eq(buf, n, want, sizeof(want)),
              "the message continues in the next packet");
    }

    printf("\n=== 10. the TX side ===\n");
    {
        const uint8_t msg[] = {0xF0, 0x11, 0x22, 0xF7};
        const uint8_t want[] = {0x80, 0x80, 0xF0, 0x11, 0x22, 0x80, 0xF7};
        g_pkt_count = 0;
        pa_ble_midi_tx_stream(collect, msg, sizeof(msg), 23);
        uint32_t n = g_pkt_count ? g_pkt_len[0] : 0u;
        printf("    got %u bytes in %u packet(s): ", n, g_pkt_count);
        hex(g_pkt[0], n); printf("\n");
        check(g_pkt_count == 1 && is_eq(g_pkt[0], n, want, sizeof(want)),
              "the frame is 80 80 F0 11 22 80 F7");
    }

    printf("\n=== 11. a long message is split, and comes back ===\n");
    {
        uint8_t msg[64];
        for (int i = 0; i < 64; i++) msg[i] = (uint8_t)(0x20 + (i % 0x5F));
        msg[0] = 0xF0;
        msg[63] = 0xF7;
        g_pkt_count = 0;
        pa_ble_midi_tx_stream(collect, msg, sizeof(msg), 23);
        uint32_t n = 0;
        for (uint32_t i = 0; i < g_pkt_count; i++) n += g_pkt_len[i];
        printf("    a 64 byte message -> %u bytes in %u packets\n", n, g_pkt_count);

        int ok = 1;
        for (uint32_t i = 0; i < g_pkt_count; i++) {
            if (g_pkt[i][0] != 0x80u) ok = 0;
        }
        check(ok, "every packet starts with the header byte 0x80");
        check(g_pkt_count > 1, "the message is split into several packets");
        check(g_pkt[g_pkt_count - 1][g_pkt_len[g_pkt_count - 1] - 2] == 0x80u &&
              g_pkt[g_pkt_count - 1][g_pkt_len[g_pkt_count - 1] - 1] == 0xF7u,
              "the closing F7 is preceded by a timestamp byte");

        /* feed the packets back in: the message must survive the round trip */
        uint8_t st = 0, sy = 0, back[256];
        uint32_t m = 0;
        for (uint32_t i = 0; i < g_pkt_count; i++) {
            m += pa_ble_midi_unframe(g_pkt[i], g_pkt_len[i], back + m,
                                     sizeof(back) - m, &st, &sy);
        }
        printf("    round trip -> %u bytes\n", m);
        check(is_eq(back, m, msg, sizeof(msg)), "the message survives the round trip");
    }

    printf("\n%s\n", fails == 0 ? "THE BLE-MIDI FRAMING WORKS" : "SOME CHECKS FAILED");
    return fails == 0 ? 0 : 1;
}
