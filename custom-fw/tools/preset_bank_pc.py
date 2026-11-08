#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-only
# Copyright (C) 2026 The Onyx Authors (@28spawn)
"""
custom-fw/tools/preset_bank_pc.py - save and restore the unit's 80 presets over USB-MIDI, as a plain JSON file.

    python custom-fw/tools/preset_bank_pc.py backup  my_presets.json            read P01..P80 from the unit into a JSON file
    python custom-fw/tools/preset_bank_pc.py verify  my_presets.json            compare the file with what the unit holds (writes nothing)
    python custom-fw/tools/preset_bank_pc.py restore my_presets.json [--slots 41-80] [--yes]
                                                                                 write the file's presets back (each one is a flash bank commit, about 0.2 s)
    python custom-fw/tools/preset_bank_pc.py show    my_presets.json            list the names in a file

The JSON holds one object per preset with every field of the unit's preset record, readable and editable (names, the 8 slots' models, the knob values as raw Q16 numbers and, for reading,
as percentages). A restore rebuilds each record from those fields and the unit checks its CRC; "params_pct" is informational and ignored on restore. A restore keeps each preset exactly as it
was saved, including its save counter and whether the slot was EMPTY. A restored preset becomes active the next time you select that slot.

The unit side: PA_CMD_PA_PRESET (cmd 83) reads one record; the restore is a preset upload with target PA_UPLOAD_PRESET_RESTORE (4). The frames are built by the web editor page's own code
(web/Onyx_Web_Editor.html, run in Node), python-rtmidi is the transport; `--emu <dll>` drives the emulator DLL instead (host tests, no hardware). Close MIDIberry / the browser editor and
every other tool that uses the unit's MIDI port first.
"""
import argparse, json, re, struct, subprocess, sys, tempfile, time, zlib
from pathlib import Path

FW = Path(__file__).resolve().parent.parent
COUNT = 80
PRESET_MAGIC = 0x32504D50
CMD_PRESET, REPLY_PRESET = 83, 0x63
UPLOAD_RESTORE = 4
# pa_preset_t as the unit stores it: magic seq name[24] bpm genre cab_idx cab_blend trim gate mask order[8] amp cab vol flags type[8] stomp[4] params[8][6] crc
LAYOUT = "<II24sHBBBbBB8sBBBB8s4s96sI"
SIZE = struct.calcsize(LAYOUT)
assert SIZE == 164, SIZE

TMP = tempfile.mkdtemp(prefix="pa_bank_")


def _js_setup():
    page = (FW / "web" / "Onyx_Web_Editor.html").read_text(encoding="utf-8")
    (Path(TMP) / "page.js").write_text(re.search(r"<script>(.*)</script>", page, re.S).group(1), encoding="utf-8")
    (Path(TMP) / "run.js").write_text("const c=require('./page.js');const ops=JSON.parse(require('fs').readFileSync(0,'utf8'));"
        "console.log(JSON.stringify(ops.map(([f,...a])=>{const r=c[f](...a);return ArrayBuffer.isView(r)?Array.from(r):r;})));")


def js(*ops):
    r = subprocess.run(["node", str(Path(TMP) / "run.js")], input=json.dumps(list(ops)), capture_output=True, text=True, cwd=TMP)
    if r.returncode:
        raise SystemExit(r.stderr)
    return json.loads(r.stdout)


class MidiLink:
    def __init__(self, port):
        import rtmidi
        self.mi, self.mo = rtmidi.MidiIn(), rtmidi.MidiOut()
        self.mi.ignore_types(sysex=False)
        pi = next((i for i, n in enumerate(self.mi.get_ports()) if port.lower() in n.lower()), None)
        po = next((i for i, n in enumerate(self.mo.get_ports()) if port.lower() in n.lower()), None)
        if pi is None or po is None:
            raise SystemExit(f"no MIDI port with '{port}' in its name: is the unit connected and running?")
        self.mi.open_port(pi)
        self.mo.open_port(po)

    def send(self, frame):
        while self.mi.get_message():
            pass
        self.mo.send_message(frame)

    def reply(self, want, wait=3.0):
        t0 = time.time()
        while time.time() - t0 < wait:
            m = self.mi.get_message()
            if not m:
                time.sleep(0.001)
                continue
            if m[0][0] == 0xF0:
                r = js(["parseReply", m[0]])[0]
                if r and (want is None or r["type"] == want):
                    return r
        return None

    def settle(self, s):
        time.sleep(s)


class EmuLink:
    """the emulator DLL: no hardware, no waiting"""
    def __init__(self, dll):
        import ctypes
        self.c = ctypes
        self.d = ctypes.CDLL(dll)
        self.d.emu_init()
        self.d.emu_feed_sysex_bytes.argtypes = [ctypes.POINTER(ctypes.c_uint8), ctypes.c_uint32]
        self.d.emu_next_sysex_reply.argtypes = [ctypes.c_char_p, ctypes.c_int]
        self.d.emu_process_audio_f32.argtypes = [ctypes.POINTER(ctypes.c_float)] * 3 + [ctypes.c_int]
        self.inp, self.l, self.r = (ctypes.c_float * 64)(), (ctypes.c_float * 64)(), (ctypes.c_float * 64)()

    def _tick(self, n=4):
        for _ in range(n):
            self.d.emu_process_audio_f32(self.inp, self.l, self.r, 64)

    def _pop(self):
        buf = self.c.create_string_buffer(1024)
        n = self.d.emu_next_sysex_reply(buf, 1024)
        return buf.raw[:n] if n else None

    def send(self, frame):
        while self._pop() is not None:
            pass
        b = (self.c.c_uint8 * len(frame))(*frame)
        self.d.emu_feed_sysex_bytes(b, len(frame))
        self._tick()

    def reply(self, want, wait=3.0):
        for _ in range(40):
            raw = self._pop()
            if raw is not None:
                r = js(["parseReply", list(raw)])[0]
                if r and (want is None or r["type"] == want):
                    return r
            else:
                self._tick()
        return None

    def settle(self, s):
        self._tick(8)


def read_preset(link, i):
    link.send(js(["buildFrame", CMD_PRESET, [i]])[0])
    r = link.reply(REPLY_PRESET)
    if not r:
        raise SystemExit(f"P{i + 1:02d}: no reply from the unit (is this a firmware with the preset-read command, 1.07 or newer?)")
    d = bytes(r["data"])
    return d[1], d[2:2 + SIZE]


def upload_reliable(link, data, target, slot, chunk=100):
    """An acknowledged upload (firmware 1.13+): begin and every chunk are answered with how many bytes the unit holds (reply 0x64), and a chunk it dropped is sent again from there.
    Returns True when the unit stored it, None when the firmware does not acknowledge (the caller then falls back to the blind frames)."""
    n = len(data)
    link.send(js(["buildFrame", 25, [n & 255, (n >> 8) & 255, (n >> 16) & 255, target, slot]])[0])
    r = link.reply(0x64, 2.5)
    if not r:
        return None
    ack = r["data"]
    if ack[3] == 2:
        return False
    off, bad = ack[0] | ack[1] << 8 | ack[2] << 16, 0
    while off < n:
        part = list(data[off:off + chunk])
        link.send(js(["buildFrame", 34, part])[0])
        r = link.reply(0x64, 2.0)
        if not r:
            link.send(js(["buildFrame", 34, []])[0]); r = link.reply(0x64, 2.0)
            bad += 1
            if not r:
                if bad > 8:
                    return False
                continue
        ack = r["data"]
        if ack[3] == 1:
            return True
        if ack[3] in (2, 3):
            return False
        got = ack[0] | ack[1] << 8 | ack[2] << 16
        if got != off + len(part):
            bad += 1
            if bad > 12:
                return False
        off = got
    return True


def rec_ok(rec):
    return len(rec) == SIZE and struct.unpack_from("<I", rec, 0)[0] == PRESET_MAGIC and zlib.crc32(rec[:-4]) & 0xFFFFFFFF == struct.unpack_from("<I", rec, SIZE - 4)[0]


def rec_to_obj(slot, rec):
    (magic, seq, name, bpm, genre, cab_idx, cab_blend, trim, gate, mask, order, amp, cab, vol, flags, types, stomp, params, crc) = struct.unpack(LAYOUT, rec)
    q = struct.unpack("<48H", params)
    return {"slot": slot + 1, "name": name.split(b"\0")[0].decode("latin-1"), "empty": bool(flags & 1), "seq": seq, "bpm": bpm, "genre": genre,
            "custom_cab_idx": cab_idx, "custom_cab_blend": cab_blend, "input_trim_db": trim, "gate_threshold_pct": gate, "slot_enable_mask": mask,
            "slot_order": list(order), "amp_model": amp, "cab_model": cab, "preset_vol": vol, "flags": flags, "slot_type": list(types), "stomp_map": list(stomp),
            "params": [list(q[i * 6:(i + 1) * 6]) for i in range(8)],
            "params_pct": [[round(v * 100 / 65535) for v in q[i * 6:(i + 1) * 6]] for i in range(8)]}


def obj_to_rec(o):
    flags = (o.get("flags", 0) & ~1) | (1 if o.get("empty") else 0)
    q = [int(v) & 0xFFFF for row in o["params"] for v in row]
    if len(q) != 48 or len(o["slot_order"]) != 8 or len(o["slot_type"]) != 8 or len(o["stomp_map"]) != 4:
        raise ValueError("slot_order/slot_type need 8 entries, stomp_map 4, params 8 rows of 6")
    name = o["name"].encode("latin-1", "replace")[:23].ljust(24, b"\0")
    body = struct.pack(LAYOUT[:-1], PRESET_MAGIC, int(o["seq"]), name, o["bpm"], o["genre"], o["custom_cab_idx"], o["custom_cab_blend"], o["input_trim_db"],
                       o["gate_threshold_pct"], o["slot_enable_mask"], bytes(o["slot_order"]), o["amp_model"], o["cab_model"], o["preset_vol"], flags,
                       bytes(o["slot_type"]), bytes(o["stomp_map"]), struct.pack("<48H", *q))
    return body + struct.pack("<I", zlib.crc32(body) & 0xFFFFFFFF)


def write_bank(path, items):
    doc = {"format": "pocket-amp-presets", "version": 1, "record_size": SIZE,
           "note": "Onyx preset backup. Edit names and values freely; 'params' are Q16 (0..65535 = 0..100 %), 'params_pct' is for reading only. Slot order: DYN DRV AMP CAB EQ MOD DLY REV.",
           "presets": [rec_to_obj(i, rec) if st == 0 and rec_ok(rec) else {"slot": i + 1, "unreadable": True} for i, (st, rec) in enumerate(items)]}
    Path(path).write_text(json.dumps(doc, indent=1), encoding="utf-8")


def read_bank(path):
    try:
        doc = json.loads(Path(path).read_text(encoding="utf-8"))
        if doc.get("format") != "pocket-amp-presets":
            raise ValueError("not a pocket-amp-presets file (an .abkx from the vendor editor is a different format)")
        out = [None] * COUNT
        for o in doc["presets"]:
            i = int(o["slot"]) - 1
            if 0 <= i < COUNT:
                out[i] = (1, b"\0" * SIZE) if o.get("unreadable") else (0, obj_to_rec(o))
    except (ValueError, KeyError, TypeError, struct.error, OSError) as e:
        raise SystemExit(f"{path}: cannot read: {e}")
    return [x if x else (1, b"\0" * SIZE) for x in out]


def rec_name(rec):
    return rec[8:32].split(b"\0")[0].decode("latin-1", "replace")


def parse_slots(spec):
    if not spec:
        return list(range(COUNT))
    out = set()
    for part in spec.split(","):
        a, _, z = part.partition("-")
        out.update(range(int(a) - 1, int(z or a)))
    return sorted(i for i in out if 0 <= i < COUNT)


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("cmd", choices=["backup", "verify", "restore", "show"])
    ap.add_argument("file")
    ap.add_argument("--port", default="Custom MIDI")
    ap.add_argument("--emu", help="drive this emulator DLL instead of the unit")
    ap.add_argument("--slots", help="restore/verify only these presets, e.g. 41-80 or 1-10,15 (1-based)")
    ap.add_argument("--yes", action="store_true", help="do not ask before restoring")
    a = ap.parse_args()

    if a.cmd == "show":
        for i, (st, rec) in enumerate(read_bank(a.file)):
            print(f"P{i + 1:02d}  {'ok ' if st == 0 and rec_ok(rec) else 'BAD'}  {rec_name(rec)}")
        return 0

    _js_setup()
    link = EmuLink(a.emu) if a.emu else MidiLink(a.port)

    if a.cmd == "backup":
        items = []
        for i in range(COUNT):
            items.append(read_preset(link, i))
            print(f"\rreading P{i + 1:02d}/{COUNT}", end="", flush=True)
        print()
        bad = [i + 1 for i, (st, rec) in enumerate(items) if st != 0 or not rec_ok(rec)]
        write_bank(a.file, items)
        print(f"saved {a.file}: {COUNT} presets" + (f"; unreadable on the unit: {bad}" if bad else ""))
        return 0 if not bad else 1

    bank = read_bank(a.file)
    slots = parse_slots(a.slots)

    if a.cmd == "verify":
        diff = [i + 1 for i in slots if read_preset(link, i) != bank[i]]
        print("identical" if not diff else f"{len(diff)} preset(s) differ: {diff}")
        return 0 if not diff else 1

    todo = [(i, bank[i][1]) for i in slots if bank[i][0] == 0 and rec_ok(bank[i][1])]
    skipped = [i + 1 for i in slots if not (bank[i][0] == 0 and rec_ok(bank[i][1]))]
    if not todo:
        print("nothing to restore")
        return 1
    print(f"restoring {len(todo)} preset(s): P{todo[0][0] + 1:02d}..P{todo[-1][0] + 1:02d}" + (f"; skipping unreadable {skipped}" if skipped else ""))
    if not a.yes and not a.emu:
        if input("This OVERWRITES those presets on the unit. Type RESTORE to go on: ").strip() != "RESTORE":
            print("cancelled")
            return 1
    failed = []
    for n, (i, rec) in enumerate(todo):
        ok = upload_reliable(link, rec, UPLOAD_RESTORE, i)
        if ok is None:                                      # a firmware that does not acknowledge: the blind frames, as before
            for f in js(["uploadFrames", list(rec), UPLOAD_RESTORE, i, 100])[0]:
                link.send(f)
                link.settle(0.01)
            link.settle(0.35)                               # the unit commits the whole bank to flash (about 0.2 s) before it takes the next one
        elif ok is False:
            failed.append(i + 1)
            continue
        if read_preset(link, i) != (0, rec):
            failed.append(i + 1)
        print(f"\rwrote P{i + 1:02d}  ({n + 1}/{len(todo)})", end="", flush=True)
    print()
    print("restore complete, every preset read back identical" if not failed else f"restore finished but these did not read back as sent: {failed}")
    return 0 if not failed else 1


if __name__ == "__main__":
    sys.exit(main())
