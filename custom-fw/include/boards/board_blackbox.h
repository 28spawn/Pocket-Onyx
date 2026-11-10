/* SPDX-License-Identifier: GPL-3.0-only */
/* Copyright (C) 2026 The Onyx Authors (@28spawn) */
/* boards/board_blackbox.h - the M-VAVE ANN BlackBox. NOT PORTED YET: this file only makes a BlackBox build stop.
 *
 * Why it must stop and not "build something": the code below this header is still the Pocket AMP's. On a BlackBox that image would drive pins that do other things there
 * and would write flash sectors that hold the BlackBox's own data. A unit flashed with it could need a recovery.
 *
 * To port: give this file every macro board_pocket_amp.h defines (with the BlackBox's own values), then give the BlackBox its own flash layout - pa_flash_map.h,
 * pa_flash_guard.h, pa_ota_session.h and pa_ble_trims.h refuse this board until that is done - and its own package and update loader parameters. */
#ifndef PA_BOARD_BLACKBOX_H
#define PA_BOARD_BLACKBOX_H

#define PA_BOARD_ID            "blackbox"

#error "BOARD_BLACKBOX is not ported yet: the pins, the flash layout and the update package are still the Pocket AMP's. Do not build or flash this for a BlackBox."

#endif /* PA_BOARD_BLACKBOX_H */
