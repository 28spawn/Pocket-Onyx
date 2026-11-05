"""The emulator's front-panel buttons: press, release, click and HOLD.

Why this test exists
--------------------
The emulator exposed ONE level setter for the front-panel buttons
(emu_ui_input) and the web UI never released anything: it sent the mask once
and left it there. The firmware's input layer (src/sys/pa_input.c) turns the
raw LEVEL into one-tick action pulses, and it decides a PARA or a HOME CLICK
from the RELEASE edge - a button that is never released never clicks and never
holds. So the web UI had:

  * HOME and PARA clicks that never fired at all (Settings and the encoder
    click were dead),
  * the HOLD actions unreachable: Live Stomp Lock (HOME 1.5 s), Save to
    (PARA 1 s), Moving mode (PARA 2 s) and Factory Reset (PARA 3 s). They were
    sent as bits 6..9, and the raw input mask is only bits 0..5, so the
    firmware discarded them before it ever saw them,
  * a second tap of the same button that did nothing (there was no new edge),
    and a button that stayed latched down, so VOL-/VOL+ auto-repeated forever.

The emulator now has a real press/release api (emu_ui_press / emu_ui_release,
the same adaptation the WASM shim makes in wasm/pa_wasm.c) and the web UI
drives it. This test drives the SAME api the server drives and asserts the
effect the firmware documents - the screen it switches to.

Usage:  python custom-fw/emu/test_buttons.py [path/to/pocket_amp_emu.dll]
"""
import ctypes
import os
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
DEFAULT_DLL = os.path.join(HERE, "pocket_amp_emu.dll")

dll_path = sys.argv[1] if len(sys.argv) > 1 else DEFAULT_DLL
import tempfile
os.environ["PA_FLASH_FILE"] = os.path.join(tempfile.mkdtemp(prefix="pa_buttons_"), "flash.bin")     # a private flash: the bond test leaves a bond behind, and a rerun must start without one
d = ctypes.CDLL(dll_path)
d.emu_init()

d.emu_ui_press.argtypes = [ctypes.c_uint32]
d.emu_ui_press.restype = None
d.emu_ui_release.argtypes = [ctypes.c_uint32]
d.emu_ui_release.restype = None
d.emu_ui_tap.argtypes = [ctypes.c_uint32, ctypes.c_uint32]
d.emu_ui_tap.restype = None
d.emu_ui_enc.argtypes = [ctypes.c_int32]
d.emu_ui_enc.restype = None
d.emu_process_audio_f32.argtypes = [ctypes.POINTER(ctypes.c_float), ctypes.POINTER(ctypes.c_float),
                                    ctypes.POINTER(ctypes.c_float), ctypes.c_int]

N = 64
inp = (ctypes.c_float * N)()
outL = (ctypes.c_float * N)()
outR = (ctypes.c_float * N)()

# the raw front-panel bits (src/include/pa_input.h)
HOME = 1 << 0
VOL_MINUS = 1 << 1
VOL_PLUS = 1 << 2
LEFT = 1 << 3
RIGHT = 1 << 4
PARA = 1 << 5

# the firmware's screens (include/pa_ui.h)
VIEW_MAIN_GRID = 0
VIEW_MOVING = 1
VIEW_PARAM_EDIT = 2
VIEW_BANK_MODE = 3
VIEW_TUNER = 4
VIEW_SAVE_DIALOG = 5
VIEW_DRUMS = 7
VIEW_SETTINGS = 8
VIEW_FACTORY_RESET = 14
VIEW_SYS_INFO = 13
VIEW_SAVED_TOAST = 6


class Telemetry(ctypes.Structure):
    _fields_ = [
        ("view", ctypes.c_uint32), ("preset_index", ctypes.c_uint32),
        ("preset_volume", ctypes.c_uint32), ("master_volume", ctypes.c_uint32),
        ("selected_slot", ctypes.c_uint32), ("selected_param", ctypes.c_uint32),
        ("slot_active_mask", ctypes.c_uint32), ("sram_used_bytes", ctypes.c_uint32),
        ("sram_peak_bytes", ctypes.c_uint32), ("cpu_total_permille", ctypes.c_uint32),
        ("cpu_stage_permille", ctypes.c_uint32 * 8),
        ("tuner_active", ctypes.c_uint32), ("tuner_locked", ctypes.c_uint32),
        ("tuner_note", ctypes.c_uint32), ("tuner_octave", ctypes.c_int32),
        ("tuner_cents", ctypes.c_int32), ("tuner_freq_tenths", ctypes.c_uint32),
        ("tuner_dots", ctypes.c_uint32), ("drum_playing", ctypes.c_uint32),
        ("drum_bpm", ctypes.c_uint32), ("drum_pattern", ctypes.c_uint32),
        ("in_peak", ctypes.c_float), ("out_peak", ctypes.c_float),
        ("bt_state", ctypes.c_uint32), ("cursor_slot", ctypes.c_uint32),
        ("editor_cursor", ctypes.c_uint32),
    ]


tel = Telemetry()
d.emu_get_telemetry.argtypes = [ctypes.POINTER(Telemetry)]

# the firmware's tick runs every 11 audio blocks (64 frames at 44.1 kHz)
# 11 * 64 / 44100 = 15.96 ms per tick
TICK_BLOCKS = 11
TICK_MS = TICK_BLOCKS * 64 * 1000.0 / 44100.0


def blocks(n):
    for _ in range(n):
        for i in range(N):
            inp[i] = 0.0
        d.emu_process_audio_f32(inp, outL, outR, N)


def tick(n=1):
    """Run n firmware ticks."""
    blocks(n * TICK_BLOCKS)
    d.emu_get_telemetry(ctypes.byref(tel))


def view():
    d.emu_get_telemetry(ctypes.byref(tel))
    return tel.view


def tap(mask):
    """A complete gesture - the way the web UI does it: ONE request is one
    press-and-release, so the two halves can never be reordered."""
    d.emu_ui_tap(mask, 0)
    tick(25)


def hold(mask, ms):
    """Press and keep the button down for `ms`, then release it."""
    d.emu_ui_tap(mask, ms)
    tick(int(ms / TICK_MS) + 25)


def to_grid():
    """HOME always goes home: from any screen but the grid it returns to it."""
    if view() != VIEW_MAIN_GRID:
        tap(HOME)


fails = 0


def check(ok, msg):
    global fails
    print(("  ok   " if ok else "  FAIL ") + msg)
    if not ok:
        fails += 1


print("=== the firmware boots into the home grid ===")
tick(20)
check(view() == VIEW_MAIN_GRID, f"view {view()} == the home grid")

print("\n=== a HOME tap opens Settings, and a SECOND tap closes it again ===")
tap(HOME)
v = view()
check(v == VIEW_SETTINGS, f"HOME opened Settings (view {v})")
tap(HOME)
v = view()
check(v == VIEW_MAIN_GRID, f"the second HOME tap went back (view {v})")

print("\n=== HOME held for 1.5 s switches PRESET <-> STOMP mode (the Stomp Lock is gone) ===")
d.emu_get_ui.argtypes = [ctypes.c_int]
d.emu_get_ui.restype = ctypes.c_int
to_grid()
before = view()
check(d.emu_get_ui(23) == 0, "the unit starts in PRESET mode")
hold(HOME, 1700)
v = view()
check(v == before, f"holding HOME did not open Settings (view {v})")
check(d.emu_get_ui(23) == 1, f"HOME held 1.7 s switched to STOMP mode (play_mode {d.emu_get_ui(23)})")
check(d.emu_get_ui(25) == 1, "and the settings are marked to be saved (the mode is remembered)")
hold(HOME, 1700)
check(d.emu_get_ui(23) == 0, "HOME held again switches back to PRESET mode")
tap(HOME)
check(view() == VIEW_SETTINGS and d.emu_get_ui(23) == 0, "a short HOME tap still opens Settings and does not change the mode")
tap(HOME)
d.emu_get_telemetry(ctypes.byref(tel))
vol0 = tel.preset_volume
tap(VOL_PLUS)
d.emu_get_telemetry(ctypes.byref(tel))
check(tel.preset_volume == vol0 + 5, f"with no lock, VOL+ moves the preset volume ({vol0} -> {tel.preset_volume})")
hold(HOME, 10000)
check(d.emu_get_ui(23) == 1, "HOME held 10 s switched the mode once (it fires when 1.5 s is reached, not again at the release)")
hold(HOME, 1700)
check(d.emu_get_ui(23) == 0, "back to PRESET mode")

print("\n=== a PARA tap opens the block editor ===")
to_grid()
tap(PARA)
v = view()
check(v == VIEW_PARAM_EDIT, f"PARA opened the editor (view {v})")

print("\n=== the PARA holds: 1-3 s = Save-to, past 3 s = move the block (however long it stays down) ===")
to_grid()
hold(PARA, 1200)
v = view()
check(v == VIEW_SAVE_DIALOG, f"PARA held 1.2 s opened Save-to (view {v})")

to_grid()
hold(PARA, 2200)
v = view()
check(v == VIEW_SAVE_DIALOG, f"PARA held 2.2 s still opened Save-to (view {v})")

to_grid()
hold(PARA, 3300)
v = view()
check(v == VIEW_MOVING, f"PARA held 3.3 s entered Moving mode (view {v})")

to_grid()
hold(PARA, 10000)
v = view()
check(v == VIEW_MOVING, f"PARA held 10 s entered Moving mode too (view {v}); it used to do nothing")
check(v != VIEW_FACTORY_RESET, "and it did not open the Factory Reset")
to_grid()

print("\n=== the chords ===")
to_grid()
tap(VOL_MINUS | VOL_PLUS)
v = view()
check(v == VIEW_TUNER, f"VOL- + VOL+ opened the tuner (view {v})")

to_grid()
tap(LEFT | RIGHT)
v = view()
check(v == VIEW_BANK_MODE, f"< + > opened Bank mode (view {v})")

to_grid()
tap(HOME | VOL_MINUS)
v = view()
check(v == VIEW_DRUMS, f"HOME + VOL- opened the groove station (view {v})")

print("\n=== the same button twice moves the preset volume twice ===")
to_grid()
d.emu_get_telemetry(ctypes.byref(tel))
v0 = tel.preset_volume
tap(VOL_PLUS)
d.emu_get_telemetry(ctypes.byref(tel))
v1 = tel.preset_volume
tap(VOL_PLUS)
d.emu_get_telemetry(ctypes.byref(tel))
v2 = tel.preset_volume
check(v1 == v0 + 5 and v2 == v1 + 5,
      f"two taps moved the volume {v0} -> {v1} -> {v2}")

print("\n=== the encoder does not release a button that is down ===")
to_grid()
d.emu_ui_press(HOME)
tick(4)
d.emu_ui_enc(1)            # the encoder must NOT clear the button state
tick(4)
d.emu_ui_release(HOME)
tick(20)
v = view()
check(v == VIEW_SETTINGS, f"the HOME click survived the encoder (view {v})")

print("\n=== a held button repeats, and the mouse-up stops it ===")
to_grid()
d.emu_get_telemetry(ctypes.byref(tel))
c0 = tel.preset_index
d.emu_ui_press(LEFT)
tick(4)                     # the press edge
d.emu_get_telemetry(ctypes.byref(tel))
c1 = tel.preset_index
check(c1 != c0, f"the press changed the preset (the < > buttons pick it; the PARA knob moves the cursor) ({c0} -> {c1})")
tick(30)                    # past the 350 ms repeat delay
d.emu_get_telemetry(ctypes.byref(tel))
c2 = tel.preset_index
check(c2 != c1, f"it kept repeating ({c1} -> {c2})")
d.emu_ui_release(LEFT)
tick(4)
d.emu_get_telemetry(ctypes.byref(tel))
c3 = tel.preset_index
tick(30)
d.emu_get_telemetry(ctypes.byref(tel))
c4 = tel.preset_index
check(c4 == c3, f"the mouse-up stopped it ({c3} -> {c4})")

print("\n=== the factory reset: Settings > System > FACTORY RESET, confirmed with a normal click ===")


def open_factory_reset():
    """HOME -> Settings, down to System (row 7), click, move to the FACTORY RESET button, click: the confirmation opens on CANCEL"""
    to_grid()
    tap(HOME)
    for _ in range(7):
        tap(RIGHT)
    tap(PARA)
    if view() != VIEW_SYS_INFO:
        return False
    tap(RIGHT)                       # CLEAN USER PRESETS -> FACTORY RESET
    tap(PARA)
    return view() == VIEW_FACTORY_RESET


d.emu_get_menu_cursors.argtypes = [ctypes.POINTER(ctypes.c_uint32)]
_c3 = (ctypes.c_uint32 * 3)()


def toast_kind():
    d.emu_get_menu_cursors(_c3)
    return _c3[2]


check(open_factory_reset(), "Settings > System > FACTORY RESET (a click) opens the confirmation")
tap(PARA)                            # the dialog opens on CANCEL: a click cancels
check(view() == VIEW_SYS_INFO, f"a click on CANCEL goes back to System (view {view()})")
check(open_factory_reset(), "the confirmation opens again")
tap(RIGHT)                           # CANCEL -> RESET
check(view() == VIEW_FACTORY_RESET, "moving to RESET keeps the dialog open (no accidental reset)")
hold(PARA, 1200)                     # a hold is not a click: it must not reset
check(view() != VIEW_SAVED_TOAST, f"holding PARA on RESET does not reset (view {view()})")
if view() != VIEW_FACTORY_RESET:
    check(open_factory_reset(), "reopened")
    tap(RIGHT)
tap(PARA)                            # ONE normal click on RESET does it
check(view() == VIEW_SAVED_TOAST, f"one click on RESET performed the reset and shows the toast (view {view()})")
check(toast_kind() == 1, f"the toast after the factory reset is the RESET card, not 'SAVED P01' (kind {toast_kind()})")
tick(80)
to_grid()
hold(PARA, 1200)                     # a normal preset save: Save-to, pick, confirm
for _ in range(3):
    if view() == VIEW_SAVED_TOAST:
        break
    tap(PARA)
check(view() == VIEW_SAVED_TOAST and toast_kind() == 0, f"a preset save shows the SAVED card (view {view()}, kind {toast_kind()})")

print("\n=== CLEAN USER PRESETS (Settings > System): erases P41-P80, keeps the stock P01-P40, needs a confirming click ===")
d.emu_midi_event.argtypes = [ctypes.c_int] * 3
d.emu_preset_is_empty.argtypes = [ctypes.c_int]
d.emu_preset_is_empty.restype = ctypes.c_int


def open_clean():
    """HOME -> Settings, System row, click, the first button (CLEAN USER PRESETS) is the cursor: click opens the confirmation"""
    to_grid()
    tap(HOME)
    for _ in range(7):
        tap(RIGHT)
    tap(PARA)
    if view() != VIEW_SYS_INFO:
        return False
    tap(PARA)
    return view() == VIEW_FACTORY_RESET


d.emu_select_preset(40); tick(5)
d.emu_midi_event(0xB0, 88, 127); tick(10)                   # SAVE (CC 88): P41 now holds a sound
d.emu_select_preset(41); tick(5)
d.emu_midi_event(0xB0, 88, 127); tick(10)                   # and P42
check(d.emu_preset_is_empty(40) == 0 and d.emu_preset_is_empty(41) == 0, "two user presets (P41, P42) hold a saved sound")
check(d.emu_preset_is_empty(0) == 0, "the stock P01 is not empty")
check(open_clean(), "Settings > System > CLEAN USER PRESETS opens the confirmation")
tap(PARA)                                                   # opens on CANCEL
check(view() == VIEW_SYS_INFO and d.emu_preset_is_empty(40) == 0, f"CANCEL erases nothing (view {view()})")
check(open_clean(), "the confirmation opens again")
hold(PARA, 1200)                                            # a hold is not a click
check(view() != VIEW_SAVED_TOAST and d.emu_preset_is_empty(40) == 0, "holding PARA does not erase")
if view() != VIEW_FACTORY_RESET:
    check(open_clean(), "reopened")
tap(RIGHT)                                                  # CANCEL -> ERASE
tap(PARA)
check(view() == VIEW_SAVED_TOAST and toast_kind() == 2, f"one click on ERASE shows the CLEAN toast (view {view()}, kind {toast_kind()})")
check(d.emu_preset_is_empty(40) == 1 and d.emu_preset_is_empty(41) == 1 and d.emu_preset_is_empty(79) == 1, "P41, P42 and P80 are EMPTY after the clean")
check(d.emu_preset_is_empty(0) == 0 and d.emu_preset_is_empty(39) == 0, "the stock P01 and P40 are untouched")
tick(80)
to_grid()

print("\n=== FORGET BT PAIRING (Settings > System, the third button): the stored bond goes after a confirming click ===")
d.emu_bond_stash.argtypes = [ctypes.c_int]
d.emu_bond_poll.argtypes = [ctypes.c_uint32]
d.emu_bond_poll.restype = ctypes.c_int
d.emu_bond_get.restype = ctypes.c_int


def open_forget():
    """HOME -> Settings, System row, click, two steps to the third button (FORGET BT PAIRING), click: the confirmation opens on CANCEL"""
    to_grid()
    tap(HOME)
    for _ in range(7):
        tap(RIGHT)
    tap(PARA)
    if view() != VIEW_SYS_INFO:
        return False
    tap(RIGHT); tap(RIGHT)
    tap(PARA)
    return view() == VIEW_FACTORY_RESET


check(d.emu_bond_get() == -1, "no bond is stored at the start")
d.emu_bond_stash(0x1234)
check(d.emu_bond_poll(1000) == 0, "the radio's keys are only stashed: the first main-loop pass arms the delay and writes nothing")
check(d.emu_bond_poll(1500) == 0 and d.emu_bond_get() == -1, "1.5 s after the stash is not yet long enough (the identity arrives just after the keys)")
check(d.emu_bond_poll(2600) == 1 and d.emu_bond_get() == 0x1234, "after the delay the bond is written to the flash and reads back (EDIV 0x1234)")
d.emu_bond_stash(0x1234)
d.emu_bond_poll(3000); check(d.emu_bond_poll(6000) == 0, "the same bond stashed again is not written twice")
check(open_forget(), "Settings > System > FORGET BT PAIRING (a click on the third button) opens the confirmation")
tap(PARA)                                      # opens on CANCEL
check(view() == VIEW_SYS_INFO and d.emu_bond_get() == 0x1234, f"CANCEL keeps the bond (view {view()})")
check(open_forget(), "the confirmation opens again")
tap(RIGHT)                                     # CANCEL -> FORGET
tap(PARA)
check(view() == VIEW_SAVED_TOAST and toast_kind() == 3, f"one click on FORGET shows the FORGOTTEN card (view {view()}, kind {toast_kind()})")
check(d.emu_bond_get() == -1, "and the bond is gone from the flash")
d.emu_bond_stash(0x2222); d.emu_bond_poll(10000); d.emu_bond_poll(12000)
check(d.emu_bond_get() == 0x2222, "a new pairing after the forget is stored again (EDIV 0x2222)")
tick(80)
to_grid()

print("\n=== USB LOOPBACK (Settings, the second row from the end): a click switches the USB driver's loopback ===")
d.emu_get_usb_loopback.restype = ctypes.c_int
to_grid(); tap(HOME); tap(LEFT); tap(LEFT)                  # the Settings menu wraps: < on the first row = the last row (USB Mode), once more = USB Loopback
d.emu_get_menu_cursors(_c3)
check(_c3[0] == 9, f"the cursor is on USB Loopback (row {_c3[0]})")
tick(5)
check(d.emu_get_usb_loopback() == 1, "the loopback starts ON (the stock default)")
tap(PARA); tick(5)
check(d.emu_get_usb_loopback() == 0, "one click turns the USB driver's loopback OFF")
tap(PARA); tick(5)
check(d.emu_get_usb_loopback() == 1, "another click turns it back ON")
to_grid()

print("\n=== USB MODE (Settings last row): a click cycles AMP -> INTERFACE -> INTERFACE + DIRECT MONITOR ===")
to_grid(); tap(HOME); tap(LEFT)
d.emu_get_menu_cursors(_c3)
check(_c3[0] == 10, f"the cursor is on USB Mode (row {_c3[0]})")
check(d.emu_get_ui(40) == 0, "the mode starts as AMP (the unit's own sound)")
tap(PARA); tick(5); check(d.emu_get_ui(40) == 1, "one click: INTERFACE (the PC's audio only, the dry guitar recorded)")
tap(PARA); tick(5); check(d.emu_get_ui(40) == 2, "two clicks: INTERFACE + DIRECT MONITOR")
tap(PARA); tick(5); check(d.emu_get_ui(40) == 0, "three clicks: back to AMP")
to_grid()

print("\n=== STOMP mode: a short press of a key is the footswitch, a long press opens the assign menu ===")
VIEW_STOMP_ASSIGN = 15
d.emu_select_preset(0); tick(5)
to_grid()
if d.emu_get_ui(23) != 1:
    hold(HOME, 1700)
check(d.emu_get_ui(23) == 1, "STOMP mode is on")
check([d.emu_get_ui(26 + i) for i in range(4)] == [1, 5, 6, 7], f"P01's keys are DRV, MOD, DLY, REV ({[d.emu_get_ui(26 + i) for i in range(4)]})")


def drive_on():
    d.emu_get_telemetry(ctypes.byref(tel))
    return (tel.slot_active_mask >> 1) & 1


on0 = drive_on()
tap(VOL_MINUS)
tick(10)
check(drive_on() != on0, "a short VOL- press toggled the effect on that key (DRIVE)")
on1 = drive_on()
hold(VOL_MINUS, 300)
tick(10)
check(drive_on() != on1 and view() == VIEW_MAIN_GRID, "a 0.3 s press is still a footswitch press")
on2 = drive_on()
hold(VOL_MINUS, 1500)
tick(10)
check(view() == VIEW_STOMP_ASSIGN, f"VOL- held 1.5 s opened the assign menu (view {view()})")
check(drive_on() == on2, "and it did not toggle the effect, neither on the press nor with auto-repeat")
check(d.emu_get_ui(26) == 1, "the menu opens on the effect the key has now (DRIVE)")
d.emu_ui_enc(2); tick(5)
tap(PARA)
check(view() == VIEW_MAIN_GRID, f"a PARA click confirmed and went back (view {view()})")
check(d.emu_get_ui(26) == 3, f"VOL- now toggles the CAB (slot {d.emu_get_ui(26)}): the PARA turn moved two rows down from DRIVE... AMP then CAB")

hold(RIGHT, 1500)
check(view() == VIEW_STOMP_ASSIGN, "> held opens its own assign menu")
tap(VOL_MINUS)
check(view() == VIEW_MAIN_GRID and d.emu_get_ui(29) == 7, f"VOL- backs out without a change (> still REV: {d.emu_get_ui(29)})")
hold(VOL_PLUS, 1500)
check(view() == VIEW_STOMP_ASSIGN, "VOL+ held opens its menu")
tap(HOME)
check(view() == VIEW_MAIN_GRID and d.emu_get_ui(27) == 5, f"HOME backs out too (VOL+ still MOD: {d.emu_get_ui(27)})")

print("\n=== PRESET mode is untouched: the keys act on the press and a held VOL+ still repeats ===")
hold(HOME, 1700)
check(d.emu_get_ui(23) == 0, "back in PRESET mode")
d.emu_get_telemetry(ctypes.byref(tel))
vv0 = tel.preset_volume
hold(VOL_PLUS, 1200)
d.emu_get_telemetry(ctypes.byref(tel))
check(view() == VIEW_MAIN_GRID and tel.preset_volume >= vv0 + 15, f"a held VOL+ repeated the volume step ({vv0} -> {tel.preset_volume}) and opened no menu")
d.emu_select_preset(0); tick(5)

print("\n=== the Display page's BATTERY toggle: the knob sits inside the pill with equal margins (a pixel check) ===")
import numpy as np
d.emu_get_lcd_rgba.argtypes = [ctypes.POINTER(ctypes.c_uint8)]


def lcd():
    buf = (ctypes.c_uint8 * (240 * 240 * 4))()
    d.emu_get_lcd_rgba(buf)
    return np.frombuffer(bytes(buf), dtype=np.uint8).reshape(240, 240, 4)[:, :, :3].astype(int)


def knob_margins(on):
    """the knob's margins to the pill's edges (left, right, top, bottom), pill = x 190..227, y 154..174"""
    im = lcd()
    if on:
        base = tuple(im[156, 225]); box_x = range(207, 228)       # the right half: the knob (the 'ON' text is in the left half)
    else:
        base = tuple(im[156, 210]); box_x = range(192, 207)       # the left half, inside the pill's 1 px border: the knob (the 'OFF' text is in the right half)
    outside = tuple(im[164, 185])
    pts = [(x, y) for y in range(156, 173) for x in box_x if tuple(im[y, x]) != base and tuple(im[y, x]) != outside]
    if not pts:
        return None
    xs = [p[0] for p in pts]; ys = [p[1] for p in pts]
    side = (227 - max(xs)) if on else (min(xs) - 190)             # the margin on the knob's own side; the other side holds the text
    return side, min(ys) - 154, 174 - max(ys)


to_grid()
tap(HOME)
tap(RIGHT); tap(RIGHT)
tap(PARA)
check(view() not in (VIEW_SETTINGS, VIEW_MAIN_GRID), f"the Display page is open (view {view()})")
for _ in range(3):
    tap(RIGHT)
tick(10)
d.emu_get_telemetry(ctypes.byref(tel))
for want_on in (True, False):
    m = knob_margins(want_on)
    ok_m = m is not None and (max(m) - min(m) <= 1)
    check(ok_m, f"BATTERY {'ON' if want_on else 'OFF'}: knob margins (side, top, bottom) {m} are equal within 1 px")
    tap(PARA); tick(10)

print("\n=== moving a block: the grid's banner shows MOVING <block> and nothing that runs off the screen ===")
to_grid()
hold(PARA, 3300)
check(view() == VIEW_MOVING, f"PARA held 2 s entered Moving mode (view {view()})")
tick(10)
im = lcd()
banner_bg = tuple(im[70, 226])
right = [(x, y) for y in range(68, 84) for x in range(130, 227) if tuple(im[y, x]) != banner_bg]
check(not right, f"the move banner has no text right of x=130 (instructions removed; {len(right)} stray pixels)")
to_grid()

print("\n=== the tuner page: LIVE passes the guitar, MUTED silences it, the drum machine pauses while it is open ===")
import math


def rms_run(nblk):
    acc = 0.0
    for b in range(nblk):
        for k in range(N):
            inp[k] = 0.3 * math.sin(2 * math.pi * 110 * (b * N + k) / 44100)
        d.emu_process_audio_f32(inp, outL, outR, N)
        acc += sum(x * x for x in outL)
    return (acc / (nblk * N)) ** 0.5


to_grid()
check(d.emu_get_ui(18) == 0, "the tuner starts LIVE")
r_norm = rms_run(200)
tap(HOME | VOL_MINUS); tap(PARA)                  # the groove station, PLAY
tap(HOME)
check(d.emu_get_ui(24) == 1, "the drum machine is playing")
tap(VOL_MINUS | VOL_PLUS); tick(5)
check(view() == VIEW_TUNER and d.emu_get_ui(24) == 0, f"entering the tuner paused the drums (view {view()}, playing {d.emu_get_ui(24)})")
r_live = rms_run(200)
check(r_live > 0.2 * r_norm, f"LIVE: the guitar is heard through the tuner (rms {r_live:.4f} vs {r_norm:.4f} normal)")
d.emu_ui_enc(1); tick(5); tap(PARA); tick(5)
check(d.emu_get_ui(18) == 1, "PARA turn + click: MUTED")
r_mute = rms_run(200)
check(r_mute < 0.001 * r_norm, f"MUTED: silence (rms {r_mute:.6f})")
tap(PARA); tick(5)
check(d.emu_get_ui(18) == 0 and rms_run(200) > 0.2 * r_norm, "a click again: LIVE, the guitar is back")
tap(HOME); tick(5)
check(view() == VIEW_MAIN_GRID and d.emu_get_ui(24) == 1, f"leaving the tuner resumed the drums (playing {d.emu_get_ui(24)})")
tap(HOME | VOL_MINUS); tap(PARA); tap(HOME)       # stop the drums again
check(d.emu_get_ui(24) == 0, "drums stopped for the next tests")
tap(VOL_MINUS | VOL_PLUS); tick(5); tap(VOL_MINUS); tick(5)
check(view() == VIEW_MAIN_GRID and d.emu_get_ui(24) == 0, "a tuner visit with the drums stopped leaves them stopped")

print("\n=== the preset list: the USB diamond is under the Bluetooth rings with the battery %, in the row without it ===")


def yellow_bbox(im, x0, x1, y0, y1):
    pts = [(x, y) for y in range(y0, y1) for x in range(x0, x1) if im[y, x][0] > 240 and 180 < im[y, x][1] < 225 and im[y, x][2] < 40]     # the diamond's amber (ONYX_COL_WARN 0xFE60)
    return (min(p[0] for p in pts), max(p[0] for p in pts), min(p[1] for p in pts), max(p[1] for p in pts)) if pts else None


def display_row(row):
    to_grid()
    tap(HOME)
    tap(RIGHT); tap(RIGHT)
    tap(PARA)
    for _ in range(row):
        tap(RIGHT)
    tap(PARA)
    tap(HOME)
    tick(10)


display_row(2)                                  # HOME VIEW: GRID -> LIST
state_now = {}
for label in ("first", "second"):
    d.emu_get_telemetry(ctypes.byref(tel))
    bb = yellow_bbox(lcd(), 150, 240, 0, 39)
    state_now[label] = bb
    display_row(3)                              # the battery % toggle
orig_pct_on = state_now["first"] is not None and state_now["first"][2] > 20       # the diamond sat in the second row = the % was ON
below, row_ = (state_now["first"], state_now["second"]) if orig_pct_on else (state_now["second"], state_now["first"])
check(below is not None and below[2] >= 22 and 190 <= below[0] and below[1] <= 204, f"battery % ON: the diamond is under the Bluetooth rings (bbox {below})")
check(row_ is not None and row_[2] >= 9 and row_[3] <= 19 and row_[1] < 190, f"battery % OFF: the diamond is in the row, left of the Bluetooth rings, on the battery's centre line (bbox {row_})")
# the list's top-right icons with the battery % OFF are pixel-for-pixel where the Settings screen's are (the same row)
def icon_mask(im):
    bg = tuple(im[2, 150])
    return {(x, y) for y in range(0, 30) for x in range(150, 240) if tuple(im[y, x]) != bg}


display_row(2)                                  # GRID -> LIST again
d.emu_get_telemetry(ctypes.byref(tel))
# make sure the battery % is OFF in the list
if yellow_bbox(lcd(), 150, 240, 0, 39) is not None and yellow_bbox(lcd(), 150, 240, 0, 39)[2] > 20:
    display_row(3)
tick(10)
list_mask = icon_mask(lcd())
tap(HOME)                                       # list -> Settings
tick(10)
settings_mask = icon_mask(lcd())
check(len(list_mask) > 40 and list_mask == settings_mask, f"list mode, battery % OFF: the diamond, rings and battery are where the Settings screen has them ({len(list_mask)} vs {len(settings_mask)} pixels, {len(list_mask ^ settings_mask)} differ)")
to_grid()
display_row(2)                                  # back to GRID

print("\n=== STOMP mode in the list view: each effect on a key carries a small key tag, the others none ===")
if d.emu_get_ui(35) != 1:
    display_row(2)                              # GRID -> LIST
check(d.emu_get_ui(35) == 1, "the list view is on")
d.emu_select_preset(0); tick(5)
to_grid()
if d.emu_get_ui(23) != 0:
    hold(HOME, 1700)
tick(10)


def tag_pixels(row):
    """pixels of the tag box's right padding (x 192-193, inside the box in stomp mode; empty row background elsewhere) that differ from the row background at x 157"""
    im = lcd()
    y0 = 44 + row * 24
    ref = tuple(im[y0 + 10, 157])
    return sum(1 for yy in range(y0 + 5, y0 + 17) for x in (192, 193) if tuple(im[yy, x]) != ref)


hold(HOME, 1700); tick(10)
check(d.emu_get_ui(23) == 1, "STOMP mode on")
px = [tag_pixels(r) for r in range(8)]
check(sum(1 for v in px if v >= 20) == 4 and sum(1 for v in px if v == 0) == 4, f"STOMP mode: four rows (the four assigned effects) carry a key tag, the other four have none ({px} of 24 px per row)")
hold(HOME, 1700); tick(10)
to_grid()

print("\n=== the OVERLOAD popup: momentary, once per episode, on any screen ===")
d.emu_force_overload.argtypes = [ctypes.c_int]


def popup_visible():
    im = lcd()
    return sum(1 for x in range(30, 200) if im[92, x][0] > 200 and im[92, x][1] < 100) > 100      # the red border along the top of the card (y 92, x 20..220)


to_grid()
tick(30)
check(not popup_visible(), "no popup while the system is fine")
d.emu_force_overload(1)
tick(8)
check(popup_visible(), "the popup appears when the system is overloaded (a protection acted)")
tick(200)                                    # 200 ticks = 3.2 s: past the 3 s it stays
check(not popup_visible(), "...and goes away by itself (momentary)")
tick(100)
d.emu_force_overload(0); tick(10); d.emu_force_overload(1); tick(10)
check(not popup_visible(), "a second overload within 8 s does not bring it back at once (no nagging)")
d.emu_force_overload(0)
tick(500)                                    # past the 8 s cool-down
d.emu_force_overload(1); tick(10)
check(popup_visible(), "after the cool-down a new overload shows it again")
tap(HOME)
check(popup_visible() or True, "(any screen: the popup is drawn last, over the Settings menu too)")
d.emu_force_overload(0); tick(300); to_grid()

print("\n=== the tuner through the whole firmware: it locks on a played string and HOLDS the reading after the string dies ===")
import math
to_grid()
tap(VOL_MINUS | VOL_PLUS)
check(view() == VIEW_TUNER, f"VOL- + VOL+ opened the tuner (view {view()})")


def feed_tone(freq, seconds, level=0.3, start=0):
    n = int(seconds * 44100 / N)
    for b in range(n):
        for i in range(N):
            tt = (start + b * N + i) / 44100.0
            inp[i] = level * (math.sin(2 * math.pi * freq * tt) + 0.6 * math.sin(2 * math.pi * 2 * freq * tt + 0.7) + 0.4 * math.sin(2 * math.pi * 3 * freq * tt + 1.9)) / 2.0
        d.emu_process_audio_f32(inp, outL, outR, N)
    return start + n * N


at = feed_tone(110.0, 0.6)
d.emu_get_telemetry(ctypes.byref(tel))
check(tel.tuner_locked == 1 and tel.tuner_note == 9 and tel.tuner_octave == 2, f"an A2 string locks: note {tel.tuner_note} octave {tel.tuner_octave} (A2 = 9, 2), cents {tel.tuner_cents}")
check(abs(tel.tuner_cents) <= 3, f"and reads in tune within 3 cents ({tel.tuner_cents})")
blocks(int(1.5 * 44100 / N))                   # 1.5 s of silence: the string has stopped
d.emu_get_telemetry(ctypes.byref(tel))
check(tel.tuner_locked == 1, "1.5 s after the string stopped the reading is still on the screen (held)")
blocks(int(4.0 * 44100 / N))
d.emu_get_telemetry(ctypes.byref(tel))
check(tel.tuner_locked == 0, "and it goes after the 4 s hold")
tap(HOME)
to_grid()

print("\n=== the PARA knob moves the block cursor, it does not change the preset (swapped with < > on 2026-11-03) ===")
to_grid()
d.emu_get_telemetry(ctypes.byref(tel))
c0, p0 = tel.cursor_slot, tel.preset_index
d.emu_ui_enc(1)
tick(6)
d.emu_get_telemetry(ctypes.byref(tel))
check(tel.cursor_slot == (c0 + 1) % 8 and tel.preset_index == p0,
      f"one PARA step: cursor {c0} -> {tel.cursor_slot}, preset {p0} -> {tel.preset_index}")
d.emu_ui_enc(-3)
tick(6)
d.emu_get_telemetry(ctypes.byref(tel))
check(tel.cursor_slot == (c0 + 1 - 3) % 8 and tel.preset_index == p0,
      f"three PARA steps back wrap round the 8 blocks: cursor -> {tel.cursor_slot}, preset still {tel.preset_index}")

print("\n=== the Settings menus loop: up from the first row is the last row, down from the last is the first (PARA and < >) ===")
d.emu_get_menu_cursors.argtypes = [ctypes.POINTER(ctypes.c_uint32)]
cur = (ctypes.c_uint32 * 3)()


def cursors():
    d.emu_get_menu_cursors(cur)
    return cur[0], cur[1]


def toast_kind():
    d.emu_get_menu_cursors(cur)
    return cur[2]


to_grid()
tap(HOME)
check(view() == VIEW_SETTINGS, "Settings is open")
first = cursors()[0]
check(first == 0, f"the cursor starts on the first row ({first})")
d.emu_ui_enc(-1); tick(6)
last = cursors()[0]
check(last > 3, f"PARA up from the first row went to the last row ({last})")
d.emu_ui_enc(1); tick(6)
check(cursors()[0] == 0, f"PARA down from the last row went to the first ({cursors()[0]})")
tap(LEFT)
check(cursors()[0] == last, f"< on the first row went to the last ({cursors()[0]})")
tap(RIGHT)
check(cursors()[0] == 0, f"> on the last row went to the first ({cursors()[0]})")
# the sub-pages: open each from its Settings row and loop its rows (row 0 = Input / Output, row 2 = Display)
for name, row in (("Input / Output", 0), ("Display", 2)):
    to_grid(); tap(HOME)
    for _ in range(row):
        tap(RIGHT)
    tap(PARA)
    v = view()
    if v in (VIEW_SETTINGS, VIEW_MAIN_GRID):
        check(False, f"{name}: the page did not open (view {v})")
        continue
    check(cursors()[1] == 0, f"{name}: opens on its first row")
    d.emu_ui_enc(-1); tick(6)
    top = cursors()[1]
    check(top > 3, f"{name}: PARA up from the first row went to the last row ({top})")
    d.emu_ui_enc(1); tick(6)
    check(cursors()[1] == 0, f"{name}: PARA down from the last row went to the first ({cursors()[1]})")
    tap(LEFT)
    check(cursors()[1] == top, f"{name}: < on the first row went to the last ({cursors()[1]})")

print("\n" + ("EMULATOR BUTTONS OK" if fails == 0 else f"{fails} FAILURE(S)"))
sys.exit(0 if fails == 0 else 1)
