/*
 * test/test_robustness.c - power-fail, flash-protection, blank-board and allocator tests.
 *
 * Build (from custom-fw/):
 *   clang -O1 -Wall -Iinclude -DPA_HOST_SIM=1 -DPA_POOL_TEST=1 test/test_robustness.c \
 *     src/sys/preset.c src/sys/pa_pool.c src/board/spi_nor.c src/fx/fx_graph.c ... -o test_robustness
 * IMPORTANT: run from a directory with no pocket_amp_flash.bin in it or its parents,
 * otherwise the sim driver will attach to (and modify) that real image.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "../include/pa_preset.h"
#include "../include/pa_settings.h"
#include "../include/pa_bond.h"
#include "../include/pa_spi_nor.h"
#include "../include/pa_wavenet.h"
#include "../include/pa_wavenet.h"

void pa_spi_nor_sim_fail_after(int n_ops);
void pa_spi_nor_sim_reboot(void);
void pa_preset_test_reset(void);
pa_preset_t *pa_preset_test_slot(uint8_t idx);
int pa_preset_test_commit(void);
uint32_t pa_preset_test_bank_seq(void);
void pa_preset_init_defaults(void);
int  pa_preset_factory_reset(void);
int  pa_preset_clean_user(void);
void *pa_alloc(uint32_t n);
void  pa_free(void *p);
uint32_t pa_pool_used_bytes(void);
int plat_battery_is_brownout(void) { return 0; }

/* preset.c grew these dependencies over time; stub them so the torn-write
 * sweep can build and run again. */
pa_wavenet_model_t g_wn_model;
int g_active_amp_engine = 0;
void pa_fx_chain_set_slot_enabled_now(pa_fx_chain_t *c, pa_fx_slot_id_t s, int on) { (void)c; (void)s; (void)on; }
void pa_fx_chain_set_slot_type(pa_fx_chain_t *c, pa_fx_slot_id_t s, uint8_t t) { (void)c; (void)s; (void)t; }
int  pa_stock_load_amp(uint8_t idx, void *m, float *dc) { (void)idx; if (m) memset(m, 0, 4096); if (dc) *dc = 0.0f; return 0; }
int  pa_user_nam_load_into_dsp(uint8_t i, void *g, void *w) { (void)i; (void)g; (void)w; return -1; }
void pa_amp_set_model_profile(uint8_t idx) { (void)idx; }
void pa_audio_request_mute(void) {}
int  pa_audio_is_muted(void) { return 1; }
void pa_audio_release_mute(void) {}
/* The deferred preset swap is owned by the portable core (pa_audio_core.c),
 * which this suite does not link - it only exercises the allocator and the
 * flash. */
void pa_audio_core_request_preset(const pa_preset_t *p) { (void)p; }
uint32_t plat_ms(void) { return 0; }
void plat_wdt_feed(void) {}
int  pa_stock_load_cab(uint8_t idx, int32_t *taps, uint32_t *n) { (void)idx; if (taps) memset(taps, 0, 1024); if (n) *n = 0; return 0; }
int  pa_cab_load(uint8_t idx, int32_t *taps, uint32_t *n) { return pa_stock_load_cab(idx, taps, n); }
void fx_cab_ir_load_q18(fx_node_t *nd, const int32_t *t, uint32_t n) { (void)nd; (void)t; (void)n; }
void fx_cab_ir_set_stock_trim(fx_node_t *nd, uint8_t i) { (void)nd; (void)i; }

/* preset.c only touches the chain in pa_preset_apply_to_chain(), which is not under test. */
void pa_fx_chain_reset_states(pa_fx_chain_t *c) { (void)c; }
void pa_fx_chain_set_slot_enabled(pa_fx_chain_t *c, pa_fx_slot_id_t s, int on) { (void)c; (void)s; (void)on; }
void pa_fx_chain_set_param(pa_fx_chain_t *c, pa_fx_slot_id_t s, int k, int32_t v) { (void)c; (void)s; (void)k; (void)v; }
void pa_fx_chain_set_preset_vol(pa_fx_chain_t *c, uint8_t v) { (void)c; (void)v; }
void pa_fx_chain_set_input_gain(pa_fx_chain_t *c, int32_t g) { (void)c; (void)g; }
void pa_fx_chain_relink_ordered(pa_fx_chain_t *c) { (void)c; }

static int g_fail = 0;
#define CHECK(c, ...) do { if (!(c)) { g_fail++; printf("  FAIL: " __VA_ARGS__); printf("\n"); } } while (0)

static void wipe_store(void) {
    pa_spi_nor_init();
    pa_spi_nor_sim_reboot();
    for (uint32_t a = 0; a < PA_FLASH_SIZE; a += PA_FLASH_SECTOR_SIZE)          /* every sector the device may rewrite (the rest is refused) */
        if (pa_flash_rw_ok(a, PA_FLASH_SECTOR_SIZE)) pa_spi_nor_erase_sector(a);
}

static void reboot(void) { pa_spi_nor_sim_reboot(); pa_preset_test_reset(); pa_preset_init(); }

static int all_valid(void) {
    pa_preset_t p;
    for (uint8_t i = 0; i < PA_PRESET_COUNT; i++) if (pa_preset_load(i, &p) != 0) return 0;
    return 1;
}

static void save_vol(uint8_t idx, uint8_t vol) {
    pa_preset_t p;
    pa_preset_load(idx, &p);
    p.preset_vol = vol;
    pa_preset_save(idx, &p);
}

static uint8_t vol_of(uint8_t idx) { pa_preset_t p; pa_preset_load(idx, &p); return p.preset_vol; }

int main(void) {
    printf("sizeof(pa_preset_t)=%u  bank payload=%u  bank slot=%u\n",
           (unsigned)sizeof(pa_preset_t), (unsigned)(16 + PA_PRESET_COUNT * sizeof(pa_preset_t)),
           (unsigned)PA_FLASH_STORE_BANK_SIZE);

    /* 1. Blank, factory-fresh flash.
     * The expected factory volume is READ from the store rather than hard-coded:
     * the previous literal (70) went stale when the factory table was
     * calibrated across the 40 factory presets, and because
     * this suite never linked it never noticed. */
    wipe_store(); reboot();
    CHECK(all_valid(), "blank flash did not yield 80 valid default presets");
    uint8_t def_vol = vol_of(0);
    CHECK(def_vol > 0u && def_vol <= 100u, "default preset vol %u out of range", def_vol);
    reboot();
    CHECK(all_valid() && vol_of(0) == def_vol, "defaults were not persisted by first boot");
    printf("[1] blank-board boot: %s\n", g_fail ? "FAIL" : "ok");

    /* 2. Power loss at EVERY flash op of a save (A->B and B->A transitions) */
    int old_n = 0, new_n = 0, tot = 0;
    for (int round = 0; round < 2; round++) {            /* round 0: A active, round 1: B active */
        for (int k = 0; k <= 9; k++) {
            wipe_store(); reboot();
            if (round == 1) save_vol(7, 55);              /* moves active bank to B */
            uint8_t before = vol_of(5);
            pa_spi_nor_sim_fail_after(k);
            save_vol(5, 33);
            reboot();
            tot++;
            CHECK(all_valid(), "round %d k=%d: corrupt/missing presets after power loss", round, k);
            uint8_t v = vol_of(5);
            CHECK(v == before || v == 33, "round %d k=%d: preset 5 vol=%u (neither old nor new)", round, k, v);
            if (v == before) old_n++; else new_n++;
            if (round == 1) CHECK(vol_of(7) == 55, "round 1 k=%d: unrelated preset 7 lost", k);
        }
    }
    printf("[2] torn save sweep: %d cases, old kept=%d, new committed=%d\n", tot, old_n, new_n);

    /* 3. Many saves keep alternating and survive reboot (wear spread) */
    wipe_store(); reboot();
    for (int i = 0; i < 40; i++) save_vol((uint8_t)(i % PA_PRESET_COUNT), (uint8_t)(10 + i));
    reboot();
    CHECK(all_valid() && vol_of(39) == 49 && vol_of(0) == 10, "40 sequential saves not persisted");
    printf("[3] 40 sequential saves persisted: ok\n");

    /* 4. Torn factory reset */
    int fr_old = 0, fr_def = 0;
    uint8_t factory_vol;
    wipe_store(); reboot();
    factory_vol = vol_of(3);
    for (int k = 0; k <= 9; k++) {
        wipe_store(); reboot();
        save_vol(3, 42);
        pa_spi_nor_sim_fail_after(k);
        pa_preset_factory_reset();
        reboot();
        CHECK(all_valid(), "factory-reset k=%d: corrupt presets", k);
        uint8_t v = vol_of(3);
        /* the factory default is read from the store, not hard-coded: the old
         * literal (70) went stale when the factory table was re-calibrated
         * (P04 is 72 now) and this suite never linked, so it never noticed. */
        CHECK(v == 42 || v == factory_vol,
              "factory-reset k=%d: vol=%u (neither old 42 nor factory %u)", k, v, factory_vol);
        if (v == 42) fr_old++; else fr_def++;
    }
    printf("[4] torn factory reset sweep: old=%d default=%d\n", fr_old, fr_def);

    /* 4a. Torn CLEAN USER PRESETS: P41-P80 back to EMPTY, the stock presets untouched; a power cut keeps the old bank or the cleaned one, never a mix */
    {
        int cu_old = 0, cu_new = 0;
        for (int k = 0; k <= 9; k++) {
            wipe_store(); reboot();
            save_vol(3, 42);                                   /* a stock slot the user edited: must survive the clean */
            save_vol(45, 66);                                  /* a user slot: must be gone after a complete clean */
            pa_spi_nor_sim_fail_after(k);
            int rc = pa_preset_clean_user();
            reboot();
            CHECK(all_valid(), "clean-user k=%d: corrupt presets", k);
            CHECK(vol_of(3) == 42, "clean-user k=%d: a stock slot changed (vol %u)", k, vol_of(3));
            int empty = pa_preset_is_empty(45);
            CHECK(empty == 0 || vol_of(45) != 66, "clean-user k=%d: user slot P46 reads EMPTY but kept its sound", k);
            CHECK(rc == 0 ? empty == 1 : 1, "clean-user k=%d: reported success but P46 is not EMPTY", k);
            if (empty) cu_new++; else { cu_old++; CHECK(vol_of(45) == 66, "clean-user k=%d: P46 half-cleaned (vol %u)", k, vol_of(45)); }
        }
        printf("[4a] torn clean-user-presets sweep: old=%d cleaned=%d\n", cu_old, cu_new);
    }

    /* 4b. A firmware update that changes the factory bank: the untouched factory slots follow
     *     the new table at boot, a factory slot the user saved and the user slots are kept,
     *     and a boot with nothing to refresh does not write the flash. */
    {
        wipe_store(); reboot();
        pa_preset_t factory5 = *pa_preset_test_slot(5);       /* what this firmware ships */
        save_vol(2, 23);                                      /* the user's own P03 (seq 2) */
        save_vol(45, 66);                                     /* and a user slot P46 */
        /* make P06 look like an older firmware's factory preset: other content, still seq 1 */
        pa_preset_t *old = pa_preset_test_slot(5);
        old->preset_vol = 11; old->amp_model_idx = 7; old->name[0] = 'Z';
        old->crc32 = pa_crc32(old, (uint32_t)sizeof(*old) - sizeof(uint32_t));
        CHECK(old->seq == 1u, "P06 should still be an untouched default (seq %u)", (unsigned)old->seq);
        CHECK(pa_preset_test_commit() == 0, "could not store the 'old firmware' bank");
        reboot();
        const pa_preset_t *p6 = pa_preset_test_slot(5);
        CHECK(all_valid(), "refresh: corrupt presets");
        CHECK(memcmp(p6, &factory5, sizeof(factory5)) == 0,
              "untouched P06 was not refreshed to the current factory preset (vol %u amp %u)",
              p6->preset_vol, p6->amp_model_idx);
        CHECK(vol_of(2) == 23, "the user's saved P03 was overwritten (vol %u)", vol_of(2));
        CHECK(vol_of(45) == 66, "user slot P46 was touched (vol %u)", vol_of(45));
        uint32_t seq = pa_preset_test_bank_seq();
        reboot();
        CHECK(pa_preset_test_bank_seq() == seq, "a boot with nothing to refresh wrote the flash");
        printf("[4b] factory-bank refresh on update: untouched P06 refreshed, saved P03 + P46 kept\n");
    }

    /* 5. Protected region: bootloader / app / factory data must be untouchable */
    {
        uint8_t before[16], after[16], junk[16];
        memset(junk, 0, sizeof(junk));
        uint32_t probes[] = { 0x000000, 0x001000, 0x005000, 0x084000, 0x085000, 0x085FF0, 0x0B4000, 0x0B4FF8, 0x0B5000, 0x0EBFFF - 15 };      /* the app image ends at 0x85FFF; 0x86000..0xB3FFF is the user store; 0xB4000 holds the V1.05 trailer */
        int prot_ok = 1;
        for (unsigned i = 0; i < sizeof(probes) / sizeof(probes[0]); i++) {
            pa_spi_nor_read(probes[i], before, 16);
            int e = pa_spi_nor_erase_sector(probes[i]);
            int w = pa_spi_nor_write(probes[i], junk, 16);
            pa_spi_nor_read(probes[i], after, 16);
            if (e == 0 || w == 0 || memcmp(before, after, 16) != 0) prot_ok = 0;
        }
        CHECK(prot_ok, "protected flash region was writable/erasable");
        {   /* ... and the user store beside it is writable, to its last byte, and not one sector more */
            uint8_t one[16] = { 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 16 }, back[16];
            int us_ok = 1;
            const uint32_t okat[] = { 0x086000, 0x0A4000, 0x0B3000 };
            for (unsigned i = 0; i < 3; i++) {
                us_ok &= (pa_spi_nor_erase_sector(okat[i]) == 0) && (pa_spi_nor_write(okat[i], one, 16) == 0);
                pa_spi_nor_read(okat[i], back, 16); us_ok &= (memcmp(one, back, 16) == 0);
            }
            us_ok &= (pa_spi_nor_write(0x0B3FF0, one, 16) == 0) && (pa_spi_nor_write(0x0B3FF0, back, 17) != 0) && (pa_spi_nor_write(0x0B4FF8, one, 16) != 0);   /* a write may end at the window's end, not pass it; the trailer sector is closed */
            CHECK(us_ok, "the user store must be writable inside 0x86000..0xB4FFF and refuse a write that leaves it");
        }
        printf("[5] bootloader/app/factory region write-protected: %s\n", prot_ok ? "ok" : "FAIL");
    }

    /* 6. Allocator: 200k enable/disable cycles must not leak or fragment to death */
    {
        void *p[8] = {0};
        uint32_t sizes[8] = { 1200, 9000, 4400, 2500, 16000, 700, 8200, 5200 } /* ~47 KB worst-case live set of the PA_DSP_POOL_BYTES pool */;
        srand(1);
        int nulls = 0;
        for (int it = 0; it < 200000; it++) {
            int s = rand() % 8;
            if (p[s]) { pa_free(p[s]); p[s] = 0; }
            else { p[s] = pa_alloc(sizes[s]); if (!p[s]) nulls++; else memset(p[s], 0xA5, 16); }
        }
        for (int s = 0; s < 8; s++) if (p[s]) { pa_free(p[s]); p[s] = 0; }
        CHECK(pa_pool_used_bytes() == 0, "pool leaked %u bytes", (unsigned)pa_pool_used_bytes());
        /* One allocation bigger than the old 80 KB pool: proves the free list
         * coalesced back into a single block (the delay line alone is 64 KB). */
        void *big = pa_alloc(PA_DSP_POOL_BYTES - 4096u);
        CHECK(big != 0, "pool fragmented: cannot allocate %u B after full free",
              (unsigned)(PA_DSP_POOL_BYTES - 4096u));
        pa_free(big);
        CHECK(nulls == 0, "allocator returned NULL %d times with working set < pool", nulls);
        printf("[6] allocator churn 200k ops: leak=%u B, NULLs=%d\n", (unsigned)pa_pool_used_bytes(), nulls);
    }

    /* 7. The settings sector is an append log of 64-byte slots (src/sys/settings.c): a save programs the next blank slot, the sector is erased only when it is full,
     * the load takes the newest valid slot, and a slot torn by a power cut is skipped. */
    {
        pa_settings_t st, ld;
        int ok = 1;
        wipe_store();
        pa_settings_defaults(&st);
        CHECK(pa_settings_load(&ld) != 0, "blank settings sector must report defaults");
        for (int i = 1; i <= 150; i++) {                              /* 150 saves cross the 64-slot sector twice */
            pa_settings_t chk;
            st.display_brightness = (uint8_t)(i % 100);
            st.drum_bpm = (uint16_t)(60 + i);
            if (pa_settings_save(&st) != 0) { ok = 0; printf("  FAIL: save %d returned an error\n", i); g_fail++; break; }
            if (pa_settings_load(&chk) != 0 || chk.display_brightness != (uint8_t)(i % 100) || chk.drum_bpm != (uint16_t)(60 + i)) {
                ok = 0; printf("  FAIL: after save %d the load returned brightness %u bpm %u\n", i, (unsigned)chk.display_brightness, (unsigned)chk.drum_bpm); g_fail++; break;
            }
        }
        CHECK(ok, "150 settings saves: every load returned the record just saved");
        /* a torn program: the write is cut after half its bytes; the previous record must still load, and the next save must still work */
        st.drum_bpm = 111; pa_settings_save(&st);
        st.drum_bpm = 222;
        pa_spi_nor_sim_fail_after(0);
        CHECK(pa_settings_save(&st) != 0, "a torn settings program must report an error");
        pa_spi_nor_sim_reboot();
        CHECK(pa_settings_load(&ld) == 0 && ld.drum_bpm == 111, "after a torn program the previous record (bpm 111) loads, got %u", (unsigned)ld.drum_bpm);
        st.drum_bpm = 333;
        CHECK(pa_settings_save(&st) == 0 && pa_settings_load(&ld) == 0 && ld.drum_bpm == 333, "the save after a torn one works and loads back");
        /* the writes stay inside the settings sector: the neighbours (preset bank B below it, the NAM registry above it) are untouched */
        {
            uint8_t nb[16], blank[16];
            memset(blank, 0xFF, sizeof blank);
            pa_spi_nor_read(PA_FLASH_USER_NAM_META_A_ADDR, nb, sizeof nb);
            CHECK(memcmp(nb, blank, sizeof nb) == 0, "the second half of the shared sector (the NAM registry) was written by the settings log");
            pa_spi_nor_read(PA_FLASH_SYS_SETTINGS_ADDR - 16u, nb, sizeof nb);
            CHECK(memcmp(nb, blank, sizeof nb) == 0, "the sector before the settings (preset bank B) was written");
        }
        /* the protected places refuse a write */
        {
            uint8_t z[16] = {0};
            CHECK(pa_spi_nor_write(0xB5000u, z, 16) == -3, "0xB5000 (the stock's record log) must be refused");
            CHECK(pa_spi_nor_write(0xFF000u, z, 16) == -3, "0xFF000 (the stock's save record) must be refused");
            CHECK(pa_spi_nor_erase_sector(0xBD000u) == -3, "0xBD000 (BTIF) must be refused");
            CHECK(pa_spi_nor_erase_sector(0x4000u) == -3, "0x4000 (the app) must be refused");
        }
        /* the NAM registry shares the sector: a registry save keeps the newest settings, and the settings log wrapping keeps the registry */
        {
            extern int pa_flash_shared_rewrite(const void *reg, uint32_t reg_len);
            uint8_t img[1200], back[1200];
            for (uint32_t i = 0; i < sizeof img; i++) img[i] = (uint8_t)(i * 13u + 1u);
            st.drum_bpm = 444; pa_settings_save(&st);
            CHECK(pa_flash_shared_rewrite(img, sizeof img) == 0, "the registry image was written");
            CHECK(pa_settings_load(&ld) == 0 && ld.drum_bpm == 444, "after a registry save the newest settings (bpm 444) are still there, got %u", (unsigned)ld.drum_bpm);
            pa_spi_nor_read(PA_FLASH_USER_NAM_META_A_ADDR, back, sizeof back);
            CHECK(memcmp(img, back, sizeof img) == 0, "the registry image reads back");
            for (int i = 0; i < 70; i++) { st.drum_bpm = (uint16_t)(500 + i); CHECK(pa_settings_save(&st) == 0, "settings save %d with a registry present", i); }
            CHECK(pa_settings_load(&ld) == 0 && ld.drum_bpm == 569, "the settings log wrapped twice and the newest record (bpm 569) loads, got %u", (unsigned)ld.drum_bpm);
            pa_spi_nor_read(PA_FLASH_USER_NAM_META_A_ADDR, back, sizeof back);
            CHECK(memcmp(img, back, sizeof img) == 0, "the registry survived the settings log wrapping");
        }
        printf("[7] settings append log: 150 saves, torn program, neighbours intact, protected sectors refused\n");
    }


    /* 8. The Bluetooth bond log (pa_bond.c): the last 512 bytes of the shared settings sector */
    {
        extern int pa_flash_shared_rewrite(const void *reg, uint32_t reg_len);
        pa_settings_t st, ld;
        pa_bond_t got;
        uint8_t r[8], l[16];
        wipe_store();
        pa_settings_defaults(&st);
#define MKBOND(seed) do { for (int q = 0; q < 8; q++) r[q] = (uint8_t)((seed) + q); for (int q = 0; q < 16; q++) l[q] = (uint8_t)((seed) * 3 + q); } while (0)
#define STORE(seed) do { MKBOND(seed); pa_bond_stash(r, (uint16_t)(seed), l); (void)pa_bond_poll(10000u); } while (0)
#define REBOOT() do { pa_spi_nor_sim_reboot(); pa_bond_rewritten(0); } while (0)     /* a power-on: the flash settles, and the log's RAM cache is read again */
#define WRITE(seed) do { STORE(seed); (void)pa_bond_poll(10000u + PA_BOND_DELAY_MS + 1u); } while (0)
        CHECK(pa_bond_load(&got) == 0, "a blank flash holds no bond");
        pa_settings_save(&st);
        WRITE(1);
        CHECK(pa_bond_load(&got) == 1 && got.ediv == 1 && got.rand[0] == 1 && got.ltk[0] == 3, "a stored bond reads back (ediv %u)", (unsigned)got.ediv);
        REBOOT();
        CHECK(pa_bond_load(&got) == 1 && got.ediv == 1, "the bond is still there after a reboot");
        /* the settings log and the registry are neither touched by a bond write ... */
        {
            uint8_t nb[16], blank[16]; memset(blank, 0xFF, sizeof blank);
            pa_spi_nor_read(PA_FLASH_USER_NAM_META_A_ADDR, nb, sizeof nb);
            CHECK(memcmp(nb, blank, sizeof nb) == 0, "a bond write touched the registry's part of the sector");
            CHECK(pa_settings_load(&ld) == 0, "the settings are still valid after a bond write");
        }
        /* ... and the newest valid slot wins, through 8 slots */
        for (int i = 2; i <= 7; i++) { WRITE(i); CHECK(pa_bond_load(&got) == 1 && got.ediv == (uint16_t)i, "bond %d is the newest", i); }
        /* the 8th slot, then the 9th: the sector is rewritten with the newest bond, the settings and the registry */
        {
            uint8_t img[1100], back[1100];
            for (uint32_t i = 0; i < sizeof img; i++) img[i] = (uint8_t)(i * 7u + 3u);
            st.drum_bpm = 321; pa_settings_save(&st);
            CHECK(pa_flash_shared_rewrite(img, sizeof img) == 0, "a registry image was written beside the bond");
            CHECK(pa_bond_load(&got) == 1 && got.ediv == 7, "the registry save kept the newest bond (ediv %u)", (unsigned)got.ediv);
            WRITE(8);
            WRITE(9);                                           /* the log was full: this one is written by the sector rewrite */
            CHECK(pa_bond_load(&got) == 1 && got.ediv == 9, "the 9th bond (the log was full) is the newest (ediv %u)", (unsigned)got.ediv);
            CHECK(pa_settings_load(&ld) == 0 && ld.drum_bpm == 321, "the rewrite kept the settings (bpm %u)", (unsigned)ld.drum_bpm);
            pa_spi_nor_read(PA_FLASH_USER_NAM_META_A_ADDR, back, sizeof back);
            CHECK(memcmp(img, back, sizeof img) == 0, "the rewrite kept the registry");
            REBOOT();
            CHECK(pa_bond_load(&got) == 1 && got.ediv == 9, "the bond after the rewrite survives a reboot");
            WRITE(10);
            CHECK(pa_bond_load(&got) == 1 && got.ediv == 10, "the bond written after the rewrite is the newest");
            /* the settings log wrapping (an erase of the same sector) keeps the bond too */
            for (int i = 0; i < 70; i++) { st.drum_bpm = (uint16_t)(600 + i); CHECK(pa_settings_save(&st) == 0, "settings save %d with a bond present", i); }
            CHECK(pa_bond_load(&got) == 1 && got.ediv == 10, "the settings log wrapped and the bond survived (ediv %u)", (unsigned)got.ediv);
            REBOOT();
            CHECK(pa_bond_load(&got) == 1 && got.ediv == 10, "...also after a reboot");
            pa_spi_nor_read(PA_FLASH_USER_NAM_META_A_ADDR, back, sizeof back);
            CHECK(memcmp(img, back, sizeof img) == 0, "the registry survived too");
        }
        /* the same bond again is not written twice */
        {
            uint8_t before[PA_FLASH_BOND_SIZE], after[PA_FLASH_BOND_SIZE];
            pa_spi_nor_read(PA_FLASH_BOND_ADDR, before, sizeof before);
            STORE(10); (void)pa_bond_poll(30000u); (void)pa_bond_poll(30000u + PA_BOND_DELAY_MS + 1u);
            pa_spi_nor_read(PA_FLASH_BOND_ADDR, after, sizeof after);
            CHECK(memcmp(before, after, sizeof before) == 0, "the same bond stashed again wrote the flash");
        }
        /* forget */
        CHECK(pa_bond_forget() == 0 && pa_bond_load(&got) == 0, "a forget leaves no bond");
        REBOOT();
        CHECK(pa_bond_load(&got) == 0, "...also after a reboot (the newest record says forgotten)");
        {
            uint8_t before[PA_FLASH_BOND_SIZE], after[PA_FLASH_BOND_SIZE];
            pa_spi_nor_read(PA_FLASH_BOND_ADDR, before, sizeof before);
            CHECK(pa_bond_forget() == 0, "a forget with nothing stored");
            pa_spi_nor_read(PA_FLASH_BOND_ADDR, after, sizeof after);
            CHECK(memcmp(before, after, sizeof before) == 0, "a forget with nothing stored wrote the flash");
        }
        WRITE(11);
        CHECK(pa_bond_load(&got) == 1 && got.ediv == 11, "a new pairing after the forget is stored");
        /* a forget when the log is full: the rewrite puts nothing back */
        for (int i = 12; i <= 20; i++) WRITE(i);
        CHECK(pa_bond_load(&got) == 1 && got.ediv == 20, "bond 20 is stored");
        CHECK(pa_bond_forget() == 0 && pa_bond_load(&got) == 0, "a forget on a full log leaves no bond");
        CHECK(pa_settings_load(&ld) == 0, "...and the settings are still valid");
        REBOOT();
        CHECK(pa_bond_load(&got) == 0, "...also after a reboot");

        /* a power cut in the middle of a bond write: the old bond, or the new one, never garbage */
        {
            int old_n = 0, new_n = 0, none_n = 0, bad = 0;
            for (int k = 0; k <= 3; k++) {
                wipe_store(); pa_settings_defaults(&st); pa_settings_save(&st);
                WRITE(30);
                MKBOND(31); pa_bond_stash(r, 31, l); (void)pa_bond_poll(50000u);
                pa_spi_nor_sim_fail_after(k);
                (void)pa_bond_poll(50000u + PA_BOND_DELAY_MS + 1u);
                REBOOT();
                const int have = pa_bond_load(&got);
                if (!have) none_n++; else if (got.ediv == 30 && got.ltk[0] == 90) old_n++; else if (got.ediv == 31 && got.ltk[0] == 93) new_n++; else bad++;
            }
            CHECK(bad == 0 && none_n == 0, "a torn bond write left garbage (%d) or no bond (%d)", bad, none_n);
            printf("    torn bond append, 4 cut points: old kept %d, new committed %d\n", old_n, new_n);
        }
        /* a power cut in the middle of the sector rewrite of a full log: a valid bond (old or new) or none, never garbage; the settings follow the documented rule of the rewrite */
        {
            int ok_n = 0, none_n = 0, bad = 0;
            for (int k = 0; k <= 6; k++) {
                wipe_store(); pa_settings_defaults(&st); pa_settings_save(&st);
                for (int i = 40; i <= 47; i++) WRITE(i);
                MKBOND(48); pa_bond_stash(r, 48, l); (void)pa_bond_poll(90000u);
                pa_spi_nor_sim_fail_after(k);
                (void)pa_bond_poll(90000u + PA_BOND_DELAY_MS + 1u);
                REBOOT();
                const int have = pa_bond_load(&got);
                if (!have) none_n++;
                else if ((got.ediv == 47 && got.ltk[0] == (uint8_t)(47 * 3)) || (got.ediv == 48 && got.ltk[0] == (uint8_t)(48 * 3))) ok_n++;
                else bad++;
            }
            CHECK(bad == 0, "a torn sector rewrite left a garbage bond (%d of 7)", bad);
            printf("    torn sector rewrite of a full log, 7 cut points: a valid bond %d, none %d (the documented loss of a cut between the erase and the last program)\n", ok_n, none_n);
        }
        /* the stock's record log is never touched */
        {
            uint8_t z[16] = {0};
            CHECK(pa_spi_nor_write(0xB5000u, z, 16) == -3, "0xB5000 (the stock's record log) must still be refused");
        }
        printf("[8] bluetooth bond log: store, newest wins, 8 slots then rewrite, settings and registry kept, forget, torn writes\n");
    }

    printf(g_fail ? "\nRESULT: %d FAILURE(S)\n" : "\nRESULT: ALL PASSED\n", g_fail);
    return g_fail ? 1 : 0;
}
