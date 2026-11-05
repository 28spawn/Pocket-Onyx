/*
 * pa_user_cab.h - the cabinets the owner stored in the flash (the editor page, or any host that sends the cabinet upload).
 *
 * 15 slots, one 4 KB sector each at PA_FLASH_CAB_ADDR (a cabinet is at most 1024 Q18 taps = 4,096 B). A slot's index is its logical index for good: a cabinet type in a preset is
 * 20 stock + the cabinets linked into the image + the slot index, so deleting a cabinet leaves a hole instead of moving the ones after it. The lists skip the holes and the count shown
 * is the number of cabinets that exist.
 *
 * The registry (names, tap counts, CRC32 of the taps) alternates between two sectors with a sequence number, so a power cut while it is written leaves the older copy valid. A slot
 * is written before the registry says it exists (erase, program, then the registry), and a CRC32 mismatch at load refuses the cabinet instead of playing noise.
 */
#ifndef PA_USER_CAB_H
#define PA_USER_CAB_H

#include <stdint.h>
#include "pa_flash_map.h"

#define PA_USER_CAB_SLOTS     PA_FLASH_CAB_SLOTS      /* 15 */
#define PA_USER_CAB_NAME_LEN  16u                     /* 15 characters and the terminator */
#define PA_USER_CAB_MAX_TAPS  1024u
#define PA_USER_CAB_MIN_TAPS  16u
#define PA_USER_CAB_MAGIC     0x43414231u             /* 'CAB1' */

typedef struct {
    uint8_t  active;
    uint8_t  pad;
    uint16_t taps;                                    /* 16..1024 */
    uint32_t crc32;                                   /* of the taps as stored */
    char     name[PA_USER_CAB_NAME_LEN];
} pa_user_cab_slot_t;                                 /* 24 bytes */

typedef struct {
    uint32_t           magic;
    uint32_t           seq;
    pa_user_cab_slot_t slots[PA_USER_CAB_SLOTS];
    uint32_t           crc32;
} pa_user_cab_registry_t;                             /* 372 bytes */

void        pa_user_cab_init(void);
uint8_t     pa_user_cab_span(void);                   /* the highest used slot + 1 (0 = none): the lists run over this many logical indices */
uint8_t     pa_user_cab_active_count(void);           /* the cabinets that exist */
int         pa_user_cab_is_active(uint8_t slot);
const char *pa_user_cab_name(uint8_t slot);           /* "" for a hole */
/* Store the taps (int32 Q18, 16..1024) in `slot` (0..14), or in the first free slot when slot is 0xFF. Returns the slot, or: -1 bad argument, -2 the slot is not free and not asked for, -5 no free
 * slot, -3 / -4 the flash refused, -6 the registry could not be written. A replaced cabinet is overwritten in place. */
int         pa_user_cab_add(const char *name, int slot, const int32_t *taps, uint32_t n);
int         pa_user_cab_clear(void);                  /* every cabinet gone and the whole area (registry sectors and all slots) erased: -> sectors erased or < 0 */
int         pa_user_cab_delete(uint8_t slot);         /* the slot is cleared (the registry first, then the sector is erased) */
int         pa_user_cab_rename(uint8_t slot, const char *name);
/* The taps of `slot`, padded with zeros to out_cap; *n is the stored tap count. Refuses (-3) a slot whose CRC32 does not match. */
int         pa_user_cab_load(uint8_t slot, int32_t *out, uint32_t out_cap, uint32_t *n);

#endif
