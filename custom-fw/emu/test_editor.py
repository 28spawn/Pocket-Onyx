"""The effect window: the < and > buttons must adjust the setting under the
cursor.

The block editor used to use < and > to MOVE between the settings and the
PARA encoder to change the value - so an effect could not be set up with the
buttons alone. Now < and > adjust the setting (like the Settings screen) and
the encoder moves to the next setting.
"""
import ctypes, sys

d = ctypes.CDLL(sys.argv[1])
d.emu_init()
d.emu_ui_input.argtypes = [ctypes.c_uint32, ctypes.c_int32, ctypes.c_uint16]
d.emu_get_slot_param.argtypes = [ctypes.c_int, ctypes.c_int]
d.emu_get_slot_param.restype = ctypes.c_int
d.emu_process_audio_f32.argtypes = [ctypes.POINTER(ctypes.c_float), ctypes.POINTER(ctypes.c_float),
                                    ctypes.POINTER(ctypes.c_float), ctypes.c_int]

N = 64
inp = (ctypes.c_float * N)()
outL = (ctypes.c_float * N)()
outR = (ctypes.c_float * N)()

HOME, VOLM, VOLP, LEFT, RIGHT, PARA = 1, 2, 4, 8, 16, 32
AMP = 2


def tick(n=11):
    for _ in range(n):
        d.emu_process_audio_f32(inp, outL, outR, N)


# The emulator's clock is the SAMPLE clock: 64 frames at 44.1 kHz is 1.45 ms
# per block, and the firmware's 60 Hz tick runs every 11 blocks. So a press has
# to span enough blocks for the 20 ms contact debounce to complete - about 14
# blocks - and the release has to span the same again.
BLOCKS = 40


def press(mask, hold=BLOCKS, release=BLOCKS):
    d.emu_ui_input(mask, 0, 0xFFFF)
    tick(hold)
    d.emu_ui_input(0, 0, 0xFFFF)
    tick(release)


def param(slot, p):
    return d.emu_get_slot_param(slot, p)


# The editor cursor, from the telemetry: -1 is the model row, -2 the ON/OFF
# row, and >= 0 a parameter knob.
class _Telemetry(ctypes.Structure):
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


_tel = _Telemetry()
d.emu_get_telemetry.argtypes = [ctypes.POINTER(_Telemetry)]


def editor_cursor():
    d.emu_get_telemetry(ctypes.byref(_tel))
    return ctypes.c_int32(_tel.editor_cursor).value


fails = 0
def check(ok, msg):
    global fails
    print(("  ok   " if ok else "  FAIL ") + msg)
    if not ok:
        fails += 1


print("=== open the effect window ===")
tick(20)
press(PARA)                      # a quick PARA press on the home grid
tick(10)
d.emu_get_slot_type.argtypes = [ctypes.c_int]
d.emu_get_slot_type.restype = ctypes.c_int
# The editor opens on the MODEL / TYPE row (the standard on every effect page).
check(editor_cursor() == -1, f"the editor opens with the cursor on the model row ({editor_cursor()})")

print("\n=== on the model row, < and > step the model ===")
model0 = d.emu_get_slot_type(AMP)
press(RIGHT)
model1 = d.emu_get_slot_type(AMP)
check(model1 != model0, f"> stepped the model ({model0} -> {model1})")
press(LEFT)
model2 = d.emu_get_slot_type(AMP)
check(model2 == model0, f"< stepped it back ({model1} -> {model2})")

print("\n=== VOL+ moves to the first knob; < and > adjust it ===")
press(VOLP)
check(editor_cursor() == 0, f"VOL+ moved the cursor to the first knob ({editor_cursor()})")
before = param(AMP, 0)
press(RIGHT)
after_up = param(AMP, 0)
check(after_up > before, f"> increased the value ({before} -> {after_up})")

press(LEFT)
after_down = param(AMP, 0)
check(after_down < after_up, f"< decreased the value ({after_up} -> {after_down})")

press(LEFT)
after_down2 = param(AMP, 0)
check(after_down2 < after_down, f"and < again decreased it ({after_down} -> {after_down2})")

print("\n=== the value is clamped to 0..100 % ===")
# (the cursor is still on the first knob; this used to run on the model row,
# so it never touched the value it checked)
for _ in range(80):              # 2 % a press, full-length presses (a 1-block tap is under the debounce)
    press(LEFT)
v = param(AMP, 0)
check(v == 0, f"it stopped at 0 ({v})")
for _ in range(120):
    press(RIGHT)
v = param(AMP, 0)
check(v == 100, f"and it stopped at 100 ({v})")

print("\n" + ("EFFECT WINDOW OK" if fails == 0 else f"{fails} FAILURE(S)"))
sys.exit(0 if fails == 0 else 1)
