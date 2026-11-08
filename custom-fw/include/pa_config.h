/* SPDX-License-Identifier: GPL-3.0-only */
/* Copyright (C) 2026 The Onyx Authors (@28spawn) */
/* pa_config.h — build-time configuration knobs.
 * Pocket Amp custom firmware (JieLi AC791N / wl82). Change these to push limits;
 * see ROADMAP.md for the rationale/costs of each. */
#ifndef PA_CONFIG_H
#define PA_CONFIG_H

/* Audio core — the NAM/amp DSP is 44.1 kHz native.
 * 48 kHz is possible on the I/O path via SRC, model stays 44.1k. */
#define PA_SAMPLE_RATE_HZ      44100
#define PA_SAMPLE_RATE         PA_SAMPLE_RATE_HZ
/* Core clock ASSUMPTION - the single source of truth for every CPU load figure
 * in the firmware and the tools. 360 MHz is what main.c's cycle conversion
 * assumes (Timer4 runs at 24 MHz, ratio x15). It is NOT measured: the AC791N
 * data sheet rate is 320 MHz, and older tools in this tree used 320 (and
 * nam_loader.py planned with 240-320). A 360 MHz assumption makes every load
 * percentage look ~11 % cheaper than a 320 MHz one, so treat CPU figures as
 * estimates with a 320-360 MHz band until one is measured on the device
 * (chip specification). Do not hard-code 320/240 anywhere else. */
#define PA_CPU_HZ              360000000u
#define PA_BLOCK_N             64      /* samples/channel per DAC block (64..128) */

/* Board identification. Defaults to Onyx if not specified */
#ifndef BOARD_POCKET_AMP
#if !defined(BOARD_BLACKBOX)
#define BOARD_POCKET_AMP       1
#endif
#endif

/* The firmware version the editor and the updater compare against;
 * the device-info record and the OTA ID block both carry this number.
 * One definition, so the two cannot drift apart. */
/* PA_BACKLIGHT_FIXED 1: the backlight is held at 100 % (a steady output, no PWM). The brightness setting is kept but has no effect, and the short dip at a screen change is off.
 * The brightness is a 10-step software PWM ticked from the audio interrupt (gpio_scan.c): any change in the audio interrupt's timing (a swap, the tuner, the drums, a heavier amp or
 * cab) made the light itself flicker, with no redraw involved. */
#ifndef PA_BACKLIGHT_FIXED
#define PA_BACKLIGHT_FIXED 1
#endif

#ifndef PA_FW_VERSION
#define PA_FW_VERSION          118u   /* shown as 1.18; the OTA ID block says V118 */
#endif
#ifndef PA_DEVICE_NAME
#if defined(BOARD_BLACKBOX)
#define PA_DEVICE_NAME         "BlackBox"
#else
#define PA_DEVICE_NAME         "Onyx"
#endif
#endif
#ifndef PA_DEVICE_MODEL
#if defined(BOARD_BLACKBOX)
#define PA_DEVICE_MODEL        "BlackBox"
#else
#define PA_DEVICE_MODEL        "Onyx"
#endif
#endif

/* The C7 DSP pool (src/sys/pa_pool.c holds the array; every effect allocates its
 * state from it). 81,920 -> 147,456 B funds the 24,576-sample reverb tank
 * (+24,576 B) and the 32,768-sample delay line (+32,768 B); the money comes from
 * the two staging buffers that shrank to 32 KB each (NAM blob reader and the
 * SysEx upload buffer), which freed 65,536 B. The measured high-water mark with
 * every effect on is budgeted - do not budget this from
 * arithmetic. */
#define PA_DSP_POOL_BYTES      147456u

/* NAM A2 GRU. Baseline architecture: single-layer GRU, input 1, hidden 16, Q21.
 * Raise PA_NAM_HIDDEN to 24/32 ("A2-Full") only with dynamic alloc + reclaimed budget (C7). */
#define PA_NAM_HIDDEN          16      /* 16 (stock) | 24 | 32 ; MAC cost ~O(H^2) */
#define PA_NAM_INPUT           1
#define PA_NAM_QSHIFT          21      /* Q21 fixed-point (scale = 2^21 - 1 = 2097151) */

/* IR cab convolution. Stock IR length 1024/2048 taps; 4096 needs bigger HW cmd buffer [D4]. */
#define PA_IR_MAX_TAPS         2048

/* Feature toggles for the improvement roadmap (ROADMAP.md) */
#define PA_FEAT_FREE_ON_BYPASS 1   /* C7: free+unlink disabled nodes (reclaim CPU+SRAM) */
#define PA_FEAT_CPU_METER      1   /* E12: per-stage cycle telemetry on ST7789 */
#define PA_FEAT_DENSE_ACT_LUT  1   /* A2: 512-entry linear-interpolated LUT vs stock 128-entry */
#define PA_FEAT_SMP_CPU1       0   /* SP-1: Dual-core CPU1 offload (disabled until verified on physical AC791N hardware) */
#define PA_FEAT_DRUMS          1   /* SP-2: Built-in procedural 808/909 drum & metronome synth */

/* Builder item: BLE (Bluetooth LE Radio)
 * Off by default (0). By default, the firmware build never powers or touches
 * the Bluetooth / Wi-Fi radio hardware of the device. Must be explicitly enabled
 * at build time (-Ble flag in build.ps1 / -DPA_BLE=1 / make BLE=1) to compile in
 * the Optimist BLE stack and enable the hardware radio. */
#ifndef PA_BLE
#define PA_BLE 0
#endif

/* The front panel has a MASTER volume pot on SARADC channel 0 (0..4095, Q12).
 * On device this ADC does not exist (digital VOL-/+ buttons);
 * in the emulator, host sim and wasm workbench, the UI slider provides virtual ADC input. */
#if defined(PA_EMU_DLL) || defined(PA_HOST_SIM) || defined(__wasm__) || defined(__EMSCRIPTEN__)
#define PA_HAS_MASTER_POT     1
#else
#define PA_HAS_MASTER_POT     1   /* the master knob is a plain SARADC channel on the device (gpio_scan.c: plat_pot_poll) */
#endif

/* PA_HOT_RAM: 1 = the hot DSP code (marked PA_HOT) runs from SRAM instead of the flash. The stock firmware copies its audio code to SRAM at
 * boot and a dev's reading of its start-up code says so; here it is an EXPERIMENT, because an earlier try on the unit with the amp alone
 * showed no gain. With two cores and a shared flash cache the question is open: the readout's slot times say whether it helps. */
#ifndef PA_HOT_RAM
#define PA_HOT_RAM 0   /* OFF: the call instruction spans only +-4 MB and the RAM is 4 MB below the flash, so RAM code cannot call any flash function (libm, memcpy, the other modules) directly. Needs every such call made indirect first. */
#endif
#if PA_HOT_RAM && !defined(PA_HOST_BUILD) && !defined(PA_HOST_SIM) && !defined(PA_EMU_DLL) && !defined(__wasm__) && !defined(__EMSCRIPTEN__)
#define PA_HOT __attribute__((section(".ram_text.hot")))
#else
#define PA_HOT
#endif

/* PA_RAMFN: the leaf loops of core 0 (the delay, the EQ) in the SRAM window 0x01C01000-0x01C01FFF (app.ld RAMFN) instead of the flash. EXPERIMENT 2026-11-02, second try:
 * core 0's effects have no accelerator to hide behind and run from the flash cache that core 1 shares; the readout's DLY slot time (129-138 us from the flash)
 * says whether it helps. Its first try (pack 100) used the low SRAM next to core 1's stub and was overwritten by the UI's text buffer: the window is now its own
 * linker region, the boot memory test skips it (main.c) and the QA gate checks that no source uses an address inside it. A function marked PA_RAMFN must make no
 * call into the flash (a call instruction reaches 4 MB and the SRAM is 4 MB below the flash), so the helpers it uses are forced inline, and it is called
 * through a pointer. */
#ifndef PA_RAMFN_ON
#define PA_RAMFN_ON 1
#endif
#if PA_RAMFN_ON && !defined(PA_HOST_BUILD) && !defined(PA_HOST_SIM) && !defined(PA_EMU_DLL) && !defined(__wasm__) && !defined(__EMSCRIPTEN__)
#define PA_RAMFN __attribute__((section(".ramfn.core0")))
#else
#define PA_RAMFN
#endif

#endif /* PA_CONFIG_H */
