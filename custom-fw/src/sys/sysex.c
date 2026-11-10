/* SPDX-License-Identifier: GPL-3.0-only */
/* Copyright (C) 2026 The Onyx Authors (@28spawn) */
/*
 * src/sys/sysex.c — M-VAVE Pocket Amp SysEx Streaming Parser & Command Dispatcher
 */
#include <string.h>
#include "../../include/pa_sysex.h"
#include "../../include/pa_user_nam.h"
#include "../../include/pa_user_cab.h"
#include "../../include/pa_fx.h"
#include "../../include/pa_preset.h"
#include "../../include/pa_settings.h"

#include "../../include/pa_spi_nor.h"
#include "../../include/pa_flash_map.h"

enum {
    SX_IDLE = 0,
    SX_MFR0,
    SX_MFR1,
    SX_CMD,
    SX_LEN0,
    SX_LEN1,
    SX_LEN2,
    SX_PAYLOAD,
    SX_CHECKSUM,
    SX_END
};

static pa_sysex_parser_t  g_sx;

/* ---- the transport: F0 ... F7, 8-to-7-bit packed --------------------------
 *
 * USB MIDI carries 7-bit data bytes, so a frame with a binary payload - a NAM
 * capture, a preset - cannot go out raw: the bytes go into one bit stream, LSB
 * first, and come out again in 7-bit groups. The stock device unpacks that in
 * its transport, so the dispatcher above it sees plain 8-bit bytes. That is
 * why the stock's checksum is 8-bit ("~sum&0xFF", SysEx specification) and
 * why its model blobs arrive intact.
 *
 * This used to mask every byte with 0x7F instead, which made the framing
 * 7-bit clean - and made it impossible to send a capture: a NAM model is full
 * of bytes above 0x7F and they all arrived with their top bit stripped, so
 * the capture that reached the flash was not the capture that was sent.
 *
 * The buffers are in .pool (NOLOAD), so they do not grow the flash image. */
#define SX_PACK_MAX 384u   /* the packed bytes of one message */

static uint8_t  g_sx_pack[SX_PACK_MAX]   __attribute__((section(".pool")));
static uint8_t  g_sx_inner[SX_PACK_MAX]  __attribute__((section(".pool")));
static uint32_t g_sx_pack_len = 0;
static uint8_t  g_sx_in_msg   = 0;

static void pa_sysex_inner_byte(uint8_t b);
static void pa_sysex_unpack(void);

/* ---- the reply path --------------------------------------------------------
 *
 * A read is answered with a SysEx reply: the same frame shape as a request,
 * with the reply TYPE in the command byte. The stock's reply builders take a
 * type too - 0x77F02 "sysex_reply_send(type, ctx, data...)" and 0x3297C
 * "sysex_reply_send_2(ctx, data, type, tag)" (SysEx specification).
 *
 * The board layer says where the bytes go (the device's USB MIDI TX path);
 * the emulator registers the same sink, so a reply can be captured on the
 * host and checked.
 */
typedef void (*pa_sysex_reply_sink_t)(const uint8_t *data, uint32_t len);
static pa_sysex_reply_sink_t g_reply_sink = 0;

void pa_sysex_set_reply_sink(pa_sysex_reply_sink_t sink) {
    g_reply_sink = sink;
}

int pa_sysex_reply_sink_set(void) {
    return g_reply_sink != 0;
}

static void pa_sysex_reply(uint8_t type, const uint8_t *data, uint32_t len) {
    if (!g_reply_sink) return;
    uint8_t frame[PA_SYSEX_MAX_PAYLOAD + 48];
    int n = pa_sysex_build_frame(type, data, len, frame, sizeof(frame));
    if (n > 0) g_reply_sink(frame, (uint32_t)n);
}

/* ---- the small string helpers (no snprintf: this is firmware) ------------- */
static uint32_t pa_sysex_put_str(char *out, uint32_t cap, uint32_t at, const char *s) {
    while (*s && (at + 1u) < cap) out[at++] = *s++;
    return at;
}
static uint32_t pa_sysex_put_u32(char *out, uint32_t cap, uint32_t at, uint32_t v) {
    char tmp[12];
    uint32_t n = 0;
    if (v == 0u) tmp[n++] = '0';
    while (v) { tmp[n++] = (char)('0' + (v % 10u)); v /= 10u; }
    while (n && (at + 1u) < cap) out[at++] = tmp[--n];
    return at;
}

/* ---- the reply types --------------------------------------------------------
 *
 * A reply carries a TYPE in the command byte - the stock's reply builders
 * take one (0x77F02 "sysex_reply_send(type, ctx, data...)", 0x3297C
 * "sysex_reply_send_2(ctx, data, type, tag)"). The two the spec pins down
 * are the state record (41) and the state block (2); the others are the
 * request's own command byte.
 *
 * If the editor ever rejects a reply ("Unable to identify the device preview
 * protocol"), THIS TABLE is the one place to change. */
#define PA_REPLY_TYPE_DEVICE_INFO  0x29u  /* 41 - the record family */
#define PA_REPLY_TYPE_DESCRIPTOR   0x02u  /* the block family */
#define PA_REPLY_TYPE_STATE_RECORD 0x29u  /* 41 - the stock's record id */
#define PA_REPLY_TYPE_STATE_BLOCK  0x02u  /* 2 - the stock's block type */
#define PA_REPLY_TYPE_DRAIN        0x02u
#define PA_REPLY_TYPE_READ         0x02u

/* ---- the device identity --------------------------------------------------

 *
 * The device identity record provides standard identification fields:
 * name, model, id, type, and version ("DeviceInfo").
 * The record is formatted as key-value text matching standard host
 * identification requirements.
 */
#ifndef PA_DEVICE_NAME
#define PA_DEVICE_NAME "Onyx"
#endif
#ifndef PA_DEVICE_MODEL
#define PA_DEVICE_MODEL "Onyx"
#endif
/* PA_FW_VERSION now lives in pa_config.h (one definition, shared with the OTA ID block) */

static uint32_t pa_sysex_device_info_text(char *out, uint32_t cap) {
    uint32_t at = 0;
    at = pa_sysex_put_str(out, cap, at, "name=" PA_DEVICE_NAME);
    at = pa_sysex_put_str(out, cap, at, ",model=" PA_DEVICE_MODEL);
    at = pa_sysex_put_str(out, cap, at, ",id=");
    at = pa_sysex_put_u32(out, cap, at, 1u);
    at = pa_sysex_put_str(out, cap, at, ",type=1");
    at = pa_sysex_put_str(out, cap, at, ",version=");
    at = pa_sysex_put_u32(out, cap, at, PA_FW_VERSION);
    at = pa_sysex_put_str(out, cap, at, ",size=");
    at = pa_sysex_put_u32(out, cap, at, at);
    if (at < cap) out[at] = '\0';
    return at;
}

/* The state record and the state block: what the editor reads back after the
 * handshake. The layout is ours (the stock's is not in our materials), so it
 * is kept to one place. */
static uint32_t pa_sysex_state_record(uint8_t *out) {
    out[0] = 0u;                                /* the view */
    out[1] = 0u;                                /* the flags */
    out[2] = 0u;                                /* the preset index, low */
    out[3] = 0u;                                /* the preset index, high */
    out[4] = 50u;                               /* the preset volume */
    out[5] = 100u;                              /* the master volume */
    out[6] = 0u;                                /* the slot count */
    out[7] = 0u;                                /* the reserved */
    return 8u;
}

static uint32_t pa_sysex_state_block(uint8_t *out) {
    (void)pa_sysex_state_record(out);
    out[8]  = 0u; out[9]  = 0u; out[10] = 0u; out[11] = 0u;
    out[12] = 0u; out[13] = 0u; out[14] = 0u; out[15] = 0u;
    return 16u;
}
/* 32 KB scratch for streaming a NAM/IR blob in over SysEx (PA_UPLOAD_BUFFER_SIZE).
 * It belongs in .pool (NOLOAD, so it does not grow the flash image) - in .bss it
 * pushed the RAM region over budget. */
static pa_upload_session_t g_upload __attribute__((section(".pool")));

void pa_upload_session_init(void) {
    memset(&g_upload, 0, sizeof(g_upload));
}

const pa_upload_session_t *pa_upload_get_session(void) {
    return &g_upload;
}

/* A capture or cabinet was stored or deleted: the audio core re-checks what the slots play (weak: a build without it has nothing to re-check). */
__attribute__((weak)) void pa_audio_core_user_store_changed(void) { }

/* A live cabinet from an upload (PA_UPLOAD_CAB): the audio core loads it. Weak: a build without the audio core refuses it. */
__attribute__((weak)) int pa_audio_core_load_user_cab(uint8_t *buf, uint32_t len) { (void)buf; (void)len; return -4; }

static pa_sysex_state_fn g_state_provider;
static pa_sysex_names_fn g_names_provider;
void pa_sysex_set_editor_providers(pa_sysex_state_fn state, pa_sysex_names_fn names) { g_state_provider = state; g_names_provider = names; }

static uint8_t s_up_result;                     /* 0 none yet, 1 the last transfer was stored, 2 it was refused (acknowledged to the sender, see PA_REPLY_PA_UPLOAD) */
static void upload_ack(uint8_t state) {
    uint8_t a[4] = { (uint8_t)g_upload.rx_bytes, (uint8_t)(g_upload.rx_bytes >> 8), (uint8_t)(g_upload.rx_bytes >> 16), state };
    pa_sysex_reply(PA_REPLY_PA_UPLOAD, a, sizeof a);
}
static void commit_upload_session(void) {
    if (!g_upload.armed || g_upload.rx_bytes == 0) return;
    /* A transfer that is not all there is refused, never stored: the MIDI receive ring (512 bytes, emptied once per UI tick, which can take 100 ms and more) drops bytes when a
     * sender is faster than that, and the old code committed whatever had arrived: a capture that said 1,871 weights and held 1,671, which the loader then rejected for ever. */
    if (g_upload.rx_bytes != g_upload.declared_len) { g_sx.frames_err++; g_upload.status = 1u; g_upload.armed = 0; s_up_result = 2u; return; }

    int rc = -1;
    if (g_upload.target_type == PA_UPLOAD_PRESET) {
        if (g_upload.rx_bytes >= (uint32_t)sizeof(pa_preset_t)) {
            const pa_preset_t *p = (const pa_preset_t *)g_upload.buffer;
            uint32_t expected_crc = pa_crc32(p, (uint32_t)sizeof(*p) - sizeof(uint32_t));
            if (p->magic == PA_PRESET_MAGIC && p->crc32 == expected_crc) {
                rc = pa_preset_save(g_upload.target_slot, p);
            }
        }
    } else if (g_upload.target_type == PA_UPLOAD_PRESET_RESTORE) {
        if (g_upload.rx_bytes >= (uint32_t)sizeof(pa_preset_t)) rc = pa_preset_restore(g_upload.target_slot, (const pa_preset_t *)g_upload.buffer);
    } else if (g_upload.target_type == PA_UPLOAD_NAM) {
        if (g_upload.target_slot == 0xFFu) {                                   /* the first free logical slot */
            uint8_t f = 0;
            while (f < PA_USER_NAM_MAX_COUNT && pa_user_nam_is_active(f)) f++;
            g_upload.target_slot = f;
        }
        if (g_upload.target_slot < PA_USER_NAM_MAX_COUNT) {
            /* pa_user_nam_add() stores the blob AND registers the capture.
             * The upload used to write the blob straight into the flash, so
             * the capture was in the flash but NOT in the registry: it never
             * showed up in the amp list, and pa_user_nam_get_count() did not
             * see it either - an uploaded NAM was invisible. */
            uint8_t type = 0;   /* 0 = the 3852 B Q21 GRU struct */
            float head_scale = 1.0f;
            float makeup_db = 0.0f;
            if (g_upload.rx_bytes >= 12u &&
                g_upload.buffer[0] == 'W' && g_upload.buffer[1] == 'A' &&
                g_upload.buffer[2] == 'V' && g_upload.buffer[3] == 'E') {
                type = 1;        /* 1 = a WaveNet float container */
                memcpy(&head_scale, g_upload.buffer + 8, 4);
                if (g_upload.rx_bytes >= 16u) {
                    /* the container's flags word: bit 0 the cabinet is baked in (read at every load), bit 1 a makeup is given, bits 8..15 that makeup as a signed quarter-dB.
                     * The editor works it out from the .nam file's "loudness" so a capture lands at the stock amps' level (web/Onyx_Web_Editor.html namMakeupDb). */
                    uint32_t fl; memcpy(&fl, g_upload.buffer + 12, 4);
                    if (fl & 2u) makeup_db = (float)(int8_t)(uint8_t)(fl >> 8) * 0.25f;
                }
            }
            pa_user_nam_meta_t meta;
            meta.input_trim = 1.0f;
            meta.makeup_db  = makeup_db;           /* pa_user_nam_add clamps it to +-12 dB */
            for (int k = 0; k < 6; k++) meta.amp_knobs[k] = 50;
            meta.flags = 0;
            char nam_slot_name[16];
            nam_slot_name[0] = 'U'; nam_slot_name[1] = 'S'; nam_slot_name[2] = 'E'; nam_slot_name[3] = 'R'; nam_slot_name[4] = ' ';
            nam_slot_name[5] = 'N'; nam_slot_name[6] = 'A'; nam_slot_name[7] = 'M'; nam_slot_name[8] = ' ';
            uint8_t slot_disp = (uint8_t)(g_upload.target_slot + 1u);
            nam_slot_name[9]  = (char)('0' + (slot_disp / 10u));
            nam_slot_name[10] = (char)('0' + (slot_disp % 10u));
            nam_slot_name[11] = '\0';
            rc = pa_user_nam_add(nam_slot_name, type, 0, 0, head_scale,
                                 g_upload.buffer, g_upload.rx_bytes, &meta);
            if (rc >= 0) g_upload.target_slot = (uint8_t)rc;
        }
    } else if (g_upload.target_type == PA_UPLOAD_CAB) {
        /* A live cabinet: raw Q18 taps (int32 LE, 16..1024) loaded into the cabinet slot in RAM. Not stored in flash (there is no free sector for it): the next preset or cabinet
         * change drops it, and the editor sends it again. A bad length or a swap still pending is refused, so the sender is told. */
        if (g_upload.target_slot == 0xFEu) {
            rc = pa_audio_core_load_user_cab(g_upload.buffer, g_upload.rx_bytes);
        } else if ((g_upload.rx_bytes & 3u) == 0u && ((uintptr_t)g_upload.buffer & 3u) == 0u) {              /* stored in the flash (pa_user_cab.h), the slot byte or the first free slot */
            char nm[12] = "USER CAB";                                                             /* the editor renames it (PA_CMD_PA_USER op 1) */
            if (g_upload.target_slot < PA_USER_CAB_SLOTS) {
                const uint8_t shown = (uint8_t)(g_upload.target_slot + 1u);
                nm[0] = 'C'; nm[1] = 'A'; nm[2] = 'B'; nm[3] = ' '; nm[4] = (char)('0' + shown / 10u); nm[5] = (char)('0' + shown % 10u); nm[6] = '\0';
            }
            rc = pa_user_cab_add(nm, g_upload.target_slot == 0xFFu ? 0xFF : (int)g_upload.target_slot, (const int32_t *)(const void *)g_upload.buffer, g_upload.rx_bytes / 4u);
            if (rc >= 0) { g_upload.target_slot = (uint8_t)rc; pa_audio_core_user_store_changed(); }
        } else rc = -1;
    } else if (g_upload.target_type == PA_UPLOAD_SETTINGS) {
        /* Route through pa_settings_save() instead of writing the live sector.
         * The old path wrote straight into sector A, bypassing the A/B ping-pong,
         * the magic/version/size header and the CRC - so pa_settings_load() then
         * rejected the block and silently fell back to factory defaults. */
        pa_settings_t st;
        if (g_upload.rx_bytes >= sizeof(pa_settings_t)) {
            memcpy(&st, g_upload.buffer, sizeof(pa_settings_t));
            if (st.magic == PA_SETTINGS_MAGIC && st.version == PA_SETTINGS_VERSION &&
                st.size == sizeof(pa_settings_t)) {
                /* Clamp fields to safe valid ranges before saving */
                if (st.last_preset >= PA_PRESET_COUNT) st.last_preset = 0;
                if (st.home_view_mode > 1) st.home_view_mode = 0;
                if (st.play_mode > 1) st.play_mode = 0;
                if (st.fizz_level > 3) st.fizz_level = 0;
                if (st.display_brightness > 100) st.display_brightness = 100;
                if (st.display_auto_dim > 1) st.display_auto_dim = 1;
                if (st.display_battery_pct > 1) st.display_battery_pct = 1;
                if (st.theme > 1) st.theme = 0;
                if (st.input_gain_db < -12) st.input_gain_db = -12;
                if (st.input_gain_db > 12)  st.input_gain_db = 12;
                if (st.gate_db < -80) st.gate_db = -80;
                if (st.gate_db > -30) st.gate_db = -30;
                if (st.out_level_pct > 100) st.out_level_pct = 80;
                if (st.io_output_type > 1) st.io_output_type = 0;
                if (st.io_cab_on_line > 1) st.io_cab_on_line = 1;
                if (st.usb_sample_rate > 1) st.usb_sample_rate = 1;
                if (st.tuner_ref_hz < 430 || st.tuner_ref_hz > 450) st.tuner_ref_hz = 440;
                if (st.drum_bpm < 40 || st.drum_bpm > 260) st.drum_bpm = 120;
                if (st.drum_pattern > 39) st.drum_pattern = 0;
                if (st.drum_volume > 100) st.drum_volume = 70;
                if (st.output_limit_mode > 2) st.output_limit_mode = 0;
                if (st.usb_loopback > 1) st.usb_loopback = 1;
                if (st.stereo_width > 101) st.stereo_width = 0;       /* stored as width + 1; 0 = not set, comes up with the default width */
                if (st.out_route > 4) st.out_route = 0;
                if (st.out_balance < -50 || st.out_balance > 50) st.out_balance = 0;
                for (int b = 0; b < 5; b++) {
                    if (st.eq_bands_db[b] < -12) st.eq_bands_db[b] = -12;
                    if (st.eq_bands_db[b] > 12)  st.eq_bands_db[b] = 12;
                }
                rc = pa_settings_save(&st);
            } else {
                rc = -3;   /* not a valid settings record */
            }
        } else {
            rc = -2;       /* too short */
        }
    }

    /* pa_user_nam_add() reports the slot INDEX on success (0..19), not 0 - it
     * used to make a successful upload to any slot but the first look like a
     * failure. */
    int ok = (g_upload.target_type == PA_UPLOAD_NAM) ? (rc >= 0) : (rc == 0);
    g_upload.status = ok ? 0u : 1u;
    s_up_result = ok ? 1u : 2u;
    g_upload.armed = 0;
}

void pa_sysex_init(void) {
    memset(&g_sx, 0, sizeof(g_sx));
    g_sx_pack_len = 0;
    g_sx_in_msg   = 0;
    pa_upload_session_init();
}

/* The audio core: is a preset / model / live-cabinet swap still waiting for the mute? (weak: a build without the audio core has none) */
__attribute__((weak)) int pa_audio_core_swap_pending(void) { return 0; }

static void pa_sysex_dispatch(uint8_t cmd, const uint8_t *payload, uint32_t len) {
    g_sx.frames_ok++;

    if (cmd == PA_CMD_WRITE_BEGIN) { /* cmd25 (0x19) */
        if (len >= 5 && payload[3] == PA_UPLOAD_CAB && payload[4] == 0xFEu && pa_audio_core_swap_pending()) {
            /* A live cabinet goes into the slot the swap is about to rewrite, and the audio core takes only one asset at a time (it answers -5 at the commit). Say so NOW, before the
             * sender spends the transfer: the previous transfer, if any, is left as it is, and the editor asks again in a moment. */
            g_sx.frames_err++; s_up_result = 2u; upload_ack(2u); return;
        }
        if (len >= 3) {
            uint32_t decl_len = ((uint32_t)payload[0]) |
                                (((uint32_t)payload[1]) << 8) |
                                (((uint32_t)payload[2]) << 16);
            g_upload.declared_len = decl_len;
            g_upload.target_type  = (len > 3) ? payload[3] : PA_UPLOAD_NAM;
            g_upload.target_slot  = (len > 4) ? payload[4] : 0u;
            g_upload.rx_bytes     = 0;
            g_upload.status       = 0;
            s_up_result           = 0u;
            g_upload.armed        = (decl_len != 0u && decl_len <= PA_UPLOAD_BUFFER_SIZE) ? 1u : 0u;
            if (!g_upload.armed) { g_upload.status = 1u; g_sx.frames_err++; s_up_result = 2u; upload_ack(2u); return; }

            /* Check if initial body data is appended after header */
            if (len > 5) {
                uint32_t chunk = len - 5u;
                if (chunk > PA_UPLOAD_BUFFER_SIZE) chunk = PA_UPLOAD_BUFFER_SIZE;
                memcpy(g_upload.buffer, &payload[5], chunk);
                g_upload.rx_bytes = chunk;
                if (g_upload.rx_bytes >= g_upload.declared_len) {
                    commit_upload_session();
                }
            }
            upload_ack(g_upload.armed ? 0u : (s_up_result == 1u ? 1u : 2u));
        }
    } else if (cmd == PA_CMD_COMMIT && g_upload.armed) {
        /* The stock's armed commit (cmd 48): the pending transfer is swapped
         * in. Our session also commits by itself as soon as the declared
         * length has arrived, so this is the belt to that braces - and it is
         * what lets the legacy editor upload (cmd 25, cmd 34 chunks, cmd
         * 48) work whichever order the commit arrives in. */
        commit_upload_session();
    } else if (cmd == PA_CMD_DEVICE_INFO_17 || cmd == PA_CMD_DEVICE_INFO) {
        /* The editor's handshake: it asks who we are and what version we are,
         * and it parses the answer into its DeviceInfo (name, model, id, type,
         * version). The dispatcher spec has the device-info handler at cmd 1
         * and the report's command map has it at cmd 17, so
         * both are answered with the same record. */
        char rec[96];
        uint32_t n = pa_sysex_device_info_text(rec, sizeof(rec));
        pa_sysex_reply(PA_REPLY_DEVICE_INFO, (const uint8_t *)rec, n);
    } else if (cmd == PA_CMD_DESCRIPTOR_18) {
        /* The device descriptor: the record and block sizes the editor reads
         * with, and the protocol revision. */
        uint8_t desc[13];
        desc[0] = 'P'; desc[1] = 'D'; desc[2] = '0'; desc[3] = '1';
        desc[4] = 1u;      /* the descriptor revision */
        desc[5] = 0u;
        desc[6] = 8u;      /* the state record size */
        desc[7] = 16u;     /* the state block size */
        for (int i = 8; i < 13; i++) desc[i] = 0u;
        pa_sysex_reply(PA_REPLY_BLOCK, desc, sizeof(desc));
    } else if (cmd == PA_CMD_READ_RECORD) {
        /* The 8-byte state record (the stock: id 41, len 8, tag 11). */
        uint8_t rec[8];
        uint32_t n = pa_sysex_state_record(rec);
        pa_sysex_reply(PA_REPLY_DEVICE_INFO, rec, n);
    } else if (cmd == PA_CMD_READ_BLOCK) {
        /* The 16-byte pending block (the stock: type 2, tag 0x22). */
        uint8_t blk[16];
        uint32_t n = pa_sysex_state_block(blk);
        pa_sysex_reply(PA_REPLY_BLOCK, blk, n);
    } else if (cmd == PA_CMD_PA_STATE) {
        /* the editor page's state record (include/pa_sysex.h) */
        uint8_t rec[PA_STATE_REC_SIZE];
        const uint32_t n = g_state_provider ? g_state_provider(rec, sizeof rec) : 0u;
        if (n) pa_sysex_reply(PA_REPLY_PA_STATE, rec, n);
    } else if (cmd == PA_CMD_PA_FLASH) {
        /* a window of the flash for the editor's backup (read only; the same bytes the diagnostic channel reads) */
        uint8_t out[6 + 128];
        uint32_t addr = 0u, n = 0u;
        if (len >= 5u) { addr = (uint32_t)payload[0] | ((uint32_t)payload[1] << 8) | ((uint32_t)payload[2] << 16) | ((uint32_t)payload[3] << 24); n = payload[4]; }
        memset(out, 0, sizeof out);
        memcpy(out, &addr, 4);
        out[4] = (uint8_t)n;
        if (n == 0u || n > 128u || addr >= PA_FLASH_SIZE || n > PA_FLASH_SIZE - addr || pa_spi_nor_read(addr, &out[6], n) != 0) { out[5] = 1u; pa_sysex_reply(PA_REPLY_PA_FLASH, out, 6u); }
        else pa_sysex_reply(PA_REPLY_PA_FLASH, out, 6u + n);
    } else if (cmd == PA_CMD_PA_PRESET) {
        /* one stored preset, for the PC's backup tool (tools/preset_bank_pc.py): [index, status] + the pa_preset_t record */
        uint8_t out[2 + sizeof(pa_preset_t)];
        const uint8_t idx = len > 0 ? payload[0] : 0xFFu;
        pa_preset_t rec;
        memset(out, 0, sizeof out);
        out[0] = idx;
        if (idx < PA_PRESET_COUNT && pa_preset_load(idx, &rec) == 0) memcpy(&out[2], &rec, sizeof rec);
        else out[1] = 1u;
        pa_sysex_reply(PA_REPLY_PA_PRESET, out, sizeof out);
    } else if (cmd == PA_CMD_PA_NAMES) {
        uint8_t out[5 + 15 * 16];
        const uint32_t n = g_names_provider ? g_names_provider(len > 0 ? payload[0] : 0u, len > 1 ? payload[1] : 0u, out, sizeof out) : 0u;
        if (n) pa_sysex_reply(PA_REPLY_PA_NAMES, out, n);
    } else if (cmd == PA_CMD_PA_USER) {
        /* delete or rename a stored capture / cabinet */
        int rc = -1;
        const uint8_t op = len > 0 ? payload[0] : 0xFFu, kind = len > 1 ? payload[1] : 0xFFu, idx = len > 2 ? payload[2] : 0xFFu;
        if (op == 2u && kind <= 1u)      rc = kind == 0u ? pa_user_nam_clear() : pa_user_cab_clear();       /* clear all: the area is wiped, the reply carries 0 or the error */
        else if (kind == 0u && op == 0u) rc = pa_user_nam_delete(idx);
        else if (kind == 1u && op == 0u) rc = pa_user_cab_delete(idx);
        else if (op == 1u && len > 3 && len < 3 + 24) {
            char nm[24]; uint32_t k = 0;
            for (; k + 3 < len && k < sizeof nm - 1u; k++) nm[k] = (char)payload[3 + k];
            nm[k] = '\0';
            rc = kind == 0u ? pa_user_nam_set_name(idx, nm) : (kind == 1u ? pa_user_cab_rename(idx, nm) : -1);
        }
        if (rc >= 0 && (op == 0u || op == 2u)) pa_audio_core_user_store_changed();
        uint8_t r[4] = { op, kind, idx, (uint8_t)(rc >= 0 ? 0 : rc) };
        pa_sysex_reply(PA_REPLY_PA_USER, r, 4u);
    } else if (cmd == PA_CMD_DRAIN_21) {
        /* Drain the common ring: the state record, up to what was asked for. */
        uint8_t rec[8];
        uint32_t n = pa_sysex_state_record(rec);
        if (len > 0u && len < n) n = len;
        pa_sysex_reply(PA_REPLY_BLOCK, rec, n);
    } else if (cmd == PA_CMD_READ_35) {
        /* The read-shaped response: the state block, up to what was asked
         * for. */
        uint8_t blk[16];
        uint32_t n = pa_sysex_state_block(blk);
        if (len > 0u && len < n) n = len;
        pa_sysex_reply(PA_REPLY_BLOCK, blk, n);
    } else if (cmd == PA_CMD_WRITE_CHUNK && !g_upload.armed) {
        upload_ack(s_up_result == 1u ? 1u : (s_up_result == 2u ? 2u : 3u));       /* a late or repeated chunk: say how the transfer ended (or that none is open) */
    } else if (cmd == PA_CMD_WRITE_CHUNK && g_upload.armed) {
        /* Subsequent streaming chunk received during active upload session */
        if (len > 0) {
            uint32_t rem = (g_upload.declared_len > g_upload.rx_bytes)
                         ? (g_upload.declared_len - g_upload.rx_bytes) : 0u;
            uint32_t chunk = (len < rem) ? len : rem;
            if (g_upload.rx_bytes + chunk <= PA_UPLOAD_BUFFER_SIZE) {
                memcpy(&g_upload.buffer[g_upload.rx_bytes], payload, chunk);
                g_upload.rx_bytes += chunk;
            }
            if (g_upload.rx_bytes >= g_upload.declared_len) {
                commit_upload_session();
            }
        }
        upload_ack(g_upload.armed ? 0u : (s_up_result == 1u ? 1u : 2u));              /* every chunk is answered, an empty one only asks: the sender resumes from what the unit holds */
    }
}

void pa_sysex_input_byte(uint8_t b) {
    if (b == PA_SYSEX_START) {
        g_sx_in_msg   = 1u;
        g_sx_pack_len = 0u;
        /* the inner parser starts again for every message */
        g_sx.state    = SX_MFR0;
        g_sx.sum      = 0u;
        g_sx.rx_idx   = 0u;
        return;
    }
    if (!g_sx_in_msg) return;
    if (b == PA_SYSEX_END) {
        g_sx_in_msg = 0u;
        pa_sysex_unpack();
        return;
    }
    if (g_sx_pack_len < SX_PACK_MAX) {
        g_sx_pack[g_sx_pack_len++] = b & 0x7Fu;
    } else {
        g_sx.frames_err++;      /* a message this long is not ours - drop it */
        g_sx_in_msg = 0u;
    }
}

/* Unpack the 8-to-7-bit transport back into the 8-bit inner frame and hand
 * it to the parser. The padding bits of the last group are dropped. */
static void pa_sysex_unpack(void) {
    uint32_t bits = 0u, nbits = 0u, n = 0u;
    for (uint32_t i = 0; i < g_sx_pack_len; i++) {
        bits |= (uint32_t)(g_sx_pack[i] & 0x7Fu) << nbits;
        nbits += 7u;
        /* At most one byte comes out per group. This used to be a
         * "while (nbits >= 8)" loop, and that shape - a data-dependent inner
         * trip count - makes LLVM's loop unroller explore exponentially many
         * possibilities: the file took over 15 minutes to compile at -O3.
         * The one case is written out instead. */
        if (nbits >= 8u && n < SX_PACK_MAX) {
            g_sx_inner[n++] = (uint8_t)(bits & 0xFFu);
            bits >>= 8u;
            nbits -= 8u;
        }
    }
    for (uint32_t i = 0; i < n; i++) pa_sysex_inner_byte(g_sx_inner[i]);
}

static void pa_sysex_inner_byte(uint8_t b) {
    switch (g_sx.state) {
        case SX_MFR0:
            g_sx.state = (b == PA_SYSEX_MFR_0) ? SX_MFR1 : SX_IDLE;
            break;
        case SX_MFR1:
            g_sx.state = (b == PA_SYSEX_MFR_1) ? SX_CMD : SX_IDLE;
            break;
        case SX_CMD:
            g_sx.cmd   = b;
            g_sx.sum   = (uint8_t)(g_sx.sum + b);
            g_sx.state = SX_LEN0;
            break;
        case SX_LEN0:
            /* The length is a plain 24-bit little-endian (the stock's
             * "len:u24le"). It used to be three 7-bit groups, which only
             * agreed with the parser by accident - the parser ORs the bytes
             * together, so the two collided and the wrong length came out for
             * anything that was not a multiple of 128. */
            g_sx.expected_len = (uint32_t)b;
            g_sx.sum          = (uint8_t)(g_sx.sum + b);
            g_sx.state        = SX_LEN1;
            break;
        case SX_LEN1:
            g_sx.expected_len |= (uint32_t)b << 8;
            g_sx.sum           = (uint8_t)(g_sx.sum + b);
            g_sx.state         = SX_LEN2;
            break;
        case SX_LEN2:
            g_sx.expected_len |= (uint32_t)b << 16;
            g_sx.sum           = (uint8_t)(g_sx.sum + b);
            g_sx.rx_idx        = 0;
            if (g_sx.expected_len == 0) {
                g_sx.state = SX_CHECKSUM;
            } else if (g_sx.expected_len <= PA_SYSEX_MAX_PAYLOAD) {
                g_sx.state = SX_PAYLOAD;
            } else {
                g_sx.frames_err++;
                g_sx.state = SX_IDLE;
            }
            break;
        case SX_PAYLOAD:
            g_sx.payload[g_sx.rx_idx++] = b;
            g_sx.sum = (uint8_t)(g_sx.sum + b);
            if (g_sx.rx_idx >= g_sx.expected_len) {
                g_sx.state = SX_CHECKSUM;
            }
            break;
        case SX_CHECKSUM: {
            uint8_t expected_chk = (uint8_t)((~g_sx.sum) & 0xFFu);
            if (b == expected_chk) {
                pa_sysex_dispatch(g_sx.cmd, g_sx.payload, g_sx.expected_len);
            } else {
                g_sx.frames_err++;
            }
            g_sx.state = SX_END;
            break;
        }
        default:
            g_sx.state = SX_IDLE;
            break;
    }
}

int pa_sysex_build_frame(uint8_t cmd, const uint8_t *payload, uint32_t len,
                          uint8_t *out_buf, uint32_t max_out) {
    /* The inner frame is 8-bit binary:
     *     00 59 <cmd> <len:u24le> <payload> <chk>
     *     chk = ~sum(cmd, len bytes, payload) & 0xFF
     * and it travels packed, 8-to-7-bit, between F0 and F7. */
    if (!out_buf) return -1;
    if (len > PA_SYSEX_MAX_PAYLOAD) return -1;

    const uint32_t inner_len = 7u + len;   /* 00 59 cmd l0 l1 l2 payload chk */
    const uint32_t need = 2u + ((inner_len * 8u) + 6u) / 7u;   /* F0 .. F7 */
    if (max_out < need) return -1;

    uint8_t inner[PA_SYSEX_MAX_PAYLOAD + 9u];
    uint8_t l0 = (uint8_t)(len & 0xFFu);
    uint8_t l1 = (uint8_t)((len >> 8) & 0xFFu);
    uint8_t l2 = (uint8_t)((len >> 16) & 0xFFu);
    uint8_t sum = (uint8_t)((cmd & 0xFFu) + l0 + l1 + l2);

    inner[0] = PA_SYSEX_MFR_0;
    inner[1] = PA_SYSEX_MFR_1;
    inner[2] = (uint8_t)(cmd & 0xFFu);
    inner[3] = l0;
    inner[4] = l1;
    inner[5] = l2;
    for (uint32_t i = 0; i < len; i++) {
        uint8_t v = payload ? payload[i] : 0u;
        inner[6u + i] = v;
        sum = (uint8_t)(sum + v);
    }
    inner[6u + len] = (uint8_t)((~sum) & 0xFFu);

    /* pack it for the 7-bit transport */
    uint32_t o = 0;
    out_buf[o++] = PA_SYSEX_START;
    uint32_t bits = 0u, nbits = 0u;
    for (uint32_t i = 0; i < inner_len; i++) {
        bits |= (uint32_t)inner[i] << nbits;
        nbits += 8u;
        /* Each byte adds 8 bits, so at most two 7-bit groups come out. The
         * "while (nbits >= 7)" loop this replaces had a data-dependent trip
         * count, which is the shape that makes LLVM's loop unroller explore
         * exponentially many possibilities - this file took over 15 minutes
         * to compile at -O3. The two cases are written out instead. */
        if (nbits >= 7u) {
            out_buf[o++] = (uint8_t)(bits & 0x7Fu);
            bits >>= 7u;
            nbits -= 7u;
        }
        if (nbits >= 7u) {
            out_buf[o++] = (uint8_t)(bits & 0x7Fu);
            bits >>= 7u;
            nbits -= 7u;
        }
    }
    if (nbits) out_buf[o++] = (uint8_t)(bits & 0x7Fu);
    out_buf[o++] = PA_SYSEX_END;
    return (int)o;
}
