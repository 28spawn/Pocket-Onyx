/*
 * pa_sysex.h — Pocket Amp M-VAVE SysEx Protocol Parser & Dispatcher (QW-7 / BR-5)
 *
 * The frame on the wire is F0 ... F7 with the inner frame packed 8-to-7-bit
 * (USB MIDI carries 7-bit data bytes; the stock device does the same and
 * unpacks it in its transport, so the dispatcher here sees plain 8-bit bytes
 * — SysEx framing protocol):
 *
 *   F0 <packed> F7      where <packed> is
 *   00 59 <cmd> <len_hi> <len_mid> <len_lo> <payload[len]> <~sum & 0xFF>
 *
 * The inner frame is 8-bit BINARY, so a NAM capture or a preset goes as it
 * is. It used to be masked to 7 bits, which made every byte above 0x7F
 * arrive with its top bit stripped — a capture could not be sent at all.
 */
#ifndef PA_SYSEX_H
#define PA_SYSEX_H

#include <stdint.h>
#include "pa_protocol.h"

#define PA_SYSEX_START        0xF0u
#define PA_SYSEX_END          0xF7u
#define PA_SYSEX_MFR_0        PA_SYSEX_FRAME_HDR0
#define PA_SYSEX_MFR_1        PA_SYSEX_FRAME_HDR1
#define PA_SYSEX_MAX_PAYLOAD  256u

typedef struct {
    uint8_t  state;
    uint8_t  cmd;
    uint32_t expected_len;
    uint32_t rx_idx;
    uint8_t  sum;
    uint8_t  payload[PA_SYSEX_MAX_PAYLOAD];
    uint32_t frames_ok;
    uint32_t frames_err;
} pa_sysex_parser_t;

enum pa_upload_target {
    PA_UPLOAD_PRESET   = 0,
    PA_UPLOAD_NAM      = 1,
    PA_UPLOAD_CAB      = 2,
    PA_UPLOAD_SETTINGS = 3
};

#if defined(PA_HOST_SIM) || defined(PA_HOST_BUILD)
#define PA_UPLOAD_BUFFER_SIZE  32768u    /* the emulator and the host tests keep the full staging buffer */
#else
#define PA_UPLOAD_BUFFER_SIZE  8192u    /* on the device the pool is full (the WaveNet block engine needs the room); the largest real payload is the 7.5 KB WaveNet */
#endif
/* The upload staging buffer. It holds ONE transfer (cmd 25 / cmd 34 / cmd 48),
 * so this is also the largest blob the SysEx path will accept - 32 KB, about
 * four times the largest real payload (8 KB IR, 7.5 KB WaveNet capture).
 * This is protocol-visible: custom-fw/test/test_sysex_upload.py and the WASM
 * workbench send the same limit. It was 65,536 B, and two such buffers (this one
 * and the NAM flash reader) were 128 KB of RAM holding at most 8 KB each. */

typedef struct {
    uint8_t  armed;
    uint8_t  target_type;
    uint8_t  target_slot;
    uint8_t  status;
    uint32_t declared_len;
    uint32_t rx_bytes;
    uint8_t  buffer[PA_UPLOAD_BUFFER_SIZE];
} pa_upload_session_t;

/* ---- what an editor page reads and writes (custom commands 80 / 81, uploads) ----
 * PA_CMD_PA_STATE (no payload) is answered with ONE frame of PA_REPLY_PA_STATE, a record of PA_STATE_REC_SIZE bytes (version 1):
 *    0 version (1)   1 view   2 preset index (0..79)   3 flags: b0 STOMP mode, b1 tuner open, b2 groove playing, b3 USB loopback, b4 stereo cabinet allowed (reserved 1)
 *    4 preset volume %   5 output level %   6..21 preset name (16, NUL padded)
 *   22..85 the eight slots in slot order DYN DRV AMP CAB EQ MOD DLY REV, 8 bytes each: [on (1 = sounding), type, knob 0..5 in %]
 *   86 input gain dB + 12   87 gate dB + 80   88 output type (0 phones, 1 line)   89 cab on line   90 output limiter   91 fizz filter   92 stereo width   93 output routing
 *   94 balance + 50   95 global EQ on   96..100 global EQ bands dB + 12   101 tuner reference Hz - 430   102 tuner mute   103 Bluetooth mode (0 off, 1 BLE, 2 BLE+A2DP)   104 display brightness
 *  105 home view (0 grid, 1 list)   106..107 groove BPM (u16 LE)   108 groove pattern   109 groove volume %   110 drums on   111..114 stomp map (slot ids for VOL-, VOL+, <, >)
 *  115 stored captures that exist   116 1 when a live (RAM) user cabinet is loaded   117..124 the chain: slot ids in signal order   125 stored cabinets that exist
 *  126 free capture slots (of 16)   127 free cabinet slots (of 15)
 * PA_CMD_PA_NAMES [kind, start] (kind 0 = every amp type in type-knob order, stock then stored captures; 1 = every cabinet type) is answered with [kind, total, start, n, first_user] + n names of 16 bytes;
 * a deleted capture or cabinet is an EMPTY name (its type number stays, presets use it), first_user is the first type that can be deleted.
 * PA_CMD_PA_USER [op, kind, index, name...]: op 0 deletes the stored capture (kind 0) or cabinet (kind 1) at that index (index = type - first_user), op 1 renames it, op 2 clears ALL of that kind and wipes the area (index ignored); replied with [op, kind, index, result].
 * Uploads (cmd 25 / 34 / 48): the 4th byte of the cmd 25 payload is the target (PA_UPLOAD_NAM = 1: the slot byte is the capture index, 0xFF = the first free one; PA_UPLOAD_CAB = 2: raw Q18 taps, int32
 * little endian, 16..1024 of them: the slot byte is the cabinet slot 0..14 or 0xFF = the first free one, STORED in the flash; 0xFE = a LIVE cabinet in RAM only, loaded under the same fade as a model change
 * and gone at the next cabinet change or power-off) and the 5th is the slot. */
#define PA_STATE_REC_SIZE 128u
typedef uint32_t (*pa_sysex_state_fn)(uint8_t *out, uint32_t cap);                                   /* -> bytes written (PA_STATE_REC_SIZE), 0 when not ready */
typedef uint32_t (*pa_sysex_names_fn)(uint8_t kind, uint8_t start, uint8_t *out, uint32_t cap);      /* -> bytes written */
void pa_sysex_set_editor_providers(pa_sysex_state_fn state, pa_sysex_names_fn names);
/* a live user cabinet from an upload: raw Q18 taps in buf (int32 LE, writable: it is normalised in place) -> 0 ok, <0 refused. A weak default refuses. */
int  pa_audio_core_load_user_cab(uint8_t *buf, uint32_t len);
void pa_audio_core_user_store_changed(void);          /* a stored capture / cabinet was added or deleted: re-check what the slots play (applied at the next tick) */

void pa_sysex_init(void);
void pa_sysex_input_byte(uint8_t b);
int  pa_sysex_build_frame(uint8_t cmd, const uint8_t *payload, uint32_t len,
                          uint8_t *out_buf, uint32_t max_out);
void pa_upload_session_init(void);
const pa_upload_session_t *pa_upload_get_session(void);

/* Where the replies go. The board layer registers the device's USB MIDI TX
 * path; the emulator registers a capture, so a reply can be checked on the
 * host. Without a sink the reads are answered into the void. */
typedef void (*pa_sysex_reply_sink_t)(const uint8_t *data, uint32_t len);
void pa_sysex_set_reply_sink(pa_sysex_reply_sink_t sink);
int  pa_sysex_reply_sink_set(void);

#endif /* PA_SYSEX_H */
