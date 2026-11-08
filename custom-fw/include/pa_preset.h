/* SPDX-License-Identifier: GPL-3.0-only */
/* Copyright (C) 2026 The Onyx Authors (@28spawn) */
/*
 * pa_preset.h — Pocket Amp 80-Preset Torn-Write-Safe A/B NOR Store (QW-4)
 *
 * Matches User Manual Page 4 & 6:
 *   - 80 Presets (P01..P80 / index 0..79; 1-40 factory defaults, 41-80 user slots)
 *   - Stores customizable 6+1 module order, slot bypass mask, amp/cab model indices,
 *     preset volume (0..100), and per-slot parameter tables with CRC32 integrity.
 */
#ifndef PA_PRESET_H
#define PA_PRESET_H

#include <stdint.h>
#include "pa_fx.h"

#define PA_PRESET_COUNT      80u
#define PA_PRESET_FACTORY    40u          /* P01..P40 factory, P41..P80 user (start EMPTY) */
#define PA_PRESET_MAGIC      0x32504D50u  /* "PMP2" : v2 record (types + 6 params / slot) */
#define PA_PRESET_MAGIC_V1   0x50414D50u  /* "PAMP" : legacy v1 record, migrated on boot   */
#define PA_PRESET_NAME_LEN   24u
#define PA_PRESET_NPARAMS    6u
#define PA_PRESET_FLAG_EMPTY 0x01u        /* slot holds no user sound (UI shows "EMPTY") */

typedef enum {
    PA_GENRE_CLEAN = 0,
    PA_GENRE_CRUNCH,
    PA_GENRE_LEAD,
    PA_GENRE_METAL,
    PA_GENRE_BASS,
    PA_GENRE_ACOUSTIC,
    PA_GENRE_AMBIENT,
    PA_GENRE_CUSTOM,
    PA_GENRE_COUNT
} pa_genre_tag_t;

/* v2 preset record: 164 B (bank = 16 + 80 x 164 = 13,136 B, fits the 16 KB bank).
 * Params are stored as Q16 (0..65535 = 0..1.0); the UI step is 2 %, so no audible loss. */
typedef struct {
    uint32_t magic;                             /* PA_PRESET_MAGIC ("PMP2") */
    uint32_t seq;                               /* Monotonic commit sequence (0/1 = untouched default) */
    char     name[PA_PRESET_NAME_LEN];
    uint16_t bpm;
    uint8_t  genre_tag;
    uint8_t  custom_cab_idx;                    /* 255 = stock cab */
    uint8_t  custom_cab_blend;
    int8_t   input_trim_db;                     /* -12..+12 dB per-preset trim */
    uint8_t  gate_threshold_pct;
    uint8_t  slot_enable_mask;
    uint8_t  slot_order[PA_SLOT_COUNT];
    uint8_t  amp_model_idx;                     /* 0..19 stock, 20..39 user NAM */
    uint8_t  cab_model_idx;
    uint8_t  preset_vol;                        /* 0..100 */
    uint8_t  flags;                             /* PA_PRESET_FLAG_* */
    uint8_t  slot_type[PA_SLOT_COUNT];          /* IMP-13: model/type of DYN, DRV, MOD, DLY, REV ... */
    uint8_t  stomp_map[4];                      /* IMP-07: slot toggled by VOL-, VOL+, <, > in STOMP mode */
    uint16_t params[PA_SLOT_COUNT][PA_PRESET_NPARAMS]; /* Q16 knob values, 6 per slot (AMP uses all 6) */
    uint32_t crc32;
} pa_preset_t;

static inline int32_t pa_q16_to_q21(uint16_t v) { return (int32_t)(((int64_t)v * 2097152 + 32767) / 65535); }
static inline uint16_t pa_q21_to_q16(int32_t v) {
    if (v <= 0) return 0;
    if (v >= 2097152) return 65535;
    return (uint16_t)(((int64_t)v * 65535 + 1048576) / 2097152);
}

typedef struct {
    uint32_t magic;
    uint32_t seq;
    uint32_t count;
    uint32_t bank_crc32;
    pa_preset_t presets[PA_PRESET_COUNT];
} pa_preset_bank_t;

void     pa_preset_init(void);
void     pa_preset_init_defaults(void);
int      pa_preset_factory_reset(void);
int      pa_preset_clean_user(void);        /* P41..P80 -> EMPTY, P01..P40 untouched */
int      pa_preset_load(uint8_t idx, pa_preset_t *out);
int      pa_preset_save(uint8_t idx, const pa_preset_t *in);
int      pa_preset_restore(uint8_t idx, const pa_preset_t *in);   /* store a record exactly as given (magic and CRC checked; seq and the EMPTY flag kept): a backup restore */
void     pa_preset_apply_to_chain(const pa_preset_t *p, pa_fx_chain_t *chain);
void     pa_preset_capture_from_chain(pa_preset_t *p, const pa_fx_chain_t *chain, uint8_t preset_vol,
                                      const int32_t params[PA_SLOT_COUNT][6], const uint8_t slot_type[PA_SLOT_COUNT]);
void     pa_preset_apply_smooth(const pa_preset_t *p, pa_fx_chain_t *chain);   /* Blk-02: click-free */
int      pa_preset_is_empty(uint8_t idx);
uint32_t pa_crc32(const void *data, uint32_t len);

#endif /* PA_PRESET_H */
