/* SPDX-License-Identifier: GPL-3.0-only */
/* Copyright (C) 2026 The Onyx Authors (@28spawn) */
/* custom-fw/src/board/usb_ble.c — Pocket Amp USB Composite, Loopback & Chocolate Plus MIDI Pedal Driver */

#include "pa_usb_ble.h"
#include "pa_ui.h"
#include "pa_config.h"
#include "pa_sysex.h"
#include "pa_ota.h"
#include "pa_diag.h"
#include "pa_usb_dev.h"

extern void pa_sysex_input_byte(uint8_t b);
extern void plat_wdt_feed(void);

static uint8_t g_ep4_midi_tx_buf[PA_USB_MIDI_MAXP]      __attribute__((aligned(4)));
static uint8_t g_ep4_midi_rx_buf[PA_USB_MIDI_MAXP]      __attribute__((aligned(4)));
static uint8_t g_ep3_uac_rx_buf[PA_USB_AUDIO_OUT_MAXP]  __attribute__((aligned(4)));
static uint8_t g_ep3_uac_tx_buf[PA_USB_AUDIO_IN_MAXP]   __attribute__((aligned(4)));

static int32_t  g_uac_spk_ring[256];
static uint32_t g_uac_spk_wr = 0;
static uint32_t g_uac_spk_rd = 0;

static const uint8_t k_cin_byte_len[16] = {
    0, 0, 2, 3, 3, 1, 2, 3, 3, 3, 3, 3, 2, 2, 3, 1
};

static inline void midi_ring_push(pa_usb_ble_ctx_t *ctx, uint8_t b) {
    uint16_t next = (uint16_t)((ctx->midi_rx_wr + 1u) & 511u);
    if (next != ctx->midi_rx_rd) {
        ctx->midi_rx_ring[ctx->midi_rx_wr] = b;
        ctx->midi_rx_wr = next;
    }
}

/* The SysEx replies go out over the USB MIDI TX path, so the editor reads
 * them on the same port it wrote the request on. */
static pa_usb_ble_ctx_t *g_usb_ctx = 0;

/* The diagnostic channel's replies go out the same way. */
#if !defined(PA_HOST_BUILD) && !defined(PA_HOST_SIM) && !defined(PA_EMU_DLL) && !defined(__wasm__)
#include "pa_ble_radio.h"
#define PA_BLE_REPLY(d, n) pa_ble_radio_midi_tx((d), (n))   /* the same reply also goes to a BLE central that has the MIDI notifications on */
#else
#define PA_BLE_REPLY(d, n) ((void)0)
#endif

static void pa_diag_usb_reply(const uint8_t *data, uint32_t len) {
    if (!g_usb_ctx) return;
    pa_usb_midi_tx_bytes(g_usb_ctx, data, len);
    PA_BLE_REPLY(data, len);
}

/* Send a reply out over the BLE link, as BLE-MIDI 1.0 packets. This is the
 * path a reply takes when it goes back over BLE; the GATT notify itself is
 * the platform's (the BLE stack is in the vendor SDK), so the packets are
 * handed to the registered sink. */
void pa_usb_ble_tx_ble(pa_usb_ble_ctx_t *ctx, const uint8_t *data, uint32_t len) {
    if (!ctx || !data || len == 0u) return;
    if (!ctx->ble_connected || !ctx->ble_tx_sink) return;
    pa_ble_midi_tx_stream(ctx->ble_tx_sink, data, len, ctx->ble_mtu);
}

static void pa_usb_sysex_reply_sink(const uint8_t *data, uint32_t len) {
    if (!g_usb_ctx) return;
    /* Over BLE the reply leaves as BLE-MIDI 1.0 packets, framed by the same
     * layer that strips the framing on the way in. */
    pa_usb_ble_tx_ble(g_usb_ctx, data, len);
    pa_usb_midi_tx_bytes(g_usb_ctx, data, len);
    PA_BLE_REPLY(data, len);
}

void pa_usb_ble_set_ble_tx_sink(pa_usb_ble_ctx_t *ctx,
                                void (*sink)(const uint8_t *pkt, uint32_t len)) {
    if (!ctx) return;
    ctx->ble_tx_sink = sink;
}

void pa_usb_ble_init(pa_usb_ble_ctx_t *ctx) {
    if (!ctx) return;
    g_usb_ctx = ctx;
    pa_sysex_set_reply_sink(pa_usb_sysex_reply_sink);
    pa_diag_set_reply_sink(pa_diag_usb_reply);
#if !defined(PA_HOST_BUILD) && !defined(PA_HOST_SIM) && !defined(PA_EMU_DLL) && !defined(__wasm__)
    pa_diag_set_profile_hook(pa_ble_radio_profile);
#endif
    ctx->sample_rate_hz         = 44100u;
    ctx->configured             = 1u;
    ctx->uac_spk_streaming      = 0u;
    ctx->uac_mic_streaming      = 0u;
    ctx->loopback_enabled       = 1u; /* Manual Page 15: "set to the 'on' position in factory condition" */
    ctx->ble_connected          = 0u;
    ctx->ota_reboot_pending     = 0u;
    ctx->ota_session_pending    = 0u;
    ctx->ble_tx_sink            = 0;
    ctx->ble_mtu                = 23u; /* the BLE minimum ATT MTU */
    ctx->midi_pedal_btn_events  = 0u;
    ctx->midi_pedal_enc_delta   = 0;
    ctx->midi_pedal_preset_req  = -1;
    ctx->midi_pedal_toggle_slot = -1;
    ctx->midi_rx_wr             = 0u;
    ctx->midi_rx_rd             = 0u;

    /* The USB device driver (src/board/usb_dev.c): a polled MIDI device on the controller at
     * 0x11800, registers proven on this chip by the open FM-1 firmware. It replaces the old code
     * that OR-ed 0x03 into 0x00016850/0x00016854 ("EP3/EP4 DMA"), addresses that were never
     * verified and could point endpoint DMA at RAM the firmware owns. On the host builds
     * pa_usb_dev_init() is an empty stub. */
    pa_usb_dev_init(ctx);
    (void)g_ep4_midi_rx_buf;
    (void)g_ep3_uac_rx_buf;
}

void pa_usb_ble_set_loopback(pa_usb_ble_ctx_t *ctx, uint8_t enabled) {
    if (!ctx) return;
    ctx->loopback_enabled = enabled ? 1u : 0u;
}

/* Decode external MIDI Pedal (e.g. M-VAVE Chocolate Plus, Manual Page 15) PC & CC packets:
 *   - Program Change (0xC0..0xCF, pgm 0..79): jump directly to Preset P01..P80
 *   - Control Change (0xB0..0xBF):
 *       * CC 20..25: Toggle the 6 stock effect modules on/off, in the STOCK module order (FX, AMP, MOD, DLY, REV, CAB); CC 26, 27 are ours (DRIVE, EQ)
 *       * CC 64..67: Footswitch 1=HOME, Footswitch 2=PARA press, Footswitch 3=PARA CCW, Footswitch 4=PARA CW */
/* The module CCs by their number: CC 20 + n is the stock map's module n (FX, AMP, MOD, DLY, REV, CAB; MIDI-MAP-CUSTOM.md 2.2), 26 and 27 are our two extra modules. The
 * firmware's slots are numbered in a different order (DYN, DRIVE, AMP, CAB, EQ, MOD, DLY, REV), so the CC number is looked up here and never subtracted from 20: it used to be
 * (CC 21 toggled the drive, CC 22 the amp, ...), which made a stock foot controller's presets act on the wrong modules. */
static const int8_t k_cc_module_slot[8] = {
    PA_SLOT_DYNAMICS, PA_SLOT_AMP, PA_SLOT_MOD, PA_SLOT_DELAY, PA_SLOT_REVERB, PA_SLOT_CAB, PA_SLOT_DRIVE, PA_SLOT_EQ,
};


/* ---- the control decoder -------------------------------------------------------------------------------------------------------------------------------------------------------
 * Value rules: a button / toggle CC acts at value >= 64; a continuous CC (7, 102..118, the NRPN data entry) acts at every value. The NRPN sequence is the standard one: CC 99 = module,
 * CC 98 = parameter, then CC 6 (the high 7 bits) and optionally CC 38 (the low 7 bits); the event is produced by the data entry, so a controller can send the high half first and refine
 * with the low half. CC 101 / 100 = 127 (RPN null) clears the selection. The numbers are in MIDI-MAP-CUSTOM.md. */
static void midi_ev_push(pa_usb_ble_ctx_t *ctx, uint8_t kind, uint8_t a, uint8_t b, uint16_t v) {
    if ((uint8_t)(ctx->midi_ev_w - ctx->midi_ev_r) >= PA_MIDI_EV_N) return;          /* full: the newest is dropped */
    pa_midi_ev_t *e = &ctx->midi_ev[ctx->midi_ev_w & (PA_MIDI_EV_N - 1u)];
    e->kind = kind; e->a = a; e->b = b; e->pad = 0u; e->v = v;
    ctx->midi_ev_w++;                                                               /* published last */
}

/* the direct knobs, CC 102..118: (firmware slot, native knob). AMP knobs: GAIN 0, BASS 1, MID 2, TREBLE 3, PRES 4, MASTER 5 (= the stock's Level); DRIVE: DRIVE 0; DLY: TIME 0, FDBK 1, MIX 2;
 * REV: DECAY 0, LOWCUT 1, TONE 2, MIX 3; MOD: RATE 0, DEPTH 1; CAB: LEVEL 0, LOWCUT 1, HICUT 2. */
static const uint8_t k_cc_knob[17][2] = {
    { PA_SLOT_AMP, 0 }, { PA_SLOT_AMP, 5 }, { PA_SLOT_AMP, 1 }, { PA_SLOT_AMP, 2 }, { PA_SLOT_AMP, 3 }, { PA_SLOT_AMP, 4 },       /* 102..107 */
    { PA_SLOT_DRIVE, 0 }, { PA_SLOT_DELAY, 0 }, { PA_SLOT_DELAY, 1 }, { PA_SLOT_DELAY, 2 },                                       /* 108..111 */
    { PA_SLOT_REVERB, 0 }, { PA_SLOT_REVERB, 3 }, { PA_SLOT_MOD, 0 }, { PA_SLOT_MOD, 1 },                                         /* 112..115 */
    { PA_SLOT_CAB, 0 }, { PA_SLOT_CAB, 1 }, { PA_SLOT_CAB, 2 },                                                                   /* 116..118 */
};

static void nrpn_apply(pa_usb_ble_ctx_t *ctx, uint16_t d, int fine) {
    const uint8_t mod = ctx->nrpn_mod, par = ctx->nrpn_par;
    if (mod <= 7u) {                                       /* an effect module */
        const uint8_t slot = (uint8_t)k_cc_module_slot[mod];
        if (par <= 5u)                    midi_ev_push(ctx, PA_MEV_KNOB, slot, par, (uint16_t)(fine ? d : (uint16_t)(d << 7)));       /* the stock knob p0..p5: same index as the firmware's native knob (see the map) */
        else if (par >= 0x60u && par <= 0x65u) midi_ev_push(ctx, PA_MEV_KNOB, slot, (uint8_t)(par - 0x60u), (uint16_t)(fine ? d : (uint16_t)(d << 7)));
        else if (par == 0x40u)            midi_ev_push(ctx, PA_MEV_TYPE, slot, 0u, (uint16_t)(fine ? (d >> 7) : d));              /* >= 0x20: the firmware's own id + 0x20 */
        else if (par == 0x41u)            midi_ev_push(ctx, PA_MEV_SLOT_SET, slot, 0u, (uint16_t)((fine ? (d >> 7) : d) >= 64u));
    } else if (mod == 8u)  midi_ev_push(ctx, PA_MEV_GLOBAL, par, 0u, (uint16_t)(fine ? d : d));
    else if (mod == 9u)    midi_ev_push(ctx, PA_MEV_GROOVE, par, 0u, d);
    else if (mod == 10u)   midi_ev_push(ctx, PA_MEV_PRESET_PARAM, par, 0u, d);
}

static void handle_midi_cc(pa_usb_ble_ctx_t *ctx, uint8_t d1, uint8_t d2) {
    if (d1 == 99u)  { ctx->nrpn_mod = d2; ctx->nrpn_have_mod = 1u; ctx->nrpn_have_par = 0u; ctx->nrpn_have_hi = 0u; return; }
    if (d1 == 98u)  { ctx->nrpn_par = d2; ctx->nrpn_have_par = 1u; ctx->nrpn_have_hi = 0u; return; }
    if (d1 == 101u || d1 == 100u) { if (d2 == 127u) { ctx->nrpn_have_mod = 0u; ctx->nrpn_have_par = 0u; ctx->nrpn_have_hi = 0u; } return; }      /* RPN null clears; other RPNs are not ours */
    if (d1 == 6u || d1 == 38u) {
        if (!ctx->nrpn_have_mod || !ctx->nrpn_have_par) return;                     /* a data entry with nothing selected: not ours */
        if (d1 == 6u) {                                                             /* the high half (or the whole value, if the low half never follows) */
            ctx->nrpn_hi = d2; ctx->nrpn_have_hi = 1u;
            nrpn_apply(ctx, d2, 0);
        } else if (ctx->nrpn_have_hi) {                                             /* the low half: the full 14-bit value */
            nrpn_apply(ctx, (uint16_t)(((uint16_t)ctx->nrpn_hi << 7) | d2), 1);
        }
        return;
    }
    if (d1 == 7u) { midi_ev_push(ctx, PA_MEV_PRESET_VOL, 0u, 0u, (uint16_t)(((uint32_t)d2 * 100u + 63u) / 127u)); return; }
    if (d1 >= 102u && d1 <= 118u) {
        const uint8_t *k = k_cc_knob[d1 - 102u];
        midi_ev_push(ctx, PA_MEV_KNOB, k[0], k[1], (uint16_t)((uint32_t)d2 * 16383u / 127u));
        return;
    }
    if (d2 < 64u) return;                                                           /* every other CC is a button: it acts at >= 64 */
    if (d1 >= 28u && d1 <= 35u) { midi_ev_push(ctx, PA_MEV_SLOT_SET, (uint8_t)k_cc_module_slot[d1 - 28u], 0u, 1u); return; }
    if (d1 == 80u || d1 == 81u) { midi_ev_push(ctx, PA_MEV_PRESET_STEP, (uint8_t)(d1 == 80u), 0u, 0u); return; }
    if (d1 == 85u) { midi_ev_push(ctx, PA_MEV_TUNER, 0u, 0u, 1u); return; }
    if (d1 == 86u) { midi_ev_push(ctx, PA_MEV_GROOVE_PLAY, 0u, 0u, 1u); return; }
    if (d1 == 87u) { midi_ev_push(ctx, PA_MEV_PLAY_MODE, 0u, 0u, 1u); return; }
    if (d1 == 88u) { midi_ev_push(ctx, PA_MEV_SAVE, 0u, 0u, 0u); return; }
}
/* the "set off" half of the toggles that have an on / off value: CC 28..35 with a value < 64 switch the module off, CC 85 / 86 / 87 with a value < 64 switch the function off */
static void handle_midi_cc_off(pa_usb_ble_ctx_t *ctx, uint8_t d1, uint8_t d2) {
    if (d2 >= 64u) return;
    if (d1 >= 28u && d1 <= 35u) midi_ev_push(ctx, PA_MEV_SLOT_SET, (uint8_t)k_cc_module_slot[d1 - 28u], 0u, 0u);
    else if (d1 == 85u) midi_ev_push(ctx, PA_MEV_TUNER, 0u, 0u, 0u);
    else if (d1 == 86u) midi_ev_push(ctx, PA_MEV_GROOVE_PLAY, 0u, 0u, 0u);
    else if (d1 == 87u) midi_ev_push(ctx, PA_MEV_PLAY_MODE, 0u, 0u, 0u);
}

static void handle_midi_pedal_event(pa_usb_ble_ctx_t *ctx, uint8_t status, uint8_t d1, uint8_t d2) {
    uint8_t type = status & 0xF0u;
    if (type == 0xB0u) { handle_midi_cc(ctx, d1, d2); handle_midi_cc_off(ctx, d1, d2); }      /* the controls of MIDI-MAP-CUSTOM.md; the older ones below stay as they were */
    if (type == 0xC0u && d1 < 80u) {
        ctx->midi_pedal_preset_req = (int8_t)d1;
    } else if (type == 0xB0u && d2 >= 64u) {
        if (d1 >= 20u && d1 <= 27u) {
            ctx->midi_pedal_toggle_slot = k_cc_module_slot[d1 - 20u];
        } else if (d1 == 93u || d1 == 94u) {
            pa_usb_dev_set_timer_prio(d1 == 93u ? 7u : 5u);        /* BENCH: CC 93 = the timer interrupt above the audio interrupt's priority by 3, CC 94 = 5 (the default) */
        } else if (d1 == 90u || d1 == 91u || d1 == 92u) {
            pa_usb_dev_set_service_mode(d1 == 91u ? 0u : (d1 == 90u ? 1u : 2u));       /* BENCH: CC 91 = USB service from the main loop only, 90 = the timer interrupt also (2 kHz), 92 = also (4 kHz, the default) */
        } else if (d1 == 64u) {
            ctx->midi_pedal_btn_events |= PA_BTN_HOME;
        } else if (d1 == 65u) {
            ctx->midi_pedal_btn_events |= PA_BTN_PARA_CLICK;
        } else if (d1 == 66u) {
            ctx->midi_pedal_enc_delta -= 1;
        } else if (d1 == 67u) {
            ctx->midi_pedal_enc_delta += 1;
        }
    }
}

static void check_ota_usb_byte(pa_usb_ble_ctx_t *ctx, uint8_t b);

void pa_usb_midi_rx_packet(pa_usb_ble_ctx_t *ctx, const uint8_t *pkt, uint32_t len) {
    if (!ctx || !pkt) return;
    for (uint32_t off = 0; off + 4u <= len; off += 4u) {
        uint8_t cin = pkt[off] & 0x0Fu;
        if (cin == 0u) break;
        if (cin == 0x0Bu || cin == 0x0Cu) {
            handle_midi_pedal_event(ctx, pkt[off + 1u], pkt[off + 2u], pkt[off + 3u]);
        }
        uint8_t nbytes = k_cin_byte_len[cin];
        for (uint8_t i = 0; i < nbytes; i++) {
            uint8_t b = pkt[off + 1u + i];
            check_ota_usb_byte(ctx, b);
            midi_ring_push(ctx, b);
        }
    }
}

int pa_usb_midi_tx_bytes(pa_usb_ble_ctx_t *ctx, const uint8_t *data, uint32_t len) {
    if (!ctx || !data || len == 0u) return 0;
    uint32_t idx = 0;
    while (idx < len) {
        uint32_t pkt_off = 0;
        while (idx < len && pkt_off + 4u <= PA_USB_MIDI_MAXP) {
            uint32_t rem = len - idx;
            uint8_t cin, b0 = 0, b1 = 0, b2 = 0;
            if (data[idx] == 0xF7u) {
                cin = 0x05u; b0 = data[idx++];
            } else if (rem >= 2u && data[idx + 1u] == 0xF7u) {
                cin = 0x06u; b0 = data[idx++]; b1 = data[idx++];
            } else if (rem >= 3u && data[idx + 2u] == 0xF7u) {
                cin = 0x07u; b0 = data[idx++]; b1 = data[idx++]; b2 = data[idx++];
            } else if (rem >= 3u) {
                cin = 0x04u; b0 = data[idx++]; b1 = data[idx++]; b2 = data[idx++];
            } else if (rem == 2u) {
                cin = 0x04u; b0 = data[idx++]; b1 = data[idx++];
            } else {
                cin = 0x04u; b0 = data[idx++];
            }
            g_ep4_midi_tx_buf[pkt_off + 0u] = cin;
            g_ep4_midi_tx_buf[pkt_off + 1u] = b0;
            g_ep4_midi_tx_buf[pkt_off + 2u] = b1;
            g_ep4_midi_tx_buf[pkt_off + 3u] = b2;
            pkt_off += 4u;
        }
        /* Queue the packets for the USB driver, which sends them from its poll (a no-op on the host builds). */
        if (pa_usb_dev_tx(g_ep4_midi_tx_buf, pkt_off) < 0) {
            return -1; /* the queue is full: nobody is reading */
        }
    }
    return 0;
}

/* The app side of the JieLi OTA handshake: the query is answered with the device's ID
 * block, and the "enter upgrade" frame asks the platform to reboot into the downloader
 * (the downloader then pulls the image from the host - the app never sees it).
 * See include/pa_ota.h. */
static void check_ota_special_sysex(pa_usb_ble_ctx_t *ctx, uint8_t *frame, uint32_t len) {
    pa_ota_frame_t f = pa_ota_classify(frame, len);
    if (f == PA_OTA_FRAME_QUERY) {
        uint8_t reply[PA_OTA_ID_REPLY_LEN];
        uint32_t n = pa_ota_build_id_reply(reply, sizeof(reply));
        if (n) { pa_usb_midi_tx_bytes(ctx, reply, n); PA_BLE_REPLY(reply, n); }      /* USB and BLE: it used to answer on USB only, so a BLE central never got it */
    } else if (f == PA_OTA_FRAME_SOFTKEY) {
        ctx->ota_reboot_pending = PA_OTA_REBOOT_MAGIC;                   /* the ROM download mode (the PC flasher's way in) */
    } else if (f == PA_OTA_FRAME_UPGRADE) {
        ctx->ota_session_pending = 1u;                                   /* the vendor's MIDI update session (the web editor's one-click install / restore) */
    }
}

/* ---- the BLE-MIDI 1.0 framing --------------------------------------------
 *
 * A BLE-MIDI peer sends [header][timestamp][message], and a SysEx that
 * spans packets is sent as continuation packets with raw data and no
 * timestamp. The framing is stripped here so the MIDI bytes land in the
 * same ring the USB MIDI path uses, and the device's control protocol is
 * identical on both links.
 *
 * The rules and the framer itself are in src/board/ble_midi.c
 * (standard BLE-MIDI 1.0). */
void pa_ble_gatt_rx_write(pa_usb_ble_ctx_t *ctx, const uint8_t *data, uint32_t len) {
    if (!ctx || !data) return;
    ctx->ble_connected = 1u;
    if (len < 2u) return;

    static uint8_t s_state = 0u;
    static uint8_t s_in_sysex = 0u;
    uint8_t buf[256];
    uint32_t n = pa_ble_midi_unframe(data, len, buf, sizeof(buf), &s_state, &s_in_sysex);
    for (uint32_t i = 0; i < n; i++) midi_ring_push(ctx, buf[i]);
}

static void check_ota_usb_byte(pa_usb_ble_ctx_t *ctx, uint8_t b) {
    static uint8_t  s_ota_buf[16];
    static uint32_t s_ota_len = 0;
    if (b == 0xF0u) s_ota_len = 0;
    if (s_ota_len < sizeof(s_ota_buf)) s_ota_buf[s_ota_len++] = b;
    if (b == 0xF7u) {
        check_ota_special_sysex(ctx, s_ota_buf, s_ota_len);
        s_ota_len = 0;
    }
}

/* the update session (ota_hw.c) takes the received bytes and sends its messages itself, bypassing the normal parser */
int pa_usb_ble_rx_pop(uint8_t *b) {
    pa_usb_ble_ctx_t *ctx = g_usb_ctx;
    if (!ctx || ctx->midi_rx_rd == ctx->midi_rx_wr) return 0;
    *b = ctx->midi_rx_ring[ctx->midi_rx_rd];
    ctx->midi_rx_rd = (uint16_t)((ctx->midi_rx_rd + 1u) & 511u);
    return 1;
}
int pa_usb_ble_tx_raw(const uint8_t *data, uint32_t len) { return g_usb_ctx ? pa_usb_midi_tx_bytes(g_usb_ctx, data, len) : -1; }

void pa_usb_ble_poll(pa_usb_ble_ctx_t *ctx) {
    if (!ctx) return;

    while (ctx->midi_rx_rd != ctx->midi_rx_wr) {
        uint8_t b = ctx->midi_rx_ring[ctx->midi_rx_rd];
        ctx->midi_rx_rd = (uint16_t)((ctx->midi_rx_rd + 1u) & 511u);
        pa_diag_feed(b);            /* the private diagnostic channel (a flash read) */
        pa_sysex_input_byte(b);
    }
}

void pa_usb_audio_mix_playback(pa_usb_ble_ctx_t *ctx, int32_t *q21_stereo_out, int frames) {
    if (!ctx || !ctx->uac_spk_streaming || !q21_stereo_out) return;
    for (int i = 0; i < frames; i++) {
        if (g_uac_spk_rd != g_uac_spk_wr) {
            q21_stereo_out[i] += g_uac_spk_ring[g_uac_spk_rd & 255u];
            g_uac_spk_rd++;
        }
    }
}

void pa_usb_audio_push_capture(pa_usb_ble_ctx_t *ctx, const int32_t *q21_mono_guitar, int frames) {
    if (!ctx || !q21_mono_guitar) return;
    uint32_t off = 0;
    for (int i = 0; i < frames && off + 6u <= PA_USB_AUDIO_IN_MAXP; i++) {
        int32_t s24 = q21_mono_guitar[i] << 2;
        if (s24 >  8388607) s24 =  8388607;
        if (s24 < -8388608) s24 = -8388608;
        g_ep3_uac_tx_buf[off + 0u] = (uint8_t)(s24 & 0xFF);
        g_ep3_uac_tx_buf[off + 1u] = (uint8_t)((s24 >> 8) & 0xFF);
        g_ep3_uac_tx_buf[off + 2u] = (uint8_t)((s24 >> 16) & 0xFF);
        g_ep3_uac_tx_buf[off + 3u] = (uint8_t)(s24 & 0xFF);
        g_ep3_uac_tx_buf[off + 4u] = (uint8_t)((s24 >> 8) & 0xFF);
        g_ep3_uac_tx_buf[off + 5u] = (uint8_t)((s24 >> 16) & 0xFF);
        off += 6u;
    }
}

/* Manual Page 15 Loopback Routing:
 *   - If `loopback_enabled == 0`, records ONLY the instrument sound to USB EP3 IN before mixing USB/BT music.
 *   - If `loopback_enabled == 1`, mixes USB/BT music first so backing track + guitar are recorded together. */
void pa_usb_audio_route_block(pa_usb_ble_ctx_t *ctx, int32_t *guitar_L, int32_t *guitar_R, int frames) {
    if (!ctx || !guitar_L) return;
    if (!ctx->loopback_enabled) {
        pa_usb_audio_push_capture(ctx, guitar_L, frames);
        pa_usb_audio_mix_playback(ctx, guitar_L, frames);
        if (guitar_R) pa_usb_audio_mix_playback(ctx, guitar_R, frames);
    } else {
        pa_usb_audio_mix_playback(ctx, guitar_L, frames);
        if (guitar_R) pa_usb_audio_mix_playback(ctx, guitar_R, frames);
        pa_usb_audio_push_capture(ctx, guitar_L, frames);
    }
}


/* ===========================================================================================
 * USB audio: capture (unit -> host, the recording) and playback (host -> unit), both rate-adapted.
 *
 * The codec runs at ~44,118 Hz, the host at its own 44,100 Hz (or 48,000 Hz), so each direction goes through a ring and a fractional read pointer whose step is nudged by the ring's fill (a few
 * ppm, slowly: the ring's fill jumps by a whole 64-frame block each time the audio interrupt delivers one, and following it quickly modulated the pitch).
 *   capture : the audio interrupt writes the amp's output (push_capture_stereo, every 64 frames); the USB service reads it once per 1 ms host frame (ep3_in_fill), 44 or 45 frames.
 *   playback: the USB service writes each 1 ms OUT packet (ep3_out_rx); the audio interrupt reads it per 64-frame block (mix_playback_stereo) and ADDS it to the output, so the host's audio
 *             is heard through the unit's output stage, and (it is mixed before the capture tap) in the recording too (the "loopback").
 * Pack 164: the rings are 512 frames (4 KB each, half of before; the RAM region had 12 bytes to spare), both start only once they hold their target fill (priming), a starved ring fades its
 * last frame out and holds instead of jumping the read position back (that repeated ~190 frames: a click), and the playback path exists (before, nothing ever called ep3_out_rx).
 * Tests: test_usb_capture_tone.c, test_usb_playback_tone.c, test_usb_stereo.c.
 * =========================================================================================== */
#ifndef PA_CAP_GAIN_X8
#define PA_CAP_GAIN_X8 2           /* the read step's correction per frame of ring-fill error, in 1/8 of a Q16 unit: 0.25 (time constant ~6 s). The original 4 followed the fill within a block
                                    * and wobbled the pitch at the block rate (a 1 kHz tone: 41 dB signal-to-noise against 59 dB at 0.25). */
#endif
#ifndef PA_CAP_DEADBAND
#define PA_CAP_DEADBAND 96         /* frames of fill error the far gain ignores: above the 64-frame swing one audio block causes, so it only sees sustained drift */
#endif
#ifndef PA_CAP_FAR_X8
#define PA_CAP_FAR_X8   16         /* the correction per frame beyond the dead band, in 1/8 Q16 units (see PA_CAP_GAIN_X8) */
#endif
#define CAP_RING   512u                           /* stereo frames, power of two (was 1024: the RAM region has no room, and the extra fill was only latency) */
#ifndef PA_CAP_TARGET
#define PA_CAP_TARGET 192u                        /* the fill the capture reader steers to and starts at (frames): 192 = 4.4 ms; the producer writes 64 frames per block */
#endif
#define PB_RING    512u
#if defined(PA_HOST_BUILD) || defined(PA_HOST_SIM) || defined(PA_EMU_DLL) || defined(__wasm__) || defined(__EMSCRIPTEN__)
#ifndef PA_PB_TARGET
#define PA_PB_TARGET 320u
#endif
#ifndef PA_PB_REPRIME
#define PA_PB_REPRIME 416u
#endif
#else
#ifndef PA_PB_TARGET
#define PA_PB_TARGET 416u                         /* EXPERIMENTAL DIAGNOSTIC: increased target occupancy by +2.0 ms (+96 frames) */
#endif
#ifndef PA_PB_REPRIME
#define PA_PB_REPRIME 448u
#endif
#endif

#if defined(PA_HOST_BUILD) || defined(PA_HOST_SIM) || defined(PA_EMU_DLL) || defined(__wasm__) || defined(__EMSCRIPTEN__)
#define PB_RING_ATTR
#define NOINIT_ATTR
__attribute__((weak)) uint32_t plat_ticks24(void) { return 0; }
uint32_t g_ep3_rx_pkts = 0;
uint32_t g_ep3_rx_last_bytes = 0;
uint32_t g_ep3_rx_last_tick = 0;
uint32_t g_usb_last_sof = 0;
uint32_t g_sie_lock_max_ticks = 0;
uint32_t g_irq_off_max_ticks = 0;
#else
#define PB_RING_ATTR
#define NOINIT_ATTR __attribute__((section(".noinit")))
extern uint32_t plat_ticks24(void);
extern volatile uint32_t g_ep3_rx_pkts;
extern volatile uint32_t g_ep3_rx_last_bytes;
extern volatile uint32_t g_ep3_rx_last_tick;
extern volatile uint32_t g_usb_last_sof;
extern volatile uint32_t g_sie_lock_max_ticks;
extern volatile uint32_t g_irq_off_max_ticks;
#endif

#define PA_STARVE_LOG_CAP 16u

typedef struct {
    int32_t  fill_before;
    uint32_t need;
    uint32_t rd_i;
    uint32_t wr;
    uint32_t ep3_rx_pkts;
    uint32_t ep3_rx_bytes;
    uint32_t usb_sof;
    uint32_t t_last_rx;
    uint32_t t_block;
    uint32_t sie_lock_max_us;
    uint32_t irq_off_max_us;
} pa_pb_starve_entry_t;

static pa_pb_starve_entry_t s_starve_log[PA_STARVE_LOG_CAP] NOINIT_ATTR;
static volatile uint32_t s_starve_log_count = 0;

void *pa_usb_audio_get_starve_log(void) { return (void *)s_starve_log; }
uint32_t pa_usb_audio_get_starve_count(void) { return s_starve_log_count; }
/* The host's audio is mixed into the analog output only while it is audio: Windows keeps the playback device open and streams zeros or a few LSBs of dither when nothing plays, and that was added to the
 * output stage too. A gate on the host's own level: opens at 24-bit peaks of 512 (-84 dBFS), stays open 0.5 s after the last one, closes and opens through a short ramp. */
#define PA_PB_GATE_OPEN   512
#define PA_PB_GATE_HOLD   22050u
/* instrumentation (usb_dev.c copies them into its counters) */
uint32_t g_pa_cap_recentres = 0, g_pa_cap_fill_min = 0xFFFFFFFFu, g_pa_cap_fill_max = 0;
uint32_t g_pa_cap_starved = 0;                    /* packets sent as faded silence because the ring was starved or still priming */
uint32_t g_pa_pb_packets = 0, g_pa_pb_starved = 0, g_pa_pb_recentres = 0, g_pa_pb_fill_min = 0xFFFFFFFFu, g_pa_pb_fill_max = 0;

static int32_t  s_cap_L[CAP_RING], s_cap_R[CAP_RING];
static volatile uint32_t s_cap_wr = 0;
/* The read position is the whole-frame counter and a Q16 fraction, kept apart: as ONE Q16 word its whole-frame part had only 16 bits, so against the write counter (32 bits) it was
 * wrong after 65,536 frames (~1.5 s of audio): every packet then re-centred (pack 161: robotic recordings; test_usb_capture_tone.c). */
static uint32_t s_cap_rd_i = 0, s_cap_rd_f = 0;
static uint8_t  s_cap_primed = 0;                 /* 0: silent until the ring holds the target fill of NEW frames (written since the stream started / the ring ran dry) */
static uint32_t s_cap_hold_wr = 0;
static int32_t  s_cap_last_l = 0, s_cap_last_r = 0;
static int32_t  s_pb_L[PB_RING] PB_RING_ATTR, s_pb_R[PB_RING] PB_RING_ATTR;
static volatile uint32_t s_pb_wr = 0;
static uint32_t s_pb_rd_i = 0, s_pb_rd_f = 0;
static uint8_t  s_pb_primed = 0;
static uint32_t s_pb_hold_wr = 0;
static uint32_t s_pb_need = PA_PB_TARGET;         /* new frames to buffer before the output starts: PA_PB_TARGET at the start of a stream, PA_PB_REPRIME after a starvation */
static volatile int32_t s_pb_peak = 0;            /* the largest 24-bit sample of the packets since the mixer last looked (written by the USB service, cleared by the audio interrupt) */
static uint32_t s_pb_gate_hold = 0;
static int32_t  s_pb_gate_g = 0;                  /* the gate's gain, Q15 */
static volatile uint8_t s_pb_on = 0;              /* the host has the playback interface open (alt 1) */
static int32_t  s_pb_last_l = 0, s_pb_last_r = 0;
static uint32_t s_pb_starve_consec = 0;
static uint8_t  s_pb_concealing = 0;
static uint32_t s_cap_rate = 44100u, s_pb_rate = 44100u, s_frac_acc = 0;   /* the host's rate per direction: Windows sets it per endpoint */

/* ep: 0x83 = recording, 0x03 = playback, 0 = both (the Settings row). A rate of 46 kHz or more is 48 kHz. */
void pa_usb_audio_set_rate_ep(pa_usb_ble_ctx_t *ctx, uint8_t ep, uint32_t hz) {
    hz = (hz >= 46000u) ? 48000u : 44100u;
    if (ep != 0x03u) { s_cap_rate = hz; s_cap_primed = 0u; s_cap_hold_wr = s_cap_wr; s_frac_acc = 0u; }
    if (ep != 0x83u && hz != s_pb_rate) { s_pb_rate = hz; s_pb_primed = 0u; s_pb_hold_wr = s_pb_wr; s_pb_need = PA_PB_TARGET; }
    if (ctx) ctx->sample_rate_hz = hz;
}
void pa_usb_audio_set_rate(pa_usb_ble_ctx_t *ctx, uint32_t host_rate_hz) { pa_usb_audio_set_rate_ep(ctx, 0u, host_rate_hz); }
uint32_t pa_usb_audio_get_rate(uint8_t ep) { return (ep == 0x03u) ? s_pb_rate : s_cap_rate; }

/* The host opened (alt 1) or closed (alt 0) the recording interface / the playback interface: start from a clean, unprimed ring. */
void pa_usb_audio_capture_start(pa_usb_ble_ctx_t *ctx) { (void)ctx; s_cap_primed = 0u; s_cap_hold_wr = s_cap_wr; s_frac_acc = 0u; }
void pa_usb_audio_playback_start(pa_usb_ble_ctx_t *ctx) {
    s_pb_primed = 0u; s_pb_wr = 0u; s_pb_hold_wr = 0u; s_pb_rd_i = 0u; s_pb_rd_f = 0u; s_pb_on = 1u;
    s_pb_need = PA_PB_TARGET; s_pb_peak = 0; s_pb_gate_hold = 0u; s_pb_gate_g = 0;
    s_pb_starve_consec = 0u; s_pb_concealing = 0u; s_pb_last_l = 0; s_pb_last_r = 0;
    if (ctx) ctx->uac_spk_streaming = 1u;
}
void pa_usb_audio_playback_stop(pa_usb_ble_ctx_t *ctx) {
    s_pb_on = 0u; s_pb_primed = 0u;
    s_pb_starve_consec = 0u; s_pb_concealing = 0u; s_pb_last_l = 0; s_pb_last_r = 0;
    if (ctx) ctx->uac_spk_streaming = 0u;
}

void pa_usb_audio_push_capture_stereo(pa_usb_ble_ctx_t *ctx, const int32_t *L, const int32_t *R, int frames) {
    if (!ctx || !L) return;
    for (int i = 0; i < frames; i++) {
        s_cap_L[s_cap_wr & (CAP_RING - 1u)] = L[i];
        s_cap_R[s_cap_wr & (CAP_RING - 1u)] = R ? R[i] : L[i];
        s_cap_wr++;
    }
}

static inline void put24(uint8_t *p, int32_t q21) {
    /* Clamp first, then scale (Q21 1.0 -> 2^23). Shifted first, a sample of 2^29 or more wrapped to the other sign before the clamp saw it (a full-scale click where it should clip),
     * and a left shift of a negative value is undefined in C. */
    if (q21 >  2097151) q21 =  2097151;
    if (q21 < -2097152) q21 = -2097152;
    const int32_t s24 = q21 * 4;
    p[0] = (uint8_t)s24; p[1] = (uint8_t)(s24 >> 8); p[2] = (uint8_t)(s24 >> 16);
}

/* Catmull-Rom cubic between ring[ip] and ring[ip + 1] at the Q16 fraction fr, integer only (this runs in an interrupt: no floating point, whose registers the interrupt wrapper
 * does not save). Linear interpolation capped a 1 kHz tone at ~50 dB signal-to-error; the cubic is ~30 dB better. `mask` = ring size - 1. */
static inline int32_t cubic_q21(const int32_t *ring, uint32_t mask, uint32_t ip, uint32_t fr) {
    const int64_t p0 = ring[(ip - 1u) & mask], p1 = ring[ip & mask];
    const int64_t p2 = ring[(ip + 1u) & mask], p3 = ring[(ip + 2u) & mask];
    const int64_t c1 = p2 - p0, c2 = 2 * p0 - 5 * p1 + 4 * p2 - p3, c3 = (p3 - p0) + 3 * (p1 - p2);      /* all times 2 */
    int64_t y = (c3 * (int64_t)fr) >> 16;
    y = ((y + c2) * (int64_t)fr) >> 16;
    y = ((y + c1) * (int64_t)fr) >> 17;
    return (int32_t)(p1 + y);
}

#define FADE_IN_FRAMES 32u

uint32_t g_pa_cap_dropped = 0;                    /* host frames whose audio was skipped in the ring (see pa_usb_audio_ep3_missed) */

/* The host let `frames` 1 ms frames go by without taking a packet. Its recording has a gap there, and the unit kept filling the ring: left alone the fill climbs by a packet's worth (~44 frames) per
 * miss, the slow rate control cannot drain it at 5 misses a second, and the ring ends in a ~270-frame skip. So the read position moves on by the same amount, as if those packets had been sent
 * and lost; never below a fill of PA_CAP_TARGET - 64 (it does not starve the ring to save latency). */
void pa_usb_audio_ep3_missed(pa_usb_ble_ctx_t *ctx, uint32_t frames) {
    (void)ctx;
    if (!s_cap_primed || !frames) return;
    if (frames > 4u) frames = 4u;
    const uint32_t adv = (uint32_t)(((uint64_t)PA_CODEC_RATE_HZ * 65536u * frames) / 1000u) + s_cap_rd_f;
    const int32_t fill_after = (int32_t)(s_cap_wr - (s_cap_rd_i + (adv >> 16)));
    if (fill_after < (int32_t)PA_CAP_TARGET - 64) return;
    s_cap_rd_i += adv >> 16; s_cap_rd_f = adv & 0xFFFFu;
    g_pa_cap_dropped += frames;
}

/* One 1 ms host frame of the recording: 44 or 45 frames (48 at 48 kHz) of 2 x 24 bit, ALWAYS a full packet. A ring that is still priming or has run dry gives faded silence and the read
 * position stays where it is (the audio interrupt keeps filling the ring); when it holds the target again the stream fades back in. */
uint32_t pa_usb_audio_ep3_in_fill(pa_usb_ble_ctx_t *ctx, uint8_t *pkt, uint32_t max_bytes) {
    if (!ctx || !pkt) return 0;
    /* frames this ms: 44.1 kHz -> 44,44,...,45 (every 10th), 48 kHz -> 48 */
    uint32_t n = s_cap_rate / 1000u;
    s_frac_acc += s_cap_rate % 1000u;
    if (s_frac_acc >= 1000u) { s_frac_acc -= 1000u; n++; }
    if (n * 6u > max_bytes) n = max_bytes / 6u;

    int32_t fill = (int32_t)(s_cap_wr - s_cap_rd_i);
    if (s_cap_primed) {
        if (fill < 0 || (uint32_t)fill < g_pa_cap_fill_min) g_pa_cap_fill_min = fill < 0 ? 0u : (uint32_t)fill;
        if ((uint32_t)fill > g_pa_cap_fill_max && fill < (int32_t)CAP_RING) g_pa_cap_fill_max = (uint32_t)fill;
    }
    uint32_t fade_in = 0u;
    if (!s_cap_primed) {
        if ((uint32_t)(s_cap_wr - s_cap_hold_wr) >= PA_CAP_TARGET) {      /* the audio interrupt has delivered a target's worth of NEW frames: start, centred, fading in (never from old ones: that repeated them) */
            s_cap_rd_i = s_cap_wr - PA_CAP_TARGET; s_cap_rd_f = 0u; s_cap_primed = 1u; fade_in = FADE_IN_FRAMES; fill = (int32_t)PA_CAP_TARGET;
        }
    } else if (fill < (int32_t)n + 8) {                 /* starved (the packet would read past what the audio interrupt has written): hold, fade out, re-prime (no jump back: that repeated ~190 frames) */
        s_cap_primed = 0u; s_cap_hold_wr = s_cap_wr; g_pa_cap_recentres++;
    } else if (fill > (int32_t)CAP_RING - 48) {         /* overflowing: skip forward to the target (drops ~fill - target frames) */
        s_cap_rd_i = s_cap_wr - PA_CAP_TARGET; s_cap_rd_f = 0u; fill = (int32_t)PA_CAP_TARGET; g_pa_cap_recentres++;
    }
    if (!s_cap_primed) {                                /* faded silence of the last frame, then zeros */
        g_pa_cap_starved++;
        for (uint32_t i = 0; i < n; i++) {
            const int32_t g = (int32_t)(n - i);        /* n .. 1 */
            put24(&pkt[i * 6u], (int32_t)(((int64_t)s_cap_last_l * g) / (int32_t)n));
            put24(&pkt[i * 6u + 3u], (int32_t)(((int64_t)s_cap_last_r * g) / (int32_t)n));
        }
        s_cap_last_l = 0; s_cap_last_r = 0;
        return n * 6u;
    }
    uint32_t step_q16 = (uint32_t)(((uint64_t)PA_CODEC_RATE_HZ << 16) / s_cap_rate);
    {   /* gentle P-control near the target; beyond the dead band (sustained drift, e.g. after missed packets) a stronger one, clamped to +-0.3 % (5 cents) */
        const int32_t err = fill - (int32_t)PA_CAP_TARGET;
        int32_t corr = (err * (int32_t)PA_CAP_GAIN_X8) >> 3;
        if (err >  (int32_t)PA_CAP_DEADBAND) corr += ((err - (int32_t)PA_CAP_DEADBAND) * (int32_t)PA_CAP_FAR_X8) >> 3;
        if (err < -(int32_t)PA_CAP_DEADBAND) corr += ((err + (int32_t)PA_CAP_DEADBAND) * (int32_t)PA_CAP_FAR_X8) >> 3;
        if (corr >  200) corr =  200;
        if (corr < -200) corr = -200;
        step_q16 = (uint32_t)((int32_t)step_q16 + corr);
    }

    for (uint32_t i = 0; i < n; i++) {
        const uint32_t ip = s_cap_rd_i, fr = s_cap_rd_f;
        int32_t l = cubic_q21(s_cap_L, CAP_RING - 1u, ip, fr);
        int32_t r = cubic_q21(s_cap_R, CAP_RING - 1u, ip, fr);
        if (fade_in && i < fade_in) { l = (int32_t)(((int64_t)l * (int32_t)i) / (int32_t)fade_in); r = (int32_t)(((int64_t)r * (int32_t)i) / (int32_t)fade_in); }
        s_cap_last_l = l; s_cap_last_r = r;
        put24(&pkt[i * 6u], l);
        put24(&pkt[i * 6u + 3u], r);
        { const uint32_t f = s_cap_rd_f + step_q16; s_cap_rd_i += f >> 16; s_cap_rd_f = f & 0xFFFFu; }
    }
    return n * 6u;
}

/* One OUT packet from the host (24-bit stereo, 6 bytes a frame). */
void pa_usb_audio_ep3_out_rx(pa_usb_ble_ctx_t *ctx, const uint8_t *pkt, uint32_t len) {
    if (!ctx || !pkt) return;
    if (!s_pb_on) pa_usb_audio_playback_start(ctx);                    /* tests and a host that never sent alt 1 first: the first packet opens it */
    g_pa_pb_packets++;
    for (uint32_t off = 0; off + 6u <= len; off += 6u) {
        int32_t l = (int32_t)(((uint32_t)pkt[off] | ((uint32_t)pkt[off + 1] << 8) | ((uint32_t)pkt[off + 2] << 16)) << 8) >> 8;      /* the shift up is unsigned: shifting a signed value into the sign bit is undefined */
        int32_t r = (int32_t)(((uint32_t)pkt[off + 3] | ((uint32_t)pkt[off + 4] << 8) | ((uint32_t)pkt[off + 5] << 16)) << 8) >> 8;
        { const int32_t a = l < 0 ? -l : l, b = r < 0 ? -r : r, m = a > b ? a : b; if (m > s_pb_peak) s_pb_peak = m; }
        s_pb_L[s_pb_wr & (PB_RING - 1u)] = l >> 2;                     /* 24-bit -> Q21 */
        s_pb_R[s_pb_wr & (PB_RING - 1u)] = r >> 2;
        s_pb_wr++;                                                     /* published last: the audio interrupt may read between two frames */
    }
}

/* ADD the host's audio to the output (L, R: Q21), `frames` of it, resampled from the host's rate to the codec's. */
void pa_usb_audio_mix_playback_stereo(pa_usb_ble_ctx_t *ctx, int32_t *L, int32_t *R, int frames) {
    if (!ctx || !ctx->uac_spk_streaming || !s_pb_on || !L) return;
    int32_t fill = (int32_t)(s_pb_wr - s_pb_rd_i);
    int32_t pb_fill_before = fill;
    uint32_t t_block = plat_ticks24();
    uint32_t fade_in = 0u;
    if (!s_pb_primed) {
        if ((uint32_t)(s_pb_wr - s_pb_hold_wr) >= s_pb_need) {
            s_pb_rd_i = s_pb_wr - PA_PB_TARGET; s_pb_rd_f = 0u; s_pb_primed = 1u; fade_in = FADE_IN_FRAMES; fill = (int32_t)PA_PB_TARGET;
            s_pb_starve_consec = 0u; s_pb_concealing = 0u;
            if (s_pb_peak >= PA_PB_GATE_OPEN) { s_pb_gate_hold = PA_PB_GATE_HOLD; s_pb_gate_g = 32767; }      /* what arrived while it buffered decides: real audio starts at full gain (the fade-in above ramps it), silence stays gated */
            else s_pb_gate_g = 0;
        }
    } else {
        if (fill < 0 || (uint32_t)fill < g_pa_pb_fill_min) g_pa_pb_fill_min = fill < 0 ? 0u : (uint32_t)fill;
        if ((uint32_t)fill > g_pa_pb_fill_max && fill < (int32_t)PB_RING) g_pa_pb_fill_max = (uint32_t)fill;
        if (fill > (int32_t)PB_RING - 16) { s_pb_rd_i = s_pb_wr - PA_PB_TARGET; s_pb_rd_f = 0u; fill = (int32_t)PA_PB_TARGET; g_pa_pb_recentres++; }
    }
    if (!s_pb_primed) {                                                 /* silence from the host's side, with the last frame faded out once */
        if (s_pb_last_l || s_pb_last_r) {
            for (int i = 0; i < frames; i++) {
                const int32_t g = frames - i;
                L[i] += (int32_t)(((int64_t)s_pb_last_l * g) / frames);
                if (R && R != L) R[i] += (int32_t)(((int64_t)s_pb_last_r * g) / frames);
            }
            s_pb_last_l = 0; s_pb_last_r = 0;
        }
        return;
    }
    /* If resuming after brief underflow concealment, crossfade back into real audio smoothly */
    if (s_pb_concealing && !fade_in) {
        fade_in = FADE_IN_FRAMES;
        s_pb_concealing = 0u;
    }
    /* step = host rate / codec rate (read fewer host frames than codec frames, since 44,100 < 44,118), steered by the fill */
    uint32_t step_q16 = (uint32_t)(((uint64_t)s_pb_rate << 16) / PA_CODEC_RATE_HZ);
    {
        int32_t corr = ((fill - (int32_t)PA_PB_TARGET) * (int32_t)PA_CAP_GAIN_X8) >> 3;
        if (corr > 200) corr = 200;
        if (corr < -200) corr = -200;
        step_q16 = (uint32_t)((int32_t)step_q16 + corr);
    }
    /* the gate on the host's level (see PA_PB_GATE_OPEN): decided once per block from the packets that arrived since the last one */
    if (!fade_in || s_pb_gate_hold == 0u) {      /* (the block that just primed was decided above, from everything that arrived while it buffered) */
        const int32_t pk = s_pb_peak; s_pb_peak = 0;
        if (pk >= PA_PB_GATE_OPEN) s_pb_gate_hold = PA_PB_GATE_HOLD;
        else if (s_pb_gate_hold > (uint32_t)frames) s_pb_gate_hold -= (uint32_t)frames;
        else s_pb_gate_hold = 0u;
    }
    const int32_t gate_target = s_pb_gate_hold ? 32767 : 0;
    int underflow = 0;
    for (int i = 0; i < frames; i++) {
        const int32_t cur_fill = (int32_t)(s_pb_wr - s_pb_rd_i);
        int32_t l, r;
        if (cur_fill >= 3) {
            const uint32_t ip = s_pb_rd_i, fr = s_pb_rd_f;
            l = cubic_q21(s_pb_L, PB_RING - 1u, ip, fr);
            r = cubic_q21(s_pb_R, PB_RING - 1u, ip, fr);
            s_pb_gate_g += (gate_target - s_pb_gate_g) >> 6;                                              /* a one-pole ramp, about 1.5 ms */
            if (s_pb_gate_g < 64 && !gate_target) s_pb_gate_g = 0;
            if (s_pb_gate_g != 32767) { l = (int32_t)(((int64_t)l * s_pb_gate_g) >> 15); r = (int32_t)(((int64_t)r * s_pb_gate_g) >> 15); }
            if (fade_in && (uint32_t)i < fade_in) { l = (int32_t)(((int64_t)l * i) / (int32_t)fade_in); r = (int32_t)(((int64_t)r * i) / (int32_t)fade_in); }
            s_pb_last_l = l; s_pb_last_r = r;
            { const uint32_t f = s_pb_rd_f + step_q16; s_pb_rd_i += f >> 16; s_pb_rd_f = f & 0xFFFFu; }
        } else {
            /* Underflow concealment: bounded linear fade toward zero for missing samples */
            underflow = 1;
            s_pb_last_l = (int32_t)(((int64_t)s_pb_last_l * 7) >> 3);
            s_pb_last_r = (int32_t)(((int64_t)s_pb_last_r * 7) >> 3);
            l = s_pb_last_l;
            r = s_pb_last_r;
        }
        L[i] += l;
        if (R && R != L) R[i] += r;
    }
    if (underflow) {
        g_pa_pb_starved++;
        s_pb_starve_consec++;
        s_pb_concealing = 1u;
        {
            uint32_t s_idx = s_starve_log_count % PA_STARVE_LOG_CAP;
            s_starve_log[s_idx].fill_before = pb_fill_before;
            s_starve_log[s_idx].need = s_pb_need;
            s_starve_log[s_idx].rd_i = s_pb_rd_i;
            s_starve_log[s_idx].wr = s_pb_wr;
            s_starve_log[s_idx].ep3_rx_pkts = g_ep3_rx_pkts;
            s_starve_log[s_idx].ep3_rx_bytes = g_ep3_rx_last_bytes;
            s_starve_log[s_idx].usb_sof = g_usb_last_sof;
            s_starve_log[s_idx].t_last_rx = g_ep3_rx_last_tick;
            s_starve_log[s_idx].t_block = t_block;
            s_starve_log[s_idx].sie_lock_max_us = g_sie_lock_max_ticks / 24u;
            s_starve_log[s_idx].irq_off_max_us = g_irq_off_max_ticks / 24u;
            s_starve_log_count++;
        }
        if (s_pb_starve_consec >= 3u) {
            /* Sustained starvation (> 10 ms of no host audio packets): reset and wait to reprime */
            s_pb_primed = 0u;
            s_pb_hold_wr = s_pb_wr;
            s_pb_need = PA_PB_REPRIME;
            s_pb_starve_consec = 0u;
            s_pb_concealing = 0u;
            s_pb_last_l = 0;
            s_pb_last_r = 0;
        }
    } else {
        s_pb_starve_consec = 0u;
    }
}
