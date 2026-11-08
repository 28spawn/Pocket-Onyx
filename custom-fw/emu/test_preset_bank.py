# SPDX-License-Identifier: GPL-3.0-only
# Copyright (C) 2026 The Onyx Authors (@28spawn)
"""Preset bank backup and restore (tools/preset_bank_pc.py, JSON files) against the emulator: read all 80 presets with PA_CMD_PA_PRESET, edit two of them in the file, restore only
those, read them back identical, then restore the whole original file and check the bank is identical again (EMPTY slots stay EMPTY, save counters unchanged).
Usage: python custom-fw/emu/test_preset_bank.py <dll>"""
import json, os, subprocess, sys, tempfile
HERE = os.path.dirname(os.path.abspath(__file__))
TOOL = os.path.join(HERE, "..", "tools", "preset_bank_pc.py")
dll = sys.argv[1]
tmp = tempfile.mkdtemp(prefix="pa_bankt_")
orig, edit = os.path.join(tmp, "orig.json"), os.path.join(tmp, "edit.json")
fails = 0
ENV = dict(os.environ, PA_FLASH_FILE=os.path.join(tmp, "flash.bin"))        # a scratch chip: the test neither depends on nor changes the emulator's own flash file


def run(*args):
    r = subprocess.run([sys.executable, TOOL, *args, "--emu", dll, "--yes"], capture_output=True, text=True, env=ENV)
    return r.returncode, (r.stdout + r.stderr).strip()


def check(ok, msg):
    global fails
    print(("  ok   " if ok else "  FAIL ") + msg)
    fails += 0 if ok else 1


rc, out = run("backup", orig)
doc = json.load(open(orig))
check(rc == 0 and len(doc["presets"]) == 80 and doc["record_size"] == 164, f"backup writes 80 presets as JSON ({out.splitlines()[-1]})")
check(doc["presets"][0]["name"] and doc["presets"][40]["empty"], f"P01 is named '{doc['presets'][0]['name']}', P41 is EMPTY")
doc["presets"][4]["name"] = "TEST BACKUP A"; doc["presets"][4]["seq"] = 7; doc["presets"][4]["params"][2][0] = 12345
doc["presets"][60]["name"] = "TEST BACKUP B"; doc["presets"][60]["empty"] = False; doc["presets"][60]["amp_model"] = 3
json.dump(doc, open(edit, "w"))
rc, out = run("verify", edit)
check(rc == 1 and "[5, 61]" in out, "verify sees exactly the two edited presets differ: " + out.splitlines()[-1])
rc, out = run("restore", edit, "--slots", "5,61")
check(rc == 0 and "identical" in out, "restoring slots 5 and 61 reads back identical")
rc, out = run("verify", edit)
check(rc == 0, "the whole bank now matches the edited file: " + out.splitlines()[-1])
rc, out = run("restore", orig)
check(rc == 0, "restoring the original file succeeds")
rc, out = run("verify", orig)
check(rc == 0 and out.endswith("identical"), "the bank is the original again (EMPTY slots still EMPTY, counters unchanged): " + out.splitlines()[-1])
open(os.path.join(tmp, "bad.json"), "w").write("{not json")
rc, out = run("show", os.path.join(tmp, "bad.json"))
check(rc != 0 and "cannot read" in out, "a damaged file is refused")
doc["presets"][4]["params"][2] = [1, 2, 3]
json.dump(doc, open(os.path.join(tmp, "short.json"), "w"))
rc, out = run("show", os.path.join(tmp, "short.json"))
check(rc != 0 and "cannot read" in out, "a record with the wrong number of knob values is refused")
print("RESULT:", "FAILED" if fails else "ALL PASSED")
sys.exit(1 if fails else 0)
