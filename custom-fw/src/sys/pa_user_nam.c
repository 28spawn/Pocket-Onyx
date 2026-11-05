/*
 * src/sys/pa_user_nam.c — Pocket Amp Persistent User NAM Capture Store & Registry
 *
 * Implements flash storage and model loading for the user NAM captures (see include/pa_user_nam.h and include/pa_flash_map.h):
 *   - Metadata registry at PA_FLASH_USER_NAM_META_ADDR (one sector, updated in place with a sequence number and a CRC32)
 *   - Model blobs at pa_flash_nam_slot_addr(slot) (PA_FLASH_USER_NAM_BLOB_SLOTS = 15 slots of 8 KB: 0xBB000, then 14 in the user store from 0x88000)
 *   - Zero dynamic allocations during audio processing
 */
#include <string.h>
#include <stdio.h>
#include <math.h>
#include "../../include/pa_user_nam.h"
#include "../../include/pa_spi_nor.h"
#include "../../include/pa_flash_map.h"
#include "../../include/pa_preset.h"
#include "../../include/pa_fx.h"
#include "../../include/pa_stock_assets.h"
#include "../../include/pa_xip.h"

/* flags bit 7 of a registry slot: the capture is linked into the image (pa_emb_nam_t), blob_slot is its index there */
#define NAM_FLAG_EMBEDDED 0x80u

#define NAM_META_ADDR_A  (PA_FLASH_USER_NAM_META_A_ADDR)
#define NAM_META_ADDR_B  (PA_FLASH_USER_NAM_META_B_ADDR)
_Static_assert(sizeof(pa_user_nam_registry_t) <= PA_FLASH_SECTOR_SIZE, "registry must fit one sector");

static pa_user_nam_registry_t g_nam_registry;
static uint32_t               s_nam_seq = 0;
static uint32_t               s_nam_active = NAM_META_ADDR_A;
static int commit_registry_to_flash(void);
_Static_assert(sizeof(pa_user_nam_registry_t) <= PA_FLASH_USER_NAM_META_SIZE, "the NAM registry must fit its half of the shared sector");
static uint8_t                g_nam_registry_inited = 0;

/* Transactional backup state for rollback */
static pa_user_nam_slot_t     s_backup_slot;
static int                    s_backup_idx = -1;
static int                    s_backup_valid = 0;

static int read_valid_nam_registry(uint32_t addr, pa_user_nam_registry_t *out) {
    if (pa_spi_nor_read(addr, out, sizeof(*out)) != 0) return 0;
    if (out->magic != PA_USER_NAM_MAGIC || out->count > PA_USER_NAM_MAX_COUNT) return 0;
    return out->crc32 == pa_crc32(out, (uint32_t)sizeof(*out) - sizeof(uint32_t));
}

/* Global FX chain (defined in main.c) */
extern pa_fx_chain_t g_fx_chain;

/* Clean up input file path/name into an 18-char display name */
static void clean_nam_name(const char *in, char *out, int max_len) {
    if (!in || !out) return;
    const char *p = in;
    const char *slash = strrchr(p, '/');
    if (slash) p = slash + 1;
    slash = strrchr(p, '\\');
    if (slash) p = slash + 1;

    int di = 0;
    while (*p && *p != '.' && di < (max_len - 1)) {
        if (*p >= 32 && *p <= 126) {
            out[di++] = *p;
        }
        p++;
    }
    while (di > 0 && out[di - 1] == ' ') di--;
    out[di] = '\0';
    if (di == 0 && max_len > 0) {
        int i = 0;
        const char *def = "USER NAM";
        while (def[i] && i < max_len - 1) { out[i] = def[i]; i++; }
        out[i] = '\0';
    }
}

static void slot_set_neutral(pa_user_nam_slot_t *sl) {
    for (int k = 0; k < 6; k++) sl->amp_knobs[k] = 50u;
    sl->makeup_db_x4 = 0;
    sl->flags = 0;
}

static void nam_init_body(void) {
    if (g_nam_registry_inited) return;
    pa_spi_nor_init();

    pa_user_nam_registry_t reg_a, reg_b;
    int va = read_valid_nam_registry(NAM_META_ADDR_A, &reg_a);
    int vb = read_valid_nam_registry(NAM_META_ADDR_B, &reg_b);
    if (vb && (!va || (int32_t)(reg_b.seq - reg_a.seq) > 0)) {
        g_nam_registry = reg_b;
        s_nam_active   = NAM_META_ADDR_B;
        s_nam_seq      = reg_b.seq;
        g_nam_registry_inited = 1;
        return;
    }
    if (va) {
        g_nam_registry = reg_a;
        s_nam_active   = NAM_META_ADDR_A;
        s_nam_seq      = reg_a.seq;
        g_nam_registry_inited = 1;
        return;
    }

    /* Check legacy v3 with blob_crc32 */
    struct {
        uint32_t                  magic;
        uint32_t                  count;
        uint32_t                  seq;
        pa_user_nam_slot_v3_crc_t slots[PA_USER_NAM_MAX_COUNT];
        uint32_t                  crc32;
    } leg_v3_crc;
    for (int b = 0; b < 2; b++) {
        uint32_t addr = (b == 0) ? NAM_META_ADDR_A : NAM_META_ADDR_B;
        if (pa_spi_nor_read(addr, &leg_v3_crc, sizeof(leg_v3_crc)) == 0 &&
            leg_v3_crc.magic == PA_USER_NAM_MAGIC_V3 && leg_v3_crc.count <= PA_USER_NAM_MAX_COUNT &&
            leg_v3_crc.crc32 == pa_crc32(&leg_v3_crc, (uint32_t)sizeof(leg_v3_crc) - sizeof(uint32_t))) {
            memset(&g_nam_registry, 0, sizeof(g_nam_registry));
            g_nam_registry.magic = PA_USER_NAM_MAGIC;
            g_nam_registry.count = leg_v3_crc.count;
            g_nam_registry.seq   = leg_v3_crc.seq;
            for (uint32_t i = 0; i < leg_v3_crc.count; i++) {
                pa_user_nam_slot_t *d = &g_nam_registry.slots[i];
                const pa_user_nam_slot_v3_crc_t *o = &leg_v3_crc.slots[i];
                d->active = o->active; d->type = o->type; d->voicing = o->voicing; d->has_cab = o->has_cab;
                d->blob_slot = (uint8_t)i;
                d->blob_len = o->blob_len; d->blob_crc32 = o->blob_crc32;
                d->head_scale = o->head_scale; d->input_trim = o->input_trim;
                memcpy(d->name, o->name, sizeof(d->name));
                for (int k = 0; k < 6; k++) d->amp_knobs[k] = o->amp_knobs[k];
                d->makeup_db_x4 = o->makeup_db_x4;
                d->flags = o->flags;
            }
            s_nam_active = addr;
            s_nam_seq    = leg_v3_crc.seq;
            g_nam_registry_inited = 1;
            commit_registry_to_flash();
            return;
        }
    }

    /* Check legacy single-sector v3 (without seq or blob_crc32) */
    struct {
        uint32_t                     magic;
        uint32_t                     count;
        pa_user_nam_slot_v3_legacy_t slots[PA_USER_NAM_MAX_COUNT];
        uint32_t                     crc32;
    } leg_v3;
    if (pa_spi_nor_read(NAM_META_ADDR_A, &leg_v3, sizeof(leg_v3)) == 0 &&
        leg_v3.magic == PA_USER_NAM_MAGIC_V3 && leg_v3.count <= PA_USER_NAM_MAX_COUNT &&
        leg_v3.crc32 == pa_crc32(&leg_v3, (uint32_t)sizeof(leg_v3) - sizeof(uint32_t))) {
        memset(&g_nam_registry, 0, sizeof(g_nam_registry));
        g_nam_registry.magic = PA_USER_NAM_MAGIC;
        g_nam_registry.count = leg_v3.count;
        for (uint32_t i = 0; i < leg_v3.count; i++) {
            pa_user_nam_slot_t *d = &g_nam_registry.slots[i];
            const pa_user_nam_slot_v3_legacy_t *o = &leg_v3.slots[i];
            d->active = o->active; d->type = o->type; d->voicing = o->voicing; d->has_cab = o->has_cab;
            d->blob_slot = (uint8_t)i;
            d->blob_len = o->blob_len; d->head_scale = o->head_scale; d->input_trim = o->input_trim;
            memcpy(d->name, o->name, sizeof(d->name));
            for (int k = 0; k < 6; k++) d->amp_knobs[k] = o->amp_knobs[k];
            d->makeup_db_x4 = o->makeup_db_x4;
            d->flags = o->flags;
            d->blob_crc32 = 0;
        }
        s_nam_active = NAM_META_ADDR_A;
        s_nam_seq    = 1u;
        g_nam_registry_inited = 1;
        commit_registry_to_flash();
        return;
    }

    /* v2 registry (40 B slots) -> v4: keep every capture, start them on neutral AMP knobs */
    struct { uint32_t magic, count; pa_user_nam_slot_v2_t slots[PA_USER_NAM_MAX_COUNT]; uint32_t crc32; } v2;
    memset(&g_nam_registry, 0, sizeof(g_nam_registry));
    g_nam_registry.magic = PA_USER_NAM_MAGIC;
    if (pa_spi_nor_read(NAM_META_ADDR_A, &v2, sizeof(v2)) == 0 &&
        v2.magic == PA_USER_NAM_MAGIC_V2 && v2.count <= PA_USER_NAM_MAX_COUNT &&
        v2.crc32 == pa_crc32(&v2, (uint32_t)sizeof(v2) - sizeof(uint32_t))) {
        g_nam_registry.count = v2.count;
        for (uint32_t i = 0; i < v2.count; i++) {
            pa_user_nam_slot_t *d = &g_nam_registry.slots[i];
            const pa_user_nam_slot_v2_t *o = &v2.slots[i];
            d->active = o->active; d->type = o->type; d->voicing = o->voicing; d->has_cab = o->has_cab;
            d->blob_slot = (uint8_t)i;
            d->blob_len = o->blob_len; d->head_scale = o->head_scale; d->input_trim = o->input_trim;
            memcpy(d->name, o->name, sizeof(d->name));
            slot_set_neutral(d);
        }
        s_nam_active = NAM_META_ADDR_A;
        s_nam_seq    = 1u;
        g_nam_registry_inited = 1;
        commit_registry_to_flash();
        return;
    }
    g_nam_registry.count = 0;      /* blank flash */
    s_nam_active = NAM_META_ADDR_A;
    s_nam_seq = 0;
    /* The captures imported on the PC and linked into the image (tools/pa_import.py). Nothing is written to the flash. */
    for (uint8_t i = 0; i < pa_emb_nam_count() && g_nam_registry.count < PA_USER_NAM_MAX_COUNT; i++) {
        const pa_emb_nam_t *e = pa_emb_nam_get(i);
        pa_user_nam_slot_t *d = &g_nam_registry.slots[g_nam_registry.count++];
        d->active = 1; d->type = e->type; d->voicing = e->voicing; d->has_cab = e->has_cab;
        d->blob_slot = i; d->blob_len = e->blob_len; d->blob_crc32 = e->blob_crc32;
        d->head_scale = e->head_scale; d->input_trim = e->input_trim;
        for (uint32_t k = 0; k < PA_USER_NAM_NAME_LEN - 1u && e->name[k]; k++) d->name[k] = e->name[k];
        for (int k = 0; k < 6; k++) d->amp_knobs[k] = e->amp_knobs[k];
        d->makeup_db_x4 = e->makeup_db_x4;
        d->flags = (uint8_t)(e->flags | NAM_FLAG_EMBEDDED);
    }
    g_nam_registry_inited = 1;
}

/* A registry saved by a build that linked captures into the image still names them (NAM_FLAG_EMBEDDED, blob_slot = the index in the image). A build with fewer of them (or none: pack 192 removed the three
 * it had, to give the flash store their room) leaves those entries pointing at nothing: they become free logical slots, in RAM now and in the flash with the next registry commit. A capture stored in
 * the flash keeps its logical index, so the presets that use it still find it. */
static void drop_dangling_embedded(void) {
    for (uint32_t i = 0; i < g_nam_registry.count; i++) {
        pa_user_nam_slot_t *sl = &g_nam_registry.slots[i];
        if (sl->active && (sl->flags & NAM_FLAG_EMBEDDED) && sl->blob_slot >= pa_emb_nam_count()) sl->active = 0;
    }
    while (g_nam_registry.count > 0 && !g_nam_registry.slots[g_nam_registry.count - 1].active) g_nam_registry.count--;
}

void pa_user_nam_init(void) {
    if (g_nam_registry_inited) return;
    nam_init_body();
    drop_dangling_embedded();
}

void pa_user_nam_get_amp_defaults(uint8_t user_idx, int32_t knobs_q21[6]) {
    if (!g_nam_registry_inited) pa_user_nam_init();
    for (int k = 0; k < 6; k++) {
        uint8_t pct = 50u;
        if (user_idx < g_nam_registry.count && g_nam_registry.slots[user_idx].active)
            pct = g_nam_registry.slots[user_idx].amp_knobs[k];
        knobs_q21[k] = (int32_t)(((int64_t)pct * (1 << 21)) / 100);
    }
}

uint8_t pa_user_nam_get_count(void) {
    if (!g_nam_registry_inited) pa_user_nam_init();
    return (uint8_t)g_nam_registry.count;
}

uint8_t pa_user_nam_get_active_count(void) {
    if (!g_nam_registry_inited) pa_user_nam_init();
    uint8_t active = 0;
    for (uint32_t i = 0; i < g_nam_registry.count; i++) {
        if (g_nam_registry.slots[i].active) active++;
    }
    return active;
}

int pa_user_nam_is_active(uint8_t user_idx) {
    if (!g_nam_registry_inited) pa_user_nam_init();
    if (user_idx >= g_nam_registry.count) return 0;
    return g_nam_registry.slots[user_idx].active ? 1 : 0;
}

const char *pa_user_nam_get_name(uint8_t user_idx) {
    if (!g_nam_registry_inited) pa_user_nam_init();
    if (user_idx < g_nam_registry.count && g_nam_registry.slots[user_idx].active) {
        return g_nam_registry.slots[user_idx].name;
    }
    return "MISSING";
}

int pa_user_nam_get_info(uint8_t user_idx, pa_user_nam_slot_t *info) {
    if (!info) return -1;
    if (!g_nam_registry_inited) pa_user_nam_init();
    if (user_idx >= g_nam_registry.count || !g_nam_registry.slots[user_idx].active) return -2;
    *info = g_nam_registry.slots[user_idx];
    return 0;
}

static int commit_registry_to_flash(void) {
    g_nam_registry.magic = PA_USER_NAM_MAGIC;
    g_nam_registry.seq   = s_nam_seq + 1u;
    g_nam_registry.crc32 = pa_crc32(&g_nam_registry, (uint32_t)sizeof(g_nam_registry) - sizeof(uint32_t));

    /* the registry shares its sector with the settings log: the settings record is put back after the erase (settings.c) */
    extern int pa_flash_shared_rewrite(const void *reg, uint32_t reg_len);
    if (pa_flash_shared_rewrite(&g_nam_registry, (uint32_t)sizeof(g_nam_registry)) != 0) return -1;
    s_nam_active = NAM_META_ADDR_A;
    s_nam_seq    = g_nam_registry.seq;
    return 0;
}

int pa_user_nam_add(const char *name, uint8_t type, uint8_t voicing, uint8_t has_cab,
                    float head_scale, const void *blob, uint32_t blob_len,
                    const pa_user_nam_meta_t *meta) {
    if (!blob || blob_len == 0 || blob_len > PA_NAM_BLOB_MAX || blob_len > PA_FLASH_USER_NAM_SLOT_SIZE) return -1;
    if (!g_nam_registry_inited) pa_user_nam_init();

    char clean[PA_USER_NAM_NAME_LEN];
    clean_nam_name(name, clean, sizeof(clean));

    /* 1. Identify logical slot. Presets reference amp_model_idx = 20 + logical_idx,
     * so logical_idx must remain unchanged when updating an existing model. */
    int logical_idx = -1;
    for (uint32_t i = 0; i < g_nam_registry.count; i++) {
        if (g_nam_registry.slots[i].active && strcmp(g_nam_registry.slots[i].name, clean) == 0) {
            logical_idx = (int)i;
            break;
        }
    }
    if (logical_idx < 0) {
        /* Reuse any inactive/tombstoned logical slot first */
        for (uint32_t i = 0; i < g_nam_registry.count; i++) {
            if (!g_nam_registry.slots[i].active) {
                logical_idx = (int)i;
                break;
            }
        }
        if (logical_idx < 0) {
            if (g_nam_registry.count < PA_USER_NAM_MAX_COUNT) {
                logical_idx = (int)g_nam_registry.count;
            } else {
                return -5; /* Store full: all 20 logical slots active */
            }
        }
    }

    /* Snapshot existing slot for transactional rollback if replacing an existing model */
    if (logical_idx >= 0 && (uint32_t)logical_idx < g_nam_registry.count && g_nam_registry.slots[logical_idx].active) {
        s_backup_slot = g_nam_registry.slots[logical_idx];
        s_backup_idx = logical_idx;
        s_backup_valid = 1;
    } else {
        s_backup_valid = 0;
        s_backup_idx = logical_idx;
    }

    /* 2. Find a free physical blob_slot for atomic copy-on-write */
    uint8_t used_phys[PA_FLASH_USER_NAM_BLOB_SLOTS];
    memset(used_phys, 0, sizeof(used_phys));
    for (uint32_t i = 0; i < g_nam_registry.count; i++) {
        /* A capture linked into the image names its index in the image in blob_slot, not a flash slot: it uses no flash. Counting it here made the
         * image's captures eat flash slots (and, with a single slot, every upload fail). */
        if (g_nam_registry.slots[i].active && !(g_nam_registry.slots[i].flags & NAM_FLAG_EMBEDDED)) {
            uint8_t bs = g_nam_registry.slots[i].blob_slot;
            if (bs < PA_FLASH_USER_NAM_BLOB_SLOTS) used_phys[bs] = 1;
        }
    }

    int target_blob_slot = -1;
    for (int p = 0; p < (int)PA_FLASH_USER_NAM_BLOB_SLOTS; p++) {
        if (!used_phys[p]) {
            target_blob_slot = p;
            break;
        }
    }
    if (target_blob_slot < 0 && s_backup_valid && logical_idx >= 0 &&
        !(g_nam_registry.slots[logical_idx].flags & NAM_FLAG_EMBEDDED) &&
        g_nam_registry.slots[logical_idx].blob_slot < PA_FLASH_USER_NAM_BLOB_SLOTS) {
        /* Replacing a capture that is stored in the flash while every flash slot is in use: with so few slots there is no second one to write the new blob to
         * first, so the new blob goes over the old one. A power cut in the middle leaves a blob whose CRC32 does not match: it is refused at load, never played. */
        target_blob_slot = g_nam_registry.slots[logical_idx].blob_slot;
    }
    if (target_blob_slot < 0) {
        return -5; /* Flash store full: no free physical slot */
    }

    /* 3. Program blob into target physical sector */
    uint32_t slot_addr = pa_flash_nam_slot_addr((uint32_t)target_blob_slot);
    uint32_t sectors_needed = (blob_len + PA_FLASH_SECTOR_SIZE - 1u) / PA_FLASH_SECTOR_SIZE;
    uint32_t b_crc = pa_crc32(blob, blob_len);

    for (uint32_t s = 0; s < sectors_needed; s++) {
        if (pa_spi_nor_erase_sector(slot_addr + s * PA_FLASH_SECTOR_SIZE) != 0) {
            return -3;
        }
    }
    if (pa_spi_nor_write(slot_addr, blob, blob_len) != 0) {
        return -4;
    }
    pa_spi_nor_sync();

    /* 4. Update logical slot metadata and commit */
    pa_user_nam_slot_t *slot = &g_nam_registry.slots[logical_idx];
    slot->active     = 1;
    slot->blob_slot  = (uint8_t)target_blob_slot;
    slot->type       = type;
    slot->voicing    = voicing;
    slot->has_cab    = has_cab;
    slot->blob_len   = blob_len;
    slot->blob_crc32 = b_crc;
    slot->head_scale = head_scale;
    slot->input_trim = (meta && meta->input_trim > 0.001f) ? meta->input_trim : 1.0f;
    slot_set_neutral(slot);
    if (meta) {
        for (int k = 0; k < 6; k++) slot->amp_knobs[k] = meta->amp_knobs[k] <= 100u ? meta->amp_knobs[k] : 50u;
        float mk = meta->makeup_db;
        if (mk < -12.0f) mk = -12.0f;
        if (mk >  12.0f) mk =  12.0f;
        slot->makeup_db_x4 = (int8_t)(mk * 4.0f);
        slot->flags = meta->flags;
    }
    strncpy(slot->name, clean, sizeof(slot->name) - 1);
    slot->name[sizeof(slot->name) - 1] = '\0';

    if ((uint32_t)logical_idx >= g_nam_registry.count) {
        g_nam_registry.count = (uint32_t)logical_idx + 1u;
    }

    int crc = commit_registry_to_flash();
    if (crc != 0) return crc;
    return logical_idx;
}

/* Clear every capture stored in the flash and wipe the blob area: the registry entries go (the ones linked into the image stay), then every blob slot is erased, whatever it held
 * (a blob an interrupted upload left, a slot no registry entry names). Returns the number of sectors erased, or a negative code. */
int pa_user_nam_clear(void) {
    if (!g_nam_registry_inited) pa_user_nam_init();
    for (uint32_t i = 0; i < PA_USER_NAM_MAX_COUNT; i++) {
        if (!(g_nam_registry.slots[i].flags & NAM_FLAG_EMBEDDED) || !g_nam_registry.slots[i].active) memset(&g_nam_registry.slots[i], 0, sizeof g_nam_registry.slots[i]);
    }
    while (g_nam_registry.count > 0 && !g_nam_registry.slots[g_nam_registry.count - 1].active) g_nam_registry.count--;
    s_backup_valid = 0;
    pa_spi_nor_begin();
    int rc = commit_registry_to_flash();
    int n = 0;
    for (uint32_t p = 0; rc == 0 && p < PA_FLASH_USER_NAM_BLOB_SLOTS; p++) {
        const uint32_t a = pa_flash_nam_slot_addr(p);
        for (uint32_t s = 0; s < PA_FLASH_USER_NAM_SLOT_SIZE / PA_FLASH_SECTOR_SIZE; s++) {
            if (pa_spi_nor_erase_sector(a + s * PA_FLASH_SECTOR_SIZE) != 0) { rc = -3; break; }
            n++;
        }
    }
    pa_spi_nor_end();
    return rc ? rc : n;
}

/* Rename a registered capture. The SysEx upload protocol carries the model
 * but not its name, so the name arrives separately (the workbench sends it
 * with the .nam it loaded). */
int pa_user_nam_set_name(uint8_t user_idx, const char *name) {
    if (!g_nam_registry_inited) pa_user_nam_init();
    if (user_idx >= g_nam_registry.count || !g_nam_registry.slots[user_idx].active) return -1;
    if (!name || !name[0]) return -2;
    char clean[PA_USER_NAM_NAME_LEN];
    clean_nam_name(name, clean, sizeof(clean));
    if (!clean[0]) return -2;
    strncpy(g_nam_registry.slots[user_idx].name, clean, PA_USER_NAM_NAME_LEN - 1);
    g_nam_registry.slots[user_idx].name[PA_USER_NAM_NAME_LEN - 1] = '\0';
    int crc = commit_registry_to_flash();
    if (crc != 0) return crc;
    return 0;
}

/* Scratch for reading a NAM blob out of flash. .pool, not .bss - see the note in sysex.c. The RAM copy is PA_NAM_BLOB_MAX; a flash slot is PA_FLASH_USER_NAM_SLOT_SIZE (8 KB).
 * The update session (ota_hw.c) borrows it: it is idle while the main loop is inside the session. */
static uint8_t s_model_blob_buf[PA_NAM_BLOB_MAX] __attribute__((section(".pool"), aligned(4)));
uint8_t *pa_user_nam_scratch(uint32_t *n) { *n = (uint32_t)sizeof s_model_blob_buf; return s_model_blob_buf; }

int pa_user_nam_delete(uint8_t user_idx) {
    if (!g_nam_registry_inited) pa_user_nam_init();
    if (user_idx >= g_nam_registry.count || !g_nam_registry.slots[user_idx].active) return -1;
    g_nam_registry.slots[user_idx].active = 0;
    while (g_nam_registry.count > 0 && !g_nam_registry.slots[g_nam_registry.count - 1].active) {
        g_nam_registry.count--;
    }
    return commit_registry_to_flash();
}

void pa_user_nam_rollback(uint8_t user_idx) {
    if (!g_nam_registry_inited) pa_user_nam_init();
    if (s_backup_valid && s_backup_idx == (int)user_idx && user_idx < PA_USER_NAM_MAX_COUNT) {
        /* Failed overwrite: restore previous working model */
        g_nam_registry.slots[user_idx] = s_backup_slot;
        commit_registry_to_flash();
        s_backup_valid = 0;
    } else {
        /* Failed new addition: delete the slot */
        pa_user_nam_delete(user_idx);
    }
}

int pa_user_nam_load_into_dsp(uint8_t user_idx, pa_nam_model_t *gru_model, pa_wavenet_model_t *wn_model) {
    if (!g_nam_registry_inited) pa_user_nam_init();
    if (user_idx >= g_nam_registry.count || !g_nam_registry.slots[user_idx].active) return -1;

    const pa_user_nam_slot_t *slot = &g_nam_registry.slots[user_idx];
    uint32_t blob_slot = slot->blob_slot;
    if (!(slot->flags & NAM_FLAG_EMBEDDED) && blob_slot >= PA_FLASH_USER_NAM_BLOB_SLOTS) return -2;   /* names a flash slot this chip does not have */
    uint32_t slot_addr = pa_flash_nam_slot_addr(blob_slot);

    /* Mute the AMP slot via the existing mute handshake (same as presets use).
     * This prevents glitches when weights are overwritten while the interrupt
     * is reading them - the audio is silenced during the weight transfer. */
    pa_fx_chain_set_slot_enabled_now(&g_fx_chain, PA_SLOT_AMP, 0);

    /* Scratch for reading a NAM blob out of flash. .pool, not .bss - see the note in sysex.c. The RAM copy is PA_NAM_BLOB_MAX; a flash slot is
 * PA_FLASH_USER_NAM_SLOT_SIZE (8 KB). */
    /* A registry written by an older build could name a blob bigger than the
     * buffer (the limit used to be the 64 KB flash stride). Refuse it instead
     * of reading past the end. */
    if (slot->blob_len == 0u || slot->blob_len > PA_NAM_BLOB_MAX) return -4;
    if (slot->flags & NAM_FLAG_EMBEDDED) {
        const pa_emb_nam_t *e = pa_emb_nam_get(slot->blob_slot);
        if (!e || e->blob_len != slot->blob_len) return -2;
        pa_xip_read(s_model_blob_buf, e->blob, slot->blob_len);
    } else if (pa_spi_nor_read(slot_addr, s_model_blob_buf, slot->blob_len) != 0) return -2;

    /* Verify blob CRC32 if registered */
    if (slot->blob_crc32 != 0u) {
        uint32_t c = pa_crc32(s_model_blob_buf, slot->blob_len);
        if (c != slot->blob_crc32) return -4; /* Corrupt model blob */
    }

    extern int g_active_amp_engine;
    extern pa_fx_chain_t g_fx_chain;

    /* Check if blob has WAVE container header */
    if (slot->blob_len >= 12u && s_model_blob_buf[0] == 'W' && s_model_blob_buf[1] == 'A' &&
        s_model_blob_buf[2] == 'V' && s_model_blob_buf[3] == 'E') {
        if (!wn_model) return -3;
        uint32_t count = 0;
        float head_scale = slot->head_scale;
        uint32_t flags = 0;
        float input_trim = (slot->input_trim > 0.001f) ? slot->input_trim : 1.0f;
        memcpy(&count, s_model_blob_buf + 4, 4);
        memcpy(&head_scale, s_model_blob_buf + 8, 4);
        uint32_t data_off = 12u;
        if (slot->blob_len >= 20u + count * sizeof(float)) {
            data_off = 20u;
            memcpy(&flags, s_model_blob_buf + 12, 4);
            memcpy(&input_trim, s_model_blob_buf + 16, 4);
        } else if (slot->blob_len >= 16u + count * sizeof(float)) {
            data_off = 16u;
            memcpy(&flags, s_model_blob_buf + 12, 4);
        }
        if (slot->blob_len >= data_off + count * sizeof(float)) {
            const float *weights = (const float *)(s_model_blob_buf + data_off);
            int rc = pa_wavenet_load_weights(wn_model, weights, count, head_scale);
            if (rc != 0) return -3;
#if !defined(PA_HOST_SIM) && !defined(PA_HOST_BUILD)
            /* The stock-style pipelined block engine (pa_wavenet.c) is expected to need about 1.3 ms a block, the budget being 1.451 ms. It is new on the unit:
             * the engine times its own blocks and gives up after eight blocks over 1.5 ms (blk_bad), and then the amp slot runs the GRU loaded before. So only
             * a capture the engine cannot run at all (accelerator marked broken, a weight outside Q24) is refused here. */
            if (!pa_wavenet_block_ready(wn_model) && !pa_emb_allow_wavenet()) { wn_model->active = 0; return -3; }
#endif
            wn_model->input_trim = (input_trim > 0.001f) ? input_trim : 1.0f;
            /* NaN protection: reject non-finite input_trim */
            if (!isfinite(wn_model->input_trim)) return -3;
            pa_wavenet_set_voicing(wn_model, (pa_voicing_profile_t)slot->voicing);
            wn_model->is_amp_cab = (flags & 1u) ? 1 : (slot->has_cab ? 1 : 0);
            /* IMP-01: calibrated loudness instead of the fixed 1.40 / 1.00 guess */
            wn_model->makeup_gain = powf(10.0f, (float)slot->makeup_db_x4 / 80.0f);
            wn_model->active = 1;
            g_active_amp_engine = 1;
        } else {
            return -3;
        }
    } else if (slot->type == 1 && wn_model) {
        /* Raw WaveNet float weights (no container) */
        uint32_t count = slot->blob_len / (uint32_t)sizeof(float);
        int rc = pa_wavenet_load_weights(wn_model, (const float *)s_model_blob_buf, count, slot->head_scale);
        if (rc != 0) return -3;
        pa_wavenet_set_voicing(wn_model, (pa_voicing_profile_t)slot->voicing);
        if (slot->input_trim > 0.001f) {
            pa_wavenet_set_input_trim(wn_model, slot->input_trim);
        }
        wn_model->is_amp_cab = slot->has_cab ? 1 : 0;
        wn_model->active = 1;
        g_active_amp_engine = 1;
    } else if (gru_model) {
        /* Standard GRU binary model blob (3852 bytes) */
        if (slot->blob_len >= 3852u) {
            memcpy(gru_model, s_model_blob_buf, PA_NAM_BLOB_BYTES);
            pa_nam_reset(gru_model);
            gru_model->scale_q21 = (int32_t)(powf(10.0f, (float)slot->makeup_db_x4 / 80.0f) * (float)(1 << PA_NAM_Q_SHIFT));
            gru_model->active    = 1;
            g_active_amp_engine  = 0;
        } else {
            return -4;
        }
    }

    /* Ensure AMP slot in the chain is enabled */
    pa_fx_chain_set_slot_enabled(&g_fx_chain, PA_SLOT_AMP, 1);

    /* Auto-bypass Slot 3 (CAB) if capture has baked-in cabinet (crossfaded, never frees under the ISR) */
    if (slot->has_cab) {
        pa_fx_chain_set_slot_enabled(&g_fx_chain, PA_SLOT_CAB, 0);
    }

    return 0;
}
