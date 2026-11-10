#!/usr/bin/env python3
"""
custom-fw/tools/hw_test.py - drive and inspect the Pocket Amp over its USB-MIDI port, and say what
happened if it stops answering.

The unit shows up as a MIDI device ("Custom MIDI") on the same USB-C cable that powers it. This runner

  * sends the firmware's MIDI controls: Program Change = jump to preset, CC 64 = HOME, CC 65 = PARA
    click, CC 66 / 67 = encoder left / right, CC 20..27 = toggle effect slot 0..7;
  * reads the unit's RAM over the private diagnostic channel (F0 7D 02 <addr> <len> F7, read-only,
    SRAM only): the test-state block (the UI's view, preset, cursors, loop counter, audio stats),
    the black box, the crash record, the USB counters, and any variable by name from the build's ELF;
  * if the unit stops answering, checks whether it dropped into the ROM download mode on its own (the
    watchdog / fault / return path) and tells you how to read the logs it left.

    python custom-fw/tools/hw_test.py ping
    python custom-fw/tools/hw_test.py state
    python custom-fw/tools/hw_test.py smoke             # liveness + preset switching + navigation
    python custom-fw/tools/hw_test.py stress 300        # 300 random controls, then check it is alive
    python custom-fw/tools/hw_test.py read g_main_iters --elf custom-fw/build_pi32/pocket_amp_custom.elf
    python custom-fw/tools/hw_test.py raw 0x01C7C390 48

Exit code 0 = every check passed, 1 = a check failed, 2 = the unit is not answering / not found.
Nothing here writes to the unit's flash or RAM: it sends MIDI controls and reads.
"""
from __future__ import annotations

import argparse
import os
import random
import struct
import subprocess
import sys
import time
from pathlib import Path

QUERY = [0xF0, 0x00, 0x32, 0x45, 0x00, 0x00, 0x00, 0x40, 0x7F, 0xF7]       # the OTA "who are you?" query (read only)
TEST_STATE = 0x01C7C390        # src/board/main.c pa_test_state_update()
BLACKBOX = 0x01C7C000
USB_DEBUG = 0x01C7C3C0         # src/board/usb_dev.c
ISR_STATS = 0x01C7C250         # src/board/audio_alnk.c

CC_HOME, CC_PARA, CC_ENC_LEFT, CC_ENC_RIGHT, CC_SLOT0 = 64, 65, 66, 67, 20
VIEWS = ["MAIN_GRID", "MOVING", "PARAM_EDIT", "BANK_MODE", "TUNER", "SAVE_DIALOG", "SAVED_TOAST", "DRUMS",
         "SETTINGS", "GLOBAL_EQ", "INPUT_OUTPUT", "DISPLAY", "TELEMETRY", "SYS_INFO", "FACTORY_RESET", "STOMP_ASSIGN"]


# ---- the 8-to-7 bit packing the SysEx frames use (the same as src/sys/pa_diag.c) ---------------------
def pack_8to7(data: bytes) -> bytes:
    out, bits, nbits = bytearray(), 0, 0
    for b in data:
        bits |= b << nbits
        nbits += 8
        while nbits >= 7:
            out.append(bits & 0x7F)
            bits >>= 7
            nbits -= 7
    if nbits:
        out.append(bits & 0x7F)
    return bytes(out)


def unpack_7to8(data: bytes) -> bytes:
    out, bits, nbits = bytearray(), 0, 0
    for b in data:
        bits |= (b & 0x7F) << nbits
        nbits += 7
        if nbits >= 8:
            out.append(bits & 0xFF)
            bits >>= 8
            nbits -= 8
    return bytes(out)


class NoAnswer(Exception):
    pass


class Unit:
    def __init__(self, port: str = "Custom MIDI"):
        import rtmidi
        self.mo, self.mi = rtmidi.MidiOut(), rtmidi.MidiIn()
        outs, ins = self.mo.get_ports(), self.mi.get_ports()
        oi = next((i for i, n in enumerate(outs) if port.lower() in n.lower()), None)
        ii = next((i for i, n in enumerate(ins) if port.lower() in n.lower()), None)
        if oi is None or ii is None:
            raise NoAnswer(f"no MIDI port with '{port}' in its name (outputs {outs}, inputs {ins})")
        self.mi.ignore_types(sysex=False, timing=True, active_sense=True)
        self.mi.open_port(ii)
        self.mo.open_port(oi)
        time.sleep(0.15)
        self.drain()

    def drain(self):
        while self.mi.get_message():
            pass

    def send(self, msg):
        self.mo.send_message(list(msg))

    def sysex(self, timeout: float = 1.0):
        t0 = time.time()
        while time.time() - t0 < timeout:
            m = self.mi.get_message()
            if m and m[0] and m[0][0] == 0xF0:
                return m[0]
            time.sleep(0.002)
        return None

    # ---- the controls
    def pc(self, n: int):
        self.send([0xC0, n & 0x7F])

    def cc(self, n: int, v: int = 127):
        self.send([0xB0, n & 0x7F, v & 0x7F])

    def home(self): self.cc(CC_HOME)
    def para(self): self.cc(CC_PARA)
    def left(self): self.cc(CC_ENC_LEFT)
    def right(self): self.cc(CC_ENC_RIGHT)
    def toggle_slot(self, s: int): self.cc(CC_SLOT0 + s)

    # ---- the reads
    def ping(self, timeout: float = 1.0) -> bytes:
        self.drain()
        self.send(QUERY)
        r = self.sysex(timeout)
        if not r:
            raise NoAnswer("no answer to the identity query")
        return bytes(r)

    def ram_read(self, addr: int, n: int, timeout: float = 1.0) -> bytes:
        out = bytearray()
        while n > 0:
            k = min(n, 192)
            inner = bytes([0x7D, 0x02]) + addr.to_bytes(4, "little") + k.to_bytes(2, "little") + b"\x00"
            self.drain()
            self.send([0xF0] + list(pack_8to7(inner)) + [0xF7])
            r = self.sysex(timeout)
            if not r:
                raise NoAnswer(f"no answer to a RAM read of {k} bytes at {addr:#010x}")
            body = unpack_7to8(bytes(r[1:-1]))
            if len(body) < 8 or body[0] != 0x7D or body[1] != 0x82:
                raise NoAnswer(f"unexpected reply to a RAM read: {bytes(r).hex(' ')}")
            ln = body[2] | body[3] << 8
            if ln == 0xFFFF:
                raise NoAnswer(f"the unit refused the RAM read at {addr:#010x} (only SRAM 0x01C00000-0x01C7FFFF is readable)")
            out += body[8:8 + ln]
            addr += ln
            n -= ln
            if ln == 0:
                break
        return bytes(out)

    def state(self) -> dict:
        raw = self.ram_read(TEST_STATE, 48)
        w = struct.unpack("<12I", raw)
        if w[0] != 0x41545354:
            raise NoAnswer(f"the test-state block is not there (magic {w[0]:#x}): an older build, or not booted yet")
        s8 = lambda v: v - 256 if v > 127 else v
        return {
            "iters": w[1], "seconds": w[2],
            "view": w[3] & 0xFF, "view_name": VIEWS[w[3] & 0xFF] if (w[3] & 0xFF) < len(VIEWS) else "?",
            "preset": (w[3] >> 8) & 0xFF, "master_vol": (w[3] >> 16) & 0xFF, "play_mode": w[3] >> 24,
            "slot": w[4] & 0xFF, "cursor_slot": (w[4] >> 8) & 0xFF, "editor_cursor": s8((w[4] >> 16) & 0xFF), "edit_mode": w[4] >> 24,
            "sub_cursor": w[5] & 0xFF, "settings_cursor": (w[5] >> 8) & 0xFF, "in_peak": (w[5] >> 16) & 0xFF, "out_peak": w[5] >> 24,
            "battery_mv": w[6] & 0xFFFF, "battery_pct": (w[6] >> 16) & 0xFF, "save_failed": (w[6] >> 24) & 1,
            "settings_dirty": (w[6] >> 25) & 1, "usb_connected": (w[6] >> 26) & 1,
            "bpm": w[7] & 0xFFFF, "drum_playing": (w[7] >> 16) & 0xFF, "pattern": w[7] >> 24,
            "buttons": w[8], "isr_entries": w[9], "bypass": w[10] & 0xFF, "accel_broken": (w[10] >> 8) & 0xFF,
        }


# ---- symbols from the build's ELF (so a variable can be read by name) -------------------------------
def symbols(elf: Path) -> dict[str, tuple[int, int]]:
    objdump = None
    cands = []
    if "PI32_ROOT" in os.environ:
        cands.append(Path(os.environ["PI32_ROOT"]) / "bin" / "llvm-objdump.exe")
    cands.extend([
        Path.home() / "tools" / "jieli" / "C$" / "JL" / "pi32" / "bin" / "llvm-objdump.exe",
        Path(r"C:\JL\pi32\bin\llvm-objdump.exe")
    ])
    for cand in cands:
        if cand.exists():
            objdump = cand
            break
    if objdump is None:
        raise SystemExit("llvm-objdump (the JieLi toolchain) not found; pass an address with 'raw' instead")
    out = subprocess.run([str(objdump), "-t", str(elf)], capture_output=True, text=True).stdout
    syms = {}
    for line in out.splitlines():
        p = line.split()
        if len(p) >= 5 and len(p[0]) == 8:
            try:
                addr = int(p[0], 16)
                size = int(p[-2], 16)
            except ValueError:
                continue
            syms[p[-1]] = (addr, size)
    return syms


# ---- what to do when the unit stops answering --------------------------------------------------------
def in_rom_download_mode() -> bool:
    out = subprocess.run(["powershell", "-NoProfile", "-Command",
                          "@(Get-PnpDevice -PresentOnly | Where-Object { $_.InstanceId -like '*VID_4C4A&PID_8057*' }).Count"],
                         capture_output=True, text=True).stdout.strip()
    return out not in ("", "0")


def still_enumerated() -> bool:
    out = subprocess.run(["powershell", "-NoProfile", "-Command",
                          "@(Get-PnpDevice -PresentOnly | Where-Object { $_.InstanceId -like '*VID_1209&PID_0001*' }).Count"],
                         capture_output=True, text=True).stdout.strip()
    return out not in ("", "0")


def explain_silence(why: str) -> int:
    print(f"\nNO ANSWER: {why}")
    time.sleep(2.0)
    if in_rom_download_mode():
        print("  The unit is in the ROM download mode (WL82 UBOOT1.00): it reset itself, which is what the watchdog,\n"
              "  the fault handler, the 170 s bench return and HOME+PARA do. Its SRAM logs are intact. In an\n"
              "  ADMINISTRATOR PowerShell:\n"
              '    python custom-fw\\tools\\jl_ram_read.py --jl-tool "<jl-uboot-tool dir>" bb --out blackbox.bin\n'
              "  prints the USB counters and audio stats; the black box is in blackbox.bin.")
    elif still_enumerated():
        print("  The unit is still on USB but not answering: the main loop has stalled. The watchdog should reset it\n"
              "  within about 8 s and it should then appear in the ROM download mode: run this again in 15 s.")
    else:
        print("  The unit is not on USB any more (powered off, cable out, or in a state with no USB).")
    return 2


# ---- the commands ---------------------------------------------------------------------------------
class Checks:
    def __init__(self):
        self.failed = 0

    def check(self, ok: bool, what: str, detail: str = ""):
        print(f"  [{'PASS' if ok else 'FAIL'}] {what}" + (f"  ({detail})" if detail else ""))
        if not ok:
            self.failed += 1


def cmd_ping(u: Unit, a) -> int:
    r = u.ping()
    text = bytes(b for b in r[5:-1] if 32 <= b < 127).decode()
    print(f"the unit answered in the identity frame: {text!r}  ({len(r)} bytes)")
    return 0


def cmd_state(u: Unit, a) -> int:
    for k, v in u.state().items():
        print(f"  {k:16s} {v}")
    return 0


def cmd_smoke(u: Unit, a) -> int:
    c = Checks()
    print("1. identity")
    r = u.ping()
    c.check(b"AC791N" in bytes(r), "identity frame names the chip", bytes(r[5:-1]).replace(b"\x00", b" ").decode(errors="replace"))
    print("2. liveness")
    s1 = u.state(); time.sleep(0.6); s2 = u.state()
    c.check(s2["iters"] > s1["iters"], "the main loop is running", f"{s1['iters']} -> {s2['iters']} passes")
    c.check(s2["isr_entries"] > s1["isr_entries"], "the audio interrupt is running", f"{s1['isr_entries']} -> {s2['isr_entries']}")
    print("3. preset switching (Program Change)")
    for n in (0, 10, 40, 79, 3):
        u.pc(n); time.sleep(0.4)
        got = u.state()["preset"]
        c.check(got == n, f"Program Change {n} -> preset index {n}", f"unit reports {got}")
    print("4. HOME navigation")
    u.home(); time.sleep(0.4)
    c.check(u.state()["view"] in (0, 4, 8), "HOME reaches a top-level view", u.state()["view_name"])
    print("5. still alive at the end")
    s3 = u.state()
    c.check(s3["iters"] > s2["iters"], "the main loop is still running", f"{s3['iters']} passes")
    print(f"\n{'ALL CHECKS PASSED' if not c.failed else str(c.failed) + ' CHECK(S) FAILED'}")
    return 1 if c.failed else 0


def cmd_stress(u: Unit, a) -> int:
    c = Checks()
    n = int(a.count)
    rng = random.Random(a.seed)
    before = u.state()
    print(f"sending {n} random controls (seed {a.seed}) ...")
    for i in range(n):
        k = rng.random()
        if k < 0.35: u.pc(rng.randrange(80))
        elif k < 0.55: u.left()
        elif k < 0.75: u.right()
        elif k < 0.85: u.para()
        elif k < 0.93: u.home()
        else: u.toggle_slot(rng.randrange(8))
        time.sleep(a.gap)
        if i % 50 == 49:
            try:
                u.state()
            except NoAnswer as e:
                return explain_silence(f"after {i + 1} controls: {e}")
    time.sleep(0.5)
    after = u.state()
    c.check(after["iters"] > before["iters"], "the main loop kept running", f"{before['iters']} -> {after['iters']}")
    c.check(after["usb_connected"] in (0, 1), "state block still sane")
    print(f"final view {after['view_name']}, preset {after['preset']}")
    print(f"\n{'STRESS RUN SURVIVED' if not c.failed else 'STRESS RUN FAILED'}")
    return 1 if c.failed else 0


def cmd_read(u: Unit, a) -> int:
    syms = symbols(Path(a.elf))
    if a.name not in syms:
        near = [s for s in syms if a.name.lower() in s.lower()][:8]
        raise SystemExit(f"no symbol '{a.name}' in {a.elf}" + (f" (did you mean: {', '.join(near)})" if near else ""))
    addr, size = syms[a.name]
    n = a.length or size or 4
    data = u.ram_read(addr, n)
    print(f"{a.name} @ {addr:#010x}, {n} bytes: {data.hex(' ')}")
    if n in (1, 2, 4):
        print(f"  = {int.from_bytes(data, 'little')}")
    return 0


def cmd_raw(u: Unit, a) -> int:
    data = u.ram_read(a.addr, a.length)
    for off in range(0, len(data), 16):
        print(f"{a.addr + off:#010x}  {data[off:off + 16].hex(' ')}")
    return 0


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--port", default="Custom MIDI")
    sub = ap.add_subparsers(dest="cmd", required=True)
    sub.add_parser("ping")
    sub.add_parser("state")
    sub.add_parser("smoke")
    s = sub.add_parser("stress")
    s.add_argument("count", nargs="?", default=200)
    s.add_argument("--gap", type=float, default=0.03)
    s.add_argument("--seed", type=int, default=1)
    r = sub.add_parser("read")
    r.add_argument("name")
    r.add_argument("length", nargs="?", type=lambda x: int(x, 0), default=0)
    r.add_argument("--elf", default=str(Path(__file__).resolve().parents[1] / "build_pi32" / "pocket_amp_custom.elf"))
    w = sub.add_parser("raw")
    w.add_argument("addr", type=lambda x: int(x, 0))
    w.add_argument("length", type=lambda x: int(x, 0))
    a = ap.parse_args()
    try:
        u = Unit(a.port)
        return {"ping": cmd_ping, "state": cmd_state, "smoke": cmd_smoke, "stress": cmd_stress,
                "read": cmd_read, "raw": cmd_raw}[a.cmd](u, a)
    except NoAnswer as e:
        return explain_silence(str(e))


if __name__ == "__main__":
    sys.exit(main())

# SPDX-License-Identifier: GPL-3.0-only
# Copyright (C) 2026 The Onyx Authors (@28spawn)
