/* SPDX-License-Identifier: MPL-2.0
 * Copyright (C) 2026 Leo Kuroshita (@kurogedelic), Hugelton Instruments (the loader this is made from: third_party/pendel_loader, pendel-bsp boards/pocketamp/pa_loader.c)
 * Changes for Onyx: the loader's own mark, its USB identity (hosts look for "ota-" and the model "Pocket AMP"), its USB names. Nothing else. */
/* The Onyx update loader: loader/loader.c of pendel-bsp with this unit's flash layout. It writes the app area [0x4000, 0xA5000) and nothing else: the boot area, the stock's settings log, BTIF,
 * USER, the factory bundle, the saved presets and the key sector are never written (FL_RANGE_OK), and it erases update records in the VM area only. A firmware stages it at 0xB8000 with its
 * record at 0xBCF00 (src/sys/pa_ota_session.c); the SPL runs it. Built by tools/build_onyx_loader.ps1 with -DOTA_LOADER_MARK='"ONYX-LOADER-0001"' (the 16 bytes pa_ota_session.c recognises). */
#define FELUCCA_USB_PID 0x000B                  /* pid.codes test range, as the loader this comes from */
#define FELUCCA_ID "ota-Pocket AMP_105"         /* the identity the firmware's update record names; hosts look for "ota-" and the model */
#define FELUCCA_USB_MANUFACTURER "Onyx"
#define FELUCCA_USB_PRODUCT "Onyx Update"
#define LDR_APP_HI 0xA5000u
#define LDR_REC_LO 0xA5000u
#define LDR_REC_HI 0xBD000u
#define LDR_DATA_LO 0u                          /* no user data the host writes */
#define LDR_DATA_HI 0u
#define FL_RANGE_OK(off, n) (FL_IN(off, n, 0x4000u, 0xA5000u) || FL_IN(off, n, 0xA5000u, 0xBD000u))
#define LDR_LCD_POCKETAMP 1
#include "../third_party/pendel_loader/loader/loader.c"
