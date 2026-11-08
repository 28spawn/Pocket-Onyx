#!/usr/bin/env python3
"""
custom-fw/tools/flash_unit.py - the whole flashing process in the order it must happen, one command (it only calls tools/jl_safe_flash.py, so every rule of that tool applies):

    0  command    --enter-download-mode sends the "enter upgrade" frame to the RUNNING unit over USB MIDI: our firmware answers it by resetting into the chip's ROM download mode (the disk
                  "WL82 UBOOT1.00", black screen). A unit that is already in the download mode needs no command.
    1  identity   the chip key and the flash ID are read and checked.
    2  dump       the WHOLE chip is read twice and compared (a snapshot of what the unit holds now, saved with a time stamp: your way back to exactly this state). The reference for the write is
                  the recorded ORIGINAL backup (--ref); the first time ever, this dump is that backup and is recorded in the ledger.
    3  dry run    the plan: which sectors would be rewritten, nothing is written.
    4  flash      you type WRITE; every sector is erased, written and read back.
    5  confirm    the whole chip is read back and must equal the expected image everywhere the tool writes; the result and the new chip's checksum are printed.

    python custom-fw/tools/flash_unit.py [--image custom-fw/Pocket_AMP_Custom_app_area.bin] [--ref <the original backup dump1.bin>] [--enter-download-mode]
    python custom-fw/tools/flash_unit.py --mock-flash <a dump>        a rehearsal with a simulated flash (no device, no command)

Run it in an Administrator terminal on Windows (raw disk access). --image takes the .fwsc the build writes, the app area .bin, or the ORIGINAL M-VAVE "Pocket AMP.fwsc": the file decides what happens.
A custom file is written as the app area (0x4000..0x85FFF, your stored captures and cabinets untouched). The original .fwsc RESTORES the stock firmware: it rewrites 0x4000..0xB4FFF (the stock app is
longer than ours), which erases your stored captures and cabinets, so it asks for a second confirmation and the pre-flash snapshot is the way to get them back. Anything else is refused.
"""
import argparse, datetime as dt, os, subprocess, sys, time
from pathlib import Path

TOOLS = Path(__file__).resolve().parent
FW = TOOLS.parent
SAFE = TOOLS / "jl_safe_flash.py"
ENTER_UPGRADE = [0xF0, 0x22, 0x24, 0x35, 0x7D, 0xF7]   # 7D: the soft key into the ROM download mode (7F starts the MIDI update session)


def classify(path: Path):
    """-> ("custom", area_bytes, how) | ("stock", None, how). Refuses anything else."""
    sys.path.insert(0, str(TOOLS))
    import jl_app_area as J, pack_ufw as P
    blob = path.read_bytes()
    if len(blob) == P.APP_IMAGE_SIZE:
        area, how = blob, "the app area image"
    else:
        try:
            flash = J.flash_from_any(blob)
        except Exception as e:
            sys.exit(f"{path.name}: not a firmware file this tool knows ({e}). Use the .fwsc from the build, Pocket_AMP_Custom_app_area.bin, or the original Pocket AMP.fwsc")
        flash = flash[0] if isinstance(flash, tuple) else flash
        area, how = bytes(flash[0x4000:0x4000 + P.APP_IMAGE_SIZE]), "a firmware container"
        if len(flash) < 0xB5000:
            sys.exit(f"{path.name}: the flash image inside is only {len(flash):,} bytes")
        if b"RCREC1:PA:" not in J.scramble(area, 0x4000):
            return "stock", blob, "the original M-VAVE firmware (no custom recovery marker)"
    if b"RCREC1:PA:" not in J.scramble(area, 0x4000):
        sys.exit(f"{path.name} is neither our firmware nor a firmware container: refusing")
    return "custom", area, how


def run(args, passthrough, capture=False):
    cmd = [sys.executable, str(SAFE)] + passthrough + args
    print("\n$ " + " ".join(cmd[1:]))
    return subprocess.run(cmd, capture_output=capture, text=True)


def enter_download_mode():
    try:
        import rtmidi
    except ImportError:
        sys.exit("python-rtmidi is needed to send the command (pip install python-rtmidi), or enter the download mode by hand (HOME + PARA held 2 s)")
    mo = rtmidi.MidiOut()
    port = next((i for i, n in enumerate(mo.get_ports()) if "custom midi" in n.lower() or "pocket" in n.lower()), None)
    if port is None:
        sys.exit("no MIDI port of the unit: is it on USB and running the custom firmware? (a unit already in the download mode needs no command)")
    print("  sending the enter-upgrade frame to", mo.get_ports()[port], "- the unit resets into the ROM download mode and leaves USB MIDI")
    mo.open_port(port)
    mo.send_message(ENTER_UPGRADE)
    time.sleep(0.5)
    mo.close_port()


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--image", default=str(FW / "Pocket_AMP_Custom_app_area.bin"), help="the app area image (532,480 bytes) the build writes")
    ap.add_argument("--ref", default=None, help="the ORIGINAL full-chip backup of this unit (default: <backup-dir>/original/dump1.bin)")
    ap.add_argument("--backup-dir", default=str(FW / "qa" / "backups" / "unit_snapshots"), help="where the pre-flash snapshots are saved")
    ap.add_argument("--enter-download-mode", action="store_true", help="step 0: send the command to the running unit first")
    ap.add_argument("--factory-dump", default=None, help="the unit's ORIGINAL full-chip dump (default qa/backups/unit1_v105/dump1.bin): a restore of the original firmware puts EVERYTHING back from it - app area, the stock's settings log and saved presets - so the unit is as it left the factory")
    ap.add_argument("--skip-snapshot", action="store_true", help="skip the pre-flash dump (the recorded original backup is still the reference)")
    ap.add_argument("--jl-tool"); ap.add_argument("--device"); ap.add_argument("--ledger"); ap.add_argument("--mock-flash"); ap.add_argument("--mock-save"); ap.add_argument("--mock-confirm", help="rehearsal only (needs --mock-flash): pre-typed answer for the writer's WRITE prompt")
    a = ap.parse_args()
    pt = []
    for k, v in (("--jl-tool", a.jl_tool), ("--device", a.device), ("--ledger", a.ledger), ("--mock-flash", a.mock_flash), ("--mock-save", a.mock_save), ("--confirm", a.mock_confirm if a.mock_flash else None)):
        if v:
            pt += [k, v]
    image = Path(a.image)
    if not image.exists():
        sys.exit(f"the file {image} does not exist: build first (powershell -File custom-fw/build.ps1); it writes Pocket_AMP_Custom.fwsc and Pocket_AMP_Custom_app_area.bin")
    kind, payload, how = classify(image)
    print(f"{image.name}: {how} -> {'WRITE the custom app area' if kind == 'custom' else 'RESTORE the original firmware'}")
    if kind == "custom":
        import tempfile
        tmpimg = Path(tempfile.mkdtemp(prefix="pa_flash_")) / "app_area.bin"
        tmpimg.write_bytes(payload)
        image = tmpimg
    ref = Path(a.ref) if a.ref else Path(a.backup_dir) / "original" / "dump1.bin"
    stamp = dt.datetime.now().strftime("%Y%m%d-%H%M%S")

    print("=== 0. command ===")
    if a.enter_download_mode and not a.mock_flash:
        enter_download_mode()
        for _ in range(20):
            time.sleep(2)
            r = run(["info"], pt, capture=True)
            if r.returncode == 0:
                break
        else:
            sys.exit("the download-mode disk did not appear within 40 s: replug the USB cable (a unit in the download mode stays there while powered) and run this again without --enter-download-mode")
    else:
        print("  nothing to send")

    print("\n=== 1. identity ===")
    r = run(["info"], pt)
    if r.returncode:
        return r.returncode

    print("\n=== 2. dump ===")
    first_time = not ref.exists()
    snap = Path(a.backup_dir) / (f"before_{stamp}" if not first_time else "original")
    if a.skip_snapshot and not first_time:
        print("  skipped (--skip-snapshot)")
    else:
        snap.mkdir(parents=True, exist_ok=True)
        r = run(["dump", str(snap)], pt)
        if r.returncode:
            return r.returncode
        if first_time:
            print(f"\n  FIRST TIME: this dump of the unit is the ORIGINAL backup ({snap / 'dump1.bin'}). Record it so the writer accepts it:")
            print(f"      python custom-fw/tools/qa_recovery.py verify {snap / 'dump1.bin'} {snap / 'dump2.bin'} --chip \"XM25QH80B\" --record")
            print("  then run this command again. Copy the dump off this PC.")
            return 0
        print(f"  snapshot saved in {snap}")

    if kind == "custom":
        verb = ["write", str(image), "--at", "0x4000", "--ref", str(ref)]
    else:
        fd = Path(a.factory_dump) if a.factory_dump else FW / "qa" / "backups" / "unit1_v105" / "dump1.bin"
        if fd.exists():
            sys.path.insert(0, str(TOOLS))
            import jl_app_area as J
            stock_flash = J.flash_from_any(Path(a.image).read_bytes()); stock_flash = stock_flash[0] if isinstance(stock_flash, tuple) else stock_flash
            dump = fd.read_bytes()
            if len(dump) != 0x100000 or dump[0x4000:0xB5000] != bytes(stock_flash[0x4000:0xB5000]):
                sys.exit(f"{fd}: its app area is not the one of the original package {Path(a.image).name}: it is not this unit's FACTORY dump. Pass the right one with --factory-dump (the dump of the original state, taken before the first custom flash).")
            print(f"  factory dump {fd.name}: its app area equals the original package; the stock's settings log and saved presets come from it too")
            verb = ["restore", "--from-dump", str(fd), "--ref", str(ref), "--with-device-areas"]
        else:
            print("  !! no factory dump found: only the app area can be restored; the stock's settings and presets stay as they are now (pass --factory-dump <the original dump>)")
            verb = ["restore", "--stock-fwsc", str(Path(a.image)), "--ref", str(ref)]
    print("\n=== 3. dry run (nothing is written) ===")
    r = run(verb, pt)
    if r.returncode:
        return r.returncode
    if kind == "stock":
        print("\n  !! This puts the ORIGINAL firmware back AND everything our firmware changed (the settings log and the saved presets too): your stored captures and cabinets (0x86000-0xB3FFF) are ERASED, and the unit is as it left the factory.")
        print("  !! The snapshot taken above is how you get them back. The boot area and the radio / factory calibration are not touched.")
        if input("  Type RESTORE to go on: ").strip() != "RESTORE":
            print("  not confirmed: nothing was written")
            return 1

    print("\n=== 4. flash ===")
    r = run(verb + ["--write"], pt)
    if r.returncode:
        print("\n  NOT done. Do not power off or unplug: run this command again, it resumes (sectors already done are skipped).")
        return r.returncode

    print("\n=== 5. confirm ===")
    print("  The writer read the whole chip back and it matches the expected image everywhere it writes (see the sha256 above).")
    if kind == "stock":
        print("  The original firmware is back: power-cycle the unit. (The custom firmware's editor and USB MIDI control do not exist on it.)")
    print("  Now power-cycle the unit (unplug USB, hold nothing), plug it in, and check it: the editor's state read, or python custom-fw/tools/ble_diagnose_pc.py (it prints the build id).")
    return 0


if __name__ == "__main__":
    sys.exit(main())

# SPDX-License-Identifier: GPL-3.0-only
# Copyright (C) 2026 The Onyx Authors (@28spawn)
