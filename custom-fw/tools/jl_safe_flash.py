#!/usr/bin/env python3
"""
custom-fw/tools/jl_safe_flash.py - the safe flash writer for the JieLi ROM download mode
(UBOOT1.00), modelled on the FM-1 community's Transporter/unbricker rules.

The Pocket Amp's AC791N (wl82) has a mask-ROM USB download mode that is reachable whatever is
in the flash (TEST-PROTOCOL.md section 4). kagaimiq's jl-uboot-tool speaks its protocol; this
script uses that tool's protocol layer, but none of its free-form commands. It only does:

    info      read the chip key and the flash ID, and check them
    dump      read the whole chip twice, compare, save both reads
    write     put an image into the flash, sector by sector, with read-back verify
    restore   put the stock app area (from the .fwsc) or a full backup back

The rules it enforces, all of them non-negotiable:

  1. IDENTITY  chip key must be 0x980F, and the flash ID must match the one recorded in
               the ledger (once recorded). Wrong chip -> nothing happens.
  2. PROTECTED never erased or written:
                 0x000000-0x003FFF  boot head (uboot.boot, isd_config) - the way back in
                 0x0BD000-0x0BDFFF  BTIF (RF calibration + MAC)        - unique per unit
                 0x0BE000-0x0BEFFF  USER (factory calibration)         - unique per unit
                 last 4 KB sector   key_mac (0xFF000 on 1 MB)           - unique per unit
               and nothing past the end of the chip. An image touching these is refused,
               not trimmed.
  3. BACKUP    a write needs --ref: the unit's full-chip backup. Its SHA-256 must be the one
               recorded in the ledger (qa_recovery.py verify --record), and the flash must
               still match it before anything is written.
  4. SECTORS   only whole 4 KB sectors are erased (no block or chip erase, ever). Each sector
               is erased, written, read back and compared; a bad sector is retried, then the
               run stops. Sectors that already hold the right bytes are skipped, so a stopped
               run is resumed by running the same command again.
  5. FINAL     the whole chip is read back and must equal the expected image byte for byte.
  6. CONSENT   the default is a dry run. --write asks you to type WRITE.
  7. NEVER     chip erase, block erase, chip-key burning and memory pokes are not exposed.

    python custom-fw/tools/jl_safe_flash.py info
    python custom-fw/tools/jl_safe_flash.py dump backups/unit1
    python custom-fw/tools/jl_safe_flash.py write app_area.bin --at 0x4000 --ref backups/unit1/dump1.bin
    python custom-fw/tools/jl_safe_flash.py write app_area.bin --at 0x4000 --ref backups/unit1/dump1.bin --write
    python custom-fw/tools/jl_safe_flash.py restore --stock-fwsc "Pocket AMP.fwsc" --ref backups/unit1/dump1.bin --write
    python custom-fw/tools/jl_safe_flash.py restore --from-dump backups/unit1/dump1.bin --ref backups/unit1/dump1.bin --write

Rehearse everything first with a simulated flash (no device):
    python custom-fw/tools/jl_safe_flash.py --mock-flash dump.bin write ...

The real device needs jl-uboot-tool (MIT, kagaimiq): --jl-tool <dir> or JL_UBOOT_TOOL=<dir>.
On Windows run from an Administrator terminal (raw SCSI access).
"""
from __future__ import annotations

import argparse
import datetime as dt
import hashlib
import json
import os
import sys
import time
from pathlib import Path

TOOLS = Path(__file__).resolve().parent
FW = TOOLS.parent
ROOT = FW.parent
REPORTS = FW / "qa" / "reports"
DEFAULT_LEDGER = FW / "qa" / "hw_ledger.json"

SECTOR = 0x1000
CHIP_KEY = 0x980F
CHIP_FAMILY = "wl82"                     # AC791N
APP_AREA = 0x4000
APP_AREA_END = 0xB5000                   # the stock .fwsc flash.bin covers 0x000000-0x0B4FFF
FIXED_PROTECTED = [
    (0x000000, 0x004000, "boot head (uboot.boot, isd_config) - the way back in"),
    (0x0BD000, 0x0BE000, "BTIF (RF calibration + MAC) - unique to this unit"),
    (0x0BE000, 0x0BF000, "USER (factory calibration) - unique to this unit"),
]
# The owner's captures and cabinets (pa_flash_map.h: 0x86000-0xB4FFF). A WRITE of a custom image never rewrites them: the image is the app area up to 0x86000 only. A RESTORE of the stock firmware does
# (the stock app is longer than the custom one and needs that room), which is why this list is used by plan() and not by the restore.
USER_STORE = (0x086000, 0x0B4000, "user store (stored captures and cabinets)")
IMAGE_WINDOW = (0x004000, 0x086000)      # the app image: replaced as a whole by a custom pack, whatever an earlier pack left in it
RETRIES = 3
# Regions the DEVICE'S OWN firmware keeps rewriting while it runs (settings, presets). They differ
# from a dump taken earlier whatever this tool did, so the "flash still matches the backup" check
# does not count them as drift. They are never written by this tool either: neither is in any plan
# (the image is the app area 0x4000-0xB4FFF) and neither is the bootloader.
DEVICE_WRITTEN = [
    (0x0B5000, 0x0BD000, "VM (settings the stock firmware writes)"),
    (0x0FB000, 0x0FF000, "SKPST (presets the stock firmware writes)"),
]


class Refused(Exception):
    """A safety rule said no. Nothing was written."""


class WriteFailed(Exception):
    """A sector would not verify. The run stopped part-way."""


def protected_ranges(chip_size: int) -> list[tuple[int, int, str]]:
    return FIXED_PROTECTED + [(chip_size - SECTOR, chip_size, "key_mac (last sector) - unique to this unit")]


def sha256(b: bytes) -> str:
    return hashlib.sha256(b).hexdigest()


# ======================================================================================
# Backends: the real ROM download mode, or a simulated flash for rehearsals and tests.
class MockFlash:
    """A NOR flash in memory: erase sets 0xFF, program can only clear bits."""

    def __init__(self, image: bytes, chip_key=CHIP_KEY, flash_id=None, fail_sectors=()):
        self.mem = bytearray(image)
        self.key = chip_key
        size_log2 = max(len(image), 1).bit_length() - 1
        self.id = flash_id if flash_id is not None else (0x856000 | size_log2)
        self.fail = {int(s, 0) if isinstance(s, str) else s for s in fail_sectors}
        self.io = 512
        self.erases: list[int] = []
        self.writes = 0

    def describe(self):
        return f"simulated flash ({len(self.mem):,} B)"

    def chip_key(self):
        return self.key

    def flash_id(self):
        return self.id

    def read(self, addr, n):
        return bytes(self.mem[addr:addr + n])

    def erase_sector(self, addr):
        assert addr % SECTOR == 0
        self.erases.append(addr)
        self.mem[addr:addr + SECTOR] = b"\xff" * SECTOR

    def write(self, addr, data):
        self.writes += 1
        if (addr & ~(SECTOR - 1)) in self.fail:          # a sector that never takes the data
            data = bytes(b & 0xFE for b in data)
        for i, b in enumerate(data):
            self.mem[addr + i] &= b

    def close(self):
        pass


class UbootFlash:
    """The chip in ROM download mode, through jl-uboot-tool's protocol classes."""

    def __init__(self, jl_tool: Path, device: str | None):
        if not (jl_tool / "jltech" / "uboot.py").exists():
            raise SystemExit(f"jl-uboot-tool not found at {jl_tool} (use --jl-tool or JL_UBOOT_TOOL)")
        sys.path.insert(0, str(jl_tool))
        import yaml
        from jltech.uboot import JL_MSCDevice, JL_UBOOT, JL_LoaderV2   # noqa: E402
        data = jl_tool / "data"
        chips = yaml.safe_load(open(data / "chips.yaml"))
        loaders = yaml.safe_load(open(data / "usb-loaders.yaml"))
        if device is None:
            from jldevfind import choose_jl_device
            device = choose_jl_device(venfilter=CHIP_FAMILY)
            if device is None:
                raise SystemExit("no JieLi device in download mode found (see TEST-PROTOCOL.md 4.2 / 4.3)")
        self.path = device
        self.msc = JL_MSCDevice(device)
        self.msc.open()
        vendor, product, rev = self.msc.inquiry()
        if vendor.strip().lower() != CHIP_FAMILY:
            self.msc.close()
            raise Refused(f"the device says it is '{vendor.strip()}', not {CHIP_FAMILY.upper()} (AC791N)")
        self.product = product.strip()
        if self.product == "UBOOT1.00":
            spec = loaders[CHIP_FAMILY]
            quirk = (chips[CHIP_FAMILY].get("uboot1.00", {}).get("quirks", {}) or {}).get("memory-rw-mengli-crypt") is True
            cipher = spec.get("encryption", "none")
            scramble = (not quirk and cipher == "MengLi") or (quirk and cipher != "MengLi")
            if scramble:
                from jltech.cipher import cipher_bytes, jl_crc_cipher
            rom = JL_UBOOT(self.msc.dev)
            blob = (data / spec["file"]).read_bytes()
            addr = spec["address"]
            for off in range(0, len(blob), 512):
                block = blob[off:off + 512]
                rom.mem_write(addr + off, cipher_bytes(jl_crc_cipher, block) if scramble else block)
            rom.mem_jump(spec["address"], 1)      # target 1 = SPI NOR, default clock and SPI mode
        self.dev = JL_LoaderV2(self.msc.dev)
        try:
            self.io = min(int(self.dev.usb_buffer_size()), 512) or 256
        except Exception:
            self.io = 256

    def describe(self):
        return f"{CHIP_FAMILY.upper()} at {self.path} ({self.product})"

    def chip_key(self):
        return self.dev.chip_key()

    def flash_id(self):
        return self.dev.online_device()["id"]

    def read(self, addr, n):
        out = bytearray()
        while n > 0:
            k = min(n, self.io)
            out += self._read_once(addr, k)
            addr += k
            n -= k
        return bytes(out)

    def _read_once(self, addr, k):
        """A flash READ changes nothing, so a transfer error (WinError 31 "a device attached to the system is not functioning" is a USB hiccup)
        is retried a few times with a pause before it is given up on. Erase and write are NOT retried anywhere."""
        last = None
        for attempt in range(6):
            try:
                return self.dev.flash_read(addr, k)
            except Exception as e:  # noqa: BLE001 - the SCSI layer raises its own exception type
                last = e
                time.sleep(0.25 * (attempt + 1))
        raise last

    def erase_sector(self, addr):
        assert addr % SECTOR == 0
        self.dev.flash_erase_sector(addr)

    def write(self, addr, data):
        for off in range(0, len(data), self.io):
            self.dev.flash_write(addr + off, data[off:off + self.io])

    def close(self):
        try:
            self.msc.close()
        except Exception:
            pass


# ======================================================================================
class SafeFlasher:
    def __init__(self, flash, ledger_path: Path, log=print):
        self.f = flash
        self.ledger_path = ledger_path
        self.log = log
        self.size = None

    # ---- rule 1 --------------------------------------------------------------------
    def identify(self) -> dict:
        key = self.f.chip_key()
        fid = self.f.flash_id()
        dens = fid & 0xFF
        size = (1 << dens) if 0x10 <= dens <= 0x1A else None
        led = self._ledger()
        want_id = (led.get("device") or {}).get("flash_jedec")
        want_size = (led.get("device") or {}).get("flash_size_bytes")
        self.log(f"  device   : {self.f.describe()}")
        self.log(f"  chip key : 0x{key:04X}   (required 0x{CHIP_KEY:04X})")
        self.log(f"  flash ID : 0x{fid:06X}  -> {size:,} B" if size else f"  flash ID : 0x{fid:06X}  -> unknown size")
        if key != CHIP_KEY:
            raise Refused(f"chip key 0x{key:04X} is not 0x{CHIP_KEY:04X}: this is not a Pocket Amp's AC791N")
        if size is None:
            raise Refused(f"flash ID 0x{fid:06X} does not encode a usable size - the flash did not answer properly")
        if want_id and int(str(want_id), 0) != fid:
            raise Refused(f"flash ID 0x{fid:06X} is not the 0x{int(str(want_id), 0):06X} recorded for this unit")
        if want_size and int(want_size) != size:
            raise Refused(f"flash size {size:,} B is not the {int(want_size):,} B recorded for this unit")
        self.size = size
        return {"chip_key": key, "flash_id": fid, "size": size}

    # ---- reading -------------------------------------------------------------------
    def read_all(self, label="reading") -> bytes:
        out = bytearray()
        step = 0x10000
        for a in range(0, self.size, step):
            out += self.f.read(a, min(step, self.size - a))
            self.log(f"\r  {label} {a + step:>8,} / {self.size:,}", end="")
        self.log("")
        return bytes(out)

    def dump(self, outdir: Path) -> tuple[Path, Path, bool]:
        outdir.mkdir(parents=True, exist_ok=True)
        d1 = self.read_all("read 1")
        d2 = self.read_all("read 2")
        p1, p2 = outdir / "dump1.bin", outdir / "dump2.bin"
        p1.write_bytes(d1)
        p2.write_bytes(d2)
        same = d1 == d2
        self.log(f"  dump1 {sha256(d1)}\n  dump2 {sha256(d2)}\n  {'identical' if same else 'DIFFERENT - do not use either'}")
        if same:
            led = self._ledger()
            dev = led.setdefault("device", {})
            dev["flash_jedec"] = f"0x{self.f.flash_id():06X}"
            self._save_ledger(led)
        return p1, p2, same

    # ---- rules 2-6 -----------------------------------------------------------------
    def _check_ref(self, ref: bytes, expected: bytes) -> None:
        if len(ref) != self.size or len(expected) != self.size:
            raise Refused(f"the backup is {len(ref):,} B but the chip is {self.size:,} B")
        led = self._ledger()
        rec_sha = (led.get("recovery") or {}).get("full_dump_sha256")
        if not rec_sha:
            raise Refused("no full-chip backup is recorded in the ledger - run qa_recovery.py verify --record first")
        if sha256(ref) != rec_sha:
            raise Refused("--ref is not the backup recorded in the ledger "
                          f"({sha256(ref)[:16]} vs {rec_sha[:16]})")

    def plan_restore(self, ref: bytes, expected: bytes, device_areas: bool = False) -> list[int]:
        """The sectors a RESTORE has to rewrite: those where the chip, as it is NOW, differs from the image to restore.

        plan() compares the new image with the backup, which is right for putting a new image on a unit that still holds the backup. A restore
        puts the backup (or the stock image, which is the same bytes) back on a unit that holds something else, so the sectors to write are
        the ones where the CHIP differs from it: planned against the backup it would be empty and the command would report "nothing to do"
        without writing a byte (found 2026-11-02 on a mock flash that held a custom image). Protected sectors and the areas the device writes
        itself are never part of it - unless device_areas is set (restore --with-device-areas, the owner's explicit choice): then the sectors of DEVICE_WRITTEN
        (the stock's settings log 0xB5000-0xBCFFF and its saved presets 0xFB000-0xFEFFF) that differ from the dump are rewritten too. The protected ranges stay protected."""
        self._check_ref(ref, expected)
        now = self.read_all("reading the flash to see what the restore has to change")
        prot = protected_ranges(self.size)
        out = []
        for a in range(0, self.size, SECTOR):
            if now[a:a + SECTOR] == expected[a:a + SECTOR]:
                continue
            if any(lo <= a < hi for lo, hi, _ in prot):
                continue
            if any(lo <= a < hi for lo, hi, _ in DEVICE_WRITTEN):
                if device_areas:
                    out.append(a)
                continue
            # Only the app window, and any sector where the image to restore deliberately differs from the baseline (a restore from a different
            # dump). A stray change anywhere else is not "fixed" silently: check_flash_matches() reports it as drift and the run is refused.
            if APP_AREA <= a < APP_AREA_END or expected[a:a + SECTOR] != ref[a:a + SECTOR]:
                out.append(a)
        return out

    def plan(self, ref: bytes, expected: bytes) -> list[int]:
        """The sectors whose content must change. Refuses anything that breaks a rule."""
        self._check_ref(ref, expected)
        sectors = [a for a in range(0, self.size, SECTOR) if expected[a:a + SECTOR] != ref[a:a + SECTOR]]
        for lo, hi, what in protected_ranges(self.size) + [USER_STORE]:
            bad = [a for a in sectors if lo <= a < hi]
            if bad:
                raise Refused(f"the image changes {len(bad)} sector(s) of the {what} "
                              f"(0x{lo:06X}-0x{hi - 1:06X}); refusing, nothing written")
        return sectors

    def check_flash_matches(self, ref: bytes, expected: bytes, planned: list[int], restore: bool = False) -> list[int]:
        """Read the whole chip and decide what is still to do.

        Every sector OUTSIDE the plan must still hold exactly the backup's bytes - that is
        what proves the backup still describes this flash. A sector INSIDE the plan may hold
        anything (the old bytes, the new ones, or the half-written result of a stopped run),
        because it is erased and rewritten anyway. So a stopped run is resumed by running the
        same command again, and only the sectors that do not hold their new bytes yet are
        rewritten.
        """
        now = self.read_all("checking the flash against the backup")
        # The app image window is the part that is being REPLACED: whatever an earlier custom pack left there (it can be longer or shorter than this one) is not drift. The backup stays the
        # reference for everything else, and it still holds the original firmware this window can be restored from. So a custom-to-custom update needs no new backup.
        lo_w, hi_w = (APP_AREA, APP_AREA_END) if restore else IMAGE_WINDOW
        extra = [a for a in range(lo_w, hi_w, SECTOR) if a not in set(planned) and now[a:a + SECTOR] != expected[a:a + SECTOR]
                 and not any(lo <= a < hi for lo, hi, _ in protected_ranges(self.size))]
        if extra:
            self.log(f"  {len(extra)} more sector(s) of the app window hold an earlier image and are rewritten too (first 0x{extra[0]:06X})")
            planned = sorted(set(planned) | set(extra))
        plan = set(planned)
        drift = [a for a in range(0, self.size, SECTOR)
                 if a not in plan and now[a:a + SECTOR] != ref[a:a + SECTOR]
                 and not any(lo <= a < hi for lo, hi, _ in DEVICE_WRITTEN)
                 and a != 0xB4000       # the tail of the app partition: preserved's own loader rewrites it when the original firmware is restored, so a backup taken before that differs
                 and not (restore and now[a:a + SECTOR] == expected[a:a + SECTOR])       # a restore: a sector that already holds the bytes being restored is not drift (a unit restored by the web editor, then topped up here)
                 and not (not restore and USER_STORE[0] <= a < USER_STORE[1])]       # the owner's captures and cabinets: never compared, never written
        if drift:
            raise Refused(f"the flash no longer matches the backup outside the sectors this run writes "
                          f"({len(drift)} sector(s), first 0x{drift[0]:06X}). Dump again and record a new "
                          "backup before writing.")
        todo = [a for a in planned if now[a:a + SECTOR] != expected[a:a + SECTOR]]
        if len(todo) < len(planned):
            self.log(f"  {len(planned) - len(todo)} sector(s) already hold their new bytes (resumed run)")
        return todo

    def apply(self, ref: bytes, expected: bytes, do_write: bool, confirm, restore: bool = False, device_areas: bool = False) -> dict:
        sectors = self.plan_restore(ref, expected, device_areas) if restore else self.plan(ref, expected)
        self.log(f"\n  plan: {len(sectors)} sector(s) to rewrite"
                 + (f", 0x{sectors[0]:06X}-0x{sectors[-1] + SECTOR - 1:06X}" if sectors else ""))
        if not sectors:
            self.log("  the flash already holds this image - nothing to do")
            return {"result": "nothing to do", "sectors": 0}
        sectors = self.check_flash_matches(ref, expected, sectors, restore)   # a read: done for dry runs too
        if not sectors:
            self.log("  the flash already holds this image - nothing to do")
            return {"result": "nothing to do", "sectors": 0}
        if not do_write:
            self.log(f"  DRY RUN - {len(sectors)} sector(s) would be rewritten; nothing was written. "
                     "Add --write to do it.")
            return {"result": "dry run", "sectors": len(sectors)}
        if not confirm(len(sectors), sha256(expected)):
            raise Refused("not confirmed - nothing written")

        journal = self._journal_start(expected, sectors)
        done = []
        try:
            for i, a in enumerate(sectors, 1):
                want = expected[a:a + SECTOR]
                for attempt in range(1, RETRIES + 1):
                    self.f.erase_sector(a)
                    if want != b"\xff" * SECTOR:
                        self.f.write(a, want)
                    if self.f.read(a, SECTOR) == want:
                        break
                    self.log(f"\n  sector 0x{a:06X} did not verify (attempt {attempt}/{RETRIES})")
                else:
                    raise WriteFailed(f"sector 0x{a:06X} would not verify after {RETRIES} attempts")
                done.append(a)
                self.log(f"\r  written and verified {i}/{len(sectors)}  (0x{a:06X})", end="")
            self.log("")
            final = self.read_all("final read-back")
            if final != expected:
                bad = sorted({x // SECTOR * SECTOR for x in range(self.size) if final[x] != expected[x]})
                # The device's own firmware rewrites its settings areas whenever it runs: a difference
                # there is not a failed write (this tool never writes them). Say so, but do not fail.
                exempt = [a for a in bad if (not device_areas and any(lo <= a < hi for lo, hi, _ in DEVICE_WRITTEN)) or (not restore and USER_STORE[0] <= a < USER_STORE[1])]       # the owner's store is never written by a custom pack
                bad = [a for a in bad if a not in exempt]
                if bad:
                    raise WriteFailed(f"final read-back differs in {len(bad)} sector(s), first 0x{bad[0]:06X}")
                self.log(f"  note: {len(exempt)} sector(s) differ in areas the device's own firmware writes "
                         f"(first 0x{exempt[0]:06X}); not written by this tool, not a failure")
            self.log(f"  final read-back matches the expected image everywhere this tool writes  sha256 {sha256(final)}")
            self._journal_end(journal, done, "ok")
            return {"result": "ok", "sectors": len(sectors), "sha256": sha256(final)}
        except WriteFailed as e:
            self._journal_end(journal, done, f"FAILED: {e}")
            self.log(f"\n  !! {e}\n  !! Do NOT power off or unplug. Run the same command again: it skips the "
                     f"{len(done)} sector(s) already done.\n  !! If it keeps failing, restore with "
                     "'restore --from-dump <your backup>' while still in download mode.")
            raise

    # ---- images ---------------------------------------------------------------------
    @staticmethod
    def overlay(ref: bytes, image: bytes, at: int) -> bytes:
        if at < 0 or at + len(image) > len(ref):
            raise Refused(f"the image (0x{at:06X}+{len(image):#x}) runs past the end of the chip")
        out = bytearray(ref)
        out[at:at + len(image)] = image
        return bytes(out)

    @staticmethod
    def check_app_area_image(expected: bytes, at: int, length: int) -> None:
        """A write that covers the app-area head (flash 0x4000) must leave an app area the boot code can load: the JieLi file table, encoded with JieLi SFC block encoding, every
        CRC consistent (tools/jl_app_area.py). A raw app.bin (build_app.bin) is not one: pack 157 was written that way, the unit then showed a black screen and enumerated
        nothing. The app area of a build is in its .fwsc: `jl_app_area.py inspect` it, or extract it as the packs' app_area_*.bin files were."""
        if not (at <= APP_AREA < at + length):
            return
        sys.path.insert(0, str(TOOLS))
        import jl_app_area as A
        try:
            bad = A.check_area(A.decode_area(expected))
        except Exception as e:  # noqa: BLE001 - any decoding failure means "not an app area"
            raise Refused(f"the image does not decode as a Pocket Amp app area ({e}); it is probably a raw app.bin, not the scrambled app area from the .fwsc. Nothing written")
        if bad:
            raise Refused("the app area this write would leave fails its own checks (" + "; ".join(bad[:3]) + "). Nothing written")

    def stock_app_area(self, ref: bytes, fwsc: Path) -> bytes:
        sys.path.insert(0, str(TOOLS))
        import qa_recovery
        stock = qa_recovery.stock_flash(fwsc)
        if stock is None:
            raise Refused(f"no flash.bin in {fwsc}")
        return self.overlay(ref, stock[APP_AREA:APP_AREA_END], APP_AREA)

    def from_dump(self, ref: bytes, dump: bytes) -> bytes:
        """A full backup, minus the protected sectors (they keep what the flash has now)."""
        if len(dump) != self.size:
            raise Refused(f"the dump is {len(dump):,} B, the chip {self.size:,} B")
        out = bytearray(dump)
        for lo, hi, what in protected_ranges(self.size):
            if dump[lo:hi] != ref[lo:hi]:
                self.log(f"  note: the dump's {what} differs from the flash; that part is NOT written")
            out[lo:hi] = ref[lo:hi]
        return bytes(out)

    # ---- ledger / journal -------------------------------------------------------------
    def _ledger(self) -> dict:
        try:
            return json.loads(self.ledger_path.read_text(encoding="utf-8"))
        except FileNotFoundError:
            return {}

    def _save_ledger(self, led: dict) -> None:
        tmp = self.ledger_path.with_suffix(".json.tmp")
        tmp.write_text(json.dumps(led, indent=2, ensure_ascii=False) + "\n", encoding="utf-8")
        tmp.replace(self.ledger_path)

    def _journal_start(self, expected: bytes, sectors: list[int]) -> Path:
        REPORTS.mkdir(parents=True, exist_ok=True)
        p = REPORTS / f"flash_journal_{dt.datetime.now():%Y%m%d-%H%M%S}.json"
        p.write_text(json.dumps({"started": dt.datetime.now().isoformat(timespec="seconds"),
                                 "device": self.f.describe(), "expected_sha256": sha256(expected),
                                 "sectors": [f"0x{a:06X}" for a in sectors], "done": [], "result": "running"},
                                indent=2), encoding="utf-8")
        return p

    def _journal_end(self, p: Path, done: list[int], result: str) -> None:
        j = json.loads(p.read_text(encoding="utf-8"))
        j.update({"done": [f"0x{a:06X}" for a in done], "result": result,
                  "ended": dt.datetime.now().isoformat(timespec="seconds")})
        p.write_text(json.dumps(j, indent=2), encoding="utf-8")
        led = self._ledger()
        if led:
            led.setdefault("recovery", {}).setdefault("flash_log", []).append(
                {"date": j["ended"], "expected_sha256": j["expected_sha256"],
                 "sectors": len(j["sectors"]), "done": len(done), "result": result, "journal": p.name})
            self._save_ledger(led)


# ======================================================================================
def typed_confirm(sectors: int, expected_sha: str) -> bool:
    print(f"\n  About to erase and rewrite {sectors} sector(s). Expected result sha256 {expected_sha[:16]}...")
    print("  Battery charged? USB straight into the PC? Do not unplug until it says done.")
    return input("  Type WRITE to proceed: ").strip() == "WRITE"


def main() -> int:
    ap = argparse.ArgumentParser(description="Safe flash writer for the JieLi ROM download mode (Pocket Amp / AC791N)")
    ap.add_argument("--jl-tool", default=os.environ.get("JL_UBOOT_TOOL") or
                    str(Path.home() / "Downloads" / "jl-uboot-tool-main" / "jl-uboot-tool-main"))
    ap.add_argument("--device", help="download-mode disk, e.g. \\\\.\\E: (default: ask jldevfind)")
    ap.add_argument("--ledger", default=str(DEFAULT_LEDGER), help="hardware ledger (default: custom-fw/qa/hw_ledger.json)")
    ap.add_argument("--mock-flash", help="REHEARSAL: use a simulated flash seeded from this dump (no device)")
    ap.add_argument("--mock-chip-key", type=lambda s: int(s, 0), default=CHIP_KEY, help=argparse.SUPPRESS)
    ap.add_argument("--mock-flash-id", type=lambda s: int(s, 0), help=argparse.SUPPRESS)
    ap.add_argument("--mock-fail-sector", action="append", default=[], help=argparse.SUPPRESS)
    ap.add_argument("--mock-save", help="REHEARSAL: write the simulated flash here afterwards")
    ap.add_argument("--confirm", help="non-interactive confirmation: must be exactly WRITE")
    sub = ap.add_subparsers(dest="cmd", required=True)
    sub.add_parser("info", help="read and check the chip key and flash ID")
    d = sub.add_parser("dump", help="read the whole chip twice and save both reads")
    d.add_argument("outdir")
    w = sub.add_parser("write", help="write an image at an address (dry run unless --write)")
    w.add_argument("image")
    w.add_argument("--at", required=True, type=lambda s: int(s, 0))
    w.add_argument("--ref", required=True, help="this unit's recorded full-chip backup")
    w.add_argument("--write", action="store_true")
    r = sub.add_parser("restore", help="restore the stock app area or a full backup (dry run unless --write)")
    src = r.add_mutually_exclusive_group(required=True)
    src.add_argument("--stock-fwsc", help="the stock 'Pocket AMP.fwsc': restores flash 0x4000-0xB4FFF")
    src.add_argument("--from-dump", help="a full-chip backup of THIS unit")
    r.add_argument("--ref", required=True, help="this unit's recorded full-chip backup")
    r.add_argument("--with-device-areas", action="store_true", help="ALSO put back the stock's settings log (0xB5000-0xBCFFF) and saved presets (0xFB000-0xFEFFF) from the dump; the boot area, BTIF, USER and key sectors stay protected")
    r.add_argument("--write", action="store_true")
    a = ap.parse_args()

    # Verify Administrator privileges for raw SCSI access
    if not a.mock_flash and os.name == "nt":
        import ctypes
        if not ctypes.windll.shell32.IsUserAnAdmin():
            print("[!] WARNING: Windows detected without Administrator privileges. Raw SCSI access to UBOOT 1.00 may hang.")

    try:
        if a.mock_flash:
            flash = MockFlash(Path(a.mock_flash).read_bytes(), a.mock_chip_key, a.mock_flash_id, a.mock_fail_sector)
        else:
            flash = UbootFlash(Path(a.jl_tool), a.device)
    except Refused as e:
        print(f"\nREFUSED: {e}")
        return 3
    sf = SafeFlasher(flash, Path(a.ledger))
    code = 0
    try:
        print("identity:")
        sf.identify()
        if a.cmd == "info":
            pass
        elif a.cmd == "dump":
            _, _, same = sf.dump(Path(a.outdir))
            print("\nnext: python custom-fw/tools/qa_recovery.py verify "
                  f"{Path(a.outdir) / 'dump1.bin'} {Path(a.outdir) / 'dump2.bin'} --chip \"<marking>\" --record")
            code = 0 if same else 1
        else:
            ref = Path(a.ref).read_bytes()
            if a.cmd == "write":
                image = Path(a.image).read_bytes()
                expected = sf.overlay(ref, image, a.at)
                sf.check_app_area_image(expected, a.at, len(image))
            elif a.stock_fwsc:
                expected = sf.stock_app_area(ref, Path(a.stock_fwsc))
            else:
                expected = sf.from_dump(ref, Path(a.from_dump).read_bytes())
            if a.confirm is not None:
                confirm = (lambda n, s: a.confirm == "WRITE")
            else:
                confirm = typed_confirm
            res = sf.apply(ref, expected, a.write, confirm, restore=(a.cmd == "restore"), device_areas=bool(getattr(a, "with_device_areas", False)))
            print(f"\nresult: {res['result']}")
    except Refused as e:
        print(f"\nREFUSED: {e}")
        code = 3
    except WriteFailed:
        code = 4
    finally:
        if a.mock_flash and a.mock_save:
            Path(a.mock_save).write_bytes(bytes(flash.mem))
        flash.close()
    return code


if __name__ == "__main__":
    sys.exit(main())

# SPDX-License-Identifier: GPL-3.0-only
# Copyright (C) 2026 The Onyx Authors (@28spawn)
