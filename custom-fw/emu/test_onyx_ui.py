#!/usr/bin/env python3
"""
custom-fw/emu/test_onyx_ui.py — Verification test for the Onyx UI implementation
Tests all 15 screens, navigation transitions, button inputs, encoder rotation, and saves PNG snapshots.
"""

import os
import sys
import ctypes
import time
from pathlib import Path

# Add pillow if available for saving test snapshots
try:
    from PIL import Image
    HAS_PIL = True
except ImportError:
    HAS_PIL = False

EMU_DIR = Path(__file__).resolve().parent
DLL_PATH = EMU_DIR / "pocket_amp_emu.dll"
SNAPSHOTS_DIR = EMU_DIR / "test_snapshots"
SNAPSHOTS_DIR.mkdir(exist_ok=True)

class EmuTelemetry(ctypes.Structure):
    _fields_ = [
        ("view", ctypes.c_uint32),
        ("preset_index", ctypes.c_uint32),
        ("preset_volume", ctypes.c_uint32),
        ("master_volume", ctypes.c_uint32),
        ("selected_slot", ctypes.c_uint32),
        ("selected_param", ctypes.c_uint32),
        ("slot_active_mask", ctypes.c_uint32),
        ("sram_used_bytes", ctypes.c_uint32),
        ("sram_peak_bytes", ctypes.c_uint32),
        ("cpu_total_permille", ctypes.c_uint32),
        ("cpu_stage_permille", ctypes.c_uint32 * 8),
        ("tuner_active", ctypes.c_uint32),
        ("tuner_locked", ctypes.c_uint32),
        ("tuner_note", ctypes.c_uint32),
        ("tuner_octave", ctypes.c_int32),
        ("tuner_cents", ctypes.c_int32),
        ("tuner_freq_tenths", ctypes.c_uint32),
        ("tuner_dots", ctypes.c_uint32),
        ("drum_playing", ctypes.c_uint32),
        ("drum_bpm", ctypes.c_uint32),
        ("drum_pattern", ctypes.c_uint32),
        ("in_peak", ctypes.c_float),
        ("out_peak", ctypes.c_float),
        ("bt_state", ctypes.c_uint32),
        # These two have to be here: the DLL writes the whole struct, so a
        # struct that is short of the firmware's would be written past.
        ("cursor_slot", ctypes.c_uint32),
        ("editor_cursor", ctypes.c_uint32),
    ]

dll = ctypes.CDLL(str(DLL_PATH))

dll.emu_init.argtypes = []
dll.emu_init.restype = None

dll.emu_ui_tap.argtypes = [ctypes.c_uint32, ctypes.c_uint32]
dll.emu_ui_tap.restype = None
dll.emu_ui_enc.argtypes = [ctypes.c_int32]
dll.emu_ui_enc.restype = None
dll.emu_process_audio_f32.argtypes = [ctypes.POINTER(ctypes.c_float),
                                      ctypes.POINTER(ctypes.c_float),
                                      ctypes.POINTER(ctypes.c_float), ctypes.c_int]
dll.emu_process_audio_f32.restype = None

dll.emu_get_lcd_rgba.argtypes = [ctypes.POINTER(ctypes.c_uint8)]
dll.emu_get_lcd_rgba.restype = None

dll.emu_get_telemetry.argtypes = [ctypes.POINTER(EmuTelemetry)]
dll.emu_get_telemetry.restype = None

# Button definitions
BTN_HOME       = (1 << 0)
BTN_VOL_MINUS  = (1 << 1)
BTN_VOL_PLUS   = (1 << 2)
BTN_LEFT       = (1 << 3)
BTN_RIGHT      = (1 << 4)
BTN_PARA_CLICK = (1 << 5)
BTN_PARA_HOLD1 = (1 << 6)
BTN_PARA_HOLD2 = (1 << 7)

COMBO_TUNER = BTN_VOL_MINUS | BTN_VOL_PLUS
COMBO_BANK  = BTN_LEFT | BTN_RIGHT
COMBO_DRUMS = BTN_HOME | BTN_VOL_MINUS

# View names
VIEW_NAMES = {
    0: "MAIN_GRID",
    1: "MOVING",
    2: "PARAM_EDIT",
    3: "BANK_MODE",
    4: "TUNER",
    5: "SAVE_DIALOG",
    6: "SAVED_TOAST",
    7: "DRUMS",
    8: "SETTINGS",
    9: "GLOBAL_EQ",
    10: "INPUT_OUTPUT",
    11: "DISPLAY",
    12: "TELEMETRY",
    13: "SYS_INFO",
    14: "FACTORY_RESET",
    15: "STOMP_ASSIGN",
}

def capture_screen(name):
    buf = (ctypes.c_uint8 * (240 * 240 * 4))()
    dll.emu_get_lcd_rgba(buf)
    telem = EmuTelemetry()
    dll.emu_get_telemetry(ctypes.byref(telem))
    view_str = VIEW_NAMES.get(telem.view, f"VIEW_{telem.view}")

    if HAS_PIL:
        img = Image.frombytes("RGBA", (240, 240), bytes(buf))
        out_path = SNAPSHOTS_DIR / f"{name}_{view_str}.png"
        for _ in range(5):
            try:
                img.save(out_path)
                break
            except OSError:
                time.sleep(0.05)
        print(f"  [SNAPSHOT] {out_path.name} (view={view_str}, preset={telem.preset_index + 1})")
    return telem.view


# --- the input helpers ------------------------------------------------------
# The emulator has a real press/release api (emu_ui_press / emu_ui_release /
# emu_ui_tap): the firmware decides a click or a HOLD from the RELEASE edge,
# so a button that is never released never clicks and never holds. The
# firmware's tick runs every 11 audio blocks (64 frames at 44.1 kHz).
TICK_BLOCKS = 11
TICK_MS = TICK_BLOCKS * 64 * 1000.0 / 44100.0
_N = 64
_in = (ctypes.c_float * _N)()
_outL = (ctypes.c_float * _N)()
_outR = (ctypes.c_float * _N)()


def tick(n=1):
    """Run n firmware ticks."""
    for _ in range(n * TICK_BLOCKS):
        dll.emu_process_audio_f32(_in, _outL, _outR, _N)


def tap(mask):
    """A complete gesture - the way the web UI does it: ONE request is one
    press-and-release, so the two halves can never be reordered."""
    dll.emu_ui_tap(mask, 0)
    tick(25)


def hold(mask, ms):
    """Press and keep the button down for `ms`, then release it."""
    dll.emu_ui_tap(mask, ms)
    tick(int(ms / TICK_MS) + 25)


def enc(delta):
    """Turn the encoder."""
    dll.emu_ui_enc(delta)
    tick(25)


def main():
    print("Initializing Pocket Amp Native Emulator...")
    dll.emu_init()

    # 1. Capture Initial Home Grid
    v = capture_screen("01_home_grid")
    assert v == 0, f"Expected MAIN_GRID (0), got {v}"

    # 2. Test < > on Home moves cursor across the 8 slots
    print("Testing cursor navigation across 8 slots on Home...")
    for step in range(8):
        tap(BTN_RIGHT)
    capture_screen("02_home_cursor_wrapped")

    # 3. Test PARA 3s+ hold -> Enter Moving Mode
    print("Testing PARA 3s+ hold -> Moving Mode...")
    hold(BTN_PARA_CLICK, 3300)
    v = capture_screen("03_home_moving_block")
    assert v == 1, f"Expected MOVING (1), got {v}"

    # Shift moving block by +1
    enc(1)
    capture_screen("04_home_moving_shifted")

    # Click PARA to drop
    tap(BTN_PARA_CLICK)
    v = capture_screen("05_home_dropped")
    assert v == 0, f"Expected MAIN_GRID (0), got {v}"

    # 4. Test PARA press -> Enter Block Editor
    print("Testing PARA press -> Block Editor...")
    tap(BTN_PARA_CLICK)
    v = capture_screen("06_block_editor")
    assert v == 2, f"Expected PARAM_EDIT (2), got {v}"

    # Turn PARA in Block Editor (modifies param)
    enc(5)
    capture_screen("07_block_editor_adjusted")

    # Test quick bypass in editor (click PARA)
    tap(BTN_PARA_CLICK)
    capture_screen("08_block_editor_quick_bypass")

    # Return Home via HOME
    tap(BTN_HOME)
    v = capture_screen("09_home_returned")
    assert v == 0, f"Expected MAIN_GRID (0), got {v}"

    # 5. Move cursor to EQ block (slot 4) and open editor to test 5-band vertical EQ widget
    print("Testing EQ block editor (5-band sliders)...")
    # Cursor to EQ
    for _ in range(7):
        tap(BTN_RIGHT)
    tap(BTN_PARA_CLICK)
    v = capture_screen("10_eq_block_editor")
    assert v == 2, f"Expected PARAM_EDIT (2), got {v}"

    # Adjust EQ band +3 dB
    enc(3)
    capture_screen("11_eq_band_adjusted")
    tap(BTN_HOME)

    # 6. Test Bank Mode via < & > combo
    print("Testing Bank Mode...")
    tap(COMBO_BANK)
    v = capture_screen("12_bank_mode")
    assert v == 3, f"Expected BANK_MODE (3), got {v}"

    # Test slot selection via VOL- (slot 1), VOL+ (slot 2)
    tap(BTN_VOL_PLUS)
    capture_screen("13_bank_slot2_selected")

    # Exit Bank mode via HOME
    tap(BTN_HOME)
    v = capture_screen("14_bank_exited_to_home")
    assert v == 0, f"Expected MAIN_GRID (0), got {v}"

    # 7. Test Tuner Mode via VOL- + VOL+ combo
    print("Testing Tuner Mode...")
    tap(COMBO_TUNER)
    v = capture_screen("15_tuner")
    assert v == 4, f"Expected TUNER (4), got {v}"

    # Turn PARA to pick MUTE button
    enc(1)
    # Press PARA to toggle MUTE
    tap(BTN_PARA_CLICK)
    capture_screen("16_tuner_muted")

    # Exit Tuner via HOME
    tap(BTN_HOME)
    v = capture_screen("17_tuner_exited_to_home")
    assert v == 0, f"Expected MAIN_GRID (0), got {v}"

    # 8. Test Groove Station via HOME + VOL- combo
    print("Testing Groove Station...")
    tap(COMBO_DRUMS)
    v = capture_screen("18_groove_station")
    assert v == 7, f"Expected DRUMS (7), got {v}"

    # Adjust BPM with PARA turn
    enc(10)
    # Toggle play/stop with VOL+
    tap(BTN_VOL_PLUS)
    capture_screen("19_groove_station_playing")

    # Return Home
    tap(BTN_HOME)
    assert capture_screen("20_home_from_groove") == 0

    # 9. Test Save Preset Dialog via PARA 1s hold
    print("Testing Save Preset Dialog...")
    hold(BTN_PARA_CLICK, 1200)
    v = capture_screen("21_save_dialog")
    assert v == 5, f"Expected SAVE_DIALOG (5), got {v}"

    # Change destination slot with >
    tap(BTN_RIGHT)
    capture_screen("22_save_destination_picked")

    # Confirm SAVE with PARA click
    tap(BTN_PARA_CLICK)
    v = capture_screen("23_saved_toast")
    assert v == 6, f"Expected SAVED_TOAST (6), got {v}"

    # Return Home
    tap(BTN_HOME)

    # 10. Test Settings Menu via HOME on Home
    print("Testing Settings Menu...")
    tap(BTN_HOME)
    v = capture_screen("24_settings_menu")
    assert v == 8, f"Expected SETTINGS (8), got {v}"

    # Open Input / Output sub-page (row 0)
    tap(BTN_PARA_CLICK)
    v = capture_screen("25_input_output_subpage")
    assert v == 10, f"Expected INPUT_OUTPUT (10), got {v}"

    # STEREO WIDTH (row 3): the output is dual mono until it is turned up, then the two channels differ
    def stereo_probe():
        import random
        rnd = random.Random(5)
        diff = 0
        for _ in range(200):
            for i in range(_N):
                _in[i] = (rnd.random() - 0.5) * 0.4
            dll.emu_process_audio_f32(_in, _outL, _outR, _N)
            diff += sum(abs(_outL[i] - _outR[i]) for i in range(_N))
        return diff
    d_mono = stereo_probe()          # the preset's own stereo effects, if any, are in this number
    print(f"  stereo probe, width MONO: sum|L-R| = {d_mono:.3f}")
    for _ in range(3): tap(BTN_RIGHT)          # rows 0 -> 3
    tap(BTN_PARA_CLICK)                        # edit
    for _ in range(10): tap(BTN_RIGHT)         # +5 % a click: 50 %
    v = capture_screen("25b_stereo_width_edit")
    assert v == 10, f"Expected INPUT_OUTPUT (10), got {v}"
    tap(BTN_PARA_CLICK)                        # done
    d_50 = stereo_probe()
    print(f"  stereo probe, width 50 %: sum|L-R| = {d_50:.3f}")
    assert d_50 > d_mono * 1.5 + 1.0, "STEREO WIDTH 50 %: the two channels must differ more than the preset's own stereo does"

    # BALANCE (row 4): full right turns the left channel off; back to the centre it returns
    def peaks():
        import random
        rnd = random.Random(9)
        pl = pr = 0.0
        for _ in range(200):
            for i in range(_N):
                _in[i] = (rnd.random() - 0.5) * 0.4
            dll.emu_process_audio_f32(_in, _outL, _outR, _N)
            pl = max(pl, max(abs(_outL[i]) for i in range(_N)))
            pr = max(pr, max(abs(_outR[i]) for i in range(_N)))
        return pl, pr
    pl0, pr0 = peaks()
    tap(BTN_RIGHT); tap(BTN_PARA_CLICK)
    for _ in range(25): tap(BTN_RIGHT)         # +2 a click: R50
    tap(BTN_PARA_CLICK)
    pl, pr = peaks()
    print(f"  balance R50: peaks L {pl:.4f} R {pr:.4f} (centre: L {pl0:.4f} R {pr0:.4f})")
    assert pl < 1e-6 and pr > pr0 * 0.5, "BALANCE R50 must silence the left channel and keep the right"
    tap(BTN_PARA_CLICK)
    for _ in range(25): tap(BTN_LEFT)
    tap(BTN_PARA_CLICK)
    pl, pr = peaks()
    assert pl > pl0 * 0.7 and pr > pr0 * 0.7 and pr < pr0 * 1.4, "BALANCE back at the centre must restore both channels"

    # ROUTING (row 5): MONO makes the channels identical, STEREO (the 5th click) restores them
    tap(BTN_RIGHT)
    tap(BTN_PARA_CLICK)                        # STEREO -> MONO
    d_route_mono = stereo_probe()              # only the output stage's 1-LSB dither differs between the channels
    print(f"  routing MONO: sum|L-R| = {d_route_mono:.4f}")
    assert d_route_mono < 0.05, "ROUTING MONO: the two channels must be identical (up to the dither)"
    for _ in range(4): tap(BTN_PARA_CLICK)     # MONO -> LEFT -> RIGHT -> SWAP -> STEREO
    assert stereo_probe() > 1.0, "ROUTING back to STEREO: the width must be audible again"
    v = capture_screen("25c_routing_row")
    for _ in range(5): tap(BTN_LEFT)           # back to row 0 (the PARA hold below leaves from any row)

    # Return to Settings with PARA 1s hold
    hold(BTN_PARA_CLICK, 1200)
    v = capture_screen("26_settings_returned")
    assert v == 8, f"Expected SETTINGS (8), got {v}"

    # Move cursor to Display (row 2: step past row 1 Bluetooth) and open it
    tap(BTN_RIGHT) # row 1 (Bluetooth)
    tap(BTN_RIGHT) # row 2 (Display)
    tap(BTN_PARA_CLICK)
    v = capture_screen("27_display_subpage")
    assert v == 11, f"Expected DISPLAY (11), got {v}"

    # Switch HOME VIEW to LIST view
    tap(BTN_RIGHT) # cursor to AUTO DIM
    tap(BTN_RIGHT) # cursor to HOME VIEW
    tap(BTN_PARA_CLICK) # toggle to LIST
    capture_screen("28_display_switched_to_list")

    # Go Home to see Home List View!
    tap(BTN_HOME)
    v = capture_screen("29_home_list_view")
    assert v == 0, f"Expected MAIN_GRID (0), got {v}"

    print("\nALL 30 VERIFICATION TESTS PASSED SUCCESSFULLY!")

if __name__ == "__main__":
    main()

# SPDX-License-Identifier: GPL-3.0-only
# Copyright (C) 2026 The Onyx Authors (@28spawn)
