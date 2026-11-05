/* custom-fw/include/pa_usb_ble.h — Pocket Amp USB Composite, OTG Loopback & MIDI Pedal Controller
 *
 * Includes User Manual Page 14–15 features:
 *   1. `loopback_enabled` (default 1 = ON):
 *      - When ON (1): USB/BT music is mixed with the guitar into the USB Audio recording (EP3 IN).
 *      - When OFF (0): USB/BT music plays in headphones/output, while USB Audio recording (EP3 IN)
 *        records ONLY the isolated guitar signal for DAW sub-track recording!
 *   2. Wired/Wireless External MIDI Pedal Support (e.g. M-VAVE Chocolate Plus):
 *      - Decodes incoming MIDI Program Change (PC) & Control Change (CC) messages from USB-MIDI / BLE
 *        to switch presets, step Bank+/Bank-, toggle the 6 effect modules, or trigger HOME / PARA actions.
 */
#ifndef PA_USB_BLE_H
#define PA_USB_BLE_H

#include <stdint.h>

#define PA_USB_CON0_REG      (*(volatile uint32_t *)0x00016800u)
#define PA_USB_EP3_DMA_REG   (*(volatile uint32_t *)0x00016850u)
#define PA_USB_EP4_DMA_REG   (*(volatile uint32_t *)0x00016854u)
#define PA_USB_EP0_FIFO_ADDR 0x00016020u
#define PA_MUSB_IDX_BASE     0x0000B080u

/* The OTA reboot flag holds this exact value only when check_ota_special_sysex() saw a real
 * "enter upgrade" frame. Anything else non-zero is memory corruption: on the bench unit a
 * USB host being attached set the flag within milliseconds of boot (black box, 2026-10-30),
 * and a plain "!= 0" test then rebooted the unit into the downloader. */
#define PA_OTA_REBOOT_MAGIC   0xA5u

#define PA_USB_EP_MIDI_IN     0x84u
#define PA_USB_EP_MIDI_OUT    0x04u
#define PA_USB_MIDI_MAXP      64u
#define PA_USB_EP_AUDIO_IN    0x83u
#define PA_USB_EP_AUDIO_OUT   0x03u
#define PA_USB_AUDIO_IN_MAXP  288u
#define PA_USB_AUDIO_OUT_MAXP 288u

/* ---- MIDI control events -------------------------------------------------------------------------------------------------------------------------------------------------------
 * The USB / BLE MIDI handler (usb_ble.c) decodes Control Change messages (including the NRPN sequence) into small events and queues them here; the main tick (pa_audio_core.c) drains the queue
 * and applies each one through pa_ui_midi_control() (ui_app.c), the same code paths the front panel uses. A queue, not one slot: a controller can send several messages between two ticks. */
enum {
    PA_MEV_NONE = 0,
    PA_MEV_PRESET_VOL,     /* v = 0..100 */
    PA_MEV_PRESET_STEP,    /* a = 1 next, 0 previous (wraps P01..P80) */
    PA_MEV_TUNER,          /* v = 1 on, 0 off */
    PA_MEV_GROOVE_PLAY,    /* v = 1 play, 0 stop */
    PA_MEV_PLAY_MODE,      /* v = 1 STOMP, 0 PRESET */
    PA_MEV_SLOT_SET,       /* a = firmware slot, v = 1 on, 0 off */
    PA_MEV_KNOB,           /* a = firmware slot, b = knob 0..5, v = 0..16383 (14 bit; the receiver scales it to the knob's range) */
    PA_MEV_TYPE,           /* a = firmware slot, v = the firmware's own type id */
    PA_MEV_GLOBAL,         /* a = GLOBAL parameter (MIDI-MAP-CUSTOM.md section 6), v = the raw number D */
    PA_MEV_GROOVE,         /* a = GROOVE parameter, v = D */
    PA_MEV_PRESET_PARAM,   /* a = PRESET parameter, v = D */
    PA_MEV_SAVE            /* a = 0 save the current preset where it is, 1 save it to slot v (0..79) */
};
typedef struct { uint8_t kind; uint8_t a; uint8_t b; uint8_t pad; uint16_t v; } pa_midi_ev_t;
#define PA_MIDI_EV_N 32u

typedef struct {
    uint32_t sample_rate_hz;
    uint8_t  configured;
    uint8_t  uac_spk_streaming;
    uint8_t  uac_mic_streaming;
    uint8_t  loopback_enabled;         /* Manual Page 14-15: 1 = record guitar+backing, 0 = record dry/isolated guitar */
    uint8_t  ble_connected;
    uint8_t  ota_reboot_pending;       /* PA_OTA_REBOOT_MAGIC when a real "enter upgrade" frame arrived */
    uint8_t  ota_session_pending;      /* 1 when the 0x7F update-session frame arrived: the main loop runs pa_ota_session_run() */

    /* The BLE TX. The device's replies leave over the same link they came
     * in on, so they are framed as BLE-MIDI 1.0 packets. The GATT notify
     * callback is not in the custom firmware - the BLE stack lives in the
     * vendor SDK - so the platform registers a sink for the framed packets.
     * (The same shape as pa_sysex_set_reply_sink() on the SysEx side.) */
    void    (*ble_tx_sink)(const uint8_t *pkt, uint32_t len);
    uint32_t ble_mtu;                  /* the negotiated ATT MTU (23 = the BLE minimum) */

    /* Pending external MIDI Pedal actions (Manual Page 15: Chocolate Plus footswitch support) */
    uint32_t midi_pedal_btn_events;    /* Mapped to PA_BTN_* bitmask */
    int32_t  midi_pedal_enc_delta;     /* Mapped to PARA CCW / CW */
    int8_t   midi_pedal_preset_req;    /* -1 = none, 0..79 = direct preset jump */
    int8_t   midi_pedal_toggle_slot;   /* -1 = none, 0..7 = toggle module on/off */
    pa_midi_ev_t midi_ev[PA_MIDI_EV_N];  /* the control events decoded from CC / NRPN (pa_usb_midi_ev_pop drains them) */
    volatile uint8_t midi_ev_w, midi_ev_r;
    uint8_t  nrpn_mod, nrpn_par, nrpn_have_mod, nrpn_have_par, nrpn_hi, nrpn_have_hi;   /* the NRPN selection (CC 99 / 98) and the data entry's high half (CC 6) */

    uint8_t  midi_rx_ring[512];
    uint16_t midi_rx_wr;
    uint16_t midi_rx_rd;
} pa_usb_ble_ctx_t;

/* 1 and *out filled if an event was waiting (main loop only) */
static inline int pa_usb_midi_ev_pop(pa_usb_ble_ctx_t *ctx, pa_midi_ev_t *out) {
    if (!ctx || ctx->midi_ev_r == ctx->midi_ev_w) return 0;
    *out = ctx->midi_ev[ctx->midi_ev_r & (PA_MIDI_EV_N - 1u)];
    ctx->midi_ev_r++;
    return 1;
}

void pa_usb_ble_init(pa_usb_ble_ctx_t *ctx);
void pa_usb_ble_set_loopback(pa_usb_ble_ctx_t *ctx, uint8_t enabled);
/* Register where the BLE-MIDI framed packets go (the GATT notify). */
void pa_usb_ble_set_ble_tx_sink(pa_usb_ble_ctx_t *ctx,
                                void (*sink)(const uint8_t *pkt, uint32_t len));
/* Send a reply out over the BLE link, as BLE-MIDI 1.0 packets. It does
 * nothing when no BLE peer is connected or no sink is registered. */
void pa_usb_ble_tx_ble(pa_usb_ble_ctx_t *ctx, const uint8_t *data, uint32_t len);
void pa_usb_midi_rx_packet(pa_usb_ble_ctx_t *ctx, const uint8_t *pkt, uint32_t len);
int  pa_usb_midi_tx_bytes(pa_usb_ble_ctx_t *ctx, const uint8_t *data, uint32_t len);
/* The BLE-MIDI 1.0 framing: a peer sends [header][timestamp][message] and the
 * SysEx continuation packets carry raw data with no timestamp. pa_ble_gatt_rx_write
 * strips that framing and puts the MIDI bytes in the same ring the USB MIDI
 * path uses, so the device's control protocol is identical on both links.
 * pa_ble_midi_tx_frame does the same for the other direction. */
void pa_ble_gatt_rx_write(pa_usb_ble_ctx_t *ctx, const uint8_t *data, uint32_t len);
/* Frame a MIDI message as BLE-MIDI 1.0 packets. The packets are handed to
 * `emit` one by one as they fill, so a long reply never needs a buffer
 * bigger than one packet. `mtu` is the negotiated ATT MTU (23 = the BLE
 * minimum). */
typedef void (*pa_ble_emit_t)(const uint8_t *pkt, uint32_t len);
void pa_ble_midi_tx_stream(pa_ble_emit_t emit, const uint8_t *msg, uint32_t len,
                           uint32_t mtu);
/* Strip the BLE-MIDI 1.0 framing from one packet and hand the MIDI bytes
 * back. `state` and `in_sysex` carry over between packets (a SysEx can be
 * split across packets). */
uint32_t pa_ble_midi_unframe(const uint8_t *pkt, uint32_t len, uint8_t *out,
                             uint32_t cap, uint8_t *state, uint8_t *in_sysex);
void pa_usb_ble_poll(pa_usb_ble_ctx_t *ctx);

/* UAC1 Audio Routing with Loopback separation (Manual Page 15):
 *   - Call `pa_usb_audio_route_block` with the guitar signal (`guitar_L`/`guitar_R`):
 *     If `loopback_enabled == 0`, captures isolated guitar to USB EP3 IN *before* mixing backing audio.
 *     If `loopback_enabled == 1`, mixes USB/BT backing audio first and captures the combined mix to USB EP3 IN! */
void pa_usb_audio_route_block(pa_usb_ble_ctx_t *ctx, int32_t *guitar_L, int32_t *guitar_R, int frames);
void pa_usb_audio_mix_playback(pa_usb_ble_ctx_t *ctx, int32_t *q21_stereo_out, int frames);
void pa_usb_audio_push_capture(pa_usb_ble_ctx_t *ctx, const int32_t *q21_mono_guitar, int frames);

/* IMP-08: stereo, rate-adapted USB audio.
 *  - ISR side (44.1 kHz audio blocks): push_capture_stereo / mix_playback_stereo
 *  - USB side (1 kHz SOF): ep3_in_fill() builds one isochronous IN packet (44/45 frames at
 *    44.1 kHz, 48 at 48 kHz) from the capture ring through a fill-level-steered resampler;
 *    ep3_out_rx() stores one OUT packet into the playback ring. */
void     pa_usb_audio_push_capture_stereo(pa_usb_ble_ctx_t *ctx, const int32_t *L, const int32_t *R, int frames);
void     pa_usb_audio_mix_playback_stereo(pa_usb_ble_ctx_t *ctx, int32_t *L, int32_t *R, int frames);
uint32_t pa_usb_audio_ep3_in_fill(pa_usb_ble_ctx_t *ctx, uint8_t *pkt, uint32_t max_bytes);
void     pa_usb_audio_ep3_out_rx(pa_usb_ble_ctx_t *ctx, const uint8_t *pkt, uint32_t len);
void     pa_usb_audio_set_rate(pa_usb_ble_ctx_t *ctx, uint32_t host_rate_hz);   /* 44100 / 48000 */
void     pa_usb_audio_set_rate_ep(pa_usb_ble_ctx_t *ctx, uint8_t ep, uint32_t hz);   /* ep 0x83 recording, 0x03 playback, 0 both */
uint32_t pa_usb_audio_get_rate(uint8_t ep);
void     pa_usb_audio_ep3_missed(pa_usb_ble_ctx_t *ctx, uint32_t frames);   /* the host let this many 1 ms frames pass without taking a recording packet */
extern uint32_t g_pa_cap_dropped;
void     pa_usb_audio_capture_start(pa_usb_ble_ctx_t *ctx);     /* the host opened the recording interface: start from a clean ring */
void     pa_usb_audio_playback_start(pa_usb_ble_ctx_t *ctx);    /* the host opened the playback interface (alt 1): its OUT packets are mixed into the output */
void     pa_usb_audio_playback_stop(pa_usb_ble_ctx_t *ctx);     /* ... and closed it (alt 0) */

#endif /* PA_USB_BLE_H */
