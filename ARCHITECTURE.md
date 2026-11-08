# Onyx Custom Firmware — Architecture

Complete module map for the **Onyx** custom firmware, targeting the
**JieLi AC791N (pi32v2, 360 MHz assumed - see `PA_CPU_HZ`)** SoC.

Source of truth: `custom-fw/`. All paths below are relative to the repository root.

---

## Table of Contents

1. [System Overview](#1-system-overview)
2. [Layer & Module Inventory](#2-layer--module-inventory)
3. [Module Interaction Matrix](#3-module-interaction-matrix)
4. [Call Graphs](#4-call-graphs)
5. [Data Flow](#5-data-flow)
6. [Concurrency & Hand-Shake Model](#6-concurrency--hand-shake-model)
7. [Shared Data Structures & Global State](#7-shared-data-structures--global-state)
8. [Host / Emulator Architecture](#8-host--emulator-architecture)
9. [Build & Packaging](#9-build--packaging)
10. [Known Gaps & Discrepancies](#10-known-gaps--discrepancies)

---

## 1. System Overview

### 1.1 Architectural contract

`custom-fw/include/pa_plat.h` is the **hardware-abstraction seam** (roadmap item QW-1).
Portable code in `src/dsp`, `src/nam`, `src/fx`, `src/sys` **never touches MMIO directly**;
only `src/board/*` does.

```
                    ┌──────────────────────────────────────────────┐
                    │           src/board  (MMIO allowed)          │
   GPIO/SARADC ───► │  gpio_scan.c   st7735s.c   spi_nor.c        │
   ALNK0 I2S   ───► │  audio_alnk.c  usb_ble.c   entry.S           │
                    │  main.c  (integration + ISR body)            │
                    └───────────────┬──────────────────────────────┘
                                    │ plat_* / pa_* API
        ┌───────────────────────────┼───────────────────────────┐
        ▼                           ▼                           ▼
┌───────────────┐        ┌──────────────────┐        ┌──────────────────┐
│   src/fx      │        │    src/nam       │        │    src/dsp       │
│ 8-slot chain  │◄──────►│ GRU + WaveNet    │◄──────►│ LUT / accel /    │
│ fx_graph.c    │        │                  │        │ tuner / drums    │
│ + 7 effects   │        │                  │        │                  │
└───────┬───────┘        └──────────────────┘        └──────────────────┘
        │ pa_fx_chain_t
        ▼
┌───────────────────────────────────────────────────────────────────────┐
│                          src/sys                                     │
│ preset.c  settings.c  pa_user_nam.c  sysex.c  pa_pool.c              │
│ cpu_meter.c  smp_cpu1.c  stock_assets.c                              │
└───────────────────────────────────────────────────────────────────────┘
        ▲
        │ pa_ui_ctx_t (state machine) + 15 views
┌───────┴───────────────────────────────────────────────────────────────┐
│              src/board/ui_app.c  +  ui_5x7.c (font)                   │
└───────────────────────────────────────────────────────────────────────┘
```

### 1.2 Global constants (`include/pa_config.h`)

| Constant | Value | Notes |
|---|---|---|
| `PA_SAMPLE_RATE` | 44 100 Hz | Codec actually runs at 44 118 Hz |
| `PA_BLOCK_N` | 64 | samples/channel per DAC block (~1.45 ms) |
| `PA_CPU_HZ` | 360 000 000 | 360 MHz measured and verified against stock firmware clock calculation code (Timer4 x15) |
| `PA_Q21_ONE` | 2 097 152 | Q21 fixed-point scale (`pa_model.h`) |
| `PA_NAM_HIDDEN` | 16 | GRU hidden units |
| `PA_IR_MAX_TAPS` | 2048 | Cabinet IR partition count = 32 |
| `PA_POOL_BUDGET_BYTES` | 0x54000 (336 KiB) | `.pool` section at `0x01C20000` |
| `PA_NAM_LUT_ENTRIES` | 512 | Activation LUT knots |

### 1.3 Execution contexts

| Context | Entry | Rate | Members |
|---|---|---|---|
| **Audio ISR** | `isr_alnk0` (`entry.S`) → `pa_alnk0_isr_c` → `pa_audio_process_half` → `pa_audio_process_block_stereo` | 44 118 Hz (64-frame DMA) | ALNK0, Core 0 chain stages, drums, USB mix |
| **UI thread** | `pa_main_loop` → `pa_firmware_tick` | 60 Hz (16 ms) | UI, presets, settings, flash, SysEx, GC |
| **Poll loop** | `pa_main_loop` | 1 kHz | `pa_gpio_scan_poll_1khz()` (buttons, encoder debouncing) |
| **CPU1** | `pa_c1_start` / `cpu1.c` (`smp_cpu1.c`) | Per block (~1.45 ms) | Amp / WaveNet neural inference offload (~1,370 µs per 64-sample block) + lock-free handoff |

---

## 2. Layer & Module Inventory

### 2.1 `include/` — public contracts (50 headers)

| Header | Owns |
|---|---|
| `pa_config.h` | Build-time knobs: sample rate, block size, Q format, feature toggles |
| `pa_plat.h` | Hardware-abstraction seam: time, pool, accel, audio, controls, LCD, flash, SysEx |
| `pa_audio_core.h` | Portable audio engine "jack", block scheduling, smooth preset/model swap |
| `pa_input.h` | Debounced button scanner, release clicks, chords, 1s/2s/3s hold gestures |
| `pa_fx.h` | `fx_node_t` vtable, `pa_fx_chain_t`, `pa_fx_slot_id_t` (8 slots), speaker/limiter modes |
| `pa_model.h` | `pa_nam_model_t` (3 852-byte NAM A2 GRU), `q28_t`, `PA_Q21_ONE` |
| `pa_wavenet.h` | `pa_wavenet_model_t`, voicing profile, 23-layer dilated structure |
| `pa_wavenet_fast.h` | Fast/ASM WaveNet execution definitions for real-time Core 1 processing |
| `pa_biquad.h` | Assembly-accelerated biquad kernel structures (DF2T) |
| `pa_ui.h` | `pa_ui_ctx_t`, `pa_ui_view_t` (15 views), button bitmasks, Onyx palette |
| `pa_preset.h` | `pa_preset_t` (164 B), Q16↔Q21 helpers, genre tags |
| `pa_settings.h` | `pa_settings_t` (60 B) |
| `pa_user_nam.h` | `pa_user_nam_slot_t`, `pa_user_nam_meta_t` (custom NAM capture storage) |
| `pa_user_cab.h` | User cabinet IR slot management and flash allocation |
| `pa_stock_assets.h` | `pa_stock_amp_info_t`, 20 amps + 20 cabs accessors |
| `pa_cab.h` | Legacy IR prototypes (superseded by `fx_cab_ir.c` / `pa_user_cab.c`) |
| `pa_activation.h` | `pa_activation_lut`, `pa_lut_sigmoid/tanh` |
| `pa_hw_accel.h` | `pa_hw_vec_cmd_t`, MATVEC/FIR command words |
| `pa_hw.h` | MMIO addresses + read/write macros |
| `pa_flash_map.h` | The 1 MB NOR layout, the two rewritable windows and `pa_flash_rw_ok()` |
| `pa_flash_guard.h` | Flash write protection boundaries |
| `pa_spi_nor.h` | `pa_spi_nor_init/read/write/erase_sector/sync` |
| `pa_audio.h` | `pa_audio_state_t` (global `g_pa_audio`), block callback typedef |
| `pa_usb_ble.h` | `pa_usb_ble_ctx_t`, EP constants, MIDI/UAC API |
| `pa_usb_dev.h` | USB device driver & hardware endpoint FIFO handling |
| `pa_ble_radio.h` | BLE baseband / RF radio registers and timing |
| `pa_ble_midi_bridge.h` | BLE MIDI service, packet framing, and footswitch bridging |
| `pa_sysex.h` | `pa_sysex_parser_t`, `pa_upload_session_t` |
| `pa_protocol.h` | Verified stock V105 & Onyx protocol map: PC 0..79, CC 20..25, CC 64..67, SysEx opcodes |
| `pa_drums.h` | `pa_drums_t`, 49 groove styles, 12 lanes + a click voice, 48 steps to a 4/4 bar, `pa_drums_bar_block()` |
| `pa_cpu_meter.h` | `pa_cpu_meter_t`, `enum pa_stage` (8 stages) |
| `pa_smp.h` | `pa_smp_mailbox_t`, CPU1 register macros and inter-core synchronization |
| `pa_debug_log.h` | Debug logging macros |
| `pa_ota_session.h` | OTA firmware upgrade session management |
| `onyx_block_icons.h` | 281-line palette-indexed 28×28 block icon table |

### 2.2 `src/dsp/` — fixed-point primitives (5 files)

| Module | Key functions | Calls out to | Data |
|---|---|---|---|
| `dsp/activation.c` | `pa_activation_init`, `pa_activation_lut(x_q21, pair)`, `pa_lut_sigmoid`, `pa_lut_tanh`, `pa_lut_fast_tanh` | `sat32` (fixedpoint.h) | 2× 513-entry Q21 LUTs (sigmoid, tanh), static BSS |
| `dsp/hw_accel.c` | `pa_hw_accel_init`, `pa_hw_accel_submit`, `pa_hw_accel_wait`, `pa_hw_accel_fir_part64` | — | `g_hw_cmd_block` (24 B) at `0x01EE0400`; host build emulates bit-accurately |
| `dsp/tuner.c` | `pa_tuner_init`, `pa_tuner_process_block` | `q21_mul` | State in caller's `pa_tuner_state_t`; zero-crossing + EMA |
| `dsp/drums_synth.c` | `pa_drums_init`, `pa_drums_set_bpm/style/level/playing`, `pa_drums_mix_block`, `pa_drums_get_style_name`, `pa_drums_style_length` | `q21_mul`, `sat32` | 12 synthesised voices in `drum_kit.inc` (procedural synthesis, no product recordings), 49 rhythm patterns in `drum_patterns.inc` (generated by `tools/gen_drum_patterns.py`) |
| `dsp/fixedpoint.h` | `q21_mul`, `sat32`, `q21_from_float`, `mac_block`, `pa_prng_tpdf` | — | Header-only |
| `dsp/pa_oversample.h` | `pa_oversample_init`, `pa_oversample_up`, `pa_oversample_down` | — | Header-only, 2× polyphase half-band |

### 2.3 `src/nam/` — neural amp modelling (2 files)

| Module | Key functions | Calls out to | Data |
|---|---|---|---|
| `nam/nam_engine.c` | `nam_model_init`, `pa_nam_init_default`, `pa_nam_reset`, `nam_gru_run_block`, `nam_step_q21`, `pa_nam_process_block` | `pa_hw_accel_submit/wait`, `pa_activation_init`, `pa_lut_sigmoid/tanh` | `pa_nam_model_t` (3 852 B) — 16-unit GRU, 3 co-scheduled MATVEC stages |
| `nam/pa_wavenet.c` | `pa_wavenet_init_default`, `pa_wavenet_load_weights`, `pa_wavenet_set_voicing`, `pa_wavenet_set_input_trim`, `pa_wavenet_process_block_q21`, `pa_wavenet_process_block_float`, `pa_wavenet_step_sample`, `pa_wavenet_reset` | `plat_ticks24` (main.c) | `pa_wavenet_model_t` (~250 KB) — 23 dilated layers, 12 146 float weights, 50 832-sample ring |

### 2.4 `src/fx/` — the 8-slot effect chain (8 files)

Every effect implements the `fx_node_t` vtable: `run`, `run_stereo`, `create`, `destroy`,
`set_param`, `reset`. All `fx_*_new()` return a **file-static singleton node descriptor**;
only `node->state` is pool-allocated.

| Module | Slot | Key functions | Type / param enums | Pool bytes |
|---|---|---|---|---|
| `fx/fx_graph.c` | *manager* | `pa_fx_chain_init`, `pa_fx_chain_run_stereo`, `pa_fx_chain_run`, `pa_fx_chain_set_slot_enabled`, `pa_fx_chain_set_slot_enabled_now`, `pa_fx_chain_set_slot_type`, `pa_fx_chain_set_param`, `pa_fx_chain_set_global_input`, `pa_fx_chain_set_output_routing`, `pa_fx_chain_set_fizz_filter`, `pa_fx_chain_set_preset_vol`, `pa_fx_chain_set_input_gain`, `pa_fx_chain_set_speaker_mode`, `pa_fx_chain_set_output_limit_mode`, `pa_fx_chain_swap_order`, `pa_fx_chain_relink_ordered`, `pa_fx_chain_reset_states`, `pa_fx_chain_collect_garbage`, `pa_fx_slot_type_count`, `fx_graph_run`*, `fx_set_enabled`* | `pa_fx_slot_id_t` (8) | 0 (manages) |
| `fx/fx_dynamics.c` | 0 DYN | `fx_dynamics_new` | `DYN_TYPE_*` (4), `DYN_PARAM_*` (6) | ~80 B |
| `fx/fx_drive.c` | 1 DRIVE | `fx_drive_new` | `DRIVE_MW_*` (6), `DRV_PARAM_*` (4) | ~56 B |
| *(main.c)* | 2 AMP | `nam_slot_init` (static in `src/board/main.c`) | 6 knobs: GAIN/BASS/MID/TREBLE/PRES/MASTER | GRU 3 852 B / WaveNet ~250 KB |
| `fx/fx_cab_ir.c` | 3 CAB | `fx_cab_ir_new`, `fx_cab_ir_load_q18`, `fx_cab_ir_set_stock_trim`, `fx_cab_ir_set_trim_db_x10` | `CAB_PARAM_LEVEL/LOW_CUT/HIGH_CUT` | ~16.7 KB (2048 taps + 2112 hist) |
| `fx/fx_eq.c` | 4 EQ | `fx_eq_new` | `EQ_PARAM_BASS/MIDDLE/TREBLE/PRESENCE/MIDCUT` | ~240 B |
| `fx/fx_mod.c` | 5 MOD | `fx_mod_new` | `MOD_TYPE_*` (7), `MOD_PARAM_*` (5) | 4 096 B + ~120 B |
| `fx/fx_delay.c` | 6 DELAY | `fx_delay_new` | `DELAY_TYPE_*` (5), `DLY_PARAM_*` (8) | 65 536 B (32,768 int16) + ~88 B |
| `fx/fx_reverb.c` | 7 REV | `fx_reverb_new` | `REV_TYPE_*` (6), `REV_PARAM_*` (8) | 49 152 B (24,576 int16 tank) + ~232 B |

\* `fx_graph_run` / `fx_set_enabled` are legacy API — used only by `test/test_reclaim.c`.

**Chain-manager internals** (`fx_graph.c`): `fx_link`, `fx_unlink`, `node_xfade_post`,
`run_global_gate`, `apply_fizz_filter_block`, `apply_output_limiter_stereo`.
Static tables: `k_fizz_coef[3][2][5]`, `k_db_q21_m12_p12[25]`.
Shared scratch: `s_dry_L[64]`, `s_dry_R[64]` (one node fades at a time).

### 2.5 `src/sys/` — system services (14 files)

| Module | Key functions | Calls out to | Data |
|---|---|---|---|
| `sys/pa_audio_core.c` | `pa_audio_core_init`, `pa_audio_core_bind`, `pa_audio_core_process_half`, `pa_audio_core_request_preset`, `pa_audio_core_request_model`, `pa_audio_core_tick` | `pa_fx_chain_run_stereo`, `pa_drums_mix_block`, `pa_tuner_process_block`, `pa_smp_wait_idle` | Unified audio jack & block scheduler, smooth gain crossfader |
| `sys/pa_input.c` | `pa_input_init`, `pa_input_set_raw`, `pa_input_update` | `pa_input.h` | 1 kHz contact debounce, release-click tracking, 1s/2s/3s hold gestures, chord decoding |
| `sys/preset.c` | `pa_preset_init`, `pa_preset_load`, `pa_preset_save`, `pa_preset_apply_to_chain`, `pa_preset_apply_smooth`, `pa_preset_capture_from_chain`, `pa_preset_factory_reset`, `pa_preset_is_empty`, `pa_preset_init_defaults`, `pa_crc32` | `pa_spi_nor_*`, `pa_stock_load_amp/cab`, `pa_user_nam_load_into_dsp`, `pa_audio_core_request_preset` | `g_presets[80]` (13 120 B), bank A/B ping-pong |
| `sys/settings.c` | `pa_settings_defaults`, `pa_settings_load`, `pa_settings_save` | `pa_spi_nor_*`, `pa_crc32` (from preset.c) | `s_seq`, `s_active` — A/B sector ping-pong |
| `sys/pa_user_nam.c` | `pa_user_nam_init`, `pa_user_nam_add`, `pa_user_nam_load_into_dsp`, `pa_user_nam_get_count`, `pa_user_nam_get_name`, `pa_user_nam_get_info`, `pa_user_nam_get_amp_defaults` | `pa_spi_nor_*`, `pa_crc32`, `pa_wavenet_load_weights/set_voicing/set_input_trim`, `pa_nam_reset`, `pa_fx_chain_set_slot_enabled` | `g_nam_registry` (20 slots), `s_model_blob_buf[64 KB]` |
| `sys/pa_user_cab.c` | `pa_user_cab_init`, `pa_user_cab_load`, `pa_user_cab_write` | `pa_spi_nor_*`, `pa_crc32` | User cabinet IR slot management and flash allocation |
| `sys/pa_pool.c` | `pa_alloc`, `pa_free`, `pa_pool_used_bytes` | — | `g_dsp_pool[147 456]` in `.pool`; first-fit + coalescing |
| `sys/sysex.c` | `pa_sysex_init`, `pa_sysex_input_byte`, `pa_sysex_build_frame`, `pa_upload_session_init`, `pa_upload_get_session` | `pa_preset_save`, `pa_crc32`, `pa_spi_nor_*` | `g_sx` parser, `g_upload` (32 KB buffer) |
| `sys/cpu_meter.c` | `cpu_meter_init`, `cpu_meter_set_clock`, `cpu_meter_begin`, `cpu_meter_end`, `cpu_meter_block_done`, `cpu_meter_total_permille`, `cpu_meter_stage_permille`, `cpu_meter_headroom_permille` | — | `pa_cpu_meter_t` — cycle telemetry |
| `sys/pa_ota.c` | `pa_ota_classify`, `pa_ota_build_id_reply`, `pa_ota_device_id`, `pa_ota_version_string` | — | JieLi OTA handshake & device ID verification |
| `sys/pa_ota_session.c` | `pa_ota_session_init`, `pa_ota_session_feed`, `pa_ota_session_finish` | `pa_spi_nor_*`, `ota_hw_reboot` | Multi-packet OTA firmware streaming & verification |
| `sys/pa_bond.c` | `pa_bond_init`, `pa_bond_add`, `pa_bond_find`, `pa_bond_save` | `pa_spi_nor_*` | Bluetooth peer bonding records |
| `sys/pa_ble_trims.c` | `pa_ble_trims_load`, `pa_ble_trims_apply` | `pa_spi_nor_read` | Factory RF calibration trims |
| `sys/pa_diag.c` | `pa_diag_feed`, `pa_diag_set_reply_sink`, `pa_diag_set_flash_reader` | `pa_spi_nor_read` | SysEx flash diagnostic reader |
| `sys/smp_cpu1.c` | `pa_smp_init`, `pa_smp_poll`, `pa_smp_wait_idle`, `pa_smp_cpu1_load_permille` | `pa_c1_start/submit/wait`, `pa_fx_chain_set_post_handoff` | Core 1 workload scheduling, real-time swap probe, stack watchdog |
| `sys/stock_assets.c` | `pa_stock_assets_init`, `pa_stock_load_amp`, `pa_stock_load_cab`, `pa_stock_get_amp_name`, `pa_stock_get_cab_name` | — | 633 KB generated: 20 NAM models + 20 IRs |

### 2.6 `src/board/` — hardware, drivers, integration (18 files)

| Module | Key functions | Calls out to | MMIO |
|---|---|---|---|
| `board/entry.S` | `isr_alnk0`, reset vector | `pa_firmware_init`, `pa_main_loop` | IRQ vectors at `0x01C7FE2C` |
| `board/main.c` | `pa_firmware_init`, `pa_firmware_tick`, `pa_main_loop`, `pa_amp_set_model_profile`, `plat_ms/ticks24/cycles/delay_us/delay_ms` | audio core, UI, drivers | TIMER4 `0x00010800` |
| `board/cpu1.c` | `pa_c1_start`, `pa_c1_submit`, `pa_c1_wait`, `pa_c1_halt_if_running`, `pa_c1_stack_used` | `smp_cpu1.c` | CPU1 control regs, mailbox IRQs |
| `board/audio_alnk.c` | `pa_audio_alnk_init`, `pa_audio_init`, `pa_audio_process_half`, `pa_audio_stop`, `pa_alnk0_isr_c` | `pa_audio_core_process_half` | ALNK0 `0x00012E00`, GPIO PA4..PA8 |
| `board/gpio_scan.c` | `pa_gpio_scan_init`, `pa_gpio_scan_poll_1khz`, `plat_buttons`, `plat_enc_delta`, `plat_pot_q12`, `plat_battery_poll/mv/bars/is_brownout`, `plat_backlight_set`, `plat_wdt_feed` | `plat_ms` (main.c), `pa_input_set_raw` | PH6/PH8/PH9/PA0/PA1/PA3, PC0/PC1 encoder |
| `board/ui_app.c` | `pa_ui_init`, `pa_ui_handle_inputs`, `pa_ui_render`, `pa_ui_load_preset`, `pa_ui_sync_from_preset`, `pa_ui_apply_io_settings`, `pa_ui_mark_settings_dirty`, `pa_ui_set_theme` | `pa_audio_core_*`, `pa_preset_*`, `pa_fx_chain_*`, `st7735s_*`, `ui_draw_*` | Display list engine (~1,370 opcodes), dirty-band refresh |
| `board/st7735s.c` | `st7735s_init`, `st7735s_fill_rect`, `st7735s_blit_line` | — | SPI1 `0x00011D00`, PB1/PB5/PB6/PB7, PA9 backlight |
| `board/ui_5x7.c` | `ui_draw_char`, `ui_draw_str`, `ui_draw_char_scaled`, `ui_draw_str_scaled` | `st7735s_fill_rect` | — |
| `board/usb_ble.c` | `pa_usb_ble_init`, `pa_usb_ble_poll`, `pa_usb_midi_rx_packet`, `pa_usb_midi_tx_bytes`, `pa_ble_gatt_rx_write`, `pa_usb_audio_push_capture_stereo`, `pa_usb_audio_mix_playback_stereo` | `pa_sysex_input_byte`, `ble_midi_bridge` | USB `0x00016800`, EP3, EP4 |
| `board/usb_dev.c` | `pa_usb_dev_init`, `pa_usb_dev_poll`, `pa_usb_ep_read`, `pa_usb_ep_write` | — | AC791N USB hardware controller & FIFO |
| `board/ble_radio.c` | `ble_radio_init`, `ble_radio_set_adv_data`, `ble_radio_send_packet` | — | 2.4 GHz RF transceiver, baseband registers |
| `board/ble_midi.c` | `ble_midi_init`, `ble_midi_rx_byte`, `ble_midi_tx_packet` | `ble_radio.c` | Standard BLE MIDI GATT service |
| `board/ble_midi_bridge.c` | `ble_midi_bridge_poll`, `ble_midi_bridge_handle_cc` | `pa_audio_core_*` | Wireless footswitch / pedal bridging (CC 20..25, CC 64..67, PC 0..79) |
| `board/spi_nor.c` | `pa_spi_nor_init`, `pa_spi_nor_read`, `pa_spi_nor_write`, `pa_spi_nor_erase_sector`, `pa_spi_nor_sync` | `plat_battery_is_brownout` | Winbond 25Q32 SPI NOR Flash |
| `board/flash_probe.c` | `flash_probe_jedec_id`, `flash_probe_quad_mode` | `spi_nor.c` | Flash device identification & speed config |
| `board/flash_ram.c` | `flash_ram_copy_text`, `flash_ram_exec` | — | RAM-relocated flash programming stubs |
| `board/ota_hw.c` | `ota_hw_reboot_to_updater`, `ota_hw_verify_partition` | — | Reboot vectors & bootloader handoff |

---

## 3. Module Interaction Matrix

Legend: **●** = calls directly · **○** = reads/writes shared state · **—** = none

| From ↓ / To → | dsp | nam | fx | sys | board/ui_app | board/main | board/drivers |
|---|---|---|---|---|---|---|---|
| **dsp/** | — | ● | ○ | — | — | ● | ○ |
| **nam/** | ● | — | — | — | — | ● | ○ |
| **fx/** | ● | — | ● | ○ | — | ● | ○ |
| **sys/** | ○ | ● | ● | ● | ● | ● | ● |
| **board/ui_app.c** | ○ | ○ | ● | ● | — | ● | ● |
| **board/main.c** | ● | ● | ● | ● | ● | — | ● |
| **board/drivers** | — | — | — | ○ | ● | ● | ● |

### 3.1 Direct cross-module calls (the real coupling surface)

| Caller | Callee | Where |
|---|---|---|
| `main.c` | `pa_fx_chain_run_stereo`, `pa_fx_chain_init`, `pa_fx_chain_collect_garbage` | audio ISR / UI tick |
| `main.c` | `nam_gru_run_block`, `pa_wavenet_process_block_q21` | `nam_slot_run` (AMP node) |
| `main.c` | `pa_tuner_process_block`, `pa_drums_mix_block` | audio ISR |
| `main.c` | `pa_usb_audio_mix_playback_stereo`, `pa_usb_audio_push_capture_stereo` | audio ISR |
| `main.c` | `pa_preset_init/load/apply_to_chain`, `pa_settings_load/save`, `pa_user_nam_init` | `boot_restore` / UI tick |
| `main.c` | `pa_stock_assets_init`, `pa_hw_accel_init`, `cpu_meter_init`, `pa_smp_init` | `pa_firmware_init` |
| `audio_alnk.c` | `pa_audio_process_block_stereo` | `pa_audio_process_half` |
| `ui_app.c` | `pa_preset_load/apply_smooth/capture_from_chain/save/is_empty/factory_reset` | load/save flows |
| `ui_app.c` | `pa_fx_chain_set_slot_enabled/set_param/set_slot_type/set_preset_vol/relink_ordered/set_global_input/set_output_routing/set_fizz_filter` | every edit |
| `ui_app.c` | `fx_cab_ir_load_q18`, `fx_cab_ir_set_stock_trim` | CAB model change |
| `ui_app.c` | `pa_stock_load_amp/load_cab/get_amp_name/get_cab_name` | model pickers |
| `ui_app.c` | `pa_user_nam_get_count/get_name/load_into_dsp/get_amp_defaults` | NAM captures |
| `ui_app.c` | `pa_amp_set_model_profile`, `pa_audio_request_mute/is_muted/release_mute` | model change (click-free) |
| `ui_app.c` | `st7735s_fill_rect`, `ui_draw_char/str/str_scaled` | rendering |
| `preset.c` | `pa_spi_nor_*`, `pa_stock_load_amp/cab`, `pa_user_nam_load_into_dsp` | storage + assets |
| `preset.c` | `pa_fx_chain_*` (7 fns), `fx_cab_ir_load_q18/set_stock_trim` | `pa_preset_apply_to_chain` |
| `preset.c` | `pa_amp_set_model_profile`, `pa_audio_*_mute` | apply / smooth |
| `pa_user_nam.c` | `pa_wavenet_load_weights/set_voicing/set_input_trim`, `pa_nam_reset` | blob → engine |
| `pa_user_nam.c` | `pa_fx_chain_set_slot_enabled` | enable AMP, bypass CAB |
| `settings.c` | `pa_crc32` (defined in `preset.c`) | CRC |
| `sysex.c` | `pa_preset_save`, `pa_crc32`, `pa_spi_nor_*` | upload commit |
| `usb_ble.c` | `pa_sysex_input_byte` | MIDI ring drain |
| `nam_engine.c` | `pa_hw_accel_submit/wait/init`, `pa_lut_sigmoid/tanh` | accelerator + LUTs |
| `fx_cab_ir.c` | `pa_hw_accel_fir_part64` | partitioned FIR |
| `fx_graph.c` | `fx_dynamics_new`, `fx_drive_new`, `fx_eq_new`, `fx_mod_new`, `fx_delay_new`, `fx_reverb_new` | node construction |
| `smp_cpu1.c` | `nd->run()` on `chain->slots[3..7]` | CPU1 offload |
| `pa_wavenet.c` | `plat_ticks24` (defined in `main.c`) | cycle telemetry |

### 3.2 Shared-state coupling (no call, but coupled)

| Shared object | Owner | Readers | Writers |
|---|---|---|---|
| `g_fx_chain` | `main.c` | `preset.c`, `pa_user_nam.c`, `smp_cpu1.c`, `ui_app.c` | `main.c`, `ui_app.c`, `preset.c` |
| `g_ui` | `main.c` | `main.c` (ISR reads `g_ui.tuner`), `emu_bridge.c` | `ui_app.c`, `main.c` |
| `g_active_amp_engine` | `main.c` | `main.c`, `preset.c`, `pa_user_nam.c` | `main.c`, `pa_user_nam.c` |
| `g_nam_model`, `g_wn_model` | `main.c` | `main.c`, `preset.c`, `pa_user_nam.c` | `main.c`, `pa_user_nam.c` |
| `g_pa_audio` | `audio_alnk.c` | `main.c`, `ui_app.c` | `audio_alnk.c` |
| `g_onyx_greys[3]` | `ui_app.c` | `pa_ui.h` macros | `ui_app.c` |
| `pa_alloc` / `pa_free` | `pa_pool.c` | all 7 fx modules | all 7 fx modules |

---

## 4. Call Graphs

### 4.1 Boot sequence

```
entry.S  (reset vector @ 0x02000120)
  ├─ copy .data / .ram_text from XIP to RAM
  ├─ clear .bss, set up stacks (_ustack_top 0x01C7A000, _sstack_top 0x01C7C000)
  ├─ pa_firmware_init()                    [main.c]
  │    ├─ timer4_init_24mhz()              (TIMER4 @ 24 MHz, free-running)
  │    ├─ pa_gpio_scan_init()              [gpio_scan.c]
  │    ├─ st7735s_init()                   [st7735s.c — 22-entry ST7789 table]
  │    ├─ pa_stock_assets_init()            [stock_assets.c]
  │    ├─ pa_hw_accel_init()               [hw_accel.c]
  │    ├─ cpu_meter_init(&g_cpu_meter, 360 MHz, 64, 44100)
  │    ├─ nam_slot_init(&g_nam_model)      [main.c]
  │    │    ├─ nam_model_init()            [nam_engine.c]
  │    │    │    ├─ pa_hw_accel_init()
  │    │    │    └─ pa_activation_init()   [activation.c]
  │    │    └─ pa_wavenet_init_default()   [pa_wavenet.c]
  │    ├─ g_cab_node = fx_cab_ir_new()     [fx_cab_ir.c]
  │    ├─ pa_fx_chain_init(&g_fx_chain, amp, cab)   [fx_graph.c]
  │    │    └─ fx_dynamics/drive/eq/mod/delay/reverb_new()
  │    ├─ pa_ui_init(&g_ui)                [ui_app.c]
  │    ├─ pa_drums_init(&g_drums)          [drums_synth.c]
  │    ├─ pa_usb_ble_init(&g_usb_ble)      [usb_ble.c]
  │    ├─ pa_smp_init(&g_fx_chain)         [smp_cpu1.c]
  │    ├─ pa_audio_alnk_init()             [audio_alnk.c — STARTS THE ISR]
  │    └─ boot_restore()                   [main.c]
  │         ├─ pa_preset_init()            → A/B bank select + v1 migration
  │         ├─ pa_user_nam_init()          → registry + v2→v3 migration
  │         ├─ pa_settings_load()          → A/B settings sector
  │         ├─ settings_to_ui()
  │         ├─ pa_ui_set_theme()
  │         ├─ plat_backlight_set()
  │         ├─ pa_ui_apply_io_settings()
  │         └─ pa_preset_load(idx) → pa_preset_apply_to_chain()
  └─ pa_main_loop()                        [main.c — UI thread]
```

### 4.2 Audio ISR path (44 118 Hz, 64 frames)

```
isr_alnk0  (entry.S: save psr/rets/reti, r3-r0)
  └─ pa_alnk0_isr_c()                     [audio_alnk.c]
       └─ pa_audio_process_half(rx_half, tx_half)
            ├─ unpack 64 stereo frames; guitar = rx_half[2*i] (Left)
            ├─ sign-extend 24→32; apply input_gain_q15; track in_peak
            ├─ Q23 → Q28  (<<5)
            ├─ g_pa_audio.bypass ? memcpy : g_audio_cb(...)
            │    └─ pa_audio_process_block_stereo(buf_L, buf_R, n)   [main.c]
            │         1. pa_tuner_process_block(&g_ui.tuner, buf_L, n)
            │            └─ if mute_output: zero, pa_drums_mix_block, L→R, RETURN
            │         2. cpu_meter_begin
            │            └─ pa_fx_chain_run_stereo(&g_fx_chain, L, R, n)  [fx_graph.c]
            │                 ├─ DC blocker (15 Hz, in_dc_x1/y1)
            │                 ├─ input_gain_q21 × global_in_gain_q21
            │                 ├─ run_global_gate()   [if gate_thresh_q21]
            │                 ├─ L → R (mono→stereo)
            │                 ├─ for idx in slot_order[0..7]:
            │                 │     └─ nd->run_stereo(nd, L, R, n)  or  nd->run(nd, L, n)
            │                 │        (mono-after-stereo downmix if needed)
            │                 │     └─ [AMP slot] apply_fizz_filter_block()  [if enabled]
            │                 │     └─ [CAB slot] skip if force_cab_off
            │                 │     └─ node_xfade_post()  → wet/dry crossfade
            │                 ├─ preset_vol_cur_q21 one-pole smoothing
            │                 ├─ speaker-mode "air" one-pole  [if HEADPHONE_AIR]
            │                 └─ apply_output_limiter_stereo()  [OFF/SOFT/BRICKWALL]
            │            └─ xf_apply()   [if crossfade in progress]
            │         3. cpu_meter_begin
            │            ├─ pa_drums_mix_block(&g_drums, drum_tmp, n)
            │            ├─ pa_usb_audio_mix_playback_stereo(&g_usb_ble, L, R, n)
            │            ├─ pa_usb_audio_push_capture_stereo(&g_usb_ble, L, R, n)
            │            └─ pa_soft_ceiling_q21()  (knee 0.891, 4:1 above)
            │         4. cpu_meter_block_done()
            ├─ master volume linear slew (Q28 × Q15, int64)
            ├─ TPDF dither (pa_prng_tpdf) at the physical DAC stage
            └─ pack 24-bit into tx_half, track out_peak
```

**Default chain order** (`fx_graph.c`, `pa_fx_chain_init`):
`DYN(0) → DRIVE(1) → AMP(2) → CAB(3) → EQ(4) → MOD(5) → DELAY(6) → REVERB(7)`

### 4.3 UI tick (60 Hz)

```
pa_firmware_tick(buttons, enc_delta, adc_pots)              [main.c]
  ├─ plat_wdt_feed()
  ├─ plat_battery_poll() → battery_mv → pa_battery_pct_from_mv() → warn levels
  ├─ toast / volume-popup timers count down
  ├─ pa_usb_ble_poll(&g_usb_ble)                            [usb_ble.c]
  │    └─ drain MIDI ring → pa_sysex_input_byte(b)          [sysex.c]
  │         └─ pa_sysex_dispatch() → commit_upload_session() → pa_preset_save / flash
  ├─ pa_ui_handle_inputs(&g_ui, &g_fx_chain, ...)            [ui_app.c]
  │    ├─ master pot → pa_ui_apply_io_settings()
  │    ├─ auto-dim wake check
  │    ├─ global combos (TUNER / BANK / DRUMS)
  │    ├─ per-view input cascade (15 views)
  │    │    └─ ui_select_model()  [mute handshake → asset load → release_mute]
  │    └─ pa_ui_load_preset()  [preset load + apply_smooth]
  ├─ pa_fx_chain_collect_garbage(&g_fx_chain)               [frees ISR-flagged nodes]
  ├─ pa_drums_set_bpm/style/level/click/playing()
  ├─ auto-dim after {0, 30 s, 60 s, 300 s}
  ├─ pa_settings_save()  [2 s debounce, brownout-guarded]   [settings.c]
  ├─ g_ui.sram_used_bytes = pa_pool_used_bytes()             [pa_pool.c]
  └─ pa_ui_render(&g_ui, &g_fx_chain, &g_cpu_meter)          [ui_app.c]
       └─ st7735s_fill_rect() / ui_draw_str()  [st7735s.c, ui_5x7.c]
```

### 4.4 Preset load (click-free)

```
pa_ui_load_preset(ui, chain, idx)                [ui_app.c]
  ├─ pa_preset_load(idx, &pr)                    [preset.c] — CRC32 + magic check
  ├─ pa_preset_apply_smooth(&pr, chain)          [preset.c]
  │    ├─ pa_audio_request_mute()                 → g_xf_state = FADE_OUT
  │    ├─ spin ≤ 30 ms on pa_audio_is_muted()     (plat_ms + plat_wdt_feed)
  │    │     [ISR: xf_apply() ramps gain→0 over 4 blocks, sets MUTED]
  │    ├─ pa_preset_apply_to_chain(&pr, chain)
  │    │    ├─ pa_fx_chain_reset_states()
  │    │    ├─ per slot: pa_fx_chain_set_slot_enabled_now()   (immediate, muted)
  │    │    │           pa_fx_chain_set_slot_type()          (except AMP/CAB/EQ)
  │    │    │           pa_fx_chain_set_param()             (Q16 → Q21)
  │    │    ├─ pa_fx_chain_set_preset_vol()
  │    │    ├─ pa_fx_chain_set_input_gain()
  │    │    ├─ pa_fx_chain_relink_ordered()
  │    │    ├─ AMP: pa_stock_load_amp() | pa_user_nam_load_into_dsp()
  │    │    │      pa_amp_set_model_profile(); re-push 6 AMP knobs
  │    │    └─ CAB: pa_stock_load_cab() → fx_cab_ir_load_q18() + fx_cab_ir_set_stock_trim()
  │    └─ pa_audio_release_mute()                 → g_xf_state = FADE_IN
  ├─ pa_ui_sync_from_preset(ui, &pr)
  └─ pa_ui_mark_settings_dirty(ui)
```

### 4.5 Preset save (torn-write safe)

```
UI: PARA click in SAVE_DIALOG
  ├─ pa_preset_load(ui->preset_index, &cur_p)    (start from current)
  ├─ convert slot_bands_db[5] → slot_params[EQ][]
  ├─ pa_preset_capture_from_chain(&cur_p, chain, vol, params, slot_type)
  │    └─ slot_order, slot_enable_mask, slot_type, Q21→Q16 params
  ├─ pa_preset_save(ui->save_target, &cur_p)     [preset.c]
  │    ├─ RAM copy updated (magic, seq++, CRC32)
  │    └─ commit_bank_to_flash(other_bank, seq+1)
  │         ├─ erase 4 sectors of the INACTIVE bank
  │         ├─ program payload (13 120 B)
  │         └─ program the 16 B header LAST  ← power-fail safe
  └─ ui->view = SAVED_TOAST; save_failed = (rc != 0)
```

### 4.6 SysEx / USB upload path

```
USB EP4 OUT or BLE GATT write
  └─ pa_usb_midi_rx_packet() / pa_ble_gatt_rx_write()
       └─ midi_ring_push()  (512-byte ring, drop-on-full)
  └─ pa_usb_ble_poll()        [UI thread]
       └─ drain ring byte-by-byte
            ├─ 0xF0 → reset OTA frame buffer;  0xF7 → check_ota_special_sysex()
            └─ pa_sysex_input_byte(b)         [sysex.c]
                 FSM: IDLE→MFR0→MFR1→CMD→LEN0→LEN1→LEN2→PAYLOAD→CHECKSUM→END
                   frame = F0 00 59 <cmd> <u24 len> <payload> <~sum & 0x7F> F7
                 └─ pa_sysex_dispatch()
                      ├─ cmd 25 (WRITE_BEGIN) → arm upload session
                      ├─ cmd 16 (DSP_PARAM_SET) → pa_fx_chain_set_param()
                      ├─ cmd 23/24 (READ_RECORD / READ_BLOCK) → pending block
                      └─ session complete → commit_upload_session()
                           ├─ PRESET   → pa_preset_save(slot, p)
                           ├─ NAM      → pa_user_nam_add()  (registry + one 8 KB blob slot)
                           ├─ CAB      → refused (-4): nothing reads an uploaded cabinet back
                           └─ SETTINGS → pa_settings_save()
```

---

## 5. Data Flow

### 5.1 Audio path

```
Guitar IN jack
  → ALNK0 CH1 (RX) DMA, 24-bit @ 44 118 Hz
  → g_alnk_rx_dma[64][2]
  → pa_audio_process_half(): sign-extend, input_gain_q15, Q23→Q28
  → [TUNER]  pa_tuner_process_block (pre-FX, Left channel only)
  → pa_fx_chain_run_stereo()
       → Q28→Q21 (>>7)
       → DC blocker → input gain → global gate → mono→stereo
       → slot_order traversal (Q21 in-place, L/R stereo)
       → [AMP] WaveNet / GRU → 4-band tone stack → trim × master
       → [CAB] partitioned Q18 FIR via pa_hw_accel_fir_part64
       → [EQ]  5-band biquad stack
       → [MOD/DLY/REV] true stereo
       → preset volume smoothing → speaker "air" → output limiter
  → [DRUMS] pa_drums_mix_block (mono → L+R)
  → [USB]   pa_usb_audio_mix_playback_stereo + push_capture_stereo
  → pa_soft_ceiling_q21 (soft knee, −1 dBFS)
  → master volume slew → TPDF dither → 24-bit
  → g_alnk_tx_dma[64][2]
  → ALNK0 CH0 (TX) DAC
```

**Fixed-point formats**

| Stage | Format |
|---|---|
| ALNK0 DMA | 24-bit signed |
| After `pa_audio_process_half` | Q28 |
| Inside the FX chain | **Q21** (`PA_Q21_ONE` = 2 097 152) |
| NAM GRU weights / activations | Q21, LUT index 9 bits + 17-bit fraction |
| Cabinet IR taps | Q18 (`PA_IR_MAX_TAPS` = 2048, 32 partitions × 64) |
| WaveNet | float32 (`pa_wavenet_model_t`) |
| Delay / Reverb / Mod internal tanks | int16 (Q21 ÷ 64) |
| Preset storage | Q16 knob values (0…65535 = 0…1.0) |
| Settings storage | dB / percent raw integers |

**Buffer ownership**

| Buffer | Owner | Size | Lifetime |
|---|---|---|---|
| `g_alnk_rx_dma` / `g_alnk_tx_dma` | `audio_alnk.c` | 2 × 1024 B | Static, ping-pong halves |
| `g_in_mono_q28`, `g_out_l_q28`, `g_out_r_q28` | `audio_alnk.c` | 3 × 256 B | Static working set |
| `drum_tmp[PA_AUDIO_BLOCK_FRAMES]` | `main.c` (ISR) | 256 B | **ISR stack** |
| `s_dry_L[64]`, `s_dry_R[64]` | `fx_graph.c` | 512 B | Static, one crossfade at a time |
| `s_cap_L/R[1024]`, `s_pb_L/R[1024]` | `usb_ble.c` | 4 × 4 KB | Static USB rings |
| `s_model_blob_buf[64 KB]` | `pa_user_nam.c` | 64 KB | Static, `.bss` |
| `g_dsp_pool[147 456]` | `pa_pool.c` | 144 KB | `.pool` section |
| `g_lcd_fb[240×240]` | `emu_bridge.c` | 115 KB | Host only |

### 5.2 Control path (user input)

```
Physical buttons (PH6/PH8/PH9/PA0/PA1/PA3) + PARA encoder (PC0/PC1)
  → pa_gpio_scan_poll_1khz()  [1 kHz, no debounce]
       → g_btn_state = (~raw_high) & 0x3F   (active-low, 6 bits)
       → g_enc_accum (quadrature, 4 sub-steps = 1 detent)
  → plat_buttons() / plat_enc_delta()      [consuming reads]
  → pa_main_loop() 60 Hz gate
  → pa_firmware_tick()
  → pa_ui_handle_inputs()
       → 15-view state machine
       → pa_fx_chain_* / pa_preset_* / pa_settings_* / pa_drums_*
```

**Button bitmask** (`pa_ui.h`)

| Bit | Symbol | Meaning |
|---|---|---|
| 0 | `PA_BTN_HOME` | HOME / BT switch |
| 1 | `PA_BTN_VOL_MINUS` | VOL− (preset vol −5, or Bank slot 1) |
| 2 | `PA_BTN_VOL_PLUS` | VOL+ (preset vol +5, or Bank slot 2) |
| 3 | `PA_BTN_LEFT` | `<` (preset down, or Bank slot 3) |
| 4 | `PA_BTN_RIGHT` | `>` (preset up, or Bank slot 4) |
| 5 | `PA_BTN_PARA_CLICK` | PARA click |
| 6 | `PA_BTN_PARA_HOLD1` | PARA hold 1 s → Save to |
| 7 | `PA_BTN_PARA_HOLD2` | PARA held past 3 s (fires at 3 s, however long it stays down) → pick the block up; 1-3 s on release = `PA_BTN_PARA_HOLD1` Save to |
| 8 | `PA_BTN_HOME_HOLD` | HOME hold 1.5 s → toggle PRESET / STOMP mode (the Live Stomp Lock was removed); in STOMP mode a 0.8 s hold of VOL-, VOL+, <, > (`PA_BTN_*_HOLD`) opens the effect-assign menu |
| 9 | `PA_BTN_PARA_HOLD3` | PARA hold 3 s → unused (the factory reset is a click in Settings > System) |

Combos: `PA_COMBO_TUNER` (VOL−+VOL+), `PA_COMBO_BANK` (<+>),
`PA_COMBO_DRUMS` (HOME+VOL−), `PA_COMBO_CHAIN` (HOME+VOL+, declared but unused).

### 5.3 Persistence path

```
                SPI NOR 1 MB (JEDEC ID 0x856014)  -  include/pa_flash_map.h
┌────────────────────────────────────────────────────────────────────────┐
│ 0x000000  SDFILE MBR + app_dir_head          ┐ READ-ONLY               │
│ 0x001000  uboot.boot SPL                     ┘                         │
│ 0x004000  app area (XIP at 0x02000000; app.bin at 0x4120)   the pack   │
│ 0x0B5000  Preset Store Bank B (16 KB)  ← ping-pong      ┐ rewritable   │
│ 0x0B9000  System Settings (4 KB, in place)               │ window 1     │
│ 0x0BA000  User NAM registry (4 KB, in place)             │ pa_flash_rw_ok│
│ 0x0BB000  User NAM blob slot 0 (8 KB)                    ┘              │
│ 0x0BD000  BTIF RF calibration + BLE MAC      ┐                         │
│ 0x0BE000  USER factory calibration           │ never touch             │
│ 0x0BF000  factory amp/cab bundle (240 KB)    ┘ read in place           │
│ 0x0FB000  Preset Store Bank A (16 KB)  ← ping-pong      ┐ rewritable   │
│ 0x0FF000  spare (4 KB)                                   ┘ window 2     │
└────────────────────────────────────────────────────────────────────────┘
```

**Bank A/B commit protocol** (the presets; the settings and the NAM registry run the same code with A and B at the same sector, because the 1 MB chip has no room for a second copy)

```
read header (16 B): { magic, seq, count, crc32 }
validate magic && count && full-payload CRC (13 120 B, 128-B chunks)
choose the bank with the greater seq  (wrap-safe: (int32_t)(b.seq - a.seq) > 0)

save:
  1. update the RAM copy (magic, seq++, recompute CRC32)
  2. commit to the INACTIVE bank:
       erase 4 sectors → program payload → program the 16 B header LAST
  3. on any flash error: return −1, do NOT advance s_active
  → a torn write leaves the inactive bank header-less, so the old bank still wins
```

Write-protection: `pa_spi_nor_erase_sector()` / `pa_spi_nor_write()` both return
`-3` for `addr < PA_FLASH_RW_BASE` and `-5` on brownout.

### 5.4 Telemetry path

```
ALNK0 ISR
  └─ cpu_meter_begin() / cpu_meter_end(PA_ST_MISC) / cpu_meter_block_done()
       └─ acc[8] → ema[8] (signed EMA, shift 3)
UI tick
  └─ g_ui.sram_used_bytes = pa_pool_used_bytes()
  └─ pa_ui_render(..., &g_cpu_meter) → render_performance()
```

---

## 6. Concurrency & Hand-Shake Model

There are **no mutexes, no semaphores, no atomics** anywhere in the repository
(`mutex|semaphore|__atomic|__sync_` → 0 hits). Concurrency is handled with three
hand-crafted mechanisms:

### 6.1 Seqlock publish (UI → ISR) — AMP coefficients

```c
// UI thread
g_amp_next = c;            // full coeff struct
g_amp_next_seq++;          // volatile publish

// ISR (nam_slot_run, block start)
if (g_amp_live_seq != g_amp_next_seq) {
    g_amp_live_seq = g_amp_next_seq;
    g_amp_live     = g_amp_next;   // atomic snapshot at a block boundary
}
```
The ISR never sees a half-written biquad.

### 6.2 Mute handshake (UI → ISR → UI) — click-free state changes

```
UI                                ISR
───                                ───
pa_audio_request_mute()
  g_xf_state = FADE_OUT     ──►   xf_apply(): ramp gain → 0
                                  at 0: g_xf_state = MUTED
                                  (ISR stops touching the chain)
spin ≤ 30 ms on
  pa_audio_is_muted()   ◄──      g_xf_state == MUTED
  (bounded, feeds WDT)
... mutate chain state ...
pa_audio_release_mute()
  g_xf_state = FADE_IN      ──►   xf_apply(): ramp gain → 1
                                  at 1: g_xf_state = IDLE
```
Fade constant: `PA_XF_BLOCKS = 4` (4 × 64 = 256 samples = 5.8 ms).

### 6.3 Deferred free (UI → ISR → UI) — memory reclaim

```
UI: pa_fx_chain_set_slot_enabled(chain, slot, 0)
      ├─ tail_enabled && state ? tail_draining = 1
      └─ else fade_dir = -1
                                ISR: node_xfade_post() ramps wet_q21 → 0
                                      then: active = 0; pending_destroy = 1
UI: pa_fx_chain_collect_garbage(chain)   (every 60 Hz tick)
      └─ if (nd->pending_destroy) { nd->destroy(nd); nd->state = 0; }
           └─ pa_free() — never called from the ISR
```

### 6.4 Pool allocator rule

`pa_pool.c` is documented as **UI/main-context only, never from the audio ISR**.
All 7 fx modules honour this: `fx_delay.c` and `fx_reverb.c` set `pending_destroy = 1`
rather than calling `pa_free()` inline.

### 6.5 CPU1 mailbox (SPSC, lock-free)

```c
typedef struct {
    volatile uint32_t req_seq;      // CPU0 → CPU1
    volatile uint32_t ack_seq;      // CPU1 → CPU0
    volatile uint32_t c1_cycles_last;
    int32_t           pipe_buf[PA_BLOCK_N];
    pa_fx_chain_t    *chain;
    uint8_t           c1_online;
} pa_smp_mailbox_t;
```
CPU0 swaps `pipe_buf` with the caller's buffer, increments `req_seq`, issues `csync`, and
raises `PA_C1_INT_SET_REG = 1`. CPU1 spins on `req_seq`, runs slots 3..7, `csync`,
sets `ack_seq`, clears the IPI. **This path is compiled but dormant** — see §10.

---

## 7. Shared Data Structures & Global State

### 7.1 `fx_node_t` (`pa_fx.h`)

```c
typedef struct fx_node {
    struct fx_node *next;          // scheduler linked list
    uint32_t        active;
    uint32_t        frame;
    void           *state;         // pool-allocated: delay lines / tanks / coeffs
    const char     *tag;
    void (*run)       (struct fx_node*, int32_t *buf, int n);
    void (*run_stereo)(struct fx_node*, int32_t *buf_L, int32_t *buf_R, int n);
    void (*create)    (struct fx_node*);
    void (*destroy)   (struct fx_node*);
    void (*set_param) (struct fx_node*, int id, int32_t val);
    void (*reset)     (struct fx_node*);
    uint8_t         tail_enabled;    // V105 DLY/REV Tail
    uint8_t         tail_draining;
    int8_t          fade_dir;        // +1 in, -1 out, 0 idle
    uint8_t         pending_destroy;
    int32_t         wet_q21;         // current wet amount 0..1.0
} fx_node_t;
```

### 7.2 `pa_fx_chain_t` (`pa_fx.h`)

Key fields: `head`, `slots[8]`, `slot_order[8]`, `input_gain_q21`, `preset_vol_q21`,
`preset_vol_cur_q21`, `global_in_gain_q21`, `gate_thresh_q21` / `gate_env_q21` / `gate_gain_q21` / `gate_hold`,
`force_cab_off`, `fizz_filter_enabled` + 8 biquad states, `speaker_mode`,
`output_limit_mode`, `lmt_env_q21`, `spk_lp_L/R`, `in_dc_x1/y1`.

### 7.3 `pa_preset_t` (`pa_preset.h`) — 164 bytes

| Offset | Field | Type | Notes |
|---|---|---|---|
| 0 | `magic` | u32 | `PA_PRESET_MAGIC` = `"PMP2"` |
| 4 | `seq` | u32 | Monotonic commit counter |
| 8 | `name` | char[24] | |
| 32 | `bpm` | u16 | |
| 34 | `genre_tag` | u8 | CLEAN/CRUNCH/LEAD/METAL/BASS/ACOUSTIC/AMBIENT/CUSTOM |
| 35 | `custom_cab_idx` | u8 | 255 = stock cab |
| 36 | `custom_cab_blend` | u8 | % |
| 37 | `input_trim_db` | i8 | −12…+12 |
| 38 | `gate_threshold_pct` | u8 | |
| 39 | `slot_enable_mask` | u8 | 1 bit per slot |
| 40 | `slot_order` | u8[8] | Chain order |
| 48 | `amp_model_idx` | u8 | 0..19 stock, 20..39 user NAM |
| 49 | `cab_model_idx` | u8 | |
| 50 | `preset_vol` | u8 | 0..100 |
| 51 | `flags` | u8 | `PA_PRESET_FLAG_EMPTY` = 0x01 |
| 52 | `slot_type` | u8[8] | Per-slot model/type |
| 60 | `stomp_map` | u8[4] | VOL−/VOL+/</> in STOMP mode |
| 64 | `params` | u16[8][6] | **Q16** knob values |
| 160 | `crc32` | u32 | CRC over bytes [0, 160) |

### 7.4 `pa_settings_t` (`pa_settings.h`) — 60 bytes

`magic` `"PSET"`, `version`, `size`, `seq`, `last_preset`, `home_view_mode`, `play_mode`,
`fizz_level`, `display_brightness`, `display_auto_dim`, `display_battery_pct`, `theme`,
`input_gain_db`, `gate_db`, `out_level_pct`, `io_output_type`, `io_cab_on_line`,
`usb_sample_rate`, `bt_enabled`, `tuner_mute`, `tuner_ref_hz`, `drum_bpm`,
`drum_pattern`, `drum_volume`, `drum_drums_on`, `reserved[19]`, `crc32`.

### 7.5 `pa_ui_ctx_t` (`pa_ui.h`) — UI state

~180 bytes. Highlights: `view` / `prev_view` (`pa_ui_view_t`, 15 values),
`preset_index` (0..79), `preset_volume`, `master_volume`, `bank_group` (0..19),
`save_target`, `save_btn`, `saved_toast_timer_ms`, `save_failed`,
`cursor_slot_idx` (position in chain), `selected_slot`, `editor_cursor`
(−2 = ON/OFF, −1 = Model, 0..N-1 = param), `edit_mode`, `sub_cursor`, `sub_edit_mode`,
`show_vol_popup`, `in_peak_pct`, `out_peak_pct`, `settings_cursor`, `settings_scroll_top`,
`input_trim_db`, `gate_thresh_db`, `out_level_pct`, `io_output_type`, `io_cab_on_line`,
`display_brightness`, `display_auto_dim`, `display_battery_pct`, `theme`, `dimmed`,
`last_input_ms`, `settings_dirty`, `batt_pct`, `batt_warn`, `stomp_map[4]`,
`preset_name[24]`, `eq_bands_db[5]`, `eq_low_cut`, `eq_high_cut`, `eq_enabled`,
`slot_bands_db[5]`, `tuner_cursor`, `sys_cursor`, `reset_confirm_cursor`, `drum_step`,
`slot_type[8]`, `user_nam_loaded`, `user_nam_name[24]`, `bt_state`, `usb_connected`,
`play_mode`, `usb_sample_rate`, `battery_mv`, `drum_bpm`, `drum_playing`, `drum_drums_on`,
`drum_pattern`, `drum_volume`, `live_stomp_lock`, `fizz_filter_enabled`,
`reset_hold_timer_ms`, `slot_params[8][6]` (Q21), `sram_used_bytes`,
`cpu_total_permille`, `tuner` (`pa_tuner_state_t`).

### 7.6 Global state table

| Symbol | Module | Type | Notes |
|---|---|---|---|
| `g_fx_chain` | `main.c` | `pa_fx_chain_t` | The chain; also referenced by `preset.c`, `pa_user_nam.c`, `smp_cpu1.c` |
| `g_nam_model` | `main.c` | `pa_nam_model_t` | Aligned 32; 3 852 B |
| `g_wn_model` | `main.c` | `pa_wavenet_model_t` | `.pool` section, aligned 32; 84,120 B (A2-Lite) |
| `g_active_amp_engine` | `main.c` | `int` | 0 = GRU, 1 = WaveNet |
| `g_ui` | `main.c` | `pa_ui_ctx_t` | Read by the ISR (`g_ui.tuner.active`) |
| `g_usb_ble` | `main.c` | `pa_usb_ble_ctx_t` | |
| `g_drums` | `main.c` | `pa_drums_t` | |
| `g_cpu_meter` | `main.c` | `pa_cpu_meter_t` | Written by the ISR |
| `g_settings` | `main.c` | `pa_settings_t` | Live settings mirror |
| `g_amp_live` / `g_amp_next` | `main.c` | `amp_coeffs_t` | ISR / UI halves of the seqlock |
| `g_amp_next_seq` / `g_amp_live_seq` | `main.c` | `volatile u32` | Seqlock counters |
| `g_xf_state` / `g_xf_gain` | `main.c` | `volatile u8` / `i32` | Mute handshake |
| `g_pa_audio` | `audio_alnk.c` | `pa_audio_state_t` | Public (declared in `pa_audio.h`) |
| `g_onyx_greys[3]` | `ui_app.c` | `u16[3]` | Runtime theme palette |
| `g_presets[80]` | `preset.c` | `pa_preset_t[80]` | 13 120 B |
| `g_dsp_pool[147 456]` | `pa_pool.c` | `u8[]` | `.pool` section |
| `g_nam_registry` | `pa_user_nam.c` | registry | 20 user NAM slots |
| `g_sx` / `g_upload` | `sysex.c` | parser / session | `g_upload` holds a 32 KB buffer (`PA_UPLOAD_BUFFER_SIZE`) |
| `g_smp_mbox` | `smp_cpu1.c` | mailbox | Aligned 32 |
| `g_hw_cmd_block` | `hw_accel.c` | cmd struct | Aligned 4 |
| `g_alnk_tx_dma` / `g_alnk_rx_dma` | `audio_alnk.c` | `i32[128][2]` | Ping-pong |
| `g_pa_stock_amps[]` | `stock_assets.c` | `pa_stock_amp_info_t[20]` | Generated |

### 7.7 Memory map (`app.ld`)

| Region | Address | Length | Contents |
|---|---|---|---|
| `XIP` | `0x02000120` | 0xB0874 (723 KB) | `.text`, `.rodata` |
| `RAMTEXT` | `0x01C00000` | 24 KB | `.ram_text`, `.c1_text` (XIP-copied) |
| `RAM` | `0x01C08000` | 96 KB | `.data`, `.bss` |
| `POOL` | `0x01C20000` | 336 KB | `.pool` (NAM/WaveNet model, DSP pool) |
| `NOINIT` | `0x01C7C000` | 15 KB | `.noinit` |
| user stack | `0x01C74100`→`0x01C7A000` | 24 KB | `_ustack_*` |
| IRQ stack | `0x01C7A100`→`0x01C7C000` | 8 KB | `_sstack_*` |

---

## 8. Host / Emulator Architecture

### 8.1 `custom-fw/emu/` — the HIL workbench

```
  Browser (ui.html, 240×240 canvas + front-panel widgets)
        │  HTTP :8080, 20 Hz poll of /api/state
        ▼
  server.py  (ThreadingHTTPServer, ctypes → DLL, sounddevice/PortAudio ASIO)
        │  ctypes
        ▼
  pocket_amp_emu.dll  (LLVM-MinGW, -DPA_HOST_SIM=1 -DPA_EMU_DLL=1)
        │  compiles the REAL firmware modules
        ├── src/dsp/*    (4 files)
        ├── src/nam/*    (2 files)
        ├── src/fx/*     (8 files)
        ├── src/sys/*    (7 files)
        ├── src/board/*  (5 files: ui_app, ui_5x7, usb_ble, spi_nor, gpio_scan)
        └── emu_bridge.c (shims + 36 exported C functions)
```

`emu_bridge.c` supplies the device-only pieces the DLL does not link:
`pa_alloc`/`pa_free` (calloc-based with SRAM accounting), `st7735s_fill_rect`
(writes a 240×240 RGB565 `g_lcd_fb[]` instead of driving SPI1),
`pa_amp_set_model_profile`, `plat_backlight_set`, `plat_wdt_feed`,
`plat_battery_is_brownout`. The mute handshake is **not** among them: it lives
in the portable core (`src/sys/pa_audio_core.c`), so the emulator, the
browser and the device all fade the same way.

The emulator's audio is the **firmware's** audio: `emu_process_audio_f32()`
converts the host's float samples to 24-bit and hands them to
`pa_audio_core_process_half()` — the device's own jack — then converts the
jack's 24-bit output back to float. The float↔24-bit conversion is the only
host-specific part, and it lives in the shim.

**36 exported functions** cover: lifecycle (`emu_init`, `emu_process_audio_f32`,
`emu_ui_input`, `emu_get_lcd_rgba`), NAM/WaveNet loading, stock assets, chain and
parameter control, SysEx feeding, Bluetooth state, and telemetry
(`emu_get_telemetry` → 26-field `emu_telemetry_t`).

**The emulator runs the firmware's tick, not a copy of it.** `emu_ui_input()`
only *latches* the front-panel input (the buttons, the encoder, the MASTER
pot); the firmware's own `pa_audio_core_tick()` consumes it, exactly as the
device's main loop does. So the emulator gets the contact debounce, the
one-tick action pulses, the HOLD bits, the chord combos, the battery model,
the toast timers, the auto-dim, the settings persistence and the deferred
preset swap - the same code the device and the browser run.

The tick is clocked by the **sample clock**, not by the host's wall clock: the
emulator counts the samples it has processed (1451 µs per 64 frames), and a
tick every 11 blocks is 15.96 ms - 60 Hz. On the device one hardware clock
drives the sample clock, the tick interrupt and the contact debounce; if the
emulator used two different clocks, the debounce would never complete and no
button would do anything. When no audio is running, the host's clock is
folded in instead, so the timers keep running.

### 8.2 HTTP API

| Method | Endpoint | Purpose |
|---|---|---|
| GET | `/`, `/index*` | `ui.html` |
| GET | `/onyx/<name>.png` | Static Onyx screen renders |
| GET | `/api/onyx_screens` | Screen list |
| GET | `/api/devices` | PortAudio device list |
| GET | `/api/stock_assets` | Amp / cab names |
| GET | `/api/state` | Full poll payload (LCD base64, telemetry, scope, slot state) |
| GET | `/api/telemetry` | Compact telemetry |
| POST | `/api/load_nam` | Raw `.nam` bytes → `emu_load_nam_struct` |
| POST | `/api/load_ir` | Raw `.wav` bytes → `emu_load_cab_ir_floats` |
| POST | `/api/load_sample_nam` | Load a `.nam` from `emu/` |
| POST | `/api/input` | `{buttons, enc_delta, master_pot, …}` |
| POST | `/api/sysex` | Raw SysEx bytes |
| POST | `/api/preset`, `/api/master_volume`, `/api/slot`, `/api/param`, `/api/drums` | Parameter control |
| POST | `/api/amp_profile`, `/api/stock_amp`, `/api/stock_cab`, `/api/voicing`, `/api/slot_type` | Model control |
| POST | `/api/bluetooth`, `/api/set_device`, `/api/test_signal`, `/api/sample_player`, `/api/ir` | Device / test-signal control |

### 8.3 Host audio path

```
Audio interface (ASIO) → sounddevice callback
  → _static_mono_in (4096 f32)
  → [resample 44.1 kHz via soxr if the device rate differs]
  → dll.emu_process_audio_f32(in, outL, outR, n)
       → chunked into 64-sample quanta
       → DC blocker → Q21
       → process_block_64(): tuner → gain → 8-slot chain (slot_order) → preset vol → drums
       → soft-knee tanh saturator → outL/outR
  → outdata[:, 0], outdata[:, 1]
```

### 8.4 Python UI reference implementation (`new_ui/pocket-amp/`)

Strict layering: `screens → widgets → Canvas`, `icons → Canvas`. No literal colours
outside `tokens.py`.

| Module | Role |
|---|---|
| `tokens.py` | Palette, 8 block identity colours, fonts, metrics |
| `canvas.py` | The only drawing surface; 4× supersample, LANCZOS downscale |
| `icons.py` | 28 vector icons on a 24×24 grid |
| `widgets.py` | 20 components (battery, title_bar, seg_meter, toggle, block_tile, eq_bands, modal, …) |
| `screens.py` | One function per screen + the `DEMO` state + `SCREENS` inventory (30 screens) |
| `navigation.py` | **Data only** — `CONTROLS`, `GLOBAL_RULES`, `BEHAVIOR` (14 screens) |
| `flow_diagram.py` | Renders `navigation.png` **and** writes `NAVIGATION.md` from `navigation.py` |
| `render_all.py` | Renders `screens/*.png`, `assets/icon_*.png` (16/24/32), `preview.png` |

`navigation.py` is the declared "single source for the navigation", so the diagram,
the markdown, and the firmware state machine cannot drift apart.

`ui_design/` (repo root) and `custom-fw/ui_design/` are **byte-identical copies** of an
older 6-block variant (no `navigation.py`).

`sim/pocket_amp_sim.html` is a self-contained static JS simulator of the CPU budget
(no fetch, no API).

---

### 8.4 The "jack" — one audio I/O path for three platforms

`pa_audio_core_process_half(const int32_t *rx_half, int32_t *tx_half)` is the
**device's** audio I/O path, moved out of `src/board/audio_alnk.c` into the
portable core (`src/sys/pa_audio_core.c`). It takes 64 stereo frames of
24-bit I2S samples and returns 64 stereo frames of 24-bit samples, applying
the input gain, the master volume slew and the 1-LSB TPDF dither.

```
   ALNK0 DMA (device)  ──┐
   PortAudio (emulator) ─┼──>  pa_audio_core_process_half()  ──┐
   Web Audio  (browser) ─┘      24-bit in  ->  24-bit out      │
                                                               │
                              24-bit out  ->  the same three ──┘
```

The three platforms differ **only** in what they put into the jack and what
they do with what comes out:

| Platform | Jack in | Jack out |
|---|---|---|
| Device | ALNK0 RX DMA half-buffer (`rx_half[2*i]`, the guitar on the Left slot) | ALNK0 TX DMA half-buffer |
| Emulator | the host's float samples, converted to 24-bit in `emu_bridge.c` | converted back to float |
| Browser | the browser's audio, converted to 24-bit in the AudioWorklet | converted back to float |

On Windows the browser's Web Audio **is** Windows Audio (WASAPI), so the
browser's jack is literally the Windows Audio input and output (and it can be
an ASIO interface behind a bridge driver).

The runtime the jack runs on is bound once at boot:

```c
pa_audio_core_bind(&g_fx_chain, &g_ui, &g_drums, &g_usb_ble);
pa_audio_core_init_jack(&g_ui);      /* unity input gain, the UI's master level */
```

so the device, the emulator and the browser all drive the same chain through
the same code. `src/` contains no WASM- or host-specific code; the host builds
compile the *device* path **out**, never the other way round.


## 9. Build & Packaging

### 9.1 `custom-fw/Makefile`

| Target | Action |
|---|---|
| `test` | Builds + runs 4 host binaries: `test_reclaim`, `test_dsp_chain`, `test_full_firmware`, `test_robustness` |
| `cross` | Compile-only smoke test — every module with `-target pi32v2 -mcpu=r3 -c -o /dev/null` (no link) |
| `fw` | Compiles to `build_pi32/*.o` → links with `app.ld` → `app.bin` → `tools/pack_ufw.py` |
| `clean` | Removes host binaries and `build_pi32/` artifacts |

Toolchain: `$PI32_ROOT` / `C:/JL/pi32` (Windows) or `/opt/jieli/common` (Linux).
Flags: `-target pi32v2 -mcpu=r3 -O2 -Wall -ffreestanding -fno-builtin`.

### 9.2 `custom-fw/build.ps1` — the master Windows script

1. **Host verification** — 4 `clang -O2 -Wall -Wextra -DPA_HOST_SIM=1` builds:
   `test_cpu_meter`, `test_reclaim`, `test_dsp_chain`, `test_full_firmware`.
2. **pi32v2 cross-compile** — finds every `custom-fw/src/**/*.c` by recursion
   (so `settings.c` and `pa_user_nam.c` *are* included, unlike the Makefile),
   links `app.ld` → `pocket_amp_custom.elf` → objcopy `.text` + `.data` →
   `custom-fw/build_app.bin`.
3. **Packaging** — `python custom-fw/tools/pack_ufw.py --in-bin … --out-ufw … --out-fwsc …`
   (standalone UFW path, no stock template).

### 9.3 `custom-fw/tools/pack_ufw.py` — the packager

| Constant | Value |
|---|---|
| `CHIP_KEY` | `0x980F` |
| `APP_SLOT_MAX` | `0xB0874` (723 060 B) |
| `FWSC_TARGET` | `"Pocket AMP_105"` |

- `crc16_ccitt(data, init=0)` — nibble-table CRC-16/CCITT.
- `xor_header(data, key=0xFFFF)` — JieLi header obfuscation.
- `encode_marker(name)` — 20-byte product marker, `c[i] = (name[i] + i + 1) & 0xFF`, `0x7D` padded.
- `wrap_fwsc` / `unwrap_fwsc` — interleaves the 20 marker bytes, one after every 47 payload bytes.
- `build_from_template(template_fwsc, app_bytes)` — injects `app.bin` into the stock
  `.fwsc`'s `flash.bin` at `+0x5000`, recomputes entry → table → header CRC16.
- `build_standalone_ufw(app_bytes)` — synthesises a `JLUFW` container.
- `verify_package(fwsc)` — re-validates product identity, CRCs, chip string, lengths.

### 9.4 Other tools

| Tool | Role |
|---|---|
| `tools/verify_package.py` | Standalone auditor of the shipped `.ufw`/`.fwsc` |
| `tools/build_full_flash_image.py` | Extracts `flash.bin` → 1 048 576-byte NOR image (0xFF-filled, the real chip's size) |
| `tools/check_flash_sanity.py` | Size/fill ratio, MBR, app partition boundary, embedded strings |
| `tools/pack_nam_am3.py` | NAM A2-Lite packer (`0xFC8` / 4 040 B) + SysEx frame builder (cmd 25) |
| `tools/gen_act_luts.py` | 512-entry Q21 sigmoid/tanh tables |
| `tools/preset_click_probe.c` | Detects clicks at preset switches via the mute handshake |
| `tools/ui_screenshot_harness.c` | Dumps raw 240×240 RGB565 frames |
| `tools/bin-strings.ps1` | String extractor for pi32v2 binaries |

### 9.5 Host test suite (`custom-fw/test/`)

| Test | Coverage |
|---|---|
| `test_dsp_chain.c` | Activation LUT symmetry, GRU block run, true-stereo 8-slot chain with tail spillover, WaveNet inference |
| `test_reclaim.c` | C7 reclaim: all ON vs reverb+delay bypassed (≈57 KB), NAM H32 over/under budget |
| `test_full_firmware.c` | 8-slot init + 30 blocks of 440 Hz, reclaim, tuner lock, drum trigger |
| `test_drums.c` | The Groove Station: the generated pattern table's invariants (loop lengths on a 16th, a velocity on every hit, the lanes the reference library uses) and the engine's clock, which must walk the whole pattern, wrap at its own length and measure a loop as exactly that long |
| `test_kit.c` | The kit: the balance of the twelve voices per unit of velocity, the cymbals' metal layer (measured as a spectral line against its neighbour), the bus level and the loop's silence |
| `test_ota.c` | The app side of the JieLi OTA handshake: the two frames the updater sends, and the 34-byte ID block the query is answered with |
| `test_groove_pixels.py` | The Groove Station screen vs the design's reference PNG, pixel for pixel, plus the later-bar playhead regression |
| `test_robustness.c` | Blank flash, torn-save sweep (k = 0..9), 40 sequential saves, torn factory reset, protected-region probes, 200 000-op allocator churn |
| `test_cpu_meter.c` | Per-stage cycle meter simulation (built by `build.ps1` only) |
| `test_wavenet_cycles.c` | WaveNet micro-benchmark vs the per-block budget (manual build) |
| `hil_stress_test.py` | HTTP HIL: 80-preset sweep, 49 drum patterns, 20 rapid switches |

`custom-fw/emu/test_onyx_ui.py` — UI state-machine regression (30 assertions, 49 snapshot PNGs).

---

## 10. Known Gaps & Discrepancies

### 10.1 Resolved Gaps & Execution-Verified Items

The following architectural gaps previously recorded in early audits have been resolved in the firmware codebase and verified on hardware or in the host execution harnesses:

| # | Item | Status | Verification & Resolution Details |
|---|---|---|---|
| **R1** | **Dual-core offload dormant** | **RESOLVED** | Core 1 actively runs the neural amp engine (Milestone 1, `src/sys/smp_cpu1.c`, `src/board/cpu1.c`). WaveNet inference runs on Core 1 in ~1,360 µs of the 1,451 µs block budget, with Core 0 running input conditioning, tone stack, fizz filter, and remaining FX nodes. Handoff is lock-free with swap probes (`SWD` buffer) and stack monitoring. |
| **R2** | **MIDI mapping unverified / manual-derived** | **RESOLVED** | Validated on live hardware via `pocket_amp_midi_discovery.py`: PC 0..79 (80 presets across 20 banks), CC 20..25 (module bypasses), CC 64..67 (front panel controls: Home, Para click, Para CCW, Para CW), full 92-byte preset binary layout, and 32 SysEx opcodes. Documented in `POCKET_AMP_MIDI_ADDRESS_MAP.md`. |
| **R3** | **Signal chain reorder (`PA_COMBO_CHAIN`)** | **RESOLVED** | Interactive Moving Mode (`PARA HOLD 2s` / `PA_BTN_PARA_HOLD2`) was adopted as the primary user-facing workflow to pick up, scroll, and drop blocks anywhere in the 8-slot chain with continuous signal reconnection. `PA_BTN_COMBO_CHAIN` remains in the input bitmask but is superseded by Moving Mode. |
| **R4** | **Cabinet convolution API** | **RESOLVED** | Partitioned FIR convolution up to 2048 taps is fully implemented in `custom-fw/src/fx/fx_cab_ir.c` with hardware accelerator dispatch (`pa_hw_accel_fir_part64`), stock AM4 asset management, and custom user slot handling via `src/sys/pa_user_cab.c`. |
| **R5** | **EQ coefficient slewing & zipper noise** | **RESOLVED** | `src/fx/fx_eq.c` uses double-buffered ping-pong coefficient tables (`coef[2][5]`) with atomic `buf_idx` swapping between UI thread updates and the audio ISR. DF2T states run in float on the hardware FPU, eliminating parameter zipper noise and race conditions. |
| **R6** | **Screen redraw & UI SPI bus load** | **RESOLVED** | Display list architecture implemented in Milestone 1 (`src/board/ui_app.c`): ~1,370 drawing commands are recorded once and replayed only across dirty bands, eliminating full 240×240 frame clears and excessive SPI transaction overhead every frame. Backlight dimming during transitions. |
| **R7** | **Preset & model change busy-wait** | **RESOLVED** | `pa_preset_apply_smooth()` in `src/sys/preset.c` delegates asynchronously to `pa_audio_core_request_preset()`. Seamless crossfades occur at audio block boundaries without blocking or spinning on the 60 Hz UI thread. |
| **R8** | **Stock CPU clock & voicing calibration** | **RESOLVED** | Hardware clock configuration verified: the AC791N runs at 360 MHz (matching `PA_CPU_HZ`). Tone stack center frequencies and gain scalings match the 20-amp asset catalog (`pa_stock_assets.h`). |
| **R9** | **Button debounce & hold gesture tracking** | **RESOLVED** | Implemented in `src/sys/pa_input.c` with 1 kHz contact debounce (`PA_IN_DEBOUNCE_MS`), release click qualification, chord decoding, and timed hold gesture classification (HOLD 1s for Save, HOLD 2s for Move mode, HOLD 3s for Lock). |
| **R10**| **Emulator display buffer documentation** | **RESOLVED** | Clarified in `custom-fw/emu/emu_bridge.c`: full 240×240 IPS RGB565 display buffer (230,400 bytes). |

---

### 10.2 Open Technical Debt & Documentation Sync

| # | Issue | Location | Impact & Planned Action |
|---|---|---|---|
| 1 | `pa_cab.h` contains legacy clean-room prototypes | `include/pa_cab.h` | 5 functions (`pa_cab_ir_init`, etc.) are declared but superseded by `fx_cab_ir.c` / `pa_user_cab.c`. Header should be pruned or unified. |
| 2 | `ui_design/POCKET_AMP_OPERATIONAL_SPEC.md` specification drift | `ui_design/` | Older UI specification draft mentions legacy combos (`HOME+VOL+` vs `HOME+VOL-`). Code in `src/sys/pa_input.c` and `src/board/ui_app.c` is canonical. Spec documentation needs alignment. |
| 3 | Stale comment in `pa_flash_map.h` | `include/pa_flash_map.h:51` | Comment still notes "80 x 184 B" from v1 format; v2 preset record size is 164 B (`16 + 13,120 <= 16,384` bytes). Code and math are correct. |
| 4 | Makefile test target omits `test_cpu_meter.c` | `custom-fw/Makefile:75-87` | `test_cpu_meter.c` is compiled and verified by `build.ps1`, but omitted from the POSIX `Makefile` `test` rule. |

---

*Generated from the source tree at `custom-fw/` (39 firmware modules + 50 headers,
plus the `emu/`, `tools/`, `test/`, `new_ui/`, `ui_design/` and `sim/` host layers).*
