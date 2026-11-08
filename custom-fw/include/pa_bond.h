/* SPDX-License-Identifier: GPL-3.0-only */
/* Copyright (C) 2026 The Onyx Authors (@28spawn) */
/* include/pa_bond.h - the Bluetooth bond kept across a power-off.
 *
 * The stack (third_party/optimist_ble) pairs a central (Just Works, LE legacy) and calls ble_app_bond() with the keys it made: Rand, EDIV and our LTK. It keeps ONE key slot in RAM
 * (ble_host_set_key), so a power-off forgot the bond and the central, which still had it, was refused when it asked to encrypt with it (the link died: "remove the device in Windows first").
 *
 * The stock firmware keeps a bond the same way: an append-only record log in flash, the key blob first and the index after, so a power cut in between leaves the old state (stock log at 0xB5000:
 * ids 114 and 113). Ours is a separate 512-byte log in the last part of the shared settings sector (pa_flash_map.h): 8 slots of 64 bytes, a slot is valid with its magic and CRC, the newest
 * sequence wins, a "forgotten" slot (flags 0) is a record like any other. When the 8 slots are used the sector is rewritten (settings.c pa_flash_shared_rewrite keeps the newest bond).
 *
 * The radio's interrupt only STASHES the keys in RAM (pa_bond_stash: no flash from an interrupt); the main loop writes them about 1.5 s later (pa_bond_poll), the way settings are saved. */
#ifndef PA_BOND_H
#define PA_BOND_H
#include <stdint.h>

#define PA_BOND_MAGIC 0x444E4F42u        /* "BOND" */
#define PA_BOND_SLOT  64u
#define PA_BOND_DELAY_MS 1500u           /* from the first stash to the write: the identity (IRK, address) arrives just after the keys */

typedef struct {
    uint8_t  rand[8];
    uint16_t ediv;
    uint8_t  ltk[16];
    uint8_t  irk[16];                    /* the peer's identity resolving key and address, when it sent them (have_irk) */
    uint8_t  addr[6];
    uint8_t  addr_rand;
    uint8_t  have_irk;
} pa_bond_t;

/* the newest valid record -> 1 and *out filled when it is a bond, 0 when there is none (nothing stored, or the newest record is a forget) */
int  pa_bond_load(pa_bond_t *out);
/* from the radio's interrupt: keep the keys in RAM and ask for the write (no flash access here) */
void pa_bond_stash(const uint8_t rand[8], uint16_t ediv, const uint8_t ltk[16]);
void pa_bond_stash_id(const uint8_t irk[16], const uint8_t addr[6], uint8_t addr_rand);
/* main loop, every pass: writes a stashed bond PA_BOND_DELAY_MS after it was stashed (unless it equals what is stored) -> 1 when it wrote, -1 when the write failed */
int  pa_bond_poll(uint32_t now_ms);
/* FORGET: a record that says there is no bond, and the stack's live key is cleared too (pa_ble_radio_forget_key) -> 0 ok */
int  pa_bond_forget(void);

/* for settings.c pa_flash_shared_rewrite: the record the erased sector must get back (the newest bond, or the one being appended), and the call that tells the log the sector was rewritten */
int  pa_bond_snapshot(uint8_t rec[PA_BOND_SLOT]);
void pa_bond_rewritten(int restored);

/* board hooks (weak no-ops on the host): the stack's live key slot */
void pa_ble_radio_forget_key(void);
#endif
