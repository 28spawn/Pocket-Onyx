/* SPDX-License-Identifier: GPL-3.0-only OR MPL-2.0 */
/* third_party/optimist_ble/src/pa_prof.h (PA-MOD) - the run-time experiment switches of the Pocket Amp firmware (custom-fw/include/pa_ble_diag.h, profile bits).
 * In the firmware (PA_BLE_GLUE, defined by src/board/ble_radio.c before it includes this stack) PA_PROF(bit) reads the live profile word; built alone (the stack's own
 * tests) it answers with the defaults. */
#ifndef PA_PROF_H
#define PA_PROF_H
#ifdef PA_BLE_GLUE
#include "pa_ble_diag.h"
extern struct pa_ble_diag pa_ble_dgb;
#define PA_PROF(b) ((pa_ble_dgb.profile & (b)) != 0u)
#else
#define PA_BLE_PROF_NO_ENC    1u
#define PA_BLE_PROF_CHM_LATE  2u
#define PA_BLE_PROF_CI30      4u
#define PA_BLE_PROF_WIDEN2    8u
#define PA_BLE_PROF_UPD_LATE  16u
#define PA_BLE_PROF_HW_REMAP  32u
#define PA_BLE_PROF_VENDOR_WIN 64u
#define PA_PROF(b) (((b) & (PA_BLE_PROF_CHM_LATE | PA_BLE_PROF_UPD_LATE)) != 0u)
#endif
#endif
