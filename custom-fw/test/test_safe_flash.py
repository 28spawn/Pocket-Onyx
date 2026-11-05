#!/usr/bin/env python3
"""
custom-fw/test/test_safe_flash.py - every safety rule of tools/jl_safe_flash.py, on a
simulated flash. No device, no network, nothing outside a temp directory is touched.

    python custom-fw/test/test_safe_flash.py

The QA gate runs this (check U-03), so a patch that weakens the writer fails the gate.
"""
import hashlib
import json
import os
import sys
import tempfile
from pathlib import Path

HERE = Path(__file__).resolve().parent
TOOLS = HERE.parent / "tools"
sys.path.insert(0, str(TOOLS))
import jl_safe_flash as J  # noqa: E402

fails = 0


def check(ok, msg):
    global fails
    print(("  ok   " if ok else "  FAIL ") + msg)
    if not ok:
        fails += 1


def expect_refused(fn, msg):
    try:
        fn()
    except J.Refused as e:
        check(True, f"{msg}  [{str(e)[:70]}]")
        return
    except Exception as e:  # noqa: BLE001
        check(False, f"{msg}: raised {type(e).__name__} instead of Refused: {e}")
        return
    check(False, f"{msg}: was NOT refused")


def base_image(size=0x100000) -> bytes:
    """A 1 MB flash: the stock .fwsc image when it is on this machine, else random data."""
    img = bytearray(os.urandom(size))
    try:
        import qa_recovery
        st = qa_recovery.stock_flash(qa_recovery.default_stock())
        if st:
            img[:len(st)] = st
    except Exception:
        pass
    img[0xBD000:0xBF000] = os.urandom(0x2000)           # this unit's calibration
    img[size - 0x1000:] = os.urandom(0x1000)             # this unit's key_mac
    return bytes(img)


def main() -> int:
    tmp = Path(tempfile.mkdtemp(prefix="pa_safeflash_"))
    J.REPORTS = tmp / "reports"
    ledger = tmp / "ledger.json"
    ref = base_image()
    ref_sha = hashlib.sha256(ref).hexdigest()

    def set_ledger(**dev):
        led = {"device": {"flash_size_bytes": len(ref), **dev},
               "recovery": {"full_dump_sha256": ref_sha, "dumps_identical": 2}}
        ledger.write_text(json.dumps(led))

    def rig(image=ref, **kw):
        f = J.MockFlash(image, **kw)
        s = J.SafeFlasher(f, ledger, log=lambda *a, **k: None)
        return f, s

    yes = lambda n, s: True   # noqa: E731
    set_ledger()
    app = os.urandom(0x20000)                     # a 128 KB "app" written at 0x4120 (not sector-aligned)

    print("[1] identity")
    f, s = rig(chip_key=0x1234)
    expect_refused(s.identify, "a chip key other than 0x980F is refused")
    f, s = rig(flash_id=0x856013)
    set_ledger(flash_jedec="0x856014")
    expect_refused(s.identify, "a flash ID different from the recorded one is refused")
    f, s = rig(flash_id=0xFFFFFF)
    set_ledger()
    expect_refused(s.identify, "a flash ID with no usable size is refused")
    f, s = rig()
    info = s.identify()
    check(info["size"] == 0x100000 and info["chip_key"] == 0x980F, "the right chip identifies as 1 MB, key 0x980F")

    print("[2] the backup")
    ledger.write_text(json.dumps({"device": {}, "recovery": {}}))
    f, s = rig(); s.identify()
    expect_refused(lambda: s.apply(ref, s.overlay(ref, app, 0x4120), True, yes), "no recorded backup -> refused")
    set_ledger()
    other = bytearray(ref); other[0x50000] ^= 1
    f, s = rig(bytes(other)); s.identify()
    expect_refused(lambda: s.apply(bytes(other), s.overlay(bytes(other), app, 0x4120), True, yes),
                   "a --ref that is not the recorded backup is refused")
    drift = bytearray(ref); drift[0xC0000] ^= 1                      # the factory bundle: outside the app window and outside the user store
    f, s = rig(bytes(drift)); s.identify()
    expect_refused(lambda: s.apply(ref, s.overlay(ref, app, 0x4120), True, yes),
                   "a flash that drifted from the backup (outside the app window and the user store) is refused")
    check(f.erases == [], "  ...and nothing was erased")
    # custom to custom: the app window holds an EARLIER, longer image and the user store holds the owner's data: both are fine, the window is rewritten, the store is not touched
    earlier = bytearray(ref); earlier[0x4000:0x88000] = os.urandom(0x84000); earlier[0x90000:0x92000] = os.urandom(0x2000)
    f, s = rig(bytes(earlier)); s.identify()
    newimg = s.overlay(ref, app, 0x4120)
    r = s.apply(ref, newimg, True, yes)
    check(r["result"] == "ok" and bytes(f.mem[0x4000:0x86000]) == newimg[0x4000:0x86000], "an update over an earlier custom image (longer, never recorded as a backup) rewrites the whole app window")
    check(bytes(f.mem[0x86000:0x90000]) == bytes(earlier[0x86000:0x90000]) and bytes(f.mem[0x90000:0x92000]) == bytes(earlier[0x90000:0x92000]), "  ...and leaves the user store (0x86000..0xB3FFF) exactly as it was")
    check(not any(0x86000 <= a < 0xB4000 for a in f.erases) and not any(a < 0x4000 for a in f.erases), "  ...erasing nothing in the user store and nothing in the boot area")
    expect_refused(lambda: s.apply(ref, s.overlay(ref, b"\x00" * 0x1000, 0x86000), True, yes), "an image that reaches into the user store is refused")

    print("[3] protected regions")
    for at, what in ((0x3000, "the boot head"), (0x0, "address 0"), (0xBD800, "BTIF"), (0xBE100, "USER"),
                     (0xFF200, "key_mac")):
        f, s = rig(); s.identify()
        expect_refused(lambda: s.apply(ref, s.overlay(ref, b"\x00" * 0x100, at), True, yes),
                       f"an image touching {what} (0x{at:06X}) is refused")
        check(f.erases == [] and f.writes == 0, "  ...and nothing was erased or written")
    f, s = rig(); s.identify()
    expect_refused(lambda: s.overlay(ref, b"\x00" * 0x2000, 0xFF000 + 0x800), "an image past the end of the chip is refused")
    f, s = rig(); s.identify()
    exp = s.overlay(ref, ref[0x3000:0x5000], 0x3000)        # identical bytes over the boot head: no change
    r = s.apply(ref, exp, True, yes)
    check(r["result"] == "nothing to do", "rewriting the boot head with ITS OWN bytes is a no-op, not a write")

    print("[4] dry run and consent")
    f, s = rig(); s.identify()
    r = s.apply(ref, s.overlay(ref, app, 0x4120), False, yes)
    check(r["result"] == "dry run" and f.erases == [] and f.writes == 0, "a dry run erases and writes nothing")
    f, s = rig(); s.identify()
    expect_refused(lambda: s.apply(ref, s.overlay(ref, app, 0x4120), True, lambda n, h: False),
                   "a declined confirmation is refused")
    check(f.erases == [], "  ...and nothing was erased")

    print("[5] a write")
    f, s = rig(); s.identify()
    exp = s.overlay(ref, app, 0x4120)
    r = s.apply(ref, exp, True, yes)
    check(r["result"] == "ok", "the write completes and the final read-back matches")
    check(bytes(f.mem) == exp, "the flash holds exactly the expected image")
    check(all(a % 0x1000 == 0 for a in f.erases), "only whole 4 KB sectors were erased")
    check(min(f.erases) >= 0x4000, "nothing below 0x4000 was erased")
    check(len(f.erases) == len(set(f.erases)) == r["sectors"], f"each changed sector erased once ({r['sectors']})")
    check(bytes(f.mem[:0x4000]) == ref[:0x4000] and bytes(f.mem[0xBD000:0xBF000]) == ref[0xBD000:0xBF000]
          and bytes(f.mem[0xFF000:]) == ref[0xFF000:], "boot head, BTIF, USER and key_mac are untouched")
    check(bytes(f.mem[0x4000:0x4120]) == ref[0x4000:0x4120], "the bytes before an unaligned image are preserved")
    led = json.loads(ledger.read_text())
    check(led["recovery"]["flash_log"][-1]["result"] == "ok", "the write is logged in the ledger")

    print("[6] a sector that will not verify, then resume")
    bad = 0x4000 + 0x10000
    f, s = rig(fail_sectors=[bad]); s.identify()
    exp = s.overlay(ref, app, 0x4120)
    try:
        s.apply(ref, exp, True, yes)
        check(False, "a sector that never verifies stops the run")
    except J.WriteFailed:
        check(True, "a sector that never verifies stops the run (WriteFailed)")
    check(f.erases.count(bad) == J.RETRIES, f"  ...after {J.RETRIES} attempts on that sector")
    check(max(f.erases) == bad, "  ...and nothing after it was touched")
    led = json.loads(ledger.read_text())
    check(led["recovery"]["flash_log"][-1]["result"].startswith("FAILED"), "  ...and the failure is logged")
    # Resume: the same command again, once the fault is gone. Sectors already done are skipped,
    # the half-written one is redone, and everything outside the plan must still match the backup.
    part = bytes(f.mem)
    done_before = len(set(f.erases)) - 1                  # the sectors finished before the bad one
    f.fail.clear(); f.erases.clear()
    s2 = J.SafeFlasher(f, ledger, log=lambda *a, **k: None); s2.size = s.size
    r = s2.apply(ref, exp, True, yes)
    check(r["result"] == "ok" and bytes(f.mem) == exp, "running the same command again completes the write")
    check(min(f.erases) == bad and len(f.erases) == r["sectors"],
          f"  ...rewriting only the {r['sectors']} sector(s) not done yet (skipped {done_before})")
    # A flash that changed OUTSIDE the plan is still refused, even mid-recovery.
    f, s = rig(part); s.identify()
    drifted = bytearray(part); drifted[0xC0000] ^= 1
    f.mem = drifted
    expect_refused(lambda: s.apply(ref, exp, True, yes), "a change outside the plan blocks a resume")

    print("[7] restore")
    f, s = rig(part); s.identify()
    # the same unit, recovered from the half-written state with its own backup
    dump_restore = s.from_dump(part, ref)
    set_ledger()
    led = json.loads(ledger.read_text()); led["recovery"]["full_dump_sha256"] = hashlib.sha256(part).hexdigest()
    ledger.write_text(json.dumps(led))
    r = s.apply(part, dump_restore, True, yes)
    check(r["result"] == "ok" and bytes(f.mem) == ref, "restore --from-dump brings the unit back to its backup")
    set_ledger()
    f, s = rig(); s.identify()
    odd = bytearray(ref); odd[0xBD000:0xBE000] = b"\x11" * 0x1000; odd[0x8000] ^= 0xFF
    r = s.apply(ref, s.from_dump(ref, bytes(odd)), True, yes)
    check(bytes(f.mem[0xBD000:0xBE000]) == ref[0xBD000:0xBE000], "a dump's different BTIF is NOT written to this unit")
    check(f.mem[0x8000] == odd[0x8000], "  ...while the rest of the dump is")
    try:
        import qa_recovery
        stock = qa_recovery.default_stock()
        if stock and stock.is_file():
            set_ledger()
            f, s = rig(s.overlay(ref, app, 0x4120))
            led = json.loads(ledger.read_text()); cur = bytes(f.mem)
            led["recovery"]["full_dump_sha256"] = hashlib.sha256(cur).hexdigest(); ledger.write_text(json.dumps(led))
            s.identify()
            r = s.apply(cur, s.stock_app_area(cur, stock), True, yes)
            st = qa_recovery.stock_flash(stock)
            check(bytes(f.mem[0x4000:0xB5000]) == st[0x4000:0xB5000], "restore --stock-fwsc puts the stock app area back")
            check(bytes(f.mem[:0x4000]) == cur[:0x4000], "  ...without touching the boot head")
        else:
            print("  skip restore --stock-fwsc (no stock .fwsc on this machine)")
    except Exception as e:  # noqa: BLE001
        check(False, f"restore --stock-fwsc: {e}")

    print("[7b] restore the recorded backup onto a unit that holds a custom image (the real case: --ref is the baseline backup)")
    set_ledger()
    custom = s.overlay(ref, app, 0x4120)                          # the unit now runs a custom image; the ledger still records `ref`
    f, s = rig(custom); s.identify()
    check(bytes(f.mem) != ref, "  the rig holds a custom image, not the backup")
    r = s.apply(ref, s.from_dump(ref, ref), True, yes, restore=True)
    check(r["result"] == "ok" and r["sectors"] > 0, f"restore plans against the chip: {r['sectors']} sector(s) rewritten, not 'nothing to do'")
    check(bytes(f.mem[0x4000:0xB5000]) == ref[0x4000:0xB5000], "  ...the app area is the backup's again")
    check(bytes(f.mem[:0x4000]) == custom[:0x4000], "  ...and the boot head was not touched")
    r2 = s.apply(ref, s.from_dump(ref, ref), True, yes, restore=True)
    check(r2["result"] == "nothing to do", "a second restore finds nothing left to do")
    # the old planning, for the record: against the backup it is empty and says so without reading the chip
    f, s = rig(custom); s.identify()
    check(s.apply(ref, s.from_dump(ref, ref), False, yes)["result"] == "nothing to do", "  (the write-style plan of the same image is empty: why restore needs its own)")
    # a restore still refuses a chip that no longer matches the backup outside the app area
    f, s = rig(custom); s.identify()
    odd2 = bytearray(custom); odd2[0xC0008] ^= 1                 # the stock cab/amp bundle: not the app area, not the device's own, so the restore must not write it
    f.mem = odd2
    expect_refused(lambda: s.apply(ref, s.from_dump(ref, ref), True, yes, restore=True), "restore refuses a chip that drifted outside the area it rewrites")

    print("[8] what the writer can never do")
    for cls in (J.MockFlash, J.UbootFlash, J.SafeFlasher):
        for name in ("erase_chip", "erase_block", "flash_erase_chip", "flash_erase_block", "write_chipkey",
                     "burn_chipkey", "mem_write", "mem_jump"):
            check(not hasattr(cls, name), f"{cls.__name__} has no {name}")
    src = (TOOLS / "jl_safe_flash.py").read_text(encoding="utf-8")
    for call in ("flash_erase_chip(", "flash_erase_block(", "write_chipkey(", "erasechip"):
        check(call not in src, f"the writer never calls {call.rstrip('(')}")

    print("[9] an image over the app-area head must be a real app area (pack 157 was written as a raw app.bin: black screen, nothing on USB)")
    import qa_recovery
    import jl_app_area as A
    stock = qa_recovery.stock_flash(qa_recovery.default_stock())
    if stock is None:
        print("  skip  (the stock .fwsc is not on this machine: no real app area to build from)")
    else:
        stock = bytes(stock)
        ref9 = stock + b"\xff" * (0x100000 - len(stock))
        s9 = J.SafeFlasher(J.MockFlash(ref9, J.CHIP_KEY, None, []), Path(tempfile.mkdtemp()) / "ledger.json")
        raw_app = os.urandom(300000)                                       # what build_app.bin is: plain code, no file table, not scrambled
        good = A.build_area(stock, raw_app)
        try:
            s9.check_app_area_image(s9.overlay(ref9, good, 0x4000), 0x4000, len(good))
            check(True, "a real app area (file table, SFC encoding, CRCs) is accepted")
        except J.Refused as e:
            check(False, f"a real app area was refused: {e}")
        expect_refused(lambda: s9.check_app_area_image(s9.overlay(ref9, raw_app, 0x4000), 0x4000, len(raw_app)), "a raw app.bin written at 0x4000 is refused")
        bent = bytearray(good)
        bent[5000] ^= 0x01                                                 # one flipped bit inside the scrambled app
        expect_refused(lambda: s9.check_app_area_image(s9.overlay(ref9, bytes(bent), 0x4000), 0x4000, len(bent)), "an app area with one bad bit (CRC) is refused")
        try:
            s9.check_app_area_image(s9.overlay(ref9, b"\x00" * 0x100, 0x20000), 0x20000, 0x100)
            check(True, "a write that does not cover 0x4000 is not judged as an app area")
        except J.Refused as e:
            check(False, f"a write elsewhere was judged as an app area: {e}")

    print(f"\n{'ALL PASSED' if not fails else f'{fails} FAILED'}")
    return 1 if fails else 0


if __name__ == "__main__":
    sys.exit(main())
