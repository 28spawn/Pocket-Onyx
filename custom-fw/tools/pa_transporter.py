#!/usr/bin/env python3
"""
custom-fw/tools/pa_transporter.py - dump the Pocket Amp's flash through the FM-1 Transporter

The FM-1 methodology on the Pocket Amp: a Seeed XIAO RP2040 running Leo Kuroshita's FM-1
Transporter firmware (MIT) sits on the Pocket Amp's USB data lines. At power-on it sends the
JieLi USB_KEY (0x16EF), which makes the AC791N's mask ROM abort its boot and enter the ROM
download mode (UBOOT1.00) whatever is in flash, and then exposes the flash over its own USB
serial port. The FM-1 is the same chip (AC791N / WL82, chip key 980F).

This client has no general write command: the Transporter firmware only writes the FM-1's
app area (0x4000-0x93000), which is not the Pocket Amp's (0x4000-0xB4FFF), so a firmware
write from it would leave the Pocket Amp half-updated. The one exception is `drill`, which
rewrites sectors of the app area with the bytes they already hold (checked against the
ledger's verified dump and against the chip) to prove erase/write/verify works.

Wiring (three wires, as for the FM-1 - a USB-C pigtail into the Pocket Amp's port):
    XIAO D6 (GP0) -> D+      XIAO D7 (GP1) -> D-      XIAO GND -> GND      VBUS: NOT connected
The Pocket Amp must be OFF and on battery (charged) when the session starts.

    python custom-fw/tools/pa_transporter.py ports              find the console / data ports
    python custom-fw/tools/pa_transporter.py wait               start, THEN switch the Pocket Amp on
    python custom-fw/tools/pa_transporter.py info               chip key, flash JEDEC ID and size
    python custom-fw/tools/pa_transporter.py dump OUT_DIR       two full reads, compared and hashed
    python custom-fw/tools/pa_transporter.py runapp             leave UBOOT, boot the installed firmware
    python custom-fw/tools/pa_transporter.py drill DUMP 0x92000 [--full-verify]   restore drill

Every command keeps reading the Transporter's console (the FM-1 guide: an unread console
backs up and stalls the UBOOT handshake) and prints it to console.log in the current folder.
"""
import argparse
import hashlib
import json
import os
import struct
import sys
import threading
import time
import zlib

import serial
from serial.tools import list_ports

VID, PID = 0x2E8A, 0x000A
DEFAULT_LEN = 0x100000          # 1 MiB; `info` reports the real size - use --len if larger


class Console(threading.Thread):
    """Drains the Transporter's console port for the whole session."""

    def __init__(self, port, log_path="console.log"):
        super().__init__(daemon=True)
        self.s = serial.Serial(port, 115200, timeout=0.2)
        self.s.dtr = True
        self.log = open(log_path, "a", encoding="utf-8", errors="replace")
        self.lines = []
        self.stop = False

    def run(self):
        buf = b""
        while not self.stop:
            buf += self.s.read(4096)
            while b"\n" in buf:
                line, buf = buf.split(b"\n", 1)
                text = line.decode(errors="replace").rstrip()
                self.lines.append(text)
                self.log.write(text + "\n")
                self.log.flush()

    def seen(self, needle):
        return any(needle in l for l in self.lines)


def transporter_ports():
    """(console, data): the data port is the one that answers 'ping' with PONG."""
    ports = sorted(p.device for p in list_ports.comports() if p.vid == VID and p.pid == PID)
    if not ports:
        sys.exit("pa_transporter: no FM-1 Transporter (2E8A:000A) found")
    data = None
    for p in ports:
        try:
            s = serial.Serial(p, 115200, timeout=0.3)
            s.dtr = True
            time.sleep(0.2)
            s.reset_input_buffer()
            s.write(b"ping\n")
            t, buf = time.time(), b""
            while time.time() - t < 1.5:
                buf += s.read(64)
                if b"PONG" in buf:
                    break
            s.close()
            if b"PONG" in buf:
                data = p
                break
        except serial.SerialException:
            continue
    if not data:
        sys.exit(f"pa_transporter: none of {ports} answers ping")
    console = next((p for p in ports if p != data), None)
    return console, data


def open_data(port):
    s = serial.Serial(port, 115200, timeout=5)
    s.dtr = True
    time.sleep(0.2)
    s.reset_input_buffer()
    return s


def request(s, line, timeout=30):
    s.reset_input_buffer()
    s.write(line.encode() + b"\n")
    s.timeout = timeout
    reply = s.readline().decode(errors="replace").strip()
    if not reply:
        sys.exit(f"pa_transporter: no reply to '{line}'")
    return reply


def uboot_ready(s):
    s.reset_input_buffer()
    s.write(b"status\n")
    s.timeout = 1.0
    reply = s.readline().decode(errors="replace").strip()
    if not reply.startswith("OK"):
        return False
    fields = dict(kv.split("=", 1) for kv in reply.split()[1:] if "=" in kv)
    return fields.get("uboot") == "1"


def wait_uboot(s, console, timeout):
    print("Waiting for the Pocket Amp's ROM download mode. Switch the Pocket Amp ON now.")
    t0 = time.time()
    while time.time() - t0 < timeout:
        if uboot_ready(s):
            print(f"UBOOT session up after {time.time() - t0:.0f} s")
            return
        time.sleep(0.5)
    hint = " The console shows an ACK - look for 'D+ released' in console.log." if console and console.seen("ACK") else ""
    sys.exit(f"pa_transporter: no UBOOT within {timeout} s.{hint}")


def read_flash(s, addr, length):
    reply = request(s, f"read {addr:#x} {length}", timeout=30)
    if not reply.startswith("DATA "):
        sys.exit(f"pa_transporter: {reply}")
    n = int(reply.split()[1])
    data = bytearray()
    s.timeout = 10
    while len(data) < n:
        chunk = s.read(min(65536, n - len(data)))
        if not chunk:
            sys.exit(f"pa_transporter: stalled at {len(data):#x} of {n:#x}")
        data += chunk
        print(f"\r{len(data) * 100 // n:3d}%  {len(data):#08x}", end="", flush=True)
    print()
    end = s.readline().decode(errors="replace").strip()
    crc = zlib.crc32(data) & 0xFFFFFFFF
    if end != f"END {crc:08X}":
        sys.exit(f"pa_transporter: transfer check failed ({end}, local {crc:08X})")
    return bytes(data)


def session(args):
    console_port, data_port = transporter_ports()
    console = Console(console_port) if console_port else None
    if console:
        console.start()
    s = open_data(data_port)
    return console, s


def cmd_ports(args):
    console, data = transporter_ports()
    print(f"console {console}, data {data}")


def cmd_wait(args):
    console, s = session(args)
    wait_uboot(s, console, args.timeout)


def cmd_info(args):
    console, s = session(args)
    if not uboot_ready(s):
        wait_uboot(s, console, args.timeout)
    reply = request(s, "info", timeout=30)
    print(reply)
    if reply.startswith("OK"):
        f = dict(kv.split("=", 1) for kv in reply.split()[1:] if "=" in kv)
        print(f"chip key {f.get('key')} (AC791N / WL82 is 980F: {'yes' if f.get('key') == '980F' else 'NO'}), "
              f"flash JEDEC id {f.get('id')}")


def cmd_dump(args):
    os.makedirs(args.out_dir, exist_ok=True)
    console, s = session(args)
    if not uboot_ready(s):
        wait_uboot(s, console, args.timeout)
    info = request(s, "info", timeout=30)
    print(info)
    reads = []
    for k in (1, 2):
        print(f"read {k} of 2: {args.len:#x} bytes from 0")
        data = read_flash(s, 0, args.len)
        path = os.path.join(args.out_dir, f"dump{k}.bin")
        with open(path, "wb") as f:
            f.write(data)
        reads.append(data)
    same = reads[0] == reads[1]
    sha = hashlib.sha256(reads[0]).hexdigest()
    manifest = {
        "date": time.strftime("%Y-%m-%d %H:%M:%S"),
        "route": "usb_key_dongle (FM-1 Transporter, USB_KEY at power-on)",
        "transporter_info": info,
        "length": args.len,
        "dumps_identical": same,
        "sha256_dump1": sha,
        "sha256_dump2": hashlib.sha256(reads[1]).hexdigest(),
    }
    with open(os.path.join(args.out_dir, "manifest.json"), "w") as f:
        json.dump(manifest, f, indent=2)
    print(f"dump1 == dump2: {same}   sha256 {sha}")
    if not same:
        diff = [i for i in range(len(reads[0])) if reads[0][i] != reads[1][i]]
        print(f"the reads differ at {len(diff)} bytes (first {diff[0]:#x}): keep both, read again")
        sys.exit(1)
    print(f"saved {args.out_dir}/dump1.bin, dump2.bin and manifest.json. Next: "
          f"python custom-fw/tools/qa_recovery.py verify {args.out_dir}/dump1.bin {args.out_dir}/dump2.bin --record")


SECTOR = 0x1000
APP_MIN = 0x4000                 # the app area (file table + app.bin) starts here; MBR and uboot below
PKG_END = 0xB5000                # end of the firmware-package region (MBR + uboot + app)
FM1_WINDOW = (0x4000, 0x93000)   # the stock FM-1 Transporter build's write window


def parse_sectors(specs):
    """'0x92000' or a range '0x4000-0xB5000' (end exclusive) -> sorted sector addresses."""
    out = set()
    for spec in specs:
        if "-" in spec:
            lo, hi = (int(v, 0) for v in spec.split("-", 1))
            out.update(range(lo, hi, SECTOR))
        else:
            out.add(int(spec, 0))
    return sorted(out)


def firmware_window(info):
    """The write window the Transporter firmware reports (win=LO-HI), else the FM-1 default."""
    f = dict(kv.split("=", 1) for kv in info.split()[1:] if "=" in kv)
    if "win" not in f:
        return FM1_WINDOW
    lo, hi = f["win"].split("-")
    return int(lo, 16), int(hi, 16)


def cmd_drill(args):
    """Restore drill: write sectors back from the verified baseline dump.

    Refuses unless the reference dump matches the ledger's recorded baseline and every sector
    lies inside the firmware-package region (0x0-0xB5000) and the Transporter's own window.
    Sectors below 0x4000 (MBR, uboot) are NEVER written, by any option (owner's rule, 2026-10-31:
    never write over the bootloader; recovery does not need it, the ROM download mode is in the
    chip's mask ROM, not in flash). By default a sector is written
    only if the chip already holds exactly the reference bytes (a no-change drill); --repair
    also writes over sectors that differ, still only with the reference bytes. It can only
    ever put the baseline back - never new code.
    """
    ref = open(args.ref, "rb").read()
    sha = hashlib.sha256(ref).hexdigest()
    ledger = os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "qa", "hw_ledger.json")
    want = json.load(open(ledger, encoding="utf-8"))["recovery"]["full_dump_sha256"]
    if sha != want:
        sys.exit(f"pa_transporter: {args.ref} (sha256 {sha[:16]}) is not the ledger baseline ({want[:16]})")
    sectors = parse_sectors(args.sectors)
    for a in sectors:
        if a % SECTOR or not 0 <= a < PKG_END:
            sys.exit(f"pa_transporter: sector {a:#x} refused (allowed 0x0-{PKG_END:#x}, 4 KiB aligned)")
        if a < APP_MIN:
            sys.exit(f"pa_transporter: sector {a:#x} is in the boot area (MBR/uboot): never written, no option allows it")

    console, s = session(args)
    if not uboot_ready(s):
        wait_uboot(s, console, args.timeout)
    info = request(s, "info", timeout=30)
    print(info)
    lo, hi = firmware_window(info)
    outside = [a for a in sectors if not lo <= a < hi]
    if outside:
        sys.exit(f"pa_transporter: the Transporter firmware only writes {lo:#x}-{hi:#x}; "
                 f"{len(outside)} sector(s) outside it (first {outside[0]:#x}). Flash the Pocket Amp build.")
    print(f"drill: {len(sectors)} sector(s), {sectors[0]:#x}-{sectors[-1] + SECTOR:#x}, firmware window {lo:#x}-{hi:#x}")
    written = 0
    for a in sectors:
        data = ref[a:a + SECTOR]
        current = read_flash(s, a, SECTOR)
        if current != data:
            if not args.repair:
                sys.exit(f"pa_transporter: chip sector {a:#x} differs from the reference - not writing "
                         f"(--repair writes the reference bytes over it)")
            print(f"sector {a:#x} differs from the reference - repairing")
        crc = zlib.crc32(data) & 0xFFFFFFFF
        s.reset_input_buffer()
        s.write(f"wsec {a:#x} {crc:08X}\n".encode())
        s.write(data)
        s.timeout = 30
        reply = s.readline().decode(errors="replace").strip()
        print(f"wsec {a:#x}: {reply}")
        if reply != "OK":
            sys.exit(f"pa_transporter: write of {a:#x} failed ({reply}). The ROM route is unaffected: "
                     f"power-cycle into UBOOT and run the drill again")
        if read_flash(s, a, SECTOR) != data:
            sys.exit(f"pa_transporter: read-back of {a:#x} differs after the write")
        written += 1
        print(f"sector {a:#x}: erased, written, read back identical  ({written}/{len(sectors)})")
    print(f"drill done: {written} sector(s) written and verified")
    if args.full_verify:
        print(f"full re-read: {len(ref):#x} bytes")
        if not verify_chip(read_flash(s, 0, len(ref)), ref):
            sys.exit("pa_transporter: full re-read differs from the reference")
        print(f"full re-read: 0x0-{PKG_END:#x} identical to {args.ref} (sha256 {sha[:16]}...)")


def cmd_break(args):
    """Recovery test: erase ONE app sector (fill it with 0xFF) so the firmware cannot start.

    Only for proving recovery from a non-booting unit. Refuses unless the reference dump is
    the ledger baseline (so `drill --repair` can put the sector back), the sector is inside
    the app area 0x4000-0xB5000, and the chip currently holds the baseline bytes there.
    Undo: drill REF ADDR --repair.
    """
    if not args.i_understand:
        sys.exit("pa_transporter: break makes the Pocket Amp unbootable until repaired - add --i-understand")
    ref = open(args.ref, "rb").read()
    sha = hashlib.sha256(ref).hexdigest()
    ledger = os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "qa", "hw_ledger.json")
    if sha != json.load(open(ledger, encoding="utf-8"))["recovery"]["full_dump_sha256"]:
        sys.exit(f"pa_transporter: {args.ref} is not the ledger baseline - the sector could not be repaired")
    a = args.sector
    if a % SECTOR or not APP_MIN <= a < PKG_END:
        sys.exit(f"pa_transporter: break only accepts one app sector ({APP_MIN:#x}-{PKG_END:#x}, 4 KiB aligned)")

    console, s = session(args)
    if not uboot_ready(s):
        wait_uboot(s, console, args.timeout)
    print(request(s, "info", timeout=30))
    if read_flash(s, a, SECTOR) != ref[a:a + SECTOR]:
        sys.exit(f"pa_transporter: chip sector {a:#x} is not the baseline - not breaking it")
    blank = b"\xff" * SECTOR
    crc = zlib.crc32(blank) & 0xFFFFFFFF
    s.reset_input_buffer()
    s.write(f"wsec {a:#x} {crc:08X}\n".encode())
    s.write(blank)
    s.timeout = 30
    reply = s.readline().decode(errors="replace").strip()
    print(f"wsec {a:#x}: {reply}")
    if reply != "OK" or read_flash(s, a, SECTOR) != blank:
        sys.exit(f"pa_transporter: break of {a:#x} did not complete ({reply}) - run drill {a:#x} --repair")
    print(f"sector {a:#x} is now blank (0xFF). Undo: drill {args.ref} {a:#x} --repair --full-verify")


REGIONS_ABOVE = [(0xB5000, 0xBD000, "VM"), (0xBD000, 0xBE000, "BTIF"), (0xBE000, 0xBF000, "USER"),
                 (0xBF000, 0xFB000, "USR (AM4 bundle)"), (0xFB000, 0xFF000, "SKPST (presets)"),
                 (0xFF000, 0x100000, "key_mac / settings")]


def verify_chip(chip, expect):
    """True if the package region (0x0-0xB5000, the only part this tool writes) is exactly
    `expect`. Above it the device's own firmware keeps writing (VM settings, presets), so
    changes there since the backup are reported, not failed - this tool cannot write them."""
    if chip[:PKG_END] != expect[:PKG_END]:
        bad = [i for i in range(PKG_END) if chip[i] != expect[i]]
        print(f"  package region differs at {len(bad)} byte(s), first {bad[0]:#x}")
        return False
    for lo, hi, name in REGIONS_ABOVE:
        if chip[lo:hi] != expect[lo:hi]:
            n = sum(1 for i in range(lo, hi) if chip[i] != expect[i])
            print(f"  note: {name} ({lo:#x}-{hi:#x}) changed in {n} byte(s) since the backup - "
                  f"written by the device's own firmware, not by this tool")
    return True


def write_sector(s, a, data):
    """wsec one 4 KiB sector (the firmware erases, writes and compares), then read it back."""
    crc = zlib.crc32(data) & 0xFFFFFFFF
    s.reset_input_buffer()
    s.write(f"wsec {a:#x} {crc:08X}\n".encode())
    s.write(data)
    s.timeout = 30
    reply = s.readline().decode(errors="replace").strip()
    if reply != "OK":
        return f"wsec replied {reply!r}"
    if read_flash(s, a, SECTOR) != data:
        return "read-back differs"
    return None


def cmd_flash(args):
    """Install a packed custom image (.fwsc/.ufw from pack_ufw.py) into the app area only.

    Refuses unless:
      - the backup is the ledger baseline (so `drill --repair` can always put stock back);
      - the image's app area decodes with every CRC consistent (jl_app_area.check_area);
      - the image's MBR + uboot (0x0-0x3FFF) are byte-identical to this unit's backup;
      - each chip sector holds either the backup's bytes or already the image's (a resumed run).
    Writes only 0x4000-0xB4FFF; VM, BTIF, USER, the AM4 bundle, presets and the last sector
    are outside the image and outside the dongle's window. Ends with a full-chip re-read.
    """
    sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
    import jl_app_area as A
    if not args.i_understand:
        sys.exit("pa_transporter: flash replaces the stock app with custom firmware - add --i-understand")
    ref = open(args.ref, "rb").read()
    sha = hashlib.sha256(ref).hexdigest()
    ledger = os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "qa", "hw_ledger.json")
    if sha != json.load(open(ledger, encoding="utf-8"))["recovery"]["full_dump_sha256"]:
        sys.exit(f"pa_transporter: {args.ref} is not the ledger baseline - no verified way back")
    img = A.flash_from_any(open(args.image, "rb").read())
    if len(img) != PKG_END:
        sys.exit(f"pa_transporter: image flash.bin is {len(img):#x} bytes, expected {PKG_END:#x}")
    probs = A.check_area(A.decode_area(img))
    if probs:
        sys.exit("pa_transporter: image app area is inconsistent: " + ", ".join(probs))
    if img[:APP_MIN] != ref[:APP_MIN]:
        sys.exit("pa_transporter: image MBR/uboot differ from this unit's - refusing (boot area is never written)")
    known = [ref]                                   # chip states we may overwrite, sector by sector
    for prev_path in args.over or []:
        prev = A.flash_from_any(open(prev_path, "rb").read())
        if len(prev) != PKG_END or prev[:APP_MIN] != ref[:APP_MIN] or A.check_area(A.decode_area(prev)):
            sys.exit(f"pa_transporter: --over {prev_path} is not a consistent image for this unit")
        known.append(prev)
    sectors = list(range(APP_MIN, PKG_END, SECTOR)) if args.over else \
        [a for a in range(APP_MIN, PKG_END, SECTOR) if img[a:a + SECTOR] != ref[a:a + SECTOR]]
    sectors = [a for a in sectors if any(img[a:a + SECTOR] != k[a:a + SECTOR] for k in known)]
    if not sectors:
        sys.exit("pa_transporter: the image's app area is identical to the backup - nothing to write")
    expect = ref[:APP_MIN] + img[APP_MIN:PKG_END] + ref[PKG_END:]
    print(f"image {args.image}: app area OK, {len(sectors)} sector(s) to write "
          f"({sectors[0]:#x}-{sectors[-1] + SECTOR:#x}); expected chip sha256 {hashlib.sha256(expect).hexdigest()[:16]}")

    console, s = session(args)
    if not uboot_ready(s):
        wait_uboot(s, console, args.timeout)
    info = request(s, "info", timeout=30)
    print(info)
    lo, hi = firmware_window(info)
    if not (lo <= APP_MIN and PKG_END <= hi):
        sys.exit(f"pa_transporter: the Transporter only writes {lo:#x}-{hi:#x}; flash the Pocket Amp build")
    plan = []
    for a in sectors:
        cur = read_flash(s, a, SECTOR)
        if cur == img[a:a + SECTOR]:
            continue                                   # already written by an earlier run
        if not any(cur == k[a:a + SECTOR] for k in known):
            sys.exit(f"pa_transporter: chip sector {a:#x} is neither the backup, an --over image nor the "
                     f"new image - stopping (restore first: drill {args.ref} 0x4000-0xB5000 --repair --full-verify)")
        plan.append(a)
    print(f"flash: writing {len(plan)} sector(s)")
    for n, a in enumerate(plan, 1):
        err = write_sector(s, a, img[a:a + SECTOR])
        if err:
            sys.exit(f"pa_transporter: sector {a:#x}: {err}. Run the same command again to resume, or restore: "
                     f"drill {args.ref} 0x4000-0xB5000 --repair --full-verify")
        if n % 25 == 0 or n == len(plan):
            print(f"  written and verified {n}/{len(plan)}")
    print(f"full re-read: {len(expect):#x} bytes")
    if not verify_chip(read_flash(s, 0, len(expect)), expect):
        sys.exit("pa_transporter: full re-read differs from the expected chip contents")
    print("full re-read: boot area = backup, app area = image")
    print(f"installed. Boot it: runapp (or power-cycle). Back to stock: "
          f"drill {args.ref} 0x4000-0xB5000 --repair --full-verify")


BB_ADDR, BB_LEN, BB_MAGIC = 0x01C7C000, 16 * 4 + 32 * 12 + 64 + 19 * 4, 0x50414242   # layout v5 (v4 + fail_streak, prev_stage, wdt_test)
CRASH_ADDR, CRASH_WORDS, CRASH_MAGIC = 0x01C7C300, 35, 0x52434150   # "PACR", pa_crash_t in main.c
# DBG_MSG bits by core (the Felucca BSP's reading: PC limit, bus and write-limit errors).
DBG_CPU1 = (1 << 6) | (1 << 8) | (1 << 10) | (1 << 11) | (7 << 16)
DBG_CPU0 = (1 << 7) | (1 << 9) | (1 << 12) | (1 << 13) | (7 << 19)


def cmd_crash(args):
    """Read the CPU exception record (src/board/main.c pa_fault_c) from the ROM download mode.

    Read-only, like blackbox: run it before anything uploads the flash loader. The firmware's
    exception handler records the fault and resets into the download mode, keeping the SRAM.
    """
    console, s = session(args)
    if not uboot_ready(s):
        wait_uboot(s, console, args.timeout)
    reply = request(s, f"memr {CRASH_ADDR:#x} 256", timeout=30)     # 0x01C7C300..0x3FF: the record + the marker words
    if not reply.startswith("DATA "):
        sys.exit(f"pa_transporter: memr: {reply} (the flash loader may already be running - power-cycle first)")
    n = int(reply.split()[1])
    s.timeout = 10
    raw = s.read(n)
    end = s.readline().decode(errors="replace").strip()
    if len(raw) != n or end != f"END {zlib.crc32(raw) & 0xFFFFFFFF:08X}":
        sys.exit(f"pa_transporter: crash record transfer check failed ({end})")
    out = os.path.join(args.out_dir, time.strftime("crash_%Y%m%d_%H%M%S.bin"))
    os.makedirs(args.out_dir, exist_ok=True)
    open(out, "wb").write(raw)
    w = struct.unpack_from(f"<{CRASH_WORDS}I", raw, 0)
    mark, cp, isr_cp = struct.unpack_from("<3I", raw, 0xF0)  # 0x01C7C3F0 rescue mark, 3F4 main-loop cp, 3F8 audio-interrupt cp
    print(f"last main-loop step (cp, SRAM 0x01C7C3F4): {cp:#x}   last audio-interrupt step (0x01C7C3F8): {isr_cp:#x}"
          f"   rescue mark: {mark:#x}")
    if w[0] != CRASH_MAGIC:
        print(f"no crash record (magic {w[0]:#010x}): no exception since the last power-on, or an "
              f"image without the handler. Raw bytes saved to {out}")
        return
    names = ("magic", "count", "pc", "rete", "retx", "rets", "psr", "icfg", "usp", "ssp", "sp",
             "emu", "dbg", "uptime_ms", "stage")
    print(f"CPU exception record: {w[1]} fault(s) since the last power-on")
    for i, name in enumerate(names[2:], start=2):
        print(f"  {name:10} {w[i]:#010x}" + (f"  ({w[i]} ms after boot)" if name == "uptime_ms" else ""))
    dbg = w[12]
    if dbg:
        print(f"  DBG_MSG bits: CPU0 group {dbg & DBG_CPU0:#x}, CPU1 group {dbg & DBG_CPU1:#x}"
              " (PC limit / bus / write-limit errors; the BSP's reading, not verified here)")
    print("  last branch targets (ETM): " + " ".join(f"{v:#010x}" for v in w[15:19]))
    print("  r0..r15: " + " ".join(f"{v:08x}" for v in w[19:35]))
    print(f"saved {out}")


BB_STAGES = {0: "init", 1: "gpio", 2: "lcd", 3: "assets", 4: "fx chain", 5: "ui/usb", 6: "settings",
             7: "audio", 8: "main loop"}
BB_EVENTS = {1: "boot", 2: "stage", 3: "buttons", 4: "ota reboot", 5: "alive", 6: "ota spurious",
             7: "stray irq"}


def cmd_blackbox(args):
    """Read the custom firmware's RAM black box (src/board/main.c) from the ROM download mode.

    Must run before anything uploads the flash loader (dump/drill/flash/info do): the loader
    goes into RAM. Read-only: it uses the ROM's memory read and writes nothing.
    """
    console, s = session(args)
    if not uboot_ready(s):
        wait_uboot(s, console, args.timeout)
    reply = request(s, f"memr {BB_ADDR:#x} {BB_LEN}", timeout=30)
    if not reply.startswith("DATA "):
        sys.exit(f"pa_transporter: memr: {reply} (the flash loader may already be running - power-cycle first)")
    n = int(reply.split()[1])
    s.timeout = 10
    raw = s.read(n)
    end = s.readline().decode(errors="replace").strip()
    if len(raw) != n or end != f"END {zlib.crc32(raw) & 0xFFFFFFFF:08X}":
        sys.exit(f"pa_transporter: black box transfer check failed ({end})")
    out = os.path.join(args.out_dir, time.strftime("blackbox_%Y%m%d_%H%M%S.bin"))
    os.makedirs(args.out_dir, exist_ok=True)
    open(out, "wb").write(raw)
    w = struct.unpack_from("<16I", raw, 0)
    if w[0] != BB_MAGIC:
        print(f"no black box (magic {w[0]:08X}): power-on since the last run, an image without the "
              f"overlay, or the ROM reused that RAM. Raw bytes saved to {out}")
        return
    names = ["magic", "version", "boot_count", "last_uptime_ms", "prev_uptime_ms", "loop_count", "stage",
             "ev_head", "porta_in", "porth_in", "portc_in", "btn_state", "warm", "spur_count", "spur_value", "spur_ms"]
    f = dict(zip(names, w))
    if f["version"] >= 3:
        irq = struct.unpack_from("<13I", raw, 16 * 4 + 32 * 12 + 64)
        cnt, first, last, mlo, mhi = irq[:5]
        hit = [i for i in range(32) if mlo >> i & 1] + [32 + i for i in range(32) if mhi >> i & 1]
        en = [i * 8 + j for i, w in enumerate(irq[5:]) for j in range(8) if w >> (4 * j) & 1]
        print(f"stray interrupts: {cnt}" + (f" (first {first}, last {last}, sources {hit})" if cnt else ""))
        print(f"  enabled by the bootloader at app start: {en or 'none'}  "
              f"(bank0 {' '.join(f'{w:08X}' for w in irq[5:])})")
    if f["version"] >= 2 and f["spur_count"]:
        snap = raw[16 * 4 + 32 * 12:]
        print(f"OTA flag corrupted {f['spur_count']} time(s), last value {f['spur_value']:02X}, first at "
              f"{f['spur_ms'] / 1000:.3f} s. g_usb_ble's first 64 bytes then:")
        for o in range(0, 64, 16):
            print(f"    +{o:02x}  {snap[o:o + 16].hex(' ')}")
    print(f"black box v{f['version']}: boot #{f['boot_count']} ({'reset with RAM kept' if f['warm'] else 'power-on'}), "
          f"ran {f['last_uptime_ms'] / 1000:.1f} s, reached stage {f['stage']} ({BB_STAGES.get(f['stage'], '?')}), "
          f"{f['loop_count']} overlay ticks")
    print(f"  previous run lasted {f['prev_uptime_ms'] / 1000:.1f} s")
    print(f"  last raw inputs: PORTA {f['porta_in']:08X}  PORTH {f['porth_in']:08X}  PORTC {f['portc_in']:02X}  "
          f"buttons {f['btn_state']:02X}")
    head = f["ev_head"]
    evs = [struct.unpack_from("<3I", raw, 64 + 12 * (i % 32)) for i in range(max(0, head - 32), head)]
    print(f"  last {len(evs)} of {head} events:")
    for t, code, arg in evs:
        name = BB_EVENTS.get(code, f"code {code}")
        if code == 2:
            arg_s = f"{arg} ({BB_STAGES.get(arg, '?')})"
        elif code == 3:
            arg_s = f"buttons {arg >> 24:02X}, PORTA low {arg & 0xFFFF:04X}"
        else:
            arg_s = str(arg)
        print(f"    {t / 1000:8.3f} s  {name:10s} {arg_s}")
    print(f"saved {out}")


def cmd_runapp(args):
    console, s = session(args)
    print(request(s, "runapp"))


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--timeout", type=int, default=120, help="seconds to wait for the UBOOT session")
    sub = ap.add_subparsers(dest="cmd", required=True)
    sub.add_parser("ports").set_defaults(fn=cmd_ports)
    sub.add_parser("wait").set_defaults(fn=cmd_wait)
    sub.add_parser("info").set_defaults(fn=cmd_info)
    d = sub.add_parser("dump")
    d.add_argument("out_dir")
    d.add_argument("--len", type=lambda v: int(v, 0), default=DEFAULT_LEN)
    d.set_defaults(fn=cmd_dump)
    sub.add_parser("runapp").set_defaults(fn=cmd_runapp)
    r = sub.add_parser("drill", help="restore drill: write sectors back from the verified baseline dump")
    r.add_argument("ref", help="the verified full dump (must match the ledger baseline)")
    r.add_argument("sectors", nargs="+", help="sector addresses or ranges, e.g. 0x92000 or 0x4000-0xB5000")
    r.add_argument("--full-verify", action="store_true", help="re-read the whole chip afterwards")
    r.add_argument("--repair", action="store_true", help="also write over sectors that differ from the reference")
    r.set_defaults(fn=cmd_drill)
    b = sub.add_parser("break", help="recovery test: blank ONE app sector so the unit cannot boot")
    b.add_argument("ref", help="the verified full dump (must match the ledger baseline)")
    b.add_argument("sector", type=lambda v: int(v, 0))
    b.add_argument("--i-understand", action="store_true")
    b.set_defaults(fn=cmd_break)
    k = sub.add_parser("blackbox", help="read the custom firmware's RAM black box (run before anything else)")
    k.add_argument("--out-dir", default=".")
    k.set_defaults(fn=cmd_blackbox)
    c = sub.add_parser("crash", help="read the CPU exception record (run before anything else)")
    c.add_argument("--out-dir", default=".")
    c.set_defaults(fn=cmd_crash)
    f = sub.add_parser("flash", help="install a packed custom image into the app area (0x4000-0xB4FFF)")
    f.add_argument("image", help="Pocket_AMP_Custom.fwsc (or .ufw) from pack_ufw.py")
    f.add_argument("ref", help="the verified full dump (must match the ledger baseline)")
    f.add_argument("--i-understand", action="store_true")
    f.add_argument("--over", action="append", metavar="PREV_IMAGE",
                   help="an image flashed earlier that the chip may hold instead of the backup (repeatable)")
    f.set_defaults(fn=cmd_flash)
    a = ap.parse_args()
    a.fn(a)


if __name__ == "__main__":
    main()
