# SPDX-License-Identifier: GPL-3.0-only
# Copyright (C) 2026 The Onyx Authors (@28spawn)
"""The .abkx import: the web editor's JavaScript converter (web/Onyx_Web_Editor.html) and tools/abkx_to_json.py give the same Onyx preset document, and the editor's record packer is
byte-identical to tools/preset_bank_pc.py's. The input is a SYNTHETIC bank made here (random valid records, some with the documented PA-EXT extension, some empty): no vendor file is needed.
Usage: python custom-fw/test/test_abkx_import.py"""
import json, random, re, struct, subprocess, sys, tempfile
from pathlib import Path
FW = Path(__file__).resolve().parent.parent
sys.path.insert(0, str(FW / "tools"))
import abkx_to_json as A
import preset_bank_pc as P

tmp = Path(tempfile.mkdtemp(prefix="pa_abkx_"))
page = (FW / "web" / "Onyx_Web_Editor.html").read_text(encoding="utf-8")
(tmp / "page.js").write_text(re.search(r"<script>(.*)</script>", page, re.S).group(1), encoding="utf-8")
(tmp / "run.js").write_text("const c=require('./page.js');const a=JSON.parse(require('fs').readFileSync(0,'utf8'));"
    "const bank=Uint8Array.from(a.bank);const out={};"
    "for(const o of a.opts){out[o.key]=c.abkxConvert(bank,o.opt);}"
    "out.recs=a.docs.map((d)=>c.presetsFromDoc(d).map((r)=>r?Array.from(r):null));"
    "out.back=a.docs.map((d)=>c.presetsFromDoc(d).map((r,i)=>r?c.recordToPreset(i,r):null));"
    "out.ok=a.docs.map((d)=>c.presetsFromDoc(d).every((r)=>!r||c.recordOk(r)));"
    "console.log(JSON.stringify(out));")
fails = 0


def check(ok, msg):
    global fails
    print(("  ok   " if ok else "  FAIL ") + msg)
    fails += 0 if ok else 1


def make_bank(seed):
    rnd = random.Random(seed)
    recs = []
    for i in range(80):
        r = bytearray(92)
        if i % 5 == 4:                                  # empty: every module off, no extension
            r[0] = 60
            order = list(range(6)); rnd.shuffle(order); r[2:8] = bytes(order)
        else:
            r[0] = rnd.randint(0, 100)
            order = list(range(6)); rnd.shuffle(order); r[2:8] = bytes(order)
            for c in range(6):
                r[8 + c] = rnd.randint(0, 1)
            r[8 + rnd.randint(0, 5)] = 1
            r[0x0E + 0] = rnd.randint(0, 5); r[0x0E + 1] = rnd.randint(0, 22); r[0x0E + 2] = rnd.randint(0, 7)
            r[0x0E + 3] = rnd.randint(0, 3); r[0x0E + 4] = rnd.randint(0, 5); r[0x0E + 5] = rnd.randint(0, 22)
            for m in (0x14, 0x20, 0x2C, 0x38, 0x44, 0x50):
                for k in range(6):
                    struct.pack_into("<H", r, m + 2 * k, rnd.randint(0, 100))
            if i % 3 == 0:                              # the PA-EXT extension
                r[1] = 0x80 | (1 << 5) | (rnd.randint(0, 4) + 5 * rnd.randint(0, 2) + 15 * rnd.randint(0, 1))
                r[0x2A] = rnd.randint(0, 100); r[0x2B] = 50 + rnd.randint(-12, 12)
                r[0x36] = 50 * rnd.randint(0, 1) + rnd.randint(0, 5); r[0x4E] = 50 * rnd.randint(0, 1) + rnd.randint(0, 2)
                for off in (0x56, 0x58, 0x5A, 0x37, 0x4F, 0x57, 0x59, 0x5B):
                    r[off] = rnd.randint(0, 100)
        recs.append(bytes(r))
    return b"".join(recs) + b"PATCHEND" + bytes([0, rnd.choice([0, 0x45])])


OPTS = [("default", {}), ("chain", {"preserve_chain": True}), ("level", {"copy_amp_level": True, "name_prefix": "AB", "genre": 3})]
JSOPT = {"default": {}, "chain": {"preserveChain": True}, "level": {"copyAmpLevel": True, "namePrefix": "AB", "genre": 3}}
for seed in (1, 2, 3):
    bank = make_bank(seed)
    py = {k: A.convert(bank, **o) for k, o in OPTS}
    inp = {"bank": list(bank), "opts": [{"key": k, "opt": JSOPT[k]} for k, _ in OPTS], "docs": [py["default"]]}
    r = subprocess.run(["node", str(tmp / "run.js")], input=json.dumps(inp), capture_output=True, text=True, cwd=tmp)
    if r.returncode:
        print(r.stderr); fails += 1; continue
    js = json.loads(r.stdout)
    for k, _ in OPTS:
        same = json.dumps(js[k]["presets"], sort_keys=True) == json.dumps(py[k]["presets"], sort_keys=True)
        check(same, f"seed {seed}, options '{k}': the page's converter gives the same 80 presets as tools/abkx_to_json.py")
    recs_py = [P.obj_to_rec(o) for o in py["default"]["presets"]]
    check([bytes(x) for x in js["recs"][0]] == recs_py, f"seed {seed}: the page packs every record byte for byte like preset_bank_pc.py (CRC included)")
    check(all(js["ok"]), f"seed {seed}: every packed record passes the page's own magic and CRC check")
    unpacked = [json.loads(json.dumps(p)) for p in js["back"][0]]
    check(all(u["name"] == o["name"] and u["slot_type"] == o["slot_type"] and u["params"] == o["params"] and u["empty"] == o["empty"] and u["seq"] == o["seq"] for u, o in zip(unpacked, py["default"]["presets"])),
          f"seed {seed}: unpacking a record gives the document's fields back")
    check(all(sorted(o["slot_order"]) == list(range(8)) for k in py for o in py[k]["presets"]) and all(o["seq"] >= 2 for o in py["default"]["presets"] if not o["empty"]),
          f"seed {seed}: every chain order is a permutation of the 8 slots and every converted preset has a save counter >= 2")
bad = bytearray(make_bank(9)); bad[-10:-2] = b"PATCHXXX"
try:
    A.convert(bytes(bad)); check(False, "a bank without PATCHEND is refused")
except ValueError:
    check(True, "a bank without PATCHEND is refused")
print("RESULT:", "FAILED" if fails else "ALL PASSED")
sys.exit(1 if fails else 0)
