/* pa_protocol.h — USB-MIDI SysEx / syscmd control contract.
 * Pocket Amp MIDI & SysEx protocol definitions. */
#ifndef PA_PROTOCOL_H
#define PA_PROTOCOL_H
#include <stdint.h>

/* Transport: USB-MIDI System-Exclusive (F0..F7), LSB-first 8-to-7-bit encoded.
 * syscmd frame (inner): 00 59 | cmd:u8 | len:u24le | payload | chk(=~sum(payload)&0xFF). */
#define PA_SYSEX_FRAME_HDR0   0x00
#define PA_SYSEX_FRAME_HDR1   0x59

/* Dispatcher: command map: */

enum pa_syscmd {
    PA_CMD_DEVICE_INFO   = 1,   /* device info handshake */
    PA_CMD_CONFIG        = 12,  /* configuration query */
    PA_CMD_DSP_PARAM_SET = 16,  /* set effect/EQ parameter in live chain */
    PA_CMD_READ_RECORD   = 23,  /* 8-byte record (readiness-gated) */
    PA_CMD_READ_BLOCK    = 24,  /* one-shot 16-byte pending block */
    PA_CMD_WRITE_BEGIN   = 25,  /* begin preset/model upload session */
    PA_CMD_WRITE_CHUNK   = 34,  /* the body of an upload session, in chunks */
    PA_CMD_COMMIT        = 48,  /* commit an armed transfer (pending -> active) */
    PA_CMD_ACK           = 13,  /* 13/21/26/30 common acknowledge */
    PA_CMD_DEVICE_INFO_17 = 17, /* the device info record (the editor's handshake) */
    PA_CMD_DESCRIPTOR_18  = 18, /* the device descriptor */
    PA_CMD_DRAIN_21       = 21, /* drain the device's common ring */
    PA_CMD_READ_35        = 35, /* the read-shaped response (a download) */
    PA_CMD_PA_STATE       = 80, /* ours: the editor's state record (reply PA_REPLY_PA_STATE) */
    PA_CMD_PA_NAMES       = 81, /* ours: a page of names (user amps, cabinets beyond the stock 20) (reply PA_REPLY_PA_NAMES) */
    PA_CMD_PA_USER        = 82, /* ours: [op, kind, index, name...] op 0 delete, 1 rename a stored capture (kind 0) or cabinet (kind 1) (reply PA_REPLY_PA_USER) */
};

/* The reply types. The stock's reply builders take a type: 0x77F02
 * "sysex_reply_send(type, ctx, data...)" and 0x3297C
 * "sysex_reply_send_2(ctx, data, type, tag)". */
#define PA_REPLY_ACK_WRITE    0x32u  /* the write session's length echo */
#define PA_REPLY_ACK_SESSION  0x31u  /* the write session's second ack */
#define PA_REPLY_DEVICE_INFO  0x29u  /* 41 - the 8-byte state record */
#define PA_REPLY_BLOCK        0x02u  /* the 16-byte pending block */
#define PA_REPLY_BLOCK_TAG    0x22u
#define PA_REPLY_RECORD_TAG   0x0Bu
#define PA_REPLY_PA_STATE     0x60u  /* the editor state record: PA_STATE_REC_SIZE bytes (include/pa_sysex.h documents the layout) */
#define PA_REPLY_PA_NAMES     0x61u  /* [kind, total, start, n, first_user] + n names of 16 bytes (an empty name = a deleted slot) */
#define PA_REPLY_PA_USER      0x62u  /* [op, kind, index, result] result 0 = done, else a negative error as an unsigned byte */

/* The upload session, the way the standard SysEx protocol sends it:
 *   cmd 25  the payload is the declared length as a u24le, and nothing else
 *   cmd 34  the body, in chunks, straight into the session's receive area
 *   cmd 48  the armed transfer is committed */

#define PA_WRITE_ACK_TYPE   0x32

/* OTA (firmware flash) is a separate family: handshake F0 00 32 45 ...,
 * upgrade F0 22 24 35, device pulls image, finish replies "success\0". */

#endif /* PA_PROTOCOL_H */
