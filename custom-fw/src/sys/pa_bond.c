/* SPDX-License-Identifier: GPL-3.0-only */
/* Copyright (C) 2026 The Onyx Authors (@28spawn) */
/* src/sys/pa_bond.c - the Bluetooth bond kept across a power-off (include/pa_bond.h).
 *
 * A 512-byte append log in the last part of the shared settings sector: 8 slots of 64 bytes. A slot is valid with its magic and its CRC; the newest sequence number wins; a slot with flags 0 is a
 * FORGET. A slot that was only half written (a power cut) fails its CRC and is simply not valid: the log then reads as the previous record. When all 8 slots are used the sector is rewritten
 * by settings.c (pa_flash_shared_rewrite), which asks pa_bond_snapshot for the record to put back first. Nothing here touches the stock's own record log at 0xB5000. */
#include <string.h>
#include "../../include/pa_bond.h"
#include "../../include/pa_flash_map.h"
#include "../../include/pa_spi_nor.h"
#include "../../include/pa_preset.h"          /* pa_crc32 */

#define BOND_SLOTS PA_FLASH_BOND_SLOTS

typedef struct {
    uint32_t magic;
    uint32_t seq;
    uint8_t  flags;              /* 1 = a bond, 0 = forgotten */
    uint8_t  addr_rand, have_irk, pad;
    uint8_t  rand[8];
    uint8_t  ltk[16];
    uint8_t  irk[16];
    uint8_t  addr[6];
    uint16_t ediv;
    uint32_t crc32;
} bond_slot_t;
_Static_assert(sizeof(bond_slot_t) == PA_BOND_SLOT, "a bond slot is 64 bytes");
_Static_assert(PA_FLASH_BOND_SLOTS * PA_BOND_SLOT == PA_FLASH_BOND_SIZE, "8 slots fill the bond area");

static uint8_t   s_loaded;                      /* the log was scanned (s_next, s_seq, s_have, s_cur are valid) */
static uint32_t  s_seq, s_next;
static uint8_t   s_have;
static pa_bond_t s_cur;

static pa_bond_t s_pend;
static volatile uint8_t s_pend_flag;
static uint8_t   s_armed;
static uint32_t  s_t0;

static uint8_t   s_ovr_mode;                    /* a rewrite is under way: 1 = put this bond back (s_ovr), 2 = put nothing back (a forget) */
static uint8_t   s_ovr[PA_BOND_SLOT];

__attribute__((weak)) void pa_ble_radio_forget_key(void) { }

static int slot_valid(const bond_slot_t *s)
{
    return s->magic == PA_BOND_MAGIC && s->crc32 == pa_crc32(s, (uint32_t)sizeof(*s) - sizeof(uint32_t));
}

static void slot_to_bond(const bond_slot_t *s, pa_bond_t *b)
{
    memcpy(b->rand, s->rand, 8); memcpy(b->ltk, s->ltk, 16); memcpy(b->irk, s->irk, 16); memcpy(b->addr, s->addr, 6);
    b->ediv = s->ediv; b->addr_rand = s->addr_rand; b->have_irk = s->have_irk;
}

static void bond_to_slot(bond_slot_t *s, const pa_bond_t *b, uint8_t flags, uint32_t seq)
{
    memset(s, 0, sizeof *s);
    s->magic = PA_BOND_MAGIC; s->seq = seq; s->flags = flags;
    if (b) {
        memcpy(s->rand, b->rand, 8); memcpy(s->ltk, b->ltk, 16); memcpy(s->irk, b->irk, 16); memcpy(s->addr, b->addr, 6);
        s->ediv = b->ediv; s->addr_rand = b->addr_rand; s->have_irk = b->have_irk;
    }
    s->crc32 = pa_crc32(s, (uint32_t)sizeof(*s) - sizeof(uint32_t));
}

/* read the 8 slots: the newest valid one, and the first slot that is free (a slot with anything in it, even a torn one, is used until the sector is erased) */
static int scan(void)
{
    static uint8_t buf[PA_FLASH_BOND_SIZE];
    s_loaded = 0; s_have = 0; s_seq = 0; s_next = 0;
    if (pa_spi_nor_read(PA_FLASH_BOND_ADDR, buf, sizeof buf) != 0) return -1;
    for (uint32_t i = 0; i < BOND_SLOTS; i++) {
        const bond_slot_t *s = (const bond_slot_t *)(buf + i * PA_BOND_SLOT);
        int blank = 1;
        for (uint32_t k = 0; k < PA_BOND_SLOT; k++) if (buf[i * PA_BOND_SLOT + k] != 0xFFu) { blank = 0; break; }
        if (!blank) s_next = i + 1u;
        if (slot_valid(s) && (s->seq >= s_seq)) {
            s_seq = s->seq;
            s_have = (uint8_t)((s->flags & 1u) ? 1u : 0u);
            if (s_have) slot_to_bond(s, &s_cur);
        }
    }
    s_loaded = 1;
    return 0;
}

int pa_bond_load(pa_bond_t *out)
{
    if (!s_loaded && scan() != 0) return 0;
    if (!s_have) return 0;
    if (out) *out = s_cur;
    return 1;
}

/* append one record (the sequence is one past the newest); a full log is rewritten by settings.c with this record put back */
static int append(const pa_bond_t *b, uint8_t flags)
{
    bond_slot_t rec;
    if (!s_loaded && scan() != 0) return -1;
    bond_to_slot(&rec, b, flags, s_seq + 1u);
    if (s_next >= BOND_SLOTS) {
        extern int pa_flash_shared_rewrite(const void *reg, uint32_t reg_len);      /* settings.c */
        if (flags & 1u) { memcpy(s_ovr, &rec, sizeof rec); s_ovr_mode = 1u; } else s_ovr_mode = 2u;
        const int rc = pa_flash_shared_rewrite(0, 0u);
        s_ovr_mode = 0u;
        s_loaded = 0;
        return rc == 0 ? 0 : -1;
    }
    const uint32_t at = PA_FLASH_BOND_ADDR + s_next * PA_BOND_SLOT;
    s_next++;                                                   /* whether or not the program works, this slot is used */
    if (pa_spi_nor_write(at, &rec, PA_BOND_SLOT) != 0) { s_loaded = 0; return -1; }
    pa_spi_nor_sync();
    s_seq = rec.seq;
    s_have = (uint8_t)(flags & 1u);
    if (b) s_cur = *b;
    return 0;
}

void pa_bond_stash(const uint8_t rand[8], uint16_t ediv, const uint8_t ltk[16])
{
    memcpy(s_pend.rand, rand, 8); memcpy(s_pend.ltk, ltk, 16); s_pend.ediv = ediv;
    s_pend_flag = 1u;                                           /* last: the main loop takes it from here */
}

void pa_bond_stash_id(const uint8_t irk[16], const uint8_t addr[6], uint8_t addr_rand)
{
    memcpy(s_pend.irk, irk, 16); memcpy(s_pend.addr, addr, 6); s_pend.addr_rand = addr_rand; s_pend.have_irk = 1u;
}

int pa_bond_poll(uint32_t now_ms)
{
    if (!s_pend_flag) { s_armed = 0u; return 0; }
    if (!s_armed) { s_armed = 1u; s_t0 = now_ms; return 0; }
    if ((uint32_t)(now_ms - s_t0) < PA_BOND_DELAY_MS) return 0;
    const pa_bond_t b = s_pend;
    s_pend_flag = 0u; s_armed = 0u;
    if (!s_loaded) (void)scan();
    if (s_loaded && s_have && memcmp(s_cur.rand, b.rand, 8) == 0 && s_cur.ediv == b.ediv && memcmp(s_cur.ltk, b.ltk, 16) == 0) return 0;   /* the same bond is already stored */
    return append(&b, 1u) == 0 ? 1 : -1;
}

int pa_bond_forget(void)
{
    s_pend_flag = 0u; s_armed = 0u;
    pa_ble_radio_forget_key();                                  /* the stack's live key goes too: a central that asks to encrypt now is refused, as with no bond */
    if (!s_loaded && scan() != 0) return -1;
    if (!s_have) return 0;                                      /* nothing is stored: nothing to write */
    return append(0, 0u);
}

int pa_bond_snapshot(uint8_t rec[PA_BOND_SLOT])
{
    if (s_ovr_mode == 1u) { memcpy(rec, s_ovr, PA_BOND_SLOT); return 1; }
    if (s_ovr_mode == 2u) return 0;
    static uint8_t buf[PA_FLASH_BOND_SIZE];
    if (pa_spi_nor_read(PA_FLASH_BOND_ADDR, buf, sizeof buf) != 0) return 0;
    uint32_t best = 0; int found = 0; uint32_t best_i = 0;
    for (uint32_t i = 0; i < BOND_SLOTS; i++) {
        const bond_slot_t *s = (const bond_slot_t *)(buf + i * PA_BOND_SLOT);
        if (slot_valid(s) && (!found || s->seq >= best)) { best = s->seq; best_i = i; found = 1; }
    }
    if (!found) return 0;
    const bond_slot_t *s = (const bond_slot_t *)(buf + best_i * PA_BOND_SLOT);
    if (!(s->flags & 1u)) return 0;                             /* the newest record is a forget: after the erase the log is empty, which says the same */
    memcpy(rec, s, PA_BOND_SLOT);
    return 1;
}

void pa_bond_rewritten(int restored)
{
    (void)restored;
    s_loaded = 0;                                               /* the next call reads the sector again */
}
