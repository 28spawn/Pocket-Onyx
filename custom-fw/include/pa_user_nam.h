/*
 * pa_user_nam.h — Pocket Amp Persistent User NAM Capture Store & Registry
 *
 * The registry has PA_USER_NAM_MAX_COUNT (20) logical entries:
 *   - Stock Amps: Index 0..19 (20 models)
 *   - User NAMs:  Index 20..39 (AMP 21 .. AMP 40)
 * An entry is either a capture linked into the image (tools/pa_import.py; it takes no flash) or one stored in the flash. The flash holds only
 * PA_FLASH_USER_NAM_BLOB_SLOTS (15) captures (include/pa_flash_map.h: slot 0 at 0xBB000 and 14 in the user store from 0x88000), so the 16th uploaded capture is refused (-5).
 *
 * Stored in the flash: the registry in one sector (PA_FLASH_USER_NAM_META_ADDR) and the blobs from PA_FLASH_USER_NAM_ADDR, one 8 KB slot each.
 */
#ifndef PA_USER_NAM_H
#define PA_USER_NAM_H

#include <stdint.h>
#include "pa_model.h"
#include "pa_wavenet.h"
#include "pa_flash_map.h"

#define PA_USER_NAM_MAX_COUNT 20u /* logical registry entries: captures linked into the image + captures stored in the flash */
#define PA_USER_NAM_NAME_LEN  24u
/* The largest NAM blob the firmware will accept, in bytes.
 *
 * PA_FLASH_USER_NAM_SLOT_SIZE (8 KB) is the size of a flash slot; a blob larger than it is refused whatever the RAM buffer is (see pa_user_nam_add). The RAM used to
 * hold one blob while it is read or uploaded is PA_NAM_BLOB_MAX: an A2-Lite WaveNet capture is about 7.5 KB, the GRU blob is 3,852 B. */
#if defined(PA_HOST_SIM) || defined(PA_HOST_BUILD)
#define PA_NAM_BLOB_MAX       32768u   /* the emulator and the host tests keep the full buffer */
#else
#define PA_NAM_BLOB_MAX       8192u    /* on the device the pool is full (the WaveNet block engine needs the room); the biggest real payload is the 7.5 KB WaveNet */
#endif
#define PA_USER_NAM_MAGIC     0x4E414D34u /* 'NAM4' : v4 adds logical-to-physical blob_slot indirection */
#define PA_USER_NAM_MAGIC_V3  0x4E414D33u /* 'NAM3' : migrated on boot */
#define PA_USER_NAM_MAGIC_V2  0x4E414D32u /* 'NAM2' : migrated on boot */

typedef struct {
    uint8_t  active;
    uint8_t  type;       /* 0 = GRU (3852 B blob), 1 = WaveNet floats */
    uint8_t  voicing;    /* 0..4 voicing profile */
    uint8_t  has_cab;    /* 1 if capture includes cab */
    uint8_t  blob_slot;  /* Physical flash sector index (0..19) */
    uint8_t  reserved[3];/* Alignment padding */
    uint32_t blob_len;   /* Length in bytes */
    uint32_t blob_crc32; /* Integrity CRC32 of model blob in flash */
    float    head_scale; /* WaveNet head scale */
    float    input_trim; /* Calibrated input level staging scalar */
    char     name[PA_USER_NAM_NAME_LEN];
    /* IMP-01: settings computed at import (nam_loader.py) and restored whenever the capture
     * is picked in the AMP editor. Presets still store their own AMP knobs. */
    uint8_t  amp_knobs[6];   /* GAIN, BASS, MID, TREBLE, PRES, MASTER in % (50 = neutral) */
    int8_t   makeup_db_x4;   /* output makeup in 0.25 dB steps (loudness -> -18 dBFS RMS) */
    uint8_t  flags;          /* bit0 = approximated GRU (not a faithful conversion) */
} pa_user_nam_slot_t;

typedef struct {             /* import metadata passed to pa_user_nam_add() */
    float   input_trim;
    float   makeup_db;
    uint8_t amp_knobs[6];
    uint8_t flags;
} pa_user_nam_meta_t;

typedef struct {             /* legacy v3 slot (without blob_slot) for migration */
    uint8_t  active, type, voicing, has_cab; uint32_t blob_len, blob_crc32; float head_scale, input_trim;
    char     name[PA_USER_NAM_NAME_LEN];
    uint8_t  amp_knobs[6]; int8_t makeup_db_x4; uint8_t flags;
} pa_user_nam_slot_v3_crc_t;

typedef struct {             /* legacy v3 slot (without blob_crc32) for migration */
    uint8_t  active, type, voicing, has_cab; uint32_t blob_len; float head_scale, input_trim;
    char     name[PA_USER_NAM_NAME_LEN];
    uint8_t  amp_knobs[6]; int8_t makeup_db_x4; uint8_t flags;
} pa_user_nam_slot_v3_legacy_t;

typedef struct {             /* legacy v2 slot, for migration */
    uint8_t active, type, voicing, has_cab; uint32_t blob_len; float head_scale, input_trim;
    char name[PA_USER_NAM_NAME_LEN];
} pa_user_nam_slot_v2_t;

typedef struct {
    uint32_t           magic;
    uint32_t           count;
    uint32_t           seq;
    pa_user_nam_slot_t slots[PA_USER_NAM_MAX_COUNT];
    uint32_t           crc32;
} pa_user_nam_registry_t;

void        pa_user_nam_init(void);
uint8_t     pa_user_nam_get_count(void);
uint8_t     pa_user_nam_get_active_count(void);
int         pa_user_nam_is_active(uint8_t user_idx);
const char *pa_user_nam_get_name(uint8_t user_idx);
int         pa_user_nam_get_info(uint8_t user_idx, pa_user_nam_slot_t *info);
int         pa_user_nam_add(const char *name, uint8_t type, uint8_t voicing, uint8_t has_cab,
                            float head_scale, const void *blob, uint32_t blob_len,
                            const pa_user_nam_meta_t *meta /* may be NULL -> neutral */);
void        pa_user_nam_get_amp_defaults(uint8_t user_idx, int32_t knobs_q21[6]);
int         pa_user_nam_load_into_dsp(uint8_t user_idx, pa_nam_model_t *gru_model, pa_wavenet_model_t *wn_model);
int         pa_user_nam_set_name(uint8_t user_idx, const char *name);
int         pa_user_nam_delete(uint8_t user_idx);
int         pa_user_nam_clear(void);   /* every stored capture and every blob sector wiped; the image's own captures stay; -> sectors erased or < 0 */
void        pa_user_nam_rollback(uint8_t user_idx);

#endif /* PA_USER_NAM_H */
