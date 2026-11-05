/* src/sys/pa_user_cab.c - the stored cabinets (include/pa_user_cab.h). */
#include <string.h>
#include "../../include/pa_user_cab.h"
#include "../../include/pa_spi_nor.h"
#include "../../include/pa_preset.h"          /* pa_crc32 */

_Static_assert(sizeof(pa_user_cab_slot_t) == 24u, "a cabinet slot is 24 bytes");
_Static_assert(sizeof(pa_user_cab_registry_t) <= PA_FLASH_CAB_SLOT_SIZE, "the registry fits one sector");

static pa_user_cab_registry_t s_reg;
static uint8_t  s_inited;
static uint32_t s_active_addr;                       /* the sector the newest valid copy is in */

static int reg_valid(uint32_t addr, pa_user_cab_registry_t *out) {
    if (pa_spi_nor_read(addr, out, (uint32_t)sizeof(*out)) != 0) return 0;
    return out->magic == PA_USER_CAB_MAGIC && out->crc32 == pa_crc32(out, (uint32_t)sizeof(*out) - 4u);
}

void pa_user_cab_init(void) {
    if (s_inited) return;
    pa_spi_nor_init();
    pa_user_cab_registry_t a, b;
    const int va = reg_valid(PA_FLASH_CAB_REG_A_ADDR, &a), vb = reg_valid(PA_FLASH_CAB_REG_B_ADDR, &b);
    memset(&s_reg, 0, sizeof s_reg);
    s_active_addr = PA_FLASH_CAB_REG_A_ADDR;
    if (va && (!vb || (int32_t)(a.seq - b.seq) >= 0)) { s_reg = a; s_active_addr = PA_FLASH_CAB_REG_A_ADDR; }
    else if (vb) { s_reg = b; s_active_addr = PA_FLASH_CAB_REG_B_ADDR; }
    else s_reg.magic = PA_USER_CAB_MAGIC;              /* a blank flash, or the padding the pack leaves there: no cabinets */
    s_inited = 1;
}

/* write the registry to the OTHER sector with the next sequence number; the older copy stays valid until this one is complete */
static int commit(void) {
    const uint32_t to = (s_active_addr == PA_FLASH_CAB_REG_A_ADDR) ? PA_FLASH_CAB_REG_B_ADDR : PA_FLASH_CAB_REG_A_ADDR;
    pa_user_cab_registry_t r = s_reg;
    r.magic = PA_USER_CAB_MAGIC; r.seq = s_reg.seq + 1u;
    r.crc32 = pa_crc32(&r, (uint32_t)sizeof(r) - 4u);
    if (pa_spi_nor_erase_sector(to) != 0) return -6;
    if (pa_spi_nor_write(to, &r, (uint32_t)sizeof(r)) != 0) return -6;
    pa_spi_nor_sync();
    s_reg = r; s_active_addr = to;
    return 0;
}

uint8_t pa_user_cab_span(void) {
    pa_user_cab_init();
    uint8_t n = 0;
    for (uint8_t i = 0; i < PA_USER_CAB_SLOTS; i++) if (s_reg.slots[i].active) n = (uint8_t)(i + 1u);
    return n;
}
uint8_t pa_user_cab_active_count(void) {
    pa_user_cab_init();
    uint8_t n = 0;
    for (uint8_t i = 0; i < PA_USER_CAB_SLOTS; i++) n = (uint8_t)(n + (s_reg.slots[i].active ? 1u : 0u));
    return n;
}
int pa_user_cab_is_active(uint8_t slot) { pa_user_cab_init(); return slot < PA_USER_CAB_SLOTS && s_reg.slots[slot].active; }
const char *pa_user_cab_name(uint8_t slot) { pa_user_cab_init(); return (slot < PA_USER_CAB_SLOTS && s_reg.slots[slot].active) ? s_reg.slots[slot].name : ""; }

static void set_name(char *dst, const char *name) {
    uint32_t k = 0;
    memset(dst, 0, PA_USER_CAB_NAME_LEN);                    /* a shorter name must not keep the end of the old one */
    for (; name && name[k] && k < PA_USER_CAB_NAME_LEN - 1u; k++) { char c = name[k]; dst[k] = (c >= 32 && c < 127) ? c : '_'; }
    while (k && dst[k - 1] == ' ') k--;
    dst[k] = '\0';
    if (!k) { const char *d = "USER CAB"; for (k = 0; d[k]; k++) dst[k] = d[k]; dst[k] = '\0'; }
}

int pa_user_cab_add(const char *name, int slot, const int32_t *taps, uint32_t n) {
    pa_user_cab_init();
    if (!taps || n < PA_USER_CAB_MIN_TAPS || n > PA_USER_CAB_MAX_TAPS) return -1;
    if (slot == 0xFF) {
        slot = -1;
        for (int i = 0; i < (int)PA_USER_CAB_SLOTS; i++) if (!s_reg.slots[i].active) { slot = i; break; }
        if (slot < 0) return -5;
    } else if (slot < 0 || slot >= (int)PA_USER_CAB_SLOTS) return -1;
    const uint32_t addr = PA_FLASH_CAB_ADDR + (uint32_t)slot * PA_FLASH_CAB_SLOT_SIZE;
    pa_spi_nor_begin();                                   /* the erase, the program and the registry are one quiesce of the audio */
    int rc = 0;
    if (pa_spi_nor_erase_sector(addr) != 0) rc = -3;
    else if (pa_spi_nor_write(addr, taps, n * 4u) != 0) rc = -4;
    else {
        pa_spi_nor_sync();
        pa_user_cab_slot_t *s = &s_reg.slots[slot];
        const pa_user_cab_slot_t old = *s;
        s->active = 1; s->pad = 0; s->taps = (uint16_t)n; s->crc32 = pa_crc32(taps, n * 4u);
        set_name(s->name, name);
        if (commit() != 0) { *s = old; rc = -6; }
    }
    pa_spi_nor_end();
    return rc ? rc : slot;
}

int pa_user_cab_delete(uint8_t slot) {
    pa_user_cab_init();
    if (slot >= PA_USER_CAB_SLOTS || !s_reg.slots[slot].active) return -1;
    pa_spi_nor_begin();
    const pa_user_cab_slot_t old = s_reg.slots[slot];
    memset(&s_reg.slots[slot], 0, sizeof s_reg.slots[slot]);
    int rc = commit();
    if (rc != 0) s_reg.slots[slot] = old;
    else (void)pa_spi_nor_erase_sector(PA_FLASH_CAB_ADDR + (uint32_t)slot * PA_FLASH_CAB_SLOT_SIZE);   /* gone from the flash too (the registry no longer names it, so a failed erase costs nothing) */
    pa_spi_nor_end();
    return rc;
}

/* Clear every stored cabinet and wipe the whole cabinet area: both registry sectors and all slot sectors are erased, so nothing an old or interrupted upload left remains.
 * Returns the number of sectors erased, or a negative code. */
int pa_user_cab_clear(void) {
    pa_user_cab_init();
    pa_spi_nor_begin();
    int rc = 0, n = 0;
    const uint32_t regs[2] = { PA_FLASH_CAB_REG_A_ADDR, PA_FLASH_CAB_REG_B_ADDR };
    for (int i = 0; i < 2 && !rc; i++) { if (pa_spi_nor_erase_sector(regs[i]) != 0) rc = -3; else n++; }
    for (uint32_t s = 0; s < PA_USER_CAB_SLOTS && !rc; s++) { if (pa_spi_nor_erase_sector(PA_FLASH_CAB_ADDR + s * PA_FLASH_CAB_SLOT_SIZE) != 0) rc = -3; else n++; }
    pa_spi_nor_sync();
    memset(&s_reg, 0, sizeof s_reg); s_reg.magic = PA_USER_CAB_MAGIC; s_active_addr = PA_FLASH_CAB_REG_A_ADDR;
    pa_spi_nor_end();
    return rc ? rc : n;
}

int pa_user_cab_rename(uint8_t slot, const char *name) {
    pa_user_cab_init();
    if (slot >= PA_USER_CAB_SLOTS || !s_reg.slots[slot].active || !name || !name[0]) return -1;
    char old[PA_USER_CAB_NAME_LEN]; memcpy(old, s_reg.slots[slot].name, sizeof old);
    set_name(s_reg.slots[slot].name, name);
    const int rc = commit();
    if (rc != 0) memcpy(s_reg.slots[slot].name, old, sizeof old);
    return rc;
}

int pa_user_cab_load(uint8_t slot, int32_t *out, uint32_t out_cap, uint32_t *n) {
    pa_user_cab_init();
    if (slot >= PA_USER_CAB_SLOTS || !s_reg.slots[slot].active || !out) return -1;
    const uint32_t t = s_reg.slots[slot].taps;
    if (t > out_cap || t < PA_USER_CAB_MIN_TAPS) return -2;
    if (pa_spi_nor_read(PA_FLASH_CAB_ADDR + (uint32_t)slot * PA_FLASH_CAB_SLOT_SIZE, out, t * 4u) != 0) return -3;
    if (pa_crc32(out, t * 4u) != s_reg.slots[slot].crc32) return -3;
    for (uint32_t i = t; i < out_cap; i++) out[i] = 0;
    if (n) *n = t;
    return 0;
}
