/* test/test_ota_session.c - the firmware's half of the vendor MIDI update (src/sys/pa_ota_session.c) against a simulated host that serves a REAL package (the .fwsc files on disk) and a simulated flash.
 *   test_ota_session <package.fwsc> [<second.fwsc> ...]
 * Checks: the loader is staged byte for byte (head, body, record), nothing outside the five staging sectors is touched, the record has the SPL's markers, a host that never says "success" commits nothing and leaves the
 * area erased, a damaged loader or a junk file is refused, the boot clean-up erases exactly a staging area and nothing else, and every flash operation the core asks for stays inside the staging area. */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "../include/pa_ota_session.h"
#include "../include/pa_flash_map.h"

static int fails;
static void check(int ok, const char *msg) { printf("  %s %s\n", ok ? "ok  " : "FAIL", msg); fails += !ok; }

static uint8_t flash[0x100000], flash0[0x100000];
static uint8_t *img; static uint32_t img_len;             /* the package as the device addresses it (the .fwsc without its 20 marker bytes) */
static uint8_t q[8][700]; static uint32_t qn[8], qh, qt;  /* frames from the host */
static int say_success, committed, bad_ops; static uint8_t parm_seen[112]; static uint32_t ms;
static uint32_t reads_total;

int pa_ota_hw_send(const uint8_t *m, uint32_t n) {
    uint8_t d[64]; uint32_t u = pa_ota_unpack7(m + 1, n - 2, d, sizeof d);
    if (u != 15 || d[0] != 0 || d[1] != 0x59 || d[2] != 0x30) return 0;           /* only read requests are answered here */
    uint32_t addr = d[7] | d[8] << 8 | d[9] << 16 | (uint32_t)d[10] << 24, len = d[11] | d[12] << 8;
    uint8_t body[700]; uint32_t k = 0, s = 0;
    uint8_t data[520]; uint32_t dl = 0;
    if (addr == 0xE0000000u) { if (!say_success) return 0; memcpy(data, "success", 8); dl = 8; }
    else { if (len > 512 || addr + len > img_len) return 0; memcpy(data, img + addr, len); dl = len; }
    reads_total++;
    body[k++] = 0; body[k++] = 0x59; body[k++] = 0x30; body[k++] = (dl + 8) & 0xFF; body[k++] = (dl + 8) >> 8; body[k++] = 0; body[k++] = 0;
    body[k++] = addr; body[k++] = addr >> 8; body[k++] = addr >> 16; body[k++] = addr >> 24; body[k++] = dl; body[k++] = dl >> 8; body[k++] = 0;
    for (uint32_t i = 0; i < dl; i++) body[k++] = data[i];
    for (uint32_t i = 6; i < k; i++) s += body[i];
    body[k++] = ~s;
    uint32_t w = pa_ota_pack7(body, k, q[qt & 7]); qn[qt & 7] = w; qt++;
    return 0;
}
int pa_ota_hw_frame(uint8_t *buf, uint32_t cap, uint32_t *n) { if (qh == qt) return 0; if (qn[qh & 7] > cap) { qh++; return 0; } memcpy(buf, q[qh & 7], qn[qh & 7]); *n = qn[qh & 7]; qh++; return 1; }
static uint8_t scratch[3072];
uint8_t *pa_ota_hw_scratch(uint32_t *n) { memset(scratch, 0xA5, sizeof scratch); *n = sizeof scratch; return scratch; }
uint32_t pa_ota_hw_ms(void) { return ms; }
void pa_ota_hw_idle(void) { ms += 1; }
static int in_area(uint32_t o, uint32_t n) { return o >= PA_OTA_AREA && o + n <= PA_OTA_AREA + PA_OTA_AREA_LEN && pa_flash_rw_ok(o, n); }   /* and the device's own fence must allow it: a window the session needs but the map forbids stopped the first web install on the unit */
int pa_ota_hw_erase(uint32_t o) { if (!in_area(o, 0x1000) || (o & 0xFFF)) { bad_ops++; return -8; } memset(flash + o, 0xFF, 0x1000); return 0; }
int pa_ota_hw_prog(uint32_t o, const void *p, uint32_t n) { if (!in_area(o, n)) { bad_ops++; return -8; } for (uint32_t i = 0; i < n; i++) flash[o + i] &= ((const uint8_t *)p)[i]; return 0; }
int pa_ota_hw_read(uint32_t o, void *p, uint32_t n) { memcpy(p, flash + o, n); return 0; }
void pa_ota_hw_show(uint32_t s, int32_t c) { (void)s; (void)c; }
static int wiped;
void pa_ota_hw_wipe(void) { wiped++; }
void pa_ota_hw_commit(const uint8_t *parm) { committed++; memcpy(parm_seen, parm, 112); }

static uint8_t *slurp(const char *path, uint32_t *n) {
    FILE *f = fopen(path, "rb"); if (!f) return NULL; fseek(f, 0, SEEK_END); long l = ftell(f); fseek(f, 0, SEEK_SET);
    uint8_t *b = malloc(l); if (fread(b, 1, l, f) != (size_t)l) { fclose(f); return NULL; } fclose(f); *n = (uint32_t)l; return b;
}
static void load_package(const char *path) {
    uint32_t n; uint8_t *b = slurp(path, &n); free(img); img = NULL; img_len = 0; if (!b) return;
    img = malloc(n); img_len = n - 20; uint32_t o = 0;
    for (int i = 0; i < 20; i++) { memcpy(img + o, b + i * 48, 47); o += 47; }
    memcpy(img + o, b + 960, n - 960); free(b);
}
static void xh(uint8_t *d, uint32_t n) { uint32_t k = 0xFFFF; for (uint32_t i = 0; i < n; i++) { d[i] ^= (uint8_t)k; k = ((k << 1) ^ (k & 0x8000 ? 0x1021 : 0)) & 0xFFFF; } }
static void rst(void) { wiped = 0; memset(flash, 0xFF, sizeof flash); qh = qt = 0; say_success = 1; committed = 0; ms = 0; reads_total = 0; }

static void one(const char *path) {
    printf("=== %s ===\n", path);
    load_package(path); if (!img) { check(0, "the package file can be read"); return; }
    /* the package's own loader: entry type 100 */
    uint8_t h[0x400]; memcpy(h, img, 0x400); xh(h, 0x40); uint32_t nent = h[8] | h[9] << 8, lo = 0, ll = 0;
    for (uint32_t i = 0; i < nent; i++) { uint8_t e[0x50]; memcpy(e, img + 0x40 + i * 0x50, 0x50); xh(e, 0x50); if ((e[0] | e[1] << 8) == 100) { lo = e[8] | e[9] << 8 | e[10] << 16 | (uint32_t)e[11] << 24; ll = e[12] | e[13] << 8 | e[14] << 16 | (uint32_t)e[15] << 24; } }
    check(lo && ll, "the package carries its update loader (entry 100)");
    /* a flash with data in it around the staging area: settings, a capture, a preset bank, the user store, the boot area */
    rst(); for (uint32_t i = 0; i < sizeof flash; i++) flash[i] = (uint8_t)(i * 7 + 3);
    memcpy(flash0, flash, sizeof flash);
    int rc = pa_ota_session_run();
    check(rc == 0 && committed == 1, "a good package: the session commits once");
    check(wiped == 1, "and our own store is wiped once, just before the reset");
    uint32_t len = ll - 0x20;
    check(memcmp(flash + PA_OTA_AREA + 32, img + lo + 0x20, len) == 0, "the loader's body is staged byte for byte");
    check(memcmp(flash + PA_OTA_AREA + 16, "LOADER.BIN", 11) == 0, "the staging head names LOADER.BIN");
    check(memcmp(flash + PA_OTA_RES, parm_seen, 112) == 0, "the update record is in the flash exactly as handed to the reset");
    check((parm_seen[2] | parm_seen[3] << 8) == 0x5A0D && (parm_seen[4] | parm_seen[5] << 8) == 0x5A01 && (parm_seen[6] | parm_seen[7] << 8) == 0x5441, "the record has the SPL's markers");
    check(memcmp(parm_seen + 8, "ota-Pocket AMP_105", 19) == 0, "the record names the loader's identity ota-Pocket AMP_105");
    check((parm_seen[72] | parm_seen[73] << 8 | parm_seen[74] << 16) == PA_OTA_AREA, "the record names the staging area");
    int outside = 0; for (uint32_t i = 0; i < sizeof flash; i++) if ((i < PA_OTA_AREA || i >= PA_OTA_AREA + PA_OTA_AREA_LEN) && flash[i] != flash0[i]) outside++;
    check(outside == 0 && bad_ops == 0, "nothing outside the five staging sectors was touched (the boot area included)");
    check(pa_ota_boot_cleanup() == 1 && flash[PA_OTA_AREA] == 0xFF && flash[PA_OTA_RES] == 0xFF, "after the update the boot clean-up erases the staging area");
    check(pa_ota_boot_cleanup() == 0, "and finds nothing the second time");
    /* no "success" from the host */
    rst(); say_success = 0; memcpy(flash0, flash, sizeof flash); rc = pa_ota_session_run();
    check(rc < 0 && committed == 0 && wiped == 0, "a host that never says success: nothing is committed and nothing of ours is wiped");
    int dirty = 0; for (uint32_t i = 0; i < PA_OTA_AREA_LEN; i++) dirty += flash[PA_OTA_AREA + i] != 0xFF;
    check(dirty == 0, "and the staging area is left erased");
    /* a damaged loader */
    uint8_t keep = img[lo + 0x100]; img[lo + 0x100] ^= 0x55; rst(); rc = pa_ota_session_run(); img[lo + 0x100] = keep;
    check(rc < 0 && committed == 0, "a loader with a flipped byte is refused");
    /* a junk package */
    uint8_t save[0x400]; memcpy(save, img, 0x400); for (int i = 0; i < 0x40; i++) img[i] ^= 0xA5; rst(); rc = pa_ota_session_run(); memcpy(img, save, 0x400);
    check(rc < 0 && committed == 0, "a package with a broken header is refused");
    /* the boot clean-up leaves this firmware's own data alone */
    rst(); for (uint32_t i = 0; i < sizeof flash; i++) flash[i] = (uint8_t)(i * 7 + 3); memcpy(flash0, flash, sizeof flash);
    check(pa_ota_boot_cleanup() == 0 && memcmp(flash, flash0, sizeof flash) == 0, "the clean-up does not touch settings, a capture or a preset bank in the same flash");
}

int main(int argc, char **argv) {
    if (argc < 2) { printf("usage: test_ota_session package.fwsc [...]\n"); return 2; }
    for (int i = 1; i < argc; i++) one(argv[i]);
    printf("\n%s\n", fails ? "THE OTA SESSION HAS FAILURES" : "THE OTA SESSION WORKS");
    return fails != 0;
}
