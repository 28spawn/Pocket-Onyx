/*
 * pa_flash_map.h - the Pocket Amp's SPI NOR flash, as it really is: ONE MEGABYTE (JEDEC ID 0x856014).
 *
 * THE RULE (owner's, 2026-11-04): this firmware writes the flash ONLY inside its own window, 0x86000..0xB3FFF (and, during an update, the five staging sectors 0xB8000..0xBCFFF). Everything else belongs to the stock firmware or to the chip and is never written
 * by it: the boot area, the stock's record log, its VM, BTIF, USER, the factory bundle, its saved presets and its save record. So a unit that goes back to the original firmware is the original again:
 * nothing of the stock's settings, presets or calibration was ever overwritten.
 *
 * Partition map facts (tools/jl_app_area.py):
 *
 *   0x000000..0x003FFF   16 KB  boot record, file table, bootloader (uboot)                      NEVER written
 *   0x004000..0x085FFF  520 KB  APP AREA: the area header at 0x4000, app.bin at 0x4120           the pack writes only this (app.bin up to 530,467 B in an Onyx package)
 *                               (the stock's own app is 723 KB and runs on to 0xB4B93; an Onyx pack ends before 0x86000, the tail beyond it is preserved)
 *   0x086000..0x0B3FFF  184 KB  OUR WINDOW, the only rewritable flash (46 sectors):
 *                               0x086000 cabinet registry A, 0x087000 cabinet registry B,
 *                               0x088000 the shared sector: system settings log (2 KB), the user NAM registry (1.5 KB), the Bluetooth bond log (512 B),
 *                               0x089000..0x08CFFF preset bank B (16 KB), 0x08D000..0x090FFF preset bank A (16 KB),
 *                               0x091000..0x0A4FFF NAM blob slots 0..9 (8 KB each),
 *                               0x0A5000..0x0B3FFF cabinet slots 0..14 (4 KB each).
 *   0x0B4000..0x0B4FFF    4 KB  the V1.05 trailer sits at 0xB4FF8 (6 bytes the stock uboot may check): NEVER WRITTEN.
 *   0x0B5000..0x0BCFFF   32 KB  the STOCK's VM (its record log 55 AA AA 55 at 0xB5000). NEVER WRITTEN by the firmware. The update session stages the update loader in 0xB8000..0xBCFFF for the
 *                               moment of an update only (src/sys/pa_ota_session.c) and erases it again.
 *   0x0BD000..0x0BDFFF    4 KB  BTIF: radio calibration + Bluetooth MAC                         NEVER written
 *   0x0BE000..0x0BEFFF    4 KB  USER: factory calibration                                       NEVER written
 *   0x0BF000..0x0FAFFF 240 KB   factory amp/cab bundle, READ ONLY: the firmware reads it in place
 *   0x0FB000..0x0FEFFF   16 KB  the STOCK's saved presets                                       NEVER written (before pack 194 this was our preset bank A)
 *   0x0FF000..0x0FFFFF    4 KB  the stock's save record ("save1")                               NEVER written
 *
 * WHY SO LITTLE ROOM: 46 sectors. Two preset banks (4 sectors each, torn-write safe) take 8, the shared settings / NAM registry / bond sector 1, the cabinet registry 2; that leaves 35: ten NAM
 * captures (2 sectors each) and fifteen cabinets (1 sector each).
 *
 * The device writes the flash through the SRAM driver in src/board/flash_ram.c (erase 4 KB, program 256 B, any read); every write is refused unless it lies inside the window below. A save mutes
 * the output, parks core 1 and masks the interrupts for its duration (a few ms for the settings, about 0.2 s for a preset bank).
 */
#ifndef PA_FLASH_MAP_H
#define PA_FLASH_MAP_H

#include <stdint.h>

#define PA_FLASH_SIZE               0x00100000u  /* 1 MB: the whole chip */
#define PA_FLASH_SECTOR_SIZE        4096u        /* 4 KB sector erase (0x20) */
#define PA_FLASH_PAGE_SIZE          256u         /* 256 B page program (0x02) */

/* The app area. The XIP window maps flash 0x4000 (the area header) at 0x02000000, so app.bin (flash 0x4120) is at 0x02000120, which is the link address. On the device,
 * reads outside [APP_WINDOW_START, APP_WINDOW_END] return -2 until the read driver exists. */
#define PA_FLASH_APP_WINDOW_START   0x00004000u
#define PA_FLASH_APP_WINDOW_END     0x00085FFFu   /* the image's last byte; 0x86000..0xB4FFF is the user store (read through the SRAM driver, like every rewritable window) */
#define PA_FLASH_IMAGE_END          PA_FLASH_APP_WINDOW_END

/* Read-only parts of the chip. */
#define PA_FLASH_MBR_ADDR           0x00000000u
#define PA_FLASH_UBOOT_ADDR         0x00001000u
#define PA_FLASH_APP_ADDR           0x00004000u
#define PA_FLASH_STOCK_LOG_ADDR     0x000B5000u  /* 4 KB - the stock firmware's record log (55 AA AA 55): live data, never written */
#define PA_FLASH_STOCK_SAVE_ADDR    0x000FF000u  /* 4 KB - the stock's save record ("save1"): never written */
#define PA_FLASH_BTIF_ADDR          0x000BD000u  /* 4 KB - RF calibration & BLE MAC: never touch */
#define PA_FLASH_USER_ADDR          0x000BE000u  /* 4 KB - factory calibration: never touch */
#define PA_FLASH_FACTORY_CAB_ADDR   0x000BF000u  /* the factory amp/cab bundle */
#define PA_FLASH_FACTORY_CAB_SIZE   0x0003C000u  /* 245,760 B = 20 amp records x 0x2000 + 20 cab records x 0x1000 */

/* The one window the device may write (and that the host simulation lets you write). Everything else is refused (-3). */
#define PA_FLASH_RW3_START          0x00086000u  /* our window: 46 sectors above the app image, up to (not including) 0xB4000, the sector with the V1.05 trailer at 0xB4FF8 */
#define PA_FLASH_RW3_END            0x000B4000u
/* The update staging area: five sectors at the top of the stock's VM, blank in the factory image. Only src/sys/pa_ota_session.c writes it, only while an update session runs: it puts the update loader
 * and its record there, and erases them again if the update does not start (or at the next boot). Nothing else of the stock's VM is writable. */
#define PA_FLASH_OTA_START          0x000B8000u
#define PA_FLASH_OTA_END            0x000BD000u  /* exclusive: BTIF starts here */

/* 1 if [addr, addr + len) lies completely inside one rewritable window. */
static inline int pa_flash_rw_ok(uint32_t addr, uint32_t len) {
    if (len == 0u || len > PA_FLASH_SIZE || addr > PA_FLASH_SIZE - len) return 0;
    const uint32_t end = addr + len;
    return (addr >= PA_FLASH_RW3_START && end <= PA_FLASH_RW3_END) ||
           (addr >= PA_FLASH_OTA_START && end <= PA_FLASH_OTA_END);
}

/* Preset store: two banks that alternate (torn-write safe), 16 KB each (16 B header + 80 x 184 B = 14,736 B; 1,648 B headroom). */
#define PA_FLASH_STORE_BANK_B_ADDR  0x00089000u
#define PA_FLASH_STORE_BANK_A_ADDR  0x0008D000u
#define PA_FLASH_STORE_BANK_SIZE    0x00004000u

/* The shared sector: the system settings log (the first 2 KB: an append log of 64-byte slots, 32 slots), the user NAM registry (1.5 KB, one copy; rewriting it erases the sector and puts the newest
 * settings record and the newest bond back, see src/sys/settings.c pa_flash_shared_rewrite) and the Bluetooth bond log (512 B). */
#define PA_FLASH_SHARED_SECTOR      0x00088000u
#define PA_FLASH_SYS_SETTINGS_ADDR  0x00088000u
#define PA_FLASH_SYS_SETTINGS_SIZE  0x00000800u
#define PA_FLASH_USER_NAM_META_A_ADDR 0x00088800u
#define PA_FLASH_USER_NAM_META_B_ADDR PA_FLASH_USER_NAM_META_A_ADDR   /* one copy */
#define PA_FLASH_USER_NAM_META_ADDR   PA_FLASH_USER_NAM_META_A_ADDR
#define PA_FLASH_USER_NAM_META_SIZE   0x00000600u   /* the registry's part of the shared sector (the registry is 1,136 B) */
#define PA_FLASH_BOND_ADDR            0x00088E00u   /* the Bluetooth bond: an append log of 64-byte slots (src/sys/pa_bond.c) */
#define PA_FLASH_BOND_SIZE            0x00000200u
#define PA_FLASH_BOND_SLOTS           8u

/* User NAM captures: a capture is an A2-Lite WaveNet of 7,504 B or a GRU of 3,852 B, so a blob slot is 8 KB (two sectors). Only PA_FLASH_USER_NAM_BLOB_SLOTS captures fit in the flash; the registry has
 * PA_USER_NAM_MAX_COUNT logical entries because the captures linked into the image (tools/pa_import.py) are entries too, and they take no flash. */
#define PA_FLASH_USER_NAM_SLOT_SIZE  0x00002000u /* 8 KB per capture */
#define PA_FLASH_USER_NAM_BLOB_SLOTS 10u         /* blob slots 0..9 from 0x91000 to 0xA4FFF */
#define PA_FLASH_USER_NAM_STORE_ADDR 0x00091000u

/* The physical address of NAM blob slot p (0..9). */
static inline uint32_t pa_flash_nam_slot_addr(uint32_t p) {
    return PA_FLASH_USER_NAM_STORE_ADDR + p * PA_FLASH_USER_NAM_SLOT_SIZE;
}

/* User cabinets: a registry that alternates between two sectors (torn-write safe), then 15 slots of one sector each (1024 Q18 taps = 4,096 B). */
#define PA_FLASH_CAB_REG_A_ADDR     0x00086000u
#define PA_FLASH_CAB_REG_B_ADDR     0x00087000u
#define PA_FLASH_CAB_ADDR           0x000A5000u
#define PA_FLASH_CAB_SLOT_SIZE      0x00001000u
#define PA_FLASH_CAB_SLOTS          15u

#endif /* PA_FLASH_MAP_H */
