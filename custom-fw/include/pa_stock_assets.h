/*
 * pa_stock_assets.h - Dynamic Flash Loader & Fallback for Stock Amps and Cabs
 *
 * Reads factory AM3/AM4 amp models and cab IRs dynamically from flash partition
 * PA_FLASH_FACTORY_CAB_ADDR (0x000BF000). If no factory assets are found on flash,
 * provides clean-room synthetic default models and cabinets.
 *
 * No model weights or impulse response samples are embedded in this header.
 */
#ifndef PA_STOCK_ASSETS_H
#define PA_STOCK_ASSETS_H

#include <stdint.h>
#include "pa_model.h"

#define PA_STOCK_AMP_COUNT  20u
#define PA_STOCK_CAB_COUNT  20u
#define PA_STOCK_CAB_TAPS   1024u
#define PA_STOCK_GENERATION 4       /* 3 = AM3, 4 = AM4 */

typedef struct {
    const char *name;
    float       bass_freq;
    float       bass_min_db;
    float       bass_max_db;
    float       mid_freq;
    float       mid_min_db;
    float       mid_max_db;
    float       treble_freq;
    float       treble_min_db;
    float       treble_max_db;
    float       dc_bias;
    const uint8_t *model_blob; /* NULL when dynamic flash loading */
} pa_stock_amp_info_t;

typedef struct {
    const char   *name;
    const int32_t *taps_q18;   /* NULL when dynamic flash loading */
} pa_stock_cab_info_t;

extern pa_stock_amp_info_t g_pa_stock_amps[PA_STOCK_AMP_COUNT];

void        pa_stock_assets_init(void);
int         pa_stock_has_factory_assets(void);
const char *pa_stock_get_amp_name(uint8_t idx);
const char *pa_stock_get_cab_name(uint8_t idx);
int         pa_stock_load_amp(uint8_t idx, pa_nam_model_t *out_model, float *out_dc_bias);
int         pa_stock_load_cab(uint8_t idx, int32_t *out_taps_q18, uint32_t *out_num_taps);

/* ---- User content linked into the image (tools/pa_import.py -> src/sys/pa_user_embedded.inc) ----
 * NAM captures and cabinet IRs the owner imports on the PC. They live in the app image (read through the same
 * XIP window as the stock tables), so no flash writer is involved. With no .inc the counts are 0. */
typedef struct {
    const char    *name;
    uint8_t        type, voicing, has_cab, flags;       /* type 0 = GRU blob, 1 = WaveNet container */
    int8_t         makeup_db_x4;
    uint8_t        amp_knobs[6];
    float          head_scale, input_trim;
    uint32_t       blob_len, blob_crc32;
    const uint8_t *blob;
} pa_emb_nam_t;

int                 pa_emb_allow_wavenet(void);   /* 1 when pa_import.py --allow-wavenet linked WaveNet captures on purpose */
uint8_t             pa_emb_nam_count(void);
const pa_emb_nam_t *pa_emb_nam_get(uint8_t i);

/* All cabinets: the 20 stock ones, then the imported ones. idx >= PA_STOCK_CAB_COUNT is an imported IR. */
uint8_t     pa_cab_total_count(void);            /* the span of the type knob: stock + image + stored slots up to the last used one (holes inside it are skipped) */
int         pa_cab_is_active(uint8_t idx);        /* 0 for a deleted stored cabinet */
uint8_t     pa_cab_active_count(void);            /* how many cabinets exist */
const char *pa_cab_name(uint8_t idx);
int         pa_cab_load(uint8_t idx, int32_t *out_taps_q18, uint32_t *out_num_taps);

#endif /* PA_STOCK_ASSETS_H */
