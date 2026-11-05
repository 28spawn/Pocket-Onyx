# Optimist BLE stack (third-party, verbatim)

**Upstream:** Optimist, <https://github.com/w0ts/optimist>, commit `fcdc4881e8788365c62ea8be7fa11b4d82966bac` (also in `UPSTREAM-COMMIT.txt`).
**Authors:** the Bluetooth code was written for Optimist by **hdavid**; Optimist is copyright (C) 2026 Leo Kuroshita (@kurogedelic), Hügelton Instruments, and the Optimist contributors.
**Licence:** each file carries its own `SPDX-License-Identifier`. The stack (`src/`, `hal/fm1_ble.h`, `hal/fm1_ble_rf.h`, `tests/`, `docs/`) is `GPL-3.0-only OR MPL-2.0`;
`hal/ble_rf_tables_v15.h` (the radio's start-up program captured from stock V15) is `Apache-2.0` (JieLi / M-VAVE). Licence texts: `LICENSES/`.

## Rules for this directory

* **Do not edit these files.** They are a pinned copy so that what runs can be compared with upstream line for line (`diff` against the commit above).
  Our integration lives outside: `custom-fw/src/board/ble_radio.c`, `custom-fw/src/board/ble_platform/` and the build option `PA_BLE` (default 0; `tools/check_ble_build.ps1` compiles it). If a change to a file here is ever unavoidable,
  mark it at the change with `/* PA-MOD: ... */` and list it below.
* Upstream layout: `src/` = `firmware/src/ble/`, `hal/` = `firmware/hal/`, `tests/` = the `tests/ble_*` files, `docs/` = `docs/BLE-*.md`.
* Documentation to read first: `docs/BLE-STACK.md` (the design, section 7 the driver) and `docs/BLE-HW-FACTS.md` (the radio's registers and rules).

## Local modifications

* `hal/fm1_ble_rf.h`: `FM1_VCO_EDGE_US` and `FM1_VCO_SETTLE_US` are wrapped in `#ifndef` (marked `PA-MOD`) so `ble_radio.c` can set them at run time. The default values are unchanged.
* `hal/fm1_ble_rf.h`: two optional hooks, `FM1_VCO_STOCK_SCAN()` (called at the end of `fm1_ble_vco_scan`) and `FM1_LUT_W1(entry, cmd, word)` (applied to the RF-die LUT's second word in `fm1_ble_rf_lut2`), both marked `PA-MOD`; without them defined nothing changes.
* `src/ble_ll.c` `ll_diag_enc`: also records CONNECTION_UPDATE_IND, CHANNEL_MAP_IND, TERMINATE_IND and CONNECTION_PARAM_REQ / RSP in the diagnostics' protocol ring (marked `PA-MOD`); diagnostics only, the stack's behaviour is unchanged.
* `src/ble_ll.c` `ll_rx_ctrl`, LL_CHANNEL_MAP_IND: an instant that has passed by fewer than 64 events no longer closes the link with 0x28; the map is applied two events from now (Windows sends it late). Marked `PA-MOD`.
* `src/ble_ll.c` `ll_rx_ctrl`, LL_CONNECTION_UPDATE_IND: the same tolerance (instant passed by fewer than 64 events: applied two events from now instead of closing with 0x28). Marked `PA-MOD`.
* `src/ble_hw_wl82.c` `ble_hw_addr`: calls the glue's `pa_ble_static_addr()` first, a fixed random static address derived from the unit's calibration records, so the address is the same on every boot; the random one stays as the fallback.
* `src/ble_hw_wl82.c` `cb_channels`: `PA_BLE_SOFT_REMAP` (include/pa_ble_remap.h) presents a full map with pre-remapped frequency tables; software remap by default (pack 146 measured 99.7% of events heard; the baseband's own remap heard 6.7% in pack 145), switchable at run time with the HW_REMAP profile bit (`src/pa_prof.h`, `include/pa_ble_diag.h`). `pa_ble_late[]` counts the late control PDUs applied. Pack 149A adds the per-event ring, per-channel tables and the interrupt histogram (`pa_ble_dgb`), and the run-time profile gates in `ble_ll.c` (late instants, LL_ENC_REQ), `ble_smp.c` (pairing request) and `ble_hw_wl82.c` (widening, remap). Also per-class event counters `pa_ble_cls[]` (diagnostics).
* `src/ble_hw_wl82.c` `hw_widening`: multiplied by `HW_WIDEN_X` (default 1, marked `PA-MOD`); `ble_radio.c` sets it to widen the connection receive window.
