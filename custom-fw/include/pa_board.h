/* SPDX-License-Identifier: GPL-3.0-only */
/* Copyright (C) 2026 The Onyx Authors (@28spawn) */
/* pa_board.h - picks the board this build is for. Everything that differs between two units with the same chip (pins, ADC channels, the panel's orientation, the name the
 * unit reports) is in ONE header per board under include/boards/; the code includes this file (through pa_config.h) and never tests BOARD_* itself.
 *
 * The build passes exactly one of -DBOARD_POCKET_AMP=1 / -DBOARD_BLACKBOX=1 (build.ps1 -Board, make BOARD=). With neither, the Pocket AMP is built.
 *
 * NOT per board yet: the flash layout (pa_flash_map.h, pa_flash_guard.h, pa_ota_session.h, pa_ble_trims.h) is the Pocket AMP's, and those headers refuse any other board. */
#ifndef PA_BOARD_H
#define PA_BOARD_H

#if defined(BOARD_POCKET_AMP) && defined(BOARD_BLACKBOX)
#error "two boards are selected: define only one of BOARD_POCKET_AMP / BOARD_BLACKBOX"
#endif

#if defined(BOARD_BLACKBOX)
#include "boards/board_blackbox.h"
#else
#ifndef BOARD_POCKET_AMP
#define BOARD_POCKET_AMP       1
#endif
#include "boards/board_pocket_amp.h"
#endif

#endif /* PA_BOARD_H */
