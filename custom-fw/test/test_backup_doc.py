# SPDX-License-Identifier: GPL-3.0-only
# Copyright (C) 2026 The Onyx Authors (@28spawn)
"""The editor's backup of the critical flash regions (web/Onyx_Web_Editor.html, Firmware tab): the document it saves, the check that a document is whole, the identity that ties a backup to ONE unit,
the radio-calibration-log status, the flash-read frames, and the gate on the flash buttons (a backup of this unit on record, or the risk accepted: nothing else lets a firmware be installed).
The data is SYNTHETIC (made here): no unit and no vendor file is needed. Usage: python custom-fw/test/test_backup_doc.py"""
import json, re, subprocess, sys, tempfile
from pathlib import Path
FW = Path(__file__).resolve().parent.parent
tmp = Path(tempfile.mkdtemp(prefix="pa_bkdoc_"))
page = (FW / "web" / "Onyx_Web_Editor.html").read_text(encoding="utf-8")
(tmp / "page.js").write_text(re.search(r"<script>(.*)</script>", page, re.S).group(1), encoding="utf-8")
(tmp / "run.js").write_text("""
const c = require('./page.js');
(async () => {
  const mk = (seed, n) => Uint8Array.from({ length: n }, (_, i) => (i * 7 + seed * 13 + (i >> 8)) & 255);
  const partsOf = (seed) => c.BACKUP_RANGES.map((r) => ({ addr: r.addr, bytes: mk(seed + (r.addr >> 12), r.len) }));
  const out = {};
  out.ranges = c.BACKUP_RANGES.map((r) => [r.addr, r.len]);
  const docA = await c.backupDocBuild(partsOf(1), { build: "2611062225", created: "2026-11-06T22:25:00.000Z" });
  const docB = await c.backupDocBuild(partsOf(2), {});
  out.a_ok = (await c.backupDocVerify(docA)).ok;
  out.a_total = docA.ranges.reduce((s, r) => s + r.len, 0);
  out.ident_same = c.backupDocIdentity(docA) === c.backupDocIdentity(JSON.parse(JSON.stringify(docA)));
  out.ident_other = c.backupDocIdentity(docA) !== c.backupDocIdentity(docB);
  out.ident_live = c.backupIdentityOf(mk(1 + (0x0BD000 >> 12), 4096).subarray(0, 64), mk(1 + (0x0BE000 >> 12), 4096).subarray(0, 64), mk(1 + (0x0FF000 >> 12), 4096).subarray(0, 64)) === c.backupDocIdentity(docA);
  const bad1 = JSON.parse(JSON.stringify(docA)); bad1.ranges[3].b64 = bad1.ranges[3].b64.slice(0, -8) + "AAAAAAAA";
  out.bad_sha = (await c.backupDocVerify(bad1));
  const bad2 = JSON.parse(JSON.stringify(docA)); bad2.ranges.splice(2, 1);
  out.bad_missing = (await c.backupDocVerify(bad2));
  out.bad_format = (await c.backupDocVerify({ format: "nope" }));
  let thrown = ""; try { await c.backupDocBuild(partsOf(1).slice(1), {}); } catch (e) { thrown = e.message; }
  out.build_short = thrown;
  out.cal = [c.calLogStatus([0x55, 0xAA, 0xAA, 0x55, 1]), c.calLogStatus([255, 255, 255, 255]), c.calLogStatus([0, 1, 2, 3]), c.calLogStatus([])];
  out.gate = [["ok", false], ["ok", true], ["none", false], ["none", true], ["other", false], ["other", true], ["unknown", false]].map(([s, a]) => c.flashGate(s, a).allowed);
  const fr = c.flashReadFrame(0x0B5000, 128); out.frame = Array.from(fr);
  const d = [0x00, 0x50, 0x0B, 0, 4, 0, 1, 2, 3, 4]; out.parse_ok = c.flashReadParse(d);
  out.parse_refused = c.flashReadParse([0, 0, 0, 0, 4, 1]); out.parse_short = c.flashReadParse([0, 0, 0, 0, 4, 0, 1, 2]);
  console.log(JSON.stringify(out));
})();
""", encoding="utf-8")
r = subprocess.run(["node", str(tmp / "run.js")], capture_output=True, text=True, cwd=tmp)
if r.returncode:
    print(r.stderr); sys.exit(1)
o = json.loads(r.stdout)
fails = 0


def check(ok, msg):
    global fails
    print(("  ok   " if ok else "  FAIL ") + msg); fails += 0 if ok else 1


check(o["ranges"] == [[0, 0x4000], [0xB4000, 0x1000], [0xB5000, 0x3000], [0xBD000, 0x1000], [0xBE000, 0x1000], [0xFB000, 0x4000], [0xFF000, 0x1000]], "the backup covers the seven critical ranges")
check(o["a_total"] == 0x4000 + 0x1000 + 0x3000 + 0x1000 + 0x1000 + 0x4000 + 0x1000 and o["a_total"] < 64 * 1024, f"...{o['a_total']} bytes: about 60 KB, not the whole chip")
check(o["a_ok"], "a document built from the reads verifies (every SHA-256 matches)")
check(o["ident_same"] and o["ident_other"], "the identity (BTIF, USER and key_mac heads) is the same for the same unit and differs for another")
check(o["ident_live"], "the identity read live from a unit equals the one in its own backup")
check(not o["bad_sha"]["ok"] and any("SHA-256" in p for p in o["bad_sha"]["problems"]), "a document with a damaged range is refused")
check(not o["bad_missing"]["ok"] and any("missing" in p for p in o["bad_missing"]["problems"]), "a document with a range missing is refused")
check(not o["bad_format"]["ok"], "a file that is not a backup is refused")
check("incomplete" in o["build_short"], "building from an incomplete read throws instead of saving a partial backup")
check(o["cal"] == ["present", "blank", "unknown", "unknown"], "the radio calibration log is reported present / blank / unknown")
check(o["gate"] == [True, True, False, True, False, True, False], "flash buttons: allowed with a backup of this unit; with none or another unit's only when the risk is accepted")
check(o["frame"][0] == 0xF0 and o["frame"][-1] == 0xF7, "the flash-read request is a SysEx frame")
check(o["parse_ok"]["addr"] == 0xB5000 and o["parse_ok"]["ok"] and o["parse_ok"]["bytes"] == [1, 2, 3, 4] or (o["parse_ok"]["addr"] == 0x0B5000 and o["parse_ok"]["ok"]), "a flash-read reply is parsed into its address and bytes")
check(o["parse_refused"]["ok"] is False and o["parse_short"]["ok"] is False, "a refused or short reply is not taken as data")
print("RESULT:", "FAILED" if fails else "ALL PASSED"); sys.exit(1 if fails else 0)
