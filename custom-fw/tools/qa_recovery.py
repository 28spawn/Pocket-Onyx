#!/usr/bin/env python3
"""
custom-fw/tools/qa_recovery.py - recovery-readiness evidence (TEST-PROTOCOL.md section 4).

Verifies full-chip flash dumps, backs up the per-unit calibration, and records the
results in custom-fw/qa/hw_ledger.json, so the QA gate (qa_gate.py, checks R-01..R-04,
I-07, S-08) can read them. Nothing here talks to the device: it only reads files you
dumped and edits the ledger.

    # R-01 + R-02: two dumps of the whole chip, made with a programmer or jl-uboot-tool
    python custom-fw/tools/qa_recovery.py verify dump1.bin dump2.bin --chip "XM25QH80B" --record

    # R-03: a recovery route was tested (proven / failed)
    python custom-fw/tools/qa_recovery.py route spi_programmer --status proven --evidence "photos/restore.jpg"

    # R-04: a full restore drill (a passing drill also marks its route proven)
    python custom-fw/tools/qa_recovery.py drill --route spi_programmer --result pass --evidence "log.txt"

    # I-07: the custom UI was seen running a given image layout
    python custom-fw/tools/qa_recovery.py layout-proof --sha <image sha256> --observed "NO RADIO row" --evidence photo.jpg

    # H: hardware test results for an image
    python custom-fw/tools/qa_recovery.py hw-run --sha <image sha256> --tester <tester-name> HW-01=PASS HW-02=PASS HW-13=N/A

    # what the ledger says, and what is still missing
    python custom-fw/tools/qa_recovery.py show

Standard library only.
"""
from __future__ import annotations

import argparse
import datetime as dt
import hashlib
import json
import math
import shutil
import struct
import sys
from collections import Counter
from pathlib import Path

TOOLS = Path(__file__).resolve().parent
FW = TOOLS.parent
ROOT = FW.parent
QA = FW / "qa"
LEDGER = QA / "hw_ledger.json"
BACKUPS = QA / "backups"          # Local test device backups

sys.path.insert(0, str(TOOLS))
import pack_ufw as P  # noqa: E402

# The stock partition map (isd_config.ini [RESERVED_CONFIG] / [RESERVED_EXPAND_CONFIG]).
REGIONS = [
    ("MBR",          0x000000, 0x001000),
    ("uboot",        0x001000, 0x003000),
    ("app",          0x004000, 0x0B1000),   # app area: file table + app.bin + cfg_tool.bin, ENC-scrambled
    ("VM",           0x0B5000, 0x008000),
    ("BTIF",         0x0BD000, 0x001000),   # RF calibration + BLE MAC: unique per unit
    ("USER",         0x0BE000, 0x001000),   # factory calibration: unique per unit
    ("factory_cab",  0x0BF000, 0x03C000),
    ("SKPST",        0x0FB000, 0x004000),   # stock preset store
]
PER_UNIT = ("BTIF", "USER")
ROUTES = ("spi_programmer", "jieli_isd_download", "uboot_update_mode", "button_dfu",
          "m_upgrade_stock", "stock_enter_upgrade", "usb_key_dongle")
HW_IDS = [f"HW-{i:02d}" for i in range(1, 17)]


# ---- helpers -------------------------------------------------------------------------
def entropy(b: bytes) -> float:
    if not b:
        return 0.0
    n = len(b)
    return -sum(c / n * math.log2(c / n) for c in Counter(b).values())


def load_ledger() -> dict:
    try:
        return json.loads(LEDGER.read_text(encoding="utf-8"))
    except FileNotFoundError:
        sys.exit(f"no ledger at {LEDGER} - restore it from git first")
    except ValueError as e:
        sys.exit(f"{LEDGER} is not valid JSON ({e}) - fix it by hand first; this tool will not overwrite it")


def save_ledger(led: dict) -> None:
    tmp = LEDGER.with_suffix(".json.tmp")
    tmp.write_text(json.dumps(led, indent=2, ensure_ascii=False) + "\n", encoding="utf-8")
    tmp.replace(LEDGER)
    print(f"ledger updated: {LEDGER.relative_to(ROOT)}")


def today() -> str:
    return dt.date.today().isoformat()


def stock_flash(path: Path | None) -> bytes | None:
    """The flash.bin payload of the stock .fwsc (what the updater writes from 0x000000)."""
    if not path or not path.is_file():
        return None
    ufw, _ = P.unwrap_fwsc(path.read_bytes())
    head = P.xor_header(ufw[:64])
    count = struct.unpack_from("<H", head, 8)[0]
    for i in range(count):
        e = P.xor_header(ufw[64 + i * 80: 64 + (i + 1) * 80])
        if e[64:].split(b"\0")[0] == b"flash.bin":
            off, size = struct.unpack_from("<II", e, 8)
            return bytes(ufw[off:off + size])
    return None


def default_stock() -> Path | None:
    text = (FW / "build.ps1").read_text(errors="replace")
    for line in text.splitlines():
        if line.strip().startswith("$TemplateFwsc"):
            return Path(line.split("=", 1)[1].strip().strip('"'))
    return None


# ---- verify ----------------------------------------------------------------------------
def cmd_verify(a) -> int:
    d1 = Path(a.dump1).read_bytes()
    d2 = Path(a.dump2).read_bytes()
    ok = True
    print(f"dump 1: {a.dump1} ({len(d1):,} B)\ndump 2: {a.dump2} ({len(d2):,} B)\n")

    # 1. the two reads agree
    if len(d1) != len(d2):
        print(f"  FAIL  the dumps have different sizes ({len(d1):,} vs {len(d2):,})")
        return 1
    if d1 != d2:
        bad = sorted({i // 4096 for i in range(len(d1)) if d1[i] != d2[i]})
        print(f"  FAIL  the dumps differ in {len(bad)} sector(s): "
              + ", ".join(f"{s * 4096:#08x}" for s in bad[:12]) + (" ..." if len(bad) > 12 else ""))
        print("        re-seat the clip (or reconnect) and dump again; never restore from a read that does not repeat")
        return 1
    sha = hashlib.sha256(d1).hexdigest()
    print(f"  ok    the two dumps are byte-identical  sha256 {sha}")

    # 2. not blank, not stuck
    if d1.count(0xFF) == len(d1) or d1.count(0x00) == len(d1):
        print("  FAIL  the dump is all 0xFF or all 0x00 - nothing was read (clip, power, or chip select)")
        return 1

    # 3. the real size: a read past the end of a smaller chip wraps around
    size = len(d1)
    eff = size
    while eff > 0x10000 and d1[:eff // 2] == d1[eff // 2:eff]:
        eff //= 2
    if size & (size - 1):
        print(f"  WARN  {size:,} B is not a power of two - was the whole chip read?")
    if eff != size:
        print(f"  WARN  the read wrapped: the upper part repeats the lower part, so the chip is {eff:,} B "
              f"({eff // 1048576 if eff >= 1048576 else eff / 1048576:g} MB), not {size:,} B")
    else:
        print(f"  ok    chip size {eff:,} B ({eff / 1048576:g} MB)")
    if a.size and int(a.size, 0) != eff:
        print(f"  WARN  you said the chip is {int(a.size, 0):,} B; the dump says {eff:,} B")

    # 4. the stock layout, region by region
    print("\n  region        range               state")
    for name, off, ln in REGIONS:
        blob = d1[off:off + ln]
        if len(blob) < ln:
            print(f"  {name:12}  {off:#08x}+{ln:#07x}   beyond the end of the chip")
            continue
        ff = blob.count(0xFF) / ln
        state = "erased (all 0xFF)" if ff == 1.0 else f"data, entropy {entropy(blob):.2f} bits/B, {ff:.0%} 0xFF"
        print(f"  {name:12}  {off:#08x}+{ln:#07x}   {state}")
        if name in PER_UNIT and ff == 1.0:
            print(f"  WARN  {name} is empty - this unit's calibration is not where the stock map puts it")

    # 5. against the stock .fwsc: does the flash hold what the updater writes?
    evidence = {"date": today(), "dump_sha256": sha, "chip_size": eff}
    stock = stock_flash(Path(a.stock) if a.stock else default_stock())
    if stock is None:
        print("\n  skip  no stock .fwsc to compare against (pass --stock)")
    else:
        print("\n  against the stock .fwsc flash.bin payload:")
        cmp = {}
        for name, off, ln in REGIONS[:3]:
            s, d = stock[off:off + ln], d1[off:off + ln]
            n = min(len(s), len(d))
            same = sum(1 for i in range(n) if s[i] == d[i]) / n if n else 0.0
            cmp[name] = round(same, 4)
            print(f"  {name:12}  {same:7.2%} identical")
        evidence["identical_to_fwsc"] = cmp
        app_e = entropy(d1[0x4120:0x4120 + 0x40000])
        evidence["app_entropy_on_flash"] = round(app_e, 3)
        if cmp["MBR"] > 0.99 and cmp["uboot"] > 0.99 and cmp["app"] > 0.99:
            evidence["app_region_matches_fwsc"] = True
            print("\n  => The device stores the app EXACTLY as the .fwsc carries it (ENC-scrambled, "
                  f"{app_e:.2f} bits/B).\n     pack_ufw.py builds images the same way (jl_app_area.py: file "
                  "table, CRCs, ENC with the chip key).")
        elif cmp["MBR"] > 0.99 and cmp["uboot"] > 0.99:
            evidence["app_region_matches_fwsc"] = False
            print("\n  => Bootloader identical to the .fwsc, app different: another firmware version, or the app\n"
                  "     has per-unit device encoding. Keep this backup image for safety.")
        else:
            evidence["app_region_matches_fwsc"] = False
            print("\n  => The boot area differs from the .fwsc: this unit's layout or key is not the one the\n"
                  "     repo assumes. Do not flash anything until this is understood.")

    # 6. back up the dump and the per-unit regions
    dest = BACKUPS / sha[:16]
    dest.mkdir(parents=True, exist_ok=True)
    shutil.copyfile(a.dump1, dest / "full_dump.bin")
    files = {"full_dump.bin": sha}
    for name, off, ln in REGIONS:
        if name in PER_UNIT + ("VM",) and off + ln <= len(d1):
            (dest / f"{name}.bin").write_bytes(d1[off:off + ln])
            files[f"{name}.bin"] = hashlib.sha256(d1[off:off + ln]).hexdigest()
    (dest / "manifest.json").write_text(json.dumps({"created": today(), "chip": a.chip, "chip_size": eff,
                                                    "files": files, "evidence": evidence}, indent=2) + "\n")
    print(f"\n  ok    backup written to {dest.relative_to(ROOT)}")
    print("        COPY THIS FOLDER TO TWO PLACES OFF THIS PC - it holds this unit's RF and factory calibration")

    if a.record:
        led = load_ledger()
        dev = led.setdefault("device", {})
        if a.chip:
            dev["flash_chip"] = a.chip
        dev["flash_size_bytes"] = eff
        dev["flash_evidence"] = evidence
        rec = led.setdefault("recovery", {})
        rec["full_dump_sha256"] = sha
        rec["dumps_identical"] = 2
        locs = rec.setdefault("dump_locations", [])
        for loc in [dest.relative_to(ROOT).as_posix(), *(a.copied_to or [])]:
            if loc not in locs:
                locs.append(loc)
        save_ledger(led)
        if not a.chip:
            print("  note  R-01 also needs the chip part number: re-run with --chip \"<marking>\"")
        if len(locs) < 3:
            print("  note  list the off-PC copies with --copied-to <place> ... (R-02 wants two)")
    return 0 if ok else 1


# ---- ledger editors --------------------------------------------------------------------
def cmd_route(a) -> int:
    if a.status in ("proven", "reachable") and not a.evidence:
        sys.exit(f"a route can only be marked {a.status} with --evidence (photo, log or file)")
    led = load_ledger()
    routes = led.setdefault("recovery", {}).setdefault("routes", {})
    r = routes.setdefault(a.name, {})
    if not isinstance(r, dict):
        r = routes[a.name] = {"status": str(r)}
    r.update({"status": a.status, "date": a.date or today(), "evidence": a.evidence or r.get("evidence", "")})
    if a.vid_pid:
        r["vid_pid_seen"] = a.vid_pid
    save_ledger(led)
    return 0


def cmd_drill(a) -> int:
    if not a.evidence:
        sys.exit("a drill needs --evidence")
    led = load_ledger()
    rec = led.setdefault("recovery", {})
    if not rec.get("full_dump_sha256"):
        sys.exit("record the full-chip dump first (verify --record): a drill without a backup is not a drill")
    rec["restore_drill"] = {"date": a.date or today(), "route": a.route, "result": a.result, "evidence": a.evidence}
    if a.result == "pass":
        r = rec.setdefault("routes", {}).setdefault(a.route, {})
        r.update({"status": "proven", "date": a.date or today(), "evidence": a.evidence})
    save_ledger(led)
    return 0


def cmd_layout_proof(a) -> int:
    led = load_ledger()
    layout = a.layout
    if not layout:
        rpt = FW / "qa" / "reports" / "latest.md"
        txt = rpt.read_text(encoding="utf-8") if rpt.exists() else ""
        if a.sha not in txt:
            sys.exit("give --layout (the gate report names it, e.g. jl-app-area@0x4120-start0x2000120)")
        for line in txt.splitlines():
            if line.startswith("| layout |"):
                layout = line.split("`")[1]
    led.setdefault("layout_proofs", []).append({
        "layout": layout, "date": a.date or today(), "sha256": a.sha, "custom_ui_seen": True,
        "observed": a.observed, "evidence": a.evidence})
    save_ledger(led)
    return 0


def cmd_hw_run(a) -> int:
    results = {}
    for item in a.results:
        if "=" not in item:
            sys.exit(f"'{item}': use HW-xx=PASS|FAIL|N/A")
        k, v = item.split("=", 1)
        k, v = k.upper(), v.upper()
        if k not in HW_IDS or v not in ("PASS", "FAIL", "N/A"):
            sys.exit(f"'{item}': unknown test or result")
        results[k] = v
    led = load_ledger()
    runs = led.setdefault("hw_runs", [])
    run = next((r for r in runs if r.get("sha256") == a.sha and r.get("date") == (a.date or today())), None)
    if run is None:
        run = {"sha256": a.sha, "date": a.date or today(), "tester": a.tester, "results": {}, "notes": "", "evidence": ""}
        runs.append(run)
    run["results"].update(results)
    if a.notes:
        run["notes"] = (run.get("notes", "") + " " + a.notes).strip()
    if a.evidence:
        run["evidence"] = (run.get("evidence", "") + " " + a.evidence).strip()
    save_ledger(led)
    return 0


def cmd_show(a) -> int:
    led = load_ledger()
    dev, rec = led.get("device", {}), led.get("recovery", {})
    routes = rec.get("routes", {})
    proven = [k for k, v in routes.items() if isinstance(v, dict) and v.get("status") == "proven"]
    drill = rec.get("restore_drill", {})
    rows = [
        ("R-01 chip identified", bool(dev.get("flash_chip") and dev.get("flash_size_bytes")),
         f"{dev.get('flash_chip') or '-'} / {dev.get('flash_size_bytes') or '-'} B"),
        ("R-02 two identical dumps", bool(rec.get("full_dump_sha256")) and int(rec.get("dumps_identical", 0)) >= 2,
         (rec.get("full_dump_sha256") or "-")[:16] + f"  copies: {len(rec.get('dump_locations', []))}"),
        ("R-03 route proven", bool(proven), ", ".join(proven) or "none"),
        ("R-04 restore drill", bool(drill.get("date")), f"{drill.get('date') or '-'} {drill.get('route') or ''} {drill.get('result') or ''}"),
    ]
    for name, good, detail in rows:
        print(f"  {'ok  ' if good else 'TODO'}  {name:26} {detail}")
    ev = dev.get("flash_evidence", {})
    if "app_region_matches_fwsc" in ev:
        print(f"  info  flash app region matches the encrypted .fwsc: {ev['app_region_matches_fwsc']}")
    print("\n  routes:")
    for k in ROUTES:
        v = routes.get(k, {})
        st = v.get("status", "untested") if isinstance(v, dict) else v
        print(f"    {k:22} {st:10} {v.get('vid_pid_seen', '') if isinstance(v, dict) else ''}")
    print(f"\n  layout proofs: {len(led.get('layout_proofs', []))}   hardware runs: {len(led.get('hw_runs', []))}")
    return 0


def main() -> int:
    ap = argparse.ArgumentParser(description="Recovery-readiness evidence for the Pocket Amp QA gate")
    sub = ap.add_subparsers(dest="cmd", required=True)

    v = sub.add_parser("verify", help="check two full-chip dumps, back them up, optionally record them")
    v.add_argument("dump1")
    v.add_argument("dump2")
    v.add_argument("--chip", help="flash part number from the chip marking (R-01)")
    v.add_argument("--size", help="the size you expect, e.g. 0x100000 (checked against the dump)")
    v.add_argument("--stock", help="stock 'Pocket AMP.fwsc' to compare against (default: build.ps1's template)")
    v.add_argument("--copied-to", nargs="*", help="where the off-PC copies of the backup are")
    v.add_argument("--record", action="store_true", help="write the result into the ledger")
    v.set_defaults(fn=cmd_verify)

    r = sub.add_parser("route", help="record the result of testing a recovery route")
    r.add_argument("name", choices=ROUTES)
    r.add_argument("--status", required=True, choices=["proven", "reachable", "failed", "untested"],
                   help="proven = a full restore worked; reachable = the mode can be entered, no restore yet")
    r.add_argument("--evidence")
    r.add_argument("--vid-pid", help="what the PC saw, e.g. 4D4A:4155")
    r.add_argument("--date")
    r.set_defaults(fn=cmd_route)

    d = sub.add_parser("drill", help="record a full restore drill")
    d.add_argument("--route", required=True, choices=ROUTES)
    d.add_argument("--result", required=True, choices=["pass", "fail"])
    d.add_argument("--evidence")
    d.add_argument("--date")
    d.set_defaults(fn=cmd_drill)

    lp = sub.add_parser("layout-proof", help="record that the custom UI was seen running an image layout")
    lp.add_argument("--sha", required=True, help="the image SHA-256 from the gate report")
    lp.add_argument("--layout", help="layout name from the gate report (read from latest.md if omitted)")
    lp.add_argument("--observed", required=True, help="the custom-only thing you saw")
    lp.add_argument("--evidence", required=True, help="photo or video path")
    lp.add_argument("--date")
    lp.set_defaults(fn=cmd_layout_proof)

    h = sub.add_parser("hw-run", help="record hardware test results for an image")
    h.add_argument("--sha", required=True)
    h.add_argument("--tester", required=True)
    h.add_argument("results", nargs="+", help="HW-xx=PASS|FAIL|N/A ...")
    h.add_argument("--notes")
    h.add_argument("--evidence")
    h.add_argument("--date")
    h.set_defaults(fn=cmd_hw_run)

    s = sub.add_parser("show", help="what the ledger says and what is still missing")
    s.set_defaults(fn=cmd_show)

    a = ap.parse_args()
    return a.fn(a)


if __name__ == "__main__":
    sys.exit(main())
