/* SPDX-License-Identifier: MPL-2.0
 * Copyright (C) 2026 Leo Kuroshita (@kurogedelic), Hügelton Instruments */
/* The Pocket AMP's update loader: loader/loader.c with this board's flash layout, USB names and panel.
 *
 * Flash: the app area [0x4000, 0xA5000), then VM [0xA5000, 0xBD000), BTIF, OTP and USR. The loader writes
 * the app area only. A firmware stages a loader at the top of VM, with its update record at 0xBCF00 (4 KiB
 * boundary - 256), so records are looked for, and erased, in VM only. BTIF, OTP, USR and the head are never
 * written. Build: crt0_ldr.S and this file, -DOTA_LOADER_MARK='"PENDEL-LOADER-01"' for both. */
#define FELUCCA_USB_PID 0x000B                  /* pid.codes test range; the app is 000A */
#define FELUCCA_ID "ota-Pocket AMP_900"         /* hosts look for "ota-" and the model */
#define FELUCCA_USB_MANUFACTURER "Hugelton"
#define FELUCCA_USB_PRODUCT "Pendel Update"
#define LDR_APP_HI 0xA5000u
#define LDR_REC_LO 0xA5000u
#define LDR_REC_HI 0xBD000u
#define LDR_DATA_LO 0u                          /* no user data the host writes */
#define LDR_DATA_HI 0u
#define FL_RANGE_OK(off, n) (FL_IN(off, n, 0x4000u, 0xA5000u) || FL_IN(off, n, 0xA5000u, 0xBD000u))
#define LDR_LCD_POCKETAMP 1
#include "../../loader/loader.c"
