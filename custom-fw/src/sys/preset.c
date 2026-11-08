/* SPDX-License-Identifier: GPL-3.0-only */
/* Copyright (C) 2026 The Onyx Authors (@28spawn) */
/*
 * src/sys/preset.c — Pocket Amp 80-Preset Manager & Torn-Write-Safe A/B Store (QW-4)
 */
#include <string.h>
#include "../../include/pa_preset.h"
#include "../../include/pa_audio_core.h"
#include "../../include/pa_config.h"
#include "../../include/pa_stock_assets.h"
#include "../../include/pa_cab.h"
#include "../../include/pa_user_nam.h"
#include "../dsp/fixedpoint.h"
#include "../../include/pa_wavenet.h"

static pa_preset_t g_presets[PA_PRESET_COUNT];
static uint8_t     g_presets_initialized = 0;

uint32_t pa_crc32(const void *data, uint32_t len) {
    const uint8_t *p = (const uint8_t *)data;
    uint32_t crc = 0xFFFFFFFFu;
    for (uint32_t i = 0; i < len; i++) {
        crc ^= p[i];
        for (int b = 0; b < 8; b++) {
            crc = (crc >> 1) ^ (0xEDB88320u & (uint32_t)-(int32_t)(crc & 1u));
        }
    }
    return ~crc;
}

static int32_t pa_q21_from_trim_db(int8_t db) {
    if (db <= -12) return 526793;   /* -12 dB = 0.2512 in Q21 */
    if (db >= 12)  return 8349045;  /* +12 dB = 3.9811 in Q21 */
    /* Table for -12..+12 dB, index = db + 12 */
    static const int32_t k_db_q21[25] = {
        526793,  591322,  663749,  745059,  836329,  938783,
        1053787, 1182877, 1327781, 1490442, 1673024, 1877977,
        2097152, /* 0 dB = 1.0 */
        2352002, 2637767, 2958253, 3317676, 3720786, 4172863,
        4679796, 5248386, 5885994, 6601097, 7403131, 8302581
    };
    return k_db_q21[db + 12];
}

/* ---------------------------------------------------------------------------------
 * Factory presets 1..40: original Onyx factory presets (independently authored),
 * knob percentages across the 8 slots.
 * --------------------------------------------------------------------------------- */
#include "factory_presets.inc"

static inline uint16_t pct16(uint8_t pct) { return (uint16_t)(((uint32_t)pct * 65535u + 50u) / 100u); }

/* IMP-13: CAB moved directly after AMP. The CAB/EQ nodes are mono, and placing them after the
 * stereo MOD/DLY/REV collapsed the image to (L+R)/2: stereo chorus partially cancelled (-3.6 dB
 * measured on P11) and every "stereo" preset was mono. AMP -> CAB -> EQ -> time FX is also the
 * order a mic'd amp + studio effects follows. */
static const uint8_t k_default_order[PA_SLOT_COUNT] = {
    PA_SLOT_DYNAMICS, PA_SLOT_DRIVE, PA_SLOT_AMP, PA_SLOT_CAB,
    PA_SLOT_EQ, PA_SLOT_MOD, PA_SLOT_DELAY, PA_SLOT_REVERB
};
static const uint8_t k_default_stomp[4] = { PA_SLOT_DRIVE, PA_SLOT_MOD, PA_SLOT_DELAY, PA_SLOT_REVERB };

/* Neutral knob set used by EMPTY presets and as the base of every factory preset */
static void preset_set_neutral(pa_preset_t *p) {
    for (uint8_t s = 0; s < PA_SLOT_COUNT; s++)
        for (uint8_t k = 0; k < PA_PRESET_NPARAMS; k++) p->params[s][k] = pct16(50);
    p->params[PA_SLOT_CAB][0] = pct16(100);      /* CAB level unity            */
    p->params[PA_SLOT_CAB][1] = 0;               /* CAB low-cut 20 Hz          */
    p->params[PA_SLOT_CAB][2] = 0;               /* CAB high-cut 18 kHz        */
    p->params[PA_SLOT_DRIVE][2] = pct16(50);
    p->params[PA_SLOT_REVERB][1] = 0;            /* reverb low-cut minimum     */
    p->params[PA_SLOT_DYNAMICS][3] = pct16(100); /* DYN level unity            */
    /* The delay, the reverb and the modulation were mixed far too hot: a
     * fresh preset was 25 % feedback / 20 % delay, 45 % reverb and 30 %
     * modulation, which is why they read as "exaggerated" on top of a guitar
     * that already has the amp and the cab. The defaults are now in the
     * range where an effect supports the tone instead of burying it. */
    p->params[PA_SLOT_DELAY][1] = pct16(15);   /* the feedback */
    p->params[PA_SLOT_DELAY][2] = pct16(10);   /* the mix      */
    p->params[PA_SLOT_MOD][2]   = pct16(12);   /* the mix      */
}

static void init_single_preset(pa_preset_t *p, uint32_t preset_idx) {
    memset(p, 0, sizeof(*p));
    p->magic = PA_PRESET_MAGIC;
    p->seq   = 1u;
    p->custom_cab_idx   = 255u;
    p->custom_cab_blend = 100u;
    memcpy(p->slot_order, k_default_order, sizeof(k_default_order));
    memcpy(p->stomp_map,  k_default_stomp, sizeof(k_default_stomp));
    preset_set_neutral(p);

    if (preset_idx >= PA_PRESET_FACTORY) {
        /* P41..P80: truly EMPTY. Every slot is OFF - not even a neutral
         * AMP+CAB - so a "clean" preset really is the guitar alone. */
        static const char k_empty[] = "EMPTY";
        memcpy(p->name, k_empty, sizeof(k_empty));
        p->flags            = PA_PRESET_FLAG_EMPTY;
        p->bpm              = 120u;
        p->genre_tag        = PA_GENRE_CUSTOM;
        p->slot_enable_mask = 0;
        p->amp_model_idx    = 0;
        p->cab_model_idx    = 0;
        p->slot_type[PA_SLOT_DYNAMICS] = 3;
        p->preset_vol       = 70u;
    } else {
        const pa_factory_t *f = &k_factory[preset_idx];
        for (uint32_t c = 0; c < PA_PRESET_NAME_LEN - 1u && f->name[c]; c++) p->name[c] = f->name[c];
        p->bpm           = 120u;
        p->genre_tag     = f->genre;
        p->preset_vol    = f->vol;
        p->amp_model_idx = f->amp;
        p->cab_model_idx = f->cab;
        for (uint8_t s = 0; s < PA_SLOT_COUNT; s++) {
            p->slot_type[s] = f->type[s];
            if (f->set & (1u << s))
                for (uint8_t k = 0; k < PA_PRESET_NPARAMS; k++) p->params[s][k] = pct16(f->pct[s][k]);
        }
        p->slot_enable_mask = f->mask;
    }
    p->crc32 = pa_crc32(p, (uint32_t)sizeof(*p) - sizeof(uint32_t));
}

void pa_preset_init_defaults(void) {
    for (uint32_t i = 0; i < PA_PRESET_COUNT; i++) {
        init_single_preset(&g_presets[i], i);
    }
    g_presets_initialized = 1u;
}


#include "../../include/pa_spi_nor.h"

/* Bank = 16-byte header + all presets. It MUST fit inside one bank slot, otherwise a
 * commit spills into the neighbouring bank (this was a real bug at 16 KB / 99 presets). */
_Static_assert(16u + sizeof(g_presets) <= PA_FLASH_STORE_BANK_SIZE,
               "preset bank does not fit in PA_FLASH_STORE_BANK_SIZE");

typedef struct {
    uint32_t magic;
    uint32_t seq;
    uint32_t count;
    uint32_t bank_crc32;
} pa_bank_hdr_t;

static uint32_t    g_active_bank_addr = PA_FLASH_STORE_BANK_A_ADDR;
static uint32_t    g_current_bank_seq = 1u;

/* Incremental CRC32 (same polynomial/finalisation as pa_crc32) so a bank can be
 * verified straight from flash in small chunks with no large stack buffers. */
static uint32_t crc32_update(uint32_t crc, const uint8_t *p, uint32_t len) {
    for (uint32_t i = 0; i < len; i++) {
        crc ^= p[i];
        for (int b = 0; b < 8; b++) {
            crc = (crc >> 1) ^ (0xEDB88320u & (uint32_t)-(int32_t)(crc & 1u));
        }
    }
    return crc;
}

/* Returns 1 if the bank at `addr` has a valid header and a payload whose CRC matches. */
static int bank_is_valid(uint32_t addr, pa_bank_hdr_t *hdr) {
    if (pa_spi_nor_read(addr, hdr, sizeof(*hdr)) != 0) return 0;
    if (hdr->magic != PA_PRESET_MAGIC || hdr->count != PA_PRESET_COUNT) return 0;

    uint8_t  chunk[128];
    uint32_t crc = 0xFFFFFFFFu;
    uint32_t off = 0;
    const uint32_t total = (uint32_t)sizeof(g_presets);
    while (off < total) {
        uint32_t n = total - off;
        if (n > sizeof(chunk)) n = sizeof(chunk);
        if (pa_spi_nor_read(addr + (uint32_t)sizeof(*hdr) + off, chunk, n) != 0) return 0;
        crc = crc32_update(crc, chunk, n);
        off += n;
    }
    return (~crc) == hdr->bank_crc32;
}

/* Commit ordering is what makes this power-fail safe:
 *   1. erase the INACTIVE bank (the active bank is never touched),
 *   2. program the payload,
 *   3. program the header LAST (magic/count/crc). Until step 3 completes the bank
 *      has no valid header and is ignored at boot, so a torn commit just means the
 *      previous bank wins.
 * Any flash error aborts and returns -1 without switching the active bank. */
static int commit_bank_to_flash(uint32_t bank_addr, uint32_t seq, const pa_preset_t *presets_to_write) {
    if (!presets_to_write) return -1;
    pa_bank_hdr_t hdr;
    hdr.magic      = PA_PRESET_MAGIC;
    hdr.seq        = seq;
    hdr.count      = PA_PRESET_COUNT;
    hdr.bank_crc32 = pa_crc32(presets_to_write, (uint32_t)(PA_PRESET_COUNT * sizeof(pa_preset_t)));

    int rc = 0;
    pa_spi_nor_begin();            /* one quiesce for the whole bank (4 erases + 58 page programs, about 0.2 s on the device): output muted, core 1 parked, interrupts masked */
    for (uint32_t s = 0; rc == 0 && s < (PA_FLASH_STORE_BANK_SIZE / PA_FLASH_SECTOR_SIZE); s++) {
        if (pa_spi_nor_erase_sector(bank_addr + s * PA_FLASH_SECTOR_SIZE) != 0) rc = -1;
    }
    if (rc == 0 && pa_spi_nor_write(bank_addr + (uint32_t)sizeof(hdr), presets_to_write, (uint32_t)(PA_PRESET_COUNT * sizeof(pa_preset_t))) != 0) rc = -1;
    if (rc == 0 && pa_spi_nor_write(bank_addr, &hdr, (uint32_t)sizeof(hdr)) != 0) rc = -1;      /* the header last: it commits the bank */
    pa_spi_nor_end();
    if (rc != 0) return -1;

    pa_spi_nor_sync();
    return 0;
}

#if defined(PA_HOST_SIM) || defined(PA_EMU_DLL) || defined(_WIN32) || defined(__linux__) || defined(__APPLE__)
/* Test hook: forget RAM state so pa_preset_init() re-reads flash (simulated reboot). */
void pa_preset_test_reset(void) { g_presets_initialized = 0; }
/* Test hooks: the RAM copy of a slot, and a raw commit of the RAM bank (seq untouched) - so a
 * test can store a bank "as an older firmware left it". */
pa_preset_t *pa_preset_test_slot(uint8_t idx) { return (idx < PA_PRESET_COUNT) ? &g_presets[idx] : 0; }
int pa_preset_test_commit(void) {
    uint32_t next_bank = (g_active_bank_addr == PA_FLASH_STORE_BANK_A_ADDR)
                         ? PA_FLASH_STORE_BANK_B_ADDR : PA_FLASH_STORE_BANK_A_ADDR;
    if (commit_bank_to_flash(next_bank, g_current_bank_seq + 1u, g_presets) != 0) return -1;
    g_current_bank_seq++;
    g_active_bank_addr = next_bank;
    return 0;
}
uint32_t pa_preset_test_bank_seq(void) { return g_current_bank_seq; }
#endif

/* ---- v1 -> v2 migration (184 B "PAMP" records written by the previous firmware) ---- */
typedef struct {
    uint32_t magic, seq; char name[PA_PRESET_NAME_LEN]; uint16_t bpm;
    uint8_t genre_tag, custom_cab_idx, custom_cab_blend; int8_t input_trim_db;
    uint8_t gate_threshold_pct, slot_enable_mask, slot_order[PA_SLOT_COUNT];
    uint8_t amp_model_idx, cab_model_idx, preset_vol, reserved;
    int32_t params[PA_SLOT_COUNT][4]; uint32_t crc32;
} pa_preset_v1_t;
_Static_assert(sizeof(pa_preset_v1_t) == 184u, "v1 layout");

static int migrate_v1_bank(uint32_t addr, pa_bank_hdr_t *hdr) {
    if (pa_spi_nor_read(addr, hdr, sizeof(*hdr)) != 0) return 0;
    if (hdr->magic != PA_PRESET_MAGIC_V1 || hdr->count != PA_PRESET_COUNT) return 0;
    uint32_t crc = 0xFFFFFFFFu;
    pa_preset_v1_t v1;
    for (uint32_t i = 0; i < PA_PRESET_COUNT; i++) {
        if (pa_spi_nor_read(addr + sizeof(*hdr) + i * sizeof(v1), &v1, sizeof(v1)) != 0) return 0;
        crc = crc32_update(crc, (const uint8_t *)&v1, sizeof(v1));
    }
    if ((~crc) != hdr->bank_crc32) return 0;

    for (uint32_t i = 0; i < PA_PRESET_COUNT; i++) {
        pa_spi_nor_read(addr + sizeof(*hdr) + i * sizeof(v1), &v1, sizeof(v1));
        pa_preset_t *p = &g_presets[i];
        init_single_preset(p, i);                       /* factory P01-P40 / EMPTY P41-P80 */
        int user_saved = (v1.seq > 1u) && (v1.name[0] != 'I' || v1.name[1] != 'n'); /* not "Init NN" */
        if (i >= PA_PRESET_FACTORY && user_saved) {     /* keep real user work, convert format */
            memcpy(p->name, v1.name, PA_PRESET_NAME_LEN);
            p->seq = v1.seq; p->bpm = v1.bpm; p->genre_tag = v1.genre_tag;
            p->input_trim_db = v1.input_trim_db; p->slot_enable_mask = v1.slot_enable_mask;
            memcpy(p->slot_order, v1.slot_order, PA_SLOT_COUNT);
            p->amp_model_idx = v1.amp_model_idx; p->cab_model_idx = v1.cab_model_idx;
            p->slot_type[PA_SLOT_AMP] = v1.amp_model_idx; p->slot_type[PA_SLOT_CAB] = v1.cab_model_idx;
            p->preset_vol = v1.preset_vol; p->flags = 0;
            for (int s = 0; s < PA_SLOT_COUNT; s++)
                for (int k = 0; k < 4; k++) p->params[s][k] = pa_q21_to_q16(v1.params[s][k]);
            p->crc32 = pa_crc32(p, (uint32_t)sizeof(*p) - sizeof(uint32_t));
        }
    }
    return 1;
}

/* A firmware update that changes the factory bank (2026-10-24: the AM4 presets) must reach a
 * device that already has a bank in flash - but never at the cost of the user's own sounds.
 * A factory slot the user has never saved still has seq 1 (init_single_preset; every save
 * bumps it, pa_preset_save): those follow the firmware's table, the rest are left alone. One
 * bank commit, and only when something actually changed. */
static void refresh_untouched_factory(void) {
    int changed = 0;
    for (uint32_t i = 0; i < PA_PRESET_FACTORY; i++) {
        if (g_presets[i].seq != 1u) continue;                 /* saved by the user: keep */
        pa_preset_t fresh;
        init_single_preset(&fresh, i);
        if (memcmp(&fresh, &g_presets[i], sizeof(fresh)) != 0) {
            g_presets[i] = fresh;
            changed = 1;
        }
    }
    if (!changed) return;
    uint32_t next_bank = (g_active_bank_addr == PA_FLASH_STORE_BANK_A_ADDR)
                         ? PA_FLASH_STORE_BANK_B_ADDR : PA_FLASH_STORE_BANK_A_ADDR;
    if (commit_bank_to_flash(next_bank, g_current_bank_seq + 1u, g_presets) == 0) {
        g_current_bank_seq++;
        g_active_bank_addr = next_bank;
    }
}

void pa_preset_init(void) {
    if (g_presets_initialized) return;

    pa_spi_nor_init();

    pa_bank_hdr_t hdr_a, hdr_b;
    int a_valid = bank_is_valid(PA_FLASH_STORE_BANK_A_ADDR, &hdr_a);
    int b_valid = bank_is_valid(PA_FLASH_STORE_BANK_B_ADDR, &hdr_b);

    /* Wrap-safe "B is newer than A" */
    int b_newer = b_valid && (!a_valid || (int32_t)(hdr_b.seq - hdr_a.seq) > 0);

    if (b_newer) {
        pa_spi_nor_read(PA_FLASH_STORE_BANK_B_ADDR + sizeof(hdr_b), g_presets, sizeof(g_presets));
        g_active_bank_addr = PA_FLASH_STORE_BANK_B_ADDR;
        g_current_bank_seq = hdr_b.seq;
        g_presets_initialized = 1u;
        refresh_untouched_factory();
    } else if (a_valid) {
        pa_spi_nor_read(PA_FLASH_STORE_BANK_A_ADDR + sizeof(hdr_a), g_presets, sizeof(g_presets));
        g_active_bank_addr = PA_FLASH_STORE_BANK_A_ADDR;
        g_current_bank_seq = hdr_a.seq;
        g_presets_initialized = 1u;
        refresh_untouched_factory();
    } else {
        /* No v2 bank: migrate the newest valid v1 bank (keeps user P41-P80), else factory. */
        pa_bank_hdr_t h1a, h1b;
        int m = 0;
        if (!m) m = migrate_v1_bank(PA_FLASH_STORE_BANK_B_ADDR, &h1b);
        if (!m) m = migrate_v1_bank(PA_FLASH_STORE_BANK_A_ADDR, &h1a);
        if (!m) pa_preset_init_defaults();
        g_presets_initialized = 1u;
        g_active_bank_addr = PA_FLASH_STORE_BANK_A_ADDR;
        g_current_bank_seq = 1u;
        /* Write v2 into B first (A may hold the only v1 copy); B becomes active, the next save goes to A. */
        if (commit_bank_to_flash(PA_FLASH_STORE_BANK_B_ADDR, 1u, g_presets) == 0) {
            g_active_bank_addr = PA_FLASH_STORE_BANK_B_ADDR;
        }
    }
}

int pa_preset_is_empty(uint8_t idx) {
    if (idx >= PA_PRESET_COUNT) return 1;
    if (!g_presets_initialized) pa_preset_init();
    return (g_presets[idx].flags & PA_PRESET_FLAG_EMPTY) ? 1 : 0;
}

int pa_preset_load(uint8_t idx, pa_preset_t *out) {
    if (!out || idx >= PA_PRESET_COUNT) return -1;
    if (!g_presets_initialized) pa_preset_init();

    const pa_preset_t *src = &g_presets[idx];
    uint32_t expected_crc = pa_crc32(src, (uint32_t)sizeof(*src) - sizeof(uint32_t));
    if (src->magic != PA_PRESET_MAGIC || src->crc32 != expected_crc) {
        return -2;
    }
    *out = *src;
    return 0;
}

/* Returns 0 only if the preset is durably in flash; -1 if the flash commit failed
 * (the RAM copy reverts to its previous valid state). */
int pa_preset_save(uint8_t idx, const pa_preset_t *in) {
    if (!in || idx >= PA_PRESET_COUNT) return -1;
    if (!g_presets_initialized) pa_preset_init();

    pa_preset_t backup = g_presets[idx];

    pa_preset_t *dst = &g_presets[idx];
    uint32_t next_seq = dst->seq + 1u;
    *dst       = *in;
    dst->magic = PA_PRESET_MAGIC;
    dst->flags &= (uint8_t)~PA_PRESET_FLAG_EMPTY;   /* a saved slot is never EMPTY */
    dst->seq   = next_seq;
    dst->crc32 = pa_crc32(dst, (uint32_t)sizeof(*dst) - sizeof(uint32_t));

    /* Flip to alternate bank for atomic torn-write-safe commit */
    uint32_t next_bank = (g_active_bank_addr == PA_FLASH_STORE_BANK_A_ADDR)
                         ? PA_FLASH_STORE_BANK_B_ADDR
                         : PA_FLASH_STORE_BANK_A_ADDR;
    if (commit_bank_to_flash(next_bank, g_current_bank_seq + 1u, g_presets) != 0) {
        g_presets[idx] = backup;
        return -1;
    }
    g_current_bank_seq++;
    g_active_bank_addr = next_bank;
    return 0;
}

/* A backup restore: the record goes in exactly as it was read (seq and the EMPTY flag included), so restoring a backup of an untouched factory slot or an empty user slot leaves it
 * untouched and empty instead of turning it into a "saved" preset. The record must carry its magic and a matching CRC. Same torn-write-safe bank flip as pa_preset_save. */
int pa_preset_restore(uint8_t idx, const pa_preset_t *in) {
    if (!in || idx >= PA_PRESET_COUNT) return -1;
    if (in->magic != PA_PRESET_MAGIC || in->crc32 != pa_crc32(in, (uint32_t)sizeof(*in) - sizeof(uint32_t))) return -2;
    if (!g_presets_initialized) pa_preset_init();

    pa_preset_t backup = g_presets[idx];
    g_presets[idx] = *in;
    uint32_t next_bank = (g_active_bank_addr == PA_FLASH_STORE_BANK_A_ADDR) ? PA_FLASH_STORE_BANK_B_ADDR : PA_FLASH_STORE_BANK_A_ADDR;
    if (commit_bank_to_flash(next_bank, g_current_bank_seq + 1u, g_presets) != 0) {
        g_presets[idx] = backup;
        return -1;
    }
    g_current_bank_seq++;
    g_active_bank_addr = next_bank;
    return 0;
}

/* One scratch bank shared by the factory reset and the user-preset clean (they never run together; a second copy overflowed the pool). */
static pa_preset_t s_reset_scratch[PA_PRESET_COUNT] __attribute__((section(".pool"), aligned(4)));

/* Erase B first, then rewrite A: if power drops in between, A (old data) is still
 * the only valid bank, so a half-finished reset can never "resurrect" newer data.
 * Restores factory presets P01..P40 while preserving user presets P41..P80. */
int pa_preset_factory_reset(void) {
    if (!g_presets_initialized) pa_preset_init();

    /* Build reset bank in scratch buffer before touching g_presets or committing */
    memcpy(s_reset_scratch, g_presets, sizeof(s_reset_scratch));

    /* Reset factory bank P01..P40; user presets P41..P80 remain intact */
    for (uint32_t i = 0; i < 40u; i++) {
        init_single_preset(&s_reset_scratch[i], i);
    }

    uint32_t next_bank = (g_active_bank_addr == PA_FLASH_STORE_BANK_A_ADDR)
                         ? PA_FLASH_STORE_BANK_B_ADDR
                         : PA_FLASH_STORE_BANK_A_ADDR;
    if (commit_bank_to_flash(next_bank, g_current_bank_seq + 1u, s_reset_scratch) != 0) {
        /* Commit failed: g_presets was never touched */
        return -1;
    }
    /* Adopt scratch bank only after flash commit succeeds */
    memcpy(g_presets, s_reset_scratch, sizeof(g_presets));
    g_current_bank_seq++;
    g_active_bank_addr = next_bank;
    return 0;
}

/* CLEAN USER PRESETS: P41..P80 back to EMPTY, the stock presets P01..P40 untouched (the mirror of the factory reset).
 * Same torn-write safety: built in a scratch bank, committed to the other bank, adopted only after the commit. */
int pa_preset_clean_user(void) {
    if (!g_presets_initialized) pa_preset_init();

    memcpy(s_reset_scratch, g_presets, sizeof(s_reset_scratch));
    for (uint32_t i = PA_PRESET_FACTORY; i < PA_PRESET_COUNT; i++) {
        init_single_preset(&s_reset_scratch[i], i);
    }

    uint32_t next_bank = (g_active_bank_addr == PA_FLASH_STORE_BANK_A_ADDR)
                         ? PA_FLASH_STORE_BANK_B_ADDR
                         : PA_FLASH_STORE_BANK_A_ADDR;
    if (commit_bank_to_flash(next_bank, g_current_bank_seq + 1u, s_reset_scratch) != 0) return -1;
    memcpy(g_presets, s_reset_scratch, sizeof(g_presets));
    g_current_bank_seq++;
    g_active_bank_addr = next_bank;
    return 0;
}

/* Number of knobs each node actually consumes (DSP param ids 0..n-1) */
static const uint8_t k_slot_nparams[PA_SLOT_COUNT] = {
    [PA_SLOT_DYNAMICS] = 4, [PA_SLOT_DRIVE] = 3, [PA_SLOT_AMP] = 6, [PA_SLOT_CAB] = 3,
    [PA_SLOT_EQ] = 5, [PA_SLOT_MOD] = 4, [PA_SLOT_DELAY] = 4, [PA_SLOT_REVERB] = 4
};

__attribute__((weak)) void pa_nam_prewarm(const pa_nam_model_t *m) { (void)m; }

/* NOT click-safe on its own: call pa_preset_apply_smooth() from the UI thread. */
void pa_preset_apply_to_chain(const pa_preset_t *p, pa_fx_chain_t *chain) {
    if (!p || !chain) return;
    pa_fx_chain_reset_states(chain);
    for (uint8_t s = 0; s < PA_SLOT_COUNT; s++) {
        chain->slot_order[s] = p->slot_order[s] < PA_SLOT_COUNT ? p->slot_order[s] : s;
        int en = (p->slot_enable_mask & (1u << s)) ? 1 : 0;
        pa_fx_chain_set_slot_enabled_now(chain, (pa_fx_slot_id_t)s, en);   /* chain is muted here */
        if (s != PA_SLOT_AMP && s != PA_SLOT_CAB) {                 /* the EQ has types now too */
            pa_fx_chain_set_slot_type(chain, (pa_fx_slot_id_t)s, p->slot_type[s]);   /* IMP-13 */
        }
        for (int k = 0; k < k_slot_nparams[s]; k++) {
            pa_fx_chain_set_param(chain, (pa_fx_slot_id_t)s, k, pa_q16_to_q21(p->params[s][k]));
        }
    }
    pa_fx_chain_set_preset_vol(chain, p->preset_vol);
    pa_fx_chain_set_input_gain(chain, pa_q21_from_trim_db(p->input_trim_db));
    pa_fx_chain_relink_ordered(chain);

    /* Load stock amp model or user NAM capture designated for this preset */
    extern pa_wavenet_model_t g_wn_model;
    extern int g_active_amp_engine;
    if (chain->slots[PA_SLOT_AMP] && chain->slots[PA_SLOT_AMP]->state) {
        uint8_t effective_model = p->amp_model_idx;
        if (p->amp_model_idx < PA_STOCK_AMP_COUNT) {
            pa_stock_load_amp(p->amp_model_idx, (pa_nam_model_t *)chain->slots[PA_SLOT_AMP]->state, NULL);
            g_active_amp_engine = 0;
            g_wn_model.active = 0;
        } else {
            uint8_t u_idx = p->amp_model_idx - PA_STOCK_AMP_COUNT;
            if (pa_user_nam_load_into_dsp(u_idx, (pa_nam_model_t *)chain->slots[PA_SLOT_AMP]->state, &g_wn_model) != 0) {
                /* Fallback to stock amp model 0 if user capture is missing or corrupted */
                pa_stock_load_amp(0, (pa_nam_model_t *)chain->slots[PA_SLOT_AMP]->state, NULL);
                g_active_amp_engine = 0;
                g_wn_model.active = 0;
                effective_model = 0;
            }
        }
        pa_amp_set_model_profile(effective_model);     /* Blk-01: voicing + output trim */
        for (int k = 0; k < 6; k++)                      /* re-apply knobs on the new profile */
            pa_fx_chain_set_param(chain, PA_SLOT_AMP, k, pa_q16_to_q21(p->params[PA_SLOT_AMP][k]));
        if (g_active_amp_engine == 0) {
            pa_nam_prewarm((const pa_nam_model_t *)chain->slots[PA_SLOT_AMP]->state);
        }
    }
    if (chain->slots[PA_SLOT_CAB]) {
        static int32_t c_taps[PA_STOCK_CAB_TAPS];        /* static: 4 KB off the UI stack */
        uint32_t n_taps = PA_STOCK_CAB_TAPS;
        if (pa_cab_load(p->cab_model_idx, c_taps, &n_taps) == 0) {
            fx_cab_ir_load_q18(chain->slots[PA_SLOT_CAB], c_taps, n_taps);
            fx_cab_ir_set_stock_trim(chain->slots[PA_SLOT_CAB], p->cab_model_idx);
        }
    }
}

/* Blk-02: fade the output to silence, apply the preset, fade back in (~6 ms out +
 * ~6 ms in, so no discontinuity reaches the DAC).
 *
 * The swap is performed BY THE AUDIO THREAD at the next block boundary, once
 * the gain has reached zero. The UI thread used to BLOCK here waiting for the
 * mute, which stalled the UI for up to 30 ms on every preset change and could
 * never complete on a host or WebAssembly build, where the clock only advances
 * between ticks. */
void pa_preset_apply_smooth(const pa_preset_t *p, pa_fx_chain_t *chain) {
    (void)chain;
    pa_audio_core_request_preset(p);
}

void pa_preset_capture_from_chain(pa_preset_t *p, const pa_fx_chain_t *chain, uint8_t preset_vol,
                                  const int32_t params[PA_SLOT_COUNT][6], const uint8_t slot_type[PA_SLOT_COUNT]) {
    if (!p || !chain) return;
    p->magic = PA_PRESET_MAGIC;
    p->preset_vol = preset_vol;
    p->slot_enable_mask = 0;
    p->flags &= (uint8_t)~PA_PRESET_FLAG_EMPTY;
    for (uint8_t s = 0; s < PA_SLOT_COUNT; s++) {
        p->slot_order[s] = chain->slot_order[s];
        if (chain->slots[s] && (chain->slots[s]->active || chain->slots[s]->tail_draining)) {
            p->slot_enable_mask |= (1u << s);
        }
        if (slot_type) p->slot_type[s] = slot_type[s];
        for (int k = 0; k < (int)PA_PRESET_NPARAMS; k++) {
            p->params[s][k] = params ? pa_q21_to_q16(params[s][k]) : 0;
        }
    }
    if (slot_type) { p->amp_model_idx = slot_type[PA_SLOT_AMP]; p->cab_model_idx = slot_type[PA_SLOT_CAB]; }
}
