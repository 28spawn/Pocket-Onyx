# SPDX-License-Identifier: GPL-3.0-only
# Copyright (C) 2026 The Onyx Authors (@28spawn)
"""The radio calibration mirror (src/sys/pa_ble_trims_mirror.c) and the flash-read command (sysex cmd 84, PA_REPLY_PA_FLASH).
The mirror: a copy of the four radio records in the firmware's own part of the shared sector, so a blank stock log (a flash tool erased it once) no longer stops the radio. It must be stored once,
survive every rewrite of the shared sector (the settings log filling up, a NAM registry commit), be replaced when the records change, and a damaged record must read as "no mirror".
The flash read: 128 bytes at most, inside the chip, read only. Usage: python custom-fw/emu/test_trims_mirror.py <dll>"""
import ctypes, os, sys, tempfile
HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, os.path.join(HERE, "..", "tools"))
flash = os.path.join(tempfile.mkdtemp(prefix="pa_mirror_"), "flash.bin")
os.environ["PA_FLASH_FILE"] = flash
import preset_bank_pc as P
P._js_setup()
link = P.EmuLink(sys.argv[1]); d = link.d; js = P.js
d.emu_trims_mirror_store.argtypes = [ctypes.c_char_p]
d.emu_trims_mirror_load.argtypes = [ctypes.c_char_p]
fails = 0


def check(ok, msg):
    global fails
    print(("  ok   " if ok else "  FAIL ") + msg); fails += 0 if ok else 1


def load():
    b = ctypes.create_string_buffer(97)
    return (d.emu_trims_mirror_load(b) == 1), b.raw


A = bytes((i * 7 + 3) & 255 for i in range(97))
B = bytes((i * 11 + 5) & 255 for i in range(97))
ok, _ = load()
check(not ok, "a unit that never stored one has no mirror")
d.emu_trims_mirror_store(A)
ok, got = load(); check(ok and got == A, "stored: the mirror reads back the four records")
check(d.emu_shared_rewrite(0) == 0, "the settings log filling up (the shared sector is erased and rewritten)...")
ok, got = load(); check(ok and got == A, "...the mirror comes back")
check(d.emu_shared_rewrite(1) == 0, "a NAM registry commit (the registry is written, the rest of the sector erased)...")
ok, got = load(); check(ok and got == A, "...the mirror comes back")
d.emu_trims_mirror_store(B)
ok, got = load(); check(ok and got == B, "different records replace the mirror (the sector is rewritten with the new one)")
d.emu_shared_rewrite(0)
ok, got = load(); check(ok and got == B, "...and the new one survives a rewrite")
# the flash read: sysex cmd 84, [addr u32 LE, length]
def rd(addr, n):
    link.send(js(["buildFrame", 84, [addr & 255, (addr >> 8) & 255, (addr >> 16) & 255, (addr >> 24) & 255, n]])[0])
    r = link.reply(0x65, 2.0)
    return r["data"] if r else None


r = rd(0x88D00, 128)
check(r is not None and r[4] == 128 and r[5] == 0 and len(r) == 6 + 128, "a 128-byte read is answered with its data")
check(r is not None and bytes(r[6:10]) == (0x524D5254).to_bytes(4, "little"), "...and the mirror's magic is in it (the editor's backup reads the same bytes)")
for addr, n, what in ((0x88D00, 129, "129 bytes"), (0x88D00, 0, "0 bytes"), (0xFFFFF, 2, "past the end of the chip"), (0x100000, 1, "outside the chip")):
    r = rd(addr, n); check(r is not None and r[5] == 1 and len(r) == 6, f"{what}: refused, no data")
print("RESULT:", "FAILED" if fails else "ALL PASSED"); sys.exit(1 if fails else 0)
