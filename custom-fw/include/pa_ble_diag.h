/* custom-fw/include/pa_ble_diag.h - the Bluetooth link instrumentation block, shared by the firmware and the host tools (tools/ble_diag_decode.py reads exactly this layout).
 *
 * One block of plain data (no pointers), filled by the radio driver and read live over USB-MIDI (the diagnostic channel's RAM read, 7D 02) or from a download-mode readout:
 *
 *   ring     the last 256 connection events: which event, was a packet heard, was a CRC bad, the channel the hop sequence says it was on (after the remap), how far the event
 *            interrupt came from where the interval says it should (microseconds, against the previous event);
 *   seen/heard per data channel, 37 entries each: events that should have been on that channel and those where a packet arrived. Two alignments (the event counter as read, and
 *            one event earlier) because it is not known which event the counter names; the right one shows as a clear difference between channels, the wrong one as a flat table;
 *   hist     how long the radio interrupts run: IRQ 45 (event) and IRQ 29 (RX) x context (advertising, connected, connected and encrypted) x seven duration buckets;
 *   profile  the experiment switches (below), written live with the diagnostic command 7D 03.
 *
 * Profile bits (apply at the next connection unless noted):
 *   0 NO_ENC      refuse the link-layer encryption request (unsupported remote feature) and answer a pairing request with "pairing not supported"  [now]
 *   1 CHM_LATE    apply a channel map whose instant has passed instead of closing the link (on by default)                                        [now]
 *   2 CI30        reserved: ask the central for a 30 ms interval after connecting (not implemented)
 *   3 WIDEN2      receive-window widening x2                                                                                                   [next connection]
 *   4 UPD_LATE    apply a connection update whose instant has passed instead of closing the link (on by default)                                 [now]
 *   5 HW_REMAP    use the baseband's own channel remap instead of the software remap                                                              [next connection / map change] */
#ifndef PA_BLE_DIAG_H
#define PA_BLE_DIAG_H

#include <stdint.h>

#define PA_BLE_DIAG_MAGIC  0x47444C42u            /* "BLDG" */
#define PA_BLE_RING_N      256u
#define PA_BLE_HIST_BUCKETS 7u                    /* <=50, <=100, <=200, <=300, <=500, <=1000, >1000 us */

#define PA_BLE_PROF_NO_ENC    (1u << 0)
#define PA_BLE_PROF_CHM_LATE  (1u << 1)
#define PA_BLE_PROF_CI30      (1u << 2)
#define PA_BLE_PROF_WIDEN2    (1u << 3)
#define PA_BLE_PROF_UPD_LATE  (1u << 4)
#define PA_BLE_PROF_HW_REMAP  (1u << 5)
/* Pack 153: adaptive receive window management, one switch for the three parts that belong together:
 *   - widening = 2 + 2 x floor(interval x 1250 x (master SCA ppm + 500) x (latency + 1) / 10^6)
 *   - the window register keeps the connection-start value (win size x 1250 + 1250 us) after the first packet; it is not cut to the normal window
 *   - "autozoom": every event without a packet moves the next anchor 100 us earlier and opens the window by 200 us more, up to a total
 *     widening below 2 ms; the first packet heard afterwards puts the window back
 * Latched at each new connection. Ring flags: bits 4-5 record autozoom adjustments. */
#define PA_BLE_PROF_VENDOR_WIN (1u << 6)
/* Pack 152: the baseband's own channel remap is the default. Measured on the unit 2026-11-03 with the same iPhone, place and build: the software remap gave 29% valid packets (2,703 CRC-bad,
 * corruption following the 37-event hop cycle); the hardware remap gave 99.6% valid (1 CRC-bad). With Windows the hardware remap also removed every CRC-bad packet.
 * Pack 153: adaptive window is on by default for this pack; 0x32 (without bit 6) is pack 152's behaviour, switchable live with 7D 03 for the next connection. */
#define PA_BLE_PROF_DEFAULT   (PA_BLE_PROF_CHM_LATE | PA_BLE_PROF_UPD_LATE | PA_BLE_PROF_HW_REMAP | PA_BLE_PROF_VENDOR_WIN)

/* ring flags */
#define PA_BLE_EVF_HEARD   0x01u
#define PA_BLE_EVF_CRCBAD  0x02u
#define PA_BLE_EVF_DIRECT  0x04u                  /* the unmapped channel is one the map uses (no remap needed) */
#define PA_BLE_EVF_ENC     0x08u                  /* the link was encrypted */
#define PA_BLE_EVF_ZOOM_SHIFT 4u                  /* bits 4-5: what the vendor-window autozoom did after this event (pack 153) */
#define PA_BLE_ZOOM_NONE    0u
#define PA_BLE_ZOOM_WIDENED 1u                    /* no packet: the next anchor moved 100 us earlier, the window opened further */
#define PA_BLE_ZOOM_RESTORED 2u                   /* a packet after a widening: the window back to the nominal one */
#define PA_BLE_ZOOM_FAILED  3u                    /* the baseband refused the move (the vendor prints 'X') */

struct pa_ble_ev {
    uint16_t evt;                                 /* the connection event counter */
    uint8_t  flags;
    uint8_t  ch;                                  /* data channel of this event (alignment 0), after the remap */
    int16_t  dt_us;                               /* interrupt time of this event minus the previous one, minus the interval x the events between them */
    uint8_t  ch_alt;                              /* the channel with the counter taken one event earlier (alignment 1) */
    uint8_t  used;                                /* channels the map uses */
};

struct pa_ble_diag {
    uint32_t magic;                               /* 0   PA_BLE_DIAG_MAGIC */
    uint32_t size;                                /* 4   sizeof (this struct): a reader trims to it */
    uint32_t profile;                             /* 8 */
    uint32_t wr;                                  /* 12  events recorded so far (the next record goes to wr % 256) */
    uint32_t hop;                                 /* 16  hop increment of the current or last link */
    uint32_t chm_lo, chm_hi;                      /* 20, 24  the channel map in force (bits 0..36) */
    uint16_t seen0[37], heard0[37], seen1[37], heard1[37];   /* 28 */
    uint16_t hist[2][3][PA_BLE_HIST_BUCKETS];     /* 324 */
    struct pa_ble_ev ring[PA_BLE_RING_N];         /* 408 */
};

#define PA_BLE_DIAG_SIZE 2456u
_Static_assert(sizeof(struct pa_ble_ev) == 8u, "pa_ble_ev is 8 bytes");
_Static_assert(sizeof(struct pa_ble_diag) == PA_BLE_DIAG_SIZE, "pa_ble_diag layout changed: update tools/ble_diag_decode.py");

#endif
