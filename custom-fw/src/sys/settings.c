/* src/sys/settings.c — persistent system settings: magic + sequence + CRC32 (IMP-10).
 * The settings sector is an APPEND LOG of 64-byte slots (64 slots): a save programs the next blank slot (one 64-byte page write, about 0.3 ms plus the quiesce) and the sector is erased only when
 * the 64th slot is used, so a save does not cost a 17 ms erase and the flash sees 1/64th of the erases. The load takes the valid slot with the highest sequence number. A slot torn by a power cut fails
 * its CRC and is skipped (it is not blank, so the next save goes to the next slot). A record written by the earlier in-place layout sits in slot 0 and is read as is. */
#include <string.h>
#include "../../include/pa_settings.h"
#include "../../include/pa_spi_nor.h"
#include "../../include/pa_flash_map.h"
#include "../../include/pa_preset.h"     /* pa_crc32 */
#include "../../include/pa_bond.h"       /* the Bluetooth bond lives in the same sector */

#define SET_ADDR    (PA_FLASH_SYS_SETTINGS_ADDR)
#define SET_SLOT    64u
#define SET_SLOTS   (PA_FLASH_SYS_SETTINGS_SIZE / SET_SLOT)         /* 32: the first half of the sector it shares with the NAM registry */
_Static_assert(sizeof(pa_settings_t) <= SET_SLOT, "a settings record must fit one 64-byte slot");
_Static_assert(PA_FLASH_SYS_SETTINGS_ADDR == PA_FLASH_SHARED_SECTOR && PA_FLASH_USER_NAM_META_A_ADDR == PA_FLASH_SHARED_SECTOR + PA_FLASH_SYS_SETTINGS_SIZE &&
               PA_FLASH_BOND_ADDR == PA_FLASH_USER_NAM_META_A_ADDR + PA_FLASH_USER_NAM_META_SIZE &&
               PA_FLASH_SYS_SETTINGS_SIZE + PA_FLASH_USER_NAM_META_SIZE + PA_FLASH_BOND_SIZE == PA_FLASH_SECTOR_SIZE, "the settings log, the NAM registry and the Bluetooth bond log fill one sector");

static uint32_t s_seq = 0;
static uint32_t s_next = 0;      /* the slot the next save programs (SET_SLOTS = the log is full: rewrite the sector first) */
static pa_settings_t s_last;     /* the newest valid record (loaded or saved): put back into slot 0 whenever the shared sector is erased */
static int s_have_last = 0;

void pa_settings_defaults(pa_settings_t *s) {
    memset(s, 0, sizeof(*s));
    s->magic = PA_SETTINGS_MAGIC; s->version = PA_SETTINGS_VERSION; s->size = sizeof(*s);
    s->last_preset = 0;          s->home_view_mode = 0;   s->play_mode = 0;     s->fizz_level = 1;
    s->display_brightness = 80;  s->display_auto_dim = 0; s->display_battery_pct = 1; s->theme = 0;
    s->input_gain_db = 0;        s->gate_db = -80;        s->out_level_pct = 100;
    s->output_limit_mode = 0;     s->usb_loopback = 1;     /* the stock ships with the loopback ON */
    s->usb_mode = 0;             /* AMP: the unit's own sound; 1 / 2 are the audio-interface modes */
    s->io_output_type = 0;       s->io_cab_on_line = 1;   s->usb_sample_rate = 1; s->bt_enabled = 1;
    s->tuner_mute = 0;           s->tuner_ref_hz = 440;
    s->drum_bpm = 120;           s->drum_pattern = 0;     s->drum_volume = 70;  s->drum_drums_on = 1;
}

static int read_valid(uint32_t addr, pa_settings_t *out) {
    if (pa_spi_nor_read(addr, out, sizeof(*out)) != 0) return 0;
    if (out->magic != PA_SETTINGS_MAGIC || out->size != sizeof(*out)) return 0;
    return out->crc32 == pa_crc32(out, (uint32_t)sizeof(*out) - sizeof(uint32_t));
}

static int slot_blank(uint32_t addr) {
    uint8_t b[SET_SLOT];
    if (pa_spi_nor_read(addr, b, SET_SLOT) != 0) return 0;
    for (uint32_t i = 0; i < SET_SLOT; i++) if (b[i] != 0xFFu) return 0;
    return 1;
}

int pa_settings_load(pa_settings_t *s) {
    pa_settings_t r, best;
    int have = 0;
    uint32_t last_used = 0;      /* 1 + the highest slot that is not blank (valid or torn) */
    for (uint32_t k = 0; k < SET_SLOTS; k++) {
        const uint32_t a = SET_ADDR + k * SET_SLOT;
        if (read_valid(a, &r)) {
            if (!have || (int32_t)(r.seq - best.seq) > 0) { best = r; have = 1; }
            last_used = k + 1u;
        } else if (!slot_blank(a)) {
            last_used = k + 1u;                                  /* a torn slot: skipped, never reused until the erase */
        }
    }
    s_next = last_used;
    if (have) { *s = best; s_seq = best.seq; s_last = best; s_have_last = 1; return 0; }
    pa_settings_defaults(s);
    return -1;
}

/* The sector is shared with the NAM registry (the second half), so an erase must put both back. Used when the log is full (reg = NULL: keep the registry that is there) and when the registry is
 * saved (reg = its new image). Reads the registry out BEFORE the erase, erases, programs the registry (skipping all-0xFF pages) and then the newest settings record into slot 0. A power cut between the
 * erase and the last program loses what was in the sector: the settings fall back to the defaults and the registry to empty (rare: once in 32 saves, or a capture upload). */
int pa_flash_shared_rewrite(const void *reg, uint32_t reg_len) {
    static uint8_t keep[PA_FLASH_USER_NAM_META_SIZE];
    uint8_t rec[SET_SLOT];
    uint8_t bond[PA_BOND_SLOT];
    const int have_bond = pa_bond_snapshot(bond);                /* the newest Bluetooth bond (or the one being appended) goes back after the erase, like the settings and the registry */
    if (reg == 0) {
        if (pa_spi_nor_read(PA_FLASH_USER_NAM_META_A_ADDR, keep, sizeof keep) != 0) return -1;
        reg = keep; reg_len = sizeof keep;
    } else if (reg_len > sizeof keep) return -1;
    pa_spi_nor_begin();
    int rc = pa_spi_nor_erase_sector(PA_FLASH_SHARED_SECTOR);
    if (rc == 0) {
        const uint8_t *p = (const uint8_t *)reg;
        for (uint32_t off = 0; rc == 0 && off < reg_len; off += 256u) {
            const uint32_t n = (reg_len - off) < 256u ? (reg_len - off) : 256u;
            int blank = 1;
            for (uint32_t i = 0; i < n; i++) if (p[off + i] != 0xFFu) { blank = 0; break; }
            if (!blank) rc = pa_spi_nor_write(PA_FLASH_USER_NAM_META_A_ADDR + off, p + off, n);
        }
    }
    s_next = 0;
    if (rc == 0 && s_have_last) {
        memset(rec, 0xFF, sizeof rec);
        memcpy(rec, &s_last, sizeof s_last);
        rc = pa_spi_nor_write(SET_ADDR, rec, SET_SLOT);
        s_next = 1;
    }
    if (rc == 0 && have_bond) rc = pa_spi_nor_write(PA_FLASH_BOND_ADDR, bond, PA_BOND_SLOT);
    pa_bond_rewritten(have_bond);
    pa_spi_nor_end();
    pa_spi_nor_sync();
    return rc ? -1 : 0;
}

int pa_settings_save(const pa_settings_t *in) {
    pa_settings_t s = *in;
    uint8_t rec[SET_SLOT];
    s.magic = PA_SETTINGS_MAGIC; s.version = PA_SETTINGS_VERSION; s.size = sizeof(s);
    s.seq   = s_seq + 1u;
    s.crc32 = pa_crc32(&s, (uint32_t)sizeof(s) - sizeof(uint32_t));
    memset(rec, 0xFF, sizeof rec);
    memcpy(rec, &s, sizeof s);
    s_last = s; s_have_last = 1;
    if (s_next >= SET_SLOTS) {                                   /* full: rewrite the shared sector (the registry is kept); the new record lands in slot 0 */
        if (pa_flash_shared_rewrite(0, 0u) != 0) return -1;
        s_seq = s.seq;
        return 0;
    }
    {   const uint32_t at = SET_ADDR + s_next * SET_SLOT;
        s_next++;                                                /* whether or not the program works, this slot is used */
        if (pa_spi_nor_write(at, rec, SET_SLOT) != 0) return -1;
    }
    pa_spi_nor_sync();
    s_seq = s.seq;
    return 0;
}
