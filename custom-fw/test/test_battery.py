# SPDX-License-Identifier: GPL-3.0-only
# Copyright (C) 2026 The Onyx Authors (@28spawn)
"""The battery, the stock firmware's way: the percentage is computed from the averaged SARADC channel 9 count with the stock formula (0 below 528, 100 above 635, else
(adc * 100 - 52646) / 109), the charging state comes from the USB power bit, and the warning levels follow the percentage. Through the real tick.

    python custom-fw/test/test_battery.py [path/to/pocket_amp_emu.dll]
"""
import ctypes, os, sys, tempfile
HERE = os.path.dirname(os.path.abspath(__file__)); ROOT = os.path.dirname(HERE)
os.environ["PA_FLASH_FILE"] = os.path.join(tempfile.mkdtemp(prefix="pa_batt_"), "flash.bin")
d = ctypes.CDLL(sys.argv[1] if len(sys.argv) > 1 else os.path.join(ROOT, "emu", "pocket_amp_emu.dll"))
d.emu_init(); d.emu_set_battery_adc.argtypes = [ctypes.c_int]; d.emu_set_charging.argtypes = [ctypes.c_int]; d.emu_get_ui.argtypes = [ctypes.c_int]
fails = 0
def check(ok, msg):
    global fails
    print(("  ok   " if ok else "  FAIL ") + msg)
    fails += ok is False
def stock(adc):
    return 0 if adc < 528 else 100 if adc > 635 else (adc * 100 - 52646) // 109
F = ctypes.POINTER(ctypes.c_float)
d.emu_process_audio_f32.argtypes = [F, F, F, ctypes.c_int]
ib, ol, orr = (ctypes.c_float * 64)(), (ctypes.c_float * 64)(), (ctypes.c_float * 64)()
def tick(n=30):
    for _ in range(n * 11): d.emu_process_audio_f32(ib, ol, orr, 64)      # one firmware tick = 11 audio blocks
print("=== the stock formula, every count 500..660, through the firmware's tick ===")
bad = []
for adc in range(500, 661):
    d.emu_set_battery_adc(adc); tick(3)
    got = d.emu_get_ui(36)
    if got != stock(adc) or d.emu_get_ui(37) != adc: bad.append((adc, got, stock(adc)))
check(not bad, "the percentage is the stock formula for every count" + (f" (first miss {bad[0]})" if bad else ""))
for adc, want in ((527, 0), (528, 1), (550, 21), (580, 49), (608, 74), (635, 99), (636, 100)):
    d.emu_set_battery_adc(adc); tick(3)
    check(d.emu_get_ui(36) == want, f"count {adc} -> {want} % (the stock formula (unsigned divide, so 74 where the pasted table rounds to 75))")
print("=== the warning levels follow the percentage ===")
d.emu_set_battery_adc(620); tick(5); check(d.emu_get_ui(39) == 0, "79 %: no warning")
d.emu_set_battery_adc(543); tick(5); check(d.emu_get_ui(39) == 1, f"15 % (count 543): the low warning ({d.emu_get_ui(39)})")
d.emu_set_battery_adc(520); tick(5); check(d.emu_get_ui(39) == 2, "0 % (count 520): the critical warning")
print("=== charging ===")
d.emu_set_battery_adc(600); d.emu_set_charging(0); tick(5); check(d.emu_get_ui(38) == 0, "on the battery: not charging")
d.emu_set_charging(1); tick(5); check(d.emu_get_ui(38) == 1, "USB power present: charging")
d.emu_set_charging(0); tick(5); check(d.emu_get_ui(38) == 0, "unplugged: not charging again")
print("\n" + ("THE BATTERY WORKS THE STOCK WAY" if not fails else f"{fails} FAILURE(S)"))
sys.exit(1 if fails else 0)
