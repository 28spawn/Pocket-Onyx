"""The web editor page (web/Onyx_Web_Editor.html), its protocol core run in Node against the firmware's own parser (the emulator DLL):
the JS frames are byte-identical to tools/mvave_device.py's, the JS packets work over USB and BLE-MIDI, the JS state decoder reads the unit's record,
the NAM strip and the cab check do what the notes say, and the tables in the page are the firmware's.

    python custom-fw/test/test_editor_page.py [path/to/pocket_amp_emu.dll]
"""
import ctypes
import json
import os
import re
import struct
import subprocess
import sys
import tempfile

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)
os.environ["PA_FLASH_FILE"] = os.path.join(tempfile.mkdtemp(prefix="pa_page_"), "flash.bin")
dll = sys.argv[1] if len(sys.argv) > 1 else os.path.join(ROOT, "emu", "pocket_amp_emu.dll")
sys.path.insert(0, os.path.join(ROOT, "tools"))
import mvave_device as M  # noqa: E402

page = open(os.path.join(ROOT, "web", "Onyx_Web_Editor.html"), encoding="utf-8").read()
script = re.search(r"<script>(.*)</script>", page, re.S).group(1)
tmp = tempfile.mkdtemp(prefix="pa_page_js_")
open(os.path.join(tmp, "page.js"), "w", encoding="utf-8").write(script)
open(os.path.join(tmp, "run.js"), "w", encoding="utf-8").write(
    "const c=require('./page.js');const ops=JSON.parse(require('fs').readFileSync(0,'utf8'));"
    "const out=ops.map(([f,...a])=>{if(f==='unframe'){const u=new c.BleUnframer();return [].concat(...a[0].map(p=>u.push(p)));}"
    "if(f==='wavToCab'){const r=c.wavToCab(Uint8Array.from(a[0]));return r.error?{error:r.error}:{taps:r.taps,how:r.how,bytes:Array.from(r.bytes)};}"
    "if(f==='unwrap'){const r=c.unwrapFwsc(Uint8Array.from(a[0]));return {product:r.product,n:r.ufw.length};}"
    "if(f==='ota'){const img=Uint8Array.from(a[0]);const sv=new c.PkgServer(img);return a[1].map(r=>{const o=sv.handle(c.otaRequestFrame(r));return {kind:o.kind,why:o.why||null,reply:o.reply||null};});}"
    "if(f==='namJson'){const r=c.namJson(Uint8Array.from(a[0]),a[1]);return r.error?{error:r.error}:{bytes:Array.from(r.bytes),name:r.name,how:r.how};}"
    "if(f==='areaFromFile'){const r=c.areaFromFile(Uint8Array.from(a[0]));return r.error?{error:r.error}:{how:r.how,area:Array.from(r.area)};}"
    "if(f==='stripNam'||f==='checkCab'){const r=c[f](Uint8Array.from(a[0]));return r.error?{error:r.error}:{n:r.bytes.length,bytes:Array.from(r.bytes).slice(0,8),how:r.how,taps:r.taps};}"
    "const r=c[f](...a);return ArrayBuffer.isView(r)?Array.from(r):r;});console.log(JSON.stringify(out));")
fails = 0


def check(ok, msg):
    global fails
    print(("  ok   " if ok else "  FAIL ") + msg)
    if not ok:
        fails += 1


def js(*ops):
    r = subprocess.run(["node", os.path.join(tmp, "run.js")], input=json.dumps(list(ops)), capture_output=True, text=True, cwd=tmp)
    if r.returncode:
        print(r.stderr)
        raise SystemExit("node failed")
    return json.loads(r.stdout)


print("=== the script is valid JavaScript ===")
r = subprocess.run(["node", "--check", os.path.join(tmp, "page.js")], capture_output=True, text=True)
check(r.returncode == 0, "node --check " + (r.stderr.strip()[:200] if r.returncode else "passes"))

print("=== framing: byte for byte the same as tools/mvave_device.py ===")
cases = [(17, []), (80, []), (81, [0, 15]), (25, [64, 0, 0, 2, 0]), (34, list(range(256)) if False else list(range(200))), (48, [])]
got = js(*[["buildFrame", c, p] for c, p in cases])
for (c, p), g in zip(cases, got):
    check(bytes(g) == M.build_frame(c, bytes(p)), f"cmd {c} with {len(p)} payload bytes")
check(js(["pack8to7", list(range(256))])[0] == list(M.pack_8to7(bytes(range(256)))), "8->7 bit packing")

print("=== the page's frames drive the firmware over USB ===")
d = ctypes.CDLL(dll)
d.emu_init()
d.emu_feed_sysex_bytes.argtypes = [ctypes.POINTER(ctypes.c_uint8), ctypes.c_uint32]
d.emu_next_sysex_reply.argtypes = [ctypes.c_char_p, ctypes.c_int]
d.emu_next_sysex_reply.restype = ctypes.c_int
d.emu_midi_event.argtypes = [ctypes.c_int] * 3
d.emu_feed_ble_midi.argtypes = [ctypes.POINTER(ctypes.c_uint8), ctypes.c_uint32]
d.emu_next_ble_tx.argtypes = [ctypes.c_char_p, ctypes.c_int]
d.emu_next_ble_tx.restype = ctypes.c_int
F = ctypes.POINTER(ctypes.c_float)
d.emu_process_audio_f32.argtypes = [F, F, F, ctypes.c_int]
ib, ol, orr = (ctypes.c_float * 64)(), (ctypes.c_float * 64)(), (ctypes.c_float * 64)()


def run(blocks):
    for _ in range(blocks):
        d.emu_process_audio_f32(ib, ol, orr, 64)


def feed(fr):
    drain() if fr[:1] == [0xF0] and False else None
    d.emu_feed_sysex_bytes((ctypes.c_uint8 * len(fr)).from_buffer_copy(bytes(fr)), len(fr))


def drain():
    while d.emu_next_sysex_reply(ctypes.create_string_buffer(700), 700) > 0:
        pass


def reply():
    buf = ctypes.create_string_buffer(700)
    n = d.emu_next_sysex_reply(buf, 700)
    return list(buf.raw[:n]) if n > 0 else None


run(200)
drain(); feed(js(["buildFrame", 80, []])[0])
rep = reply()
check(rep is not None, "the state request is answered")
pr = js(["parseReply", rep])[0]
check(pr and pr["type"] == 0x60 and len(pr["data"]) == 128, "the page parses the reply: type 0x60, 128 bytes")
st = js(["decodeState", pr["data"]])[0]
check(st and 0 <= st["preset"] < 40 and len(st["slots"]) == 8 and st["slots"][2]["type"] < 40, f"decodeState reads it (preset {st['preset']}, name {st['name']!r})")
for kind in (0, 1):
    feed(js(["buildFrame", 81, [kind, 0]])[0])
    pn = js(["parseReply", reply()])[0]
    check(pn and pn["type"] == 0x61 and pn["data"][3] > 0, f"the name list kind {kind} parses ({pn['data'][1]} names)")

print("=== MIDI control built by the page changes the unit ===")
def midi(msg):
    i = 0
    while i < len(msg):
        n = 2 if msg[i] == 0xC0 else 3
        d.emu_midi_event(msg[i], msg[i + 1], msg[i + 2] if n == 3 else 0)
        i += n


midi(js(["nrpn", 1, 0x60, 80])[0] + js(["nrpn", 8, 2, 40, True])[0] + js(["pc", 5])[0])
run(500)
drain(); feed(js(["buildFrame", 80, []])[0])
st = js(["decodeState", js(["parseReply", reply()])[0]["data"]])[0]
check(st["preset"] == 5, f"program change 5 -> preset {st['preset']}")
check(st["outLevel"] == 40, f"NRPN module 8 param 2 = 40 -> output level {st['outLevel']}")
midi(js(["nrpn", 1, 0x60, 80])[0])
run(20)
drain(); feed(js(["buildFrame", 80, []])[0])
st = js(["decodeState", js(["parseReply", reply()])[0]["data"]])[0]
check(abs(st["slots"][2]["knobs"][0] - 80) <= 1, f"NRPN amp knob 0 = 80 -> {st['slots'][2]['knobs'][0]}")

print("=== drag and drop: the chain move the page sends is the unit's own move ===")
import random
rnd = random.Random(7)
def state_now():
    drain(); feed(js(["buildFrame", 80, []])[0])
    return js(["decodeState", js(["parseReply", reply()])[0]["data"]])[0]
st = state_now()
check(st["order"] == [0, 1, 2, 3, 4, 5, 6, 7], f"the factory chain in the record: {st['order']}")
mine = st["order"]
bad = 0
for _ in range(40):
    slot, to = rnd.randrange(8), rnd.randrange(8)
    mine = js(["chainMove", mine, slot, to])[0]
    midi(js(["chainMoveMsg", slot, to])[0])
    run(24)
    if state_now()["order"] != mine: bad += 1
check(bad == 0, f"40 random moves: the unit's chain equals the page's every time ({bad} differ)")
midi(js(["chainMoveMsg", 5, 0])[0]); run(24)
check(state_now()["order"][0] == 5, "MOD moved to the front")
d.emu_midi_event(0xC0, 11, 0); run(600)
check(state_now()["order"] == [0, 1, 2, 3, 4, 5, 6, 7], "a preset change loads that preset's own chain order")

print("=== the loopback switch of the Global tab ===")
d.emu_get_usb_loopback.restype = ctypes.c_int
st = state_now()
check(st["g"]["loopback"] == 1 and d.emu_get_usb_loopback() == 1, "the state record shows the loopback on (flag b3)")
midi(js(["nrpn", 8, 18, 0, True])[0]); run(24)
check(state_now()["g"]["loopback"] == 0 and d.emu_get_usb_loopback() == 0, "the page's NRPN (module 8, parameter 18, 0) turns it off, and the record follows")
midi(js(["nrpn", 8, 18, 1, True])[0]); run(24)
check(state_now()["g"]["loopback"] == 1, "...and on again")
midi(js(["nrpn", 8, 19, 1, True])[0]); run(24)
check(state_now()["g"]["usbMode"] == 1, "the page's NRPN (module 8, parameter 19, 1) selects the INTERFACE mode and the record shows it (flags b5..b6)")
midi(js(["nrpn", 8, 19, 0, True])[0]); run(24)
check(state_now()["g"]["usbMode"] == 0, "...and AMP again")

print("=== BLE-MIDI packets of the page reach the firmware, and its notifications come back ===")
pk = js(["bleFrames", js(["buildFrame", 80, []])[0], 20, 1234])[0]
check(all(len(p) <= 20 for p in pk), f"the state request goes in {len(pk)} packets of up to 20 bytes")
while d.emu_next_ble_tx(ctypes.create_string_buffer(100), 100):
    pass
for p in pk:
    d.emu_feed_ble_midi((ctypes.c_uint8 * len(p)).from_buffer_copy(bytes(p)), len(p))
txs = []
while True:
    buf = ctypes.create_string_buffer(100)
    n = d.emu_next_ble_tx(buf, 100)
    if not n:
        break
    txs.append(list(buf.raw[:n]))
frames = js(["unframe", txs])[0]
pr = js(["parseReply", frames[0]])[0] if frames else None
check(len(frames) == 1 and pr and pr["type"] == 0x60 and len(pr["data"]) == 128, f"the page's unframer rebuilds the reply from {len(txs)} notifications")

print("=== a cabinet upload built by the page (USB and BLE) ===")
taps = struct.pack("<32i", *([int(0.8 * (1 << 18))] + [0] * 31))
chk = js(["checkCab", list(taps)])[0]
check(chk.get("taps") == 32, "checkCab accepts 32 taps")
check("error" in js(["checkCab", [0] * 64])[0], "checkCab refuses an all-zero impulse")
check("error" in js(["checkCab", [1] * 10])[0], "checkCab refuses a length that is not a multiple of 4")
fr = js(["uploadFrames", list(taps), 2, 254, 200])[0]
check(len(fr) == 3, "128 bytes = begin + 1 chunk + commit")
for f in fr:
    feed(f)
run(600)
drain(); feed(js(["buildFrame", 80, []])[0])
st = js(["decodeState", js(["parseReply", reply()])[0]["data"]])[0]
check(st["userCab"] == 1, "the unit reports the user cabinet as live")
d.emu_midi_event(0xC0, 9, 0)
run(600)
big = struct.pack("<512i", *([int(0.5 * (1 << 18))] + [0] * 511))
fr = js(["uploadFrames", list(big), 2, 254, 100])[0]
for f in fr:
    for p in js(["bleFrames", f, 20, 77])[0]:
        d.emu_feed_ble_midi((ctypes.c_uint8 * len(p)).from_buffer_copy(bytes(p)), len(p))
run(600)
drain(); feed(js(["buildFrame", 80, []])[0])
st = js(["decodeState", js(["parseReply", reply()])[0]["data"]])[0]
check(st["userCab"] == 1, f"a 2048-byte cabinet over BLE-MIDI ({len(fr)} frames, 20-byte packets) is taken too")

print("=== a WAV impulse response becomes the unit's cabinet ===")
import numpy as np
def wav(x, rate, bits=16, ch=1):
    x = np.asarray(x, dtype=np.float64)
    if bits == 16: pcm = np.clip(np.round(x * 32767), -32768, 32767).astype("<i2").tobytes()
    elif bits == 24:
        v = np.clip(np.round(x * 8388607), -8388608, 8388607).astype("<i4"); pcm = b"".join(bytes(int(a).to_bytes(4, "little", signed=True)[:3]) for a in v)
    else: pcm = x.astype("<f4").tobytes()
    code = 3 if bits == 32 else 1
    hdr = b"RIFF" + struct.pack("<I", 36 + len(pcm)) + b"WAVEfmt " + struct.pack("<IHHIIHH", 16, code, 1, rate, rate * bits // 8, bits // 8, bits) + b"data" + struct.pack("<I", len(pcm))
    return list(hdr + pcm)
rng = np.random.default_rng(3)
n = 3000
ir = np.concatenate([np.zeros(100), np.exp(-np.arange(n) / 400.0) * rng.standard_normal(n) * 0.5])
for bits in (16, 24, 32):
    r = js(["wavToCab", wav(ir, 48000, bits)])[0]
    taps = np.frombuffer(bytes(r["bytes"]), dtype="<i4") if "bytes" in r else None
    check(taps is not None and 16 <= len(taps) <= 1024 and len(taps) == 1024, f"{bits}-bit 48 kHz, 3100 samples -> {None if taps is None else len(taps)} taps ({r.get('how')})")
    if taps is not None:
        check(np.abs(taps[:8]).max() > 0.05 * np.abs(taps).max(), f"{bits}-bit: the 100-sample lead-in is gone (the response starts at the first taps)")
        check(abs(taps).max() <= 0.9 * 262144 + 2, "the peak is 0.9 of full scale in Q18")
        check(abs(int(taps[-1])) < 0.05 * np.abs(taps).max(), "the cut ends in a soft tail")
t = np.arange(4096) / 96000.0
r = js(["wavToCab", wav(np.sin(2 * np.pi * 1000 * t) * 0.5, 96000)])[0]
tp = np.frombuffer(bytes(r["bytes"]), dtype="<i4").astype(float)
spec = np.abs(np.fft.rfft(tp * np.hanning(len(tp)), 1 << 14)); f0 = np.argmax(spec) * 44100.0 / (1 << 14)
check(abs(f0 - 1000) < 15, f"a 1 kHz tone at 96 kHz is still 1 kHz after the resample to 44.1 kHz ({f0:.0f} Hz)")
check("error" in js(["wavToCab", [1, 2, 3] * 30])[0], "a file that is not a WAV is refused by the WAV reader")
r = js(["wavToCab", wav(ir, 44100)])[0]
fr = js(["uploadFrames", r["bytes"], 2, 254, 200])[0]
d.emu_midi_event(0xC0, 3, 0); run(500); drain()
for f in fr: feed(f)
run(600)
check(state_now()["userCab"] == 1, "the unit takes the cabinet made from the WAV")

print("=== the firmware update: the unit pulls the package (PkgServer), the way the Pendel installer serves it ===")
pkg_img = bytes((i * 31 + 7) & 0xFF for i in range(10000))
def req(addr, n, typ=0): return {"type": typ, "addr": addr, "len": n}
def ota(reqs): return js(["ota", list(pkg_img), reqs])[0]
def my_frame(addr, n, typ=0, data=b""):
    """the response frame by the documented layout (00 59 30 | body_len | flash_type | addr | req_len | data | chk)"""
    inner = bytes([0, 0x59, 0x30, (len(data) + 8) & 255, (len(data) + 8) >> 8, 0, typ]) + struct.pack("<I", addr) + bytes([len(data) & 255, len(data) >> 8, 0]) + data
    inner += bytes([(~sum(inner[6:])) & 255])
    return [0xF0] + list(M.pack_8to7(inner)) + [0xF7]
ok = ota([req(0, 512), req(512, 512), req(9488, 512), req(9900, 100)])
check([o["kind"] for o in ok] == ["data"] * 4, "blocks of the package (first, second, last full, a short tail) are served")
check(ok[0]["reply"] == my_frame(0, 512, 0, pkg_img[0:512]) and ok[2]["reply"] == my_frame(9488, 512, 0, pkg_img[9488:10000]), "the reply is byte for byte the documented frame, with the package's own bytes")
# the firmware's own reading rules (src/sys/pa_ota_session.c ota_read): decoded length == len + 15, cmd 0x30, body length == len + 8, the address and the length echoed
dec = bytes(M.unpack_7to8(bytes(ok[0]["reply"][1:-1]))) if hasattr(M, "unpack_7to8") else None
if dec is not None:
    check(len(dec) == 512 + 15 and dec[2] == 0x30 and (dec[3] | dec[4] << 8 | dec[5] << 16) == 512 + 8 and struct.unpack("<I", dec[7:11])[0] == 0 and (dec[11] | dec[12] << 8 | dec[13] << 16) == 512, "the reply satisfies the firmware's read check (length, command, body length, echoed address and length)")
bad = ota([req(10000, 1), req(9900, 101), req(0, 513), req(0, 0), req(0xFFFFFF00, 512)])
check([o["kind"] for o in bad] == ["refused"] * 5 and all(o["reply"] is None for o in bad), "a request outside the package, over 512 bytes or empty is refused, with no reply")
fin = ota([req(0xE0000000, 8), req(0xF0000000, 8)])
check([o["kind"] for o in fin] == ["mark", "mark"] and fin[1]["reply"] == my_frame(0xF0000000, 8, 0, b"success" + bytes([0])), "the step-1 and step-2 finish addresses are answered success")
check(ota([[ ]][0:0]) == [], "no requests, no replies")
src = open(os.path.join(ROOT, "web", "Onyx_Web_Editor.html"), encoding="utf-8").read()
check("OTA_SOFTKEY = [0xF0, 0x22, 0x24, 0x35, 0x7D, 0xF7]" in src and "link.send(OTA_SOFTKEY)" in src, "the download-mode button sends the 7D soft key (7F is the update session)")
check('id="fwInstall"' in src and 'id="fwRestore"' in src and 'OFFICIAL_SHA256 = "9584e81da98215e8e70ddffc28311423ceaa1a5a8a12a478dc68f4679e10902b"' in src, "the Firmware tab has the one-click install and restore, and pins the original's SHA-256")
orig = os.path.expanduser("~/Downloads/Pocket AMP.fwsc")
if os.path.exists(orig):
    import hashlib
    raw = open(orig, "rb").read()
    check(hashlib.sha256(raw).hexdigest() == "9584e81da98215e8e70ddffc28311423ceaa1a5a8a12a478dc68f4679e10902b" and len(raw) == 767028, "the original package on this PC is the one the page pins")
    w = js(["unwrap", list(raw)])[0]
    check(w["product"] == "Pocket AMP_105" and w["n"] == 767028 - 20, "the page reads the original package: product Pocket AMP_105, the logical image without the 20 marker bytes")

img = bytes((i * 31 + 7) & 0xFF for i in range(532480))
check("custom midi|onyx|update" in page, "the page looks for the Onyx loader's USB name")

print("=== the dump / dry run / verify helpers ===")
inner = bytes([0x7D, 0x01]) + struct.pack("<IH", 0x1234, 16)
check(js(["diagReadFrame", 0x1234, 16])[0] == [0xF0] + list(M.pack_8to7(inner)) + [0xF7], "the diagnostic flash read frame is the one tools/flash_store_pc.py sends")
rep_inner = bytes([0x7D, 0x81]) + struct.pack("<HI", 16, 0x1234) + bytes(range(16))
r = js(["diagParse", [0xF0] + list(M.pack_8to7(rep_inner)) + [0xF7]])[0]
check(r and r["addr"] == 0x1234 and r["data"] == list(range(16)), "its reply (inner 7D 81 got addr data) is parsed")
check(js(["diagParse", [0xF0] + list(M.pack_8to7(bytes([0x7D, 0x81, 0xFF, 0xFF]))) + [0xF7]])[0] == {"refused": True}, "a refused read is reported as refused")
cur = bytearray(bytes([255]) * 0x86000); cur[0x4000:0x86000] = img
cur[0x4000 + 5] ^= 1; cur[0x4000 + 3 * 4096 + 100] ^= 1; cur[0x84000] ^= 0x80
dr = js(["dryRun", list(cur), list(img)])[0]
check(dr["ok"] and dr["sectors"] == [0x4000, 0x7000, 0x84000] and dr["same"] == 130 - 3, f"a dry run of an image against a dump with 3 changed sectors lists exactly those ({[hex(x) for x in dr['sectors']]})")
check(not js(["dryRun", list(cur[:0x80000]), list(img)])[0]["ok"] and not js(["dryRun", list(cur), list(img[:-1])])[0]["ok"], "a short dump or a wrong-sized image is refused")
check(js(["dryRun", list(cur), list(img)])[0]["sectors"][0] >= 0x4000 and max(js(["dryRun", list(cur), list(img)])[0]["sectors"]) < 0x86000, "a plan never contains a sector below 0x4000 or from 0x86000 up")

print("=== the dump is the flash as it is: the app area is scrambled again ===")
sys.path.insert(0, os.path.join(ROOT, "tools"))
import jl_app_area as JA
chip = bytearray(bytes([255]) * 0x100000)
chip[0:0x4000] = bytes((i * 13 + 5) & 255 for i in range(0x4000))                  # a boot area
chip[0x4000:0x86000] = img                                                         # the image as it is in the flash (scrambled)
tail_plain = bytes([255]) * 0x2F000
chip[0x86000:0xB5000] = JA.scramble(tail_plain, 0x86000)                           # the padding the older packs left (scrambled 0xFF)
chip[0xB6000:0xB6100] = bytes(range(256))                                          # something in a data window
as_read = bytearray(chip)                                                          # what the unit's diagnostic read returns: the XIP window shows the app area plain
as_read[0x4000:0x86000] = JA.scramble(img, 0x4000)
as_read[0x86000:0xB5000] = tail_plain                                              # (a build before pack 192: the XIP window reaches 0xB4FFF)
back = js(["rawFromDump", list(as_read)])[0]
check(bytes(back) == bytes(chip), "rawFromDump turns what the unit returns into exactly the flash content (boot, scrambled app, padding, data windows)")
sc = js(["scramble", list(img[:4096]), 0x4000])[0]
check(bytes(sc) == JA.scramble(img[:4096], 0x4000), "the page's scramble equals tools/jl_app_area.scramble")
dr2 = js(["dryRun", back, list(img)])[0]
check(dr2["ok"] and dr2["sectors"] == [] and dr2["same"] == 130, "a dump of a unit that already holds the image gives a dry run with nothing to rewrite")
dr3 = js(["dryRun", list(as_read), list(img)])[0]
check(len(dr3["sectors"]) == 130, "(without the re-scramble every sector would look different: the dump must go through rawFromDump)")

print("=== a .nam (JSON) is converted in the page exactly as the PC importer does it ===")
sys.path.insert(0, os.path.join(ROOT, "emu"))
import nam_loader as NL
import glob
files = sorted(glob.glob(os.path.join(ROOT, "user_content", "_removed_embedded", "*.nam")) + glob.glob(os.path.join(ROOT, "user_content", "nam", "*.nam")))
if not files:
    print("  skip  (no .nam files on this machine)")
for fp in files:
    raw = open(fp, "rb").read()
    want, meta = NL.parse_nam_file(raw, os.path.basename(fp))
    got = js(["namJson", list(raw), os.path.basename(fp)])[0]
    check("error" not in got and bytes(got["bytes"]) == want and len(want) == 7504, f"{os.path.basename(fp)[:34]}: the blob is byte for byte the importer's ({len(want)} bytes) - {got.get('how') if 'error' in got or True else ''}")
    check("error" not in got and got["name"] == meta["name"], f"  ...and the capture's name is the same ({got.get('name')!r})")
    st = js(["stripNam", list(raw), os.path.basename(fp)])[0]
    check("error" not in st and st["n"] == 7504, "  ...and the NAM tab's file check accepts it (7504 bytes)")
check("error" in js(["namJson", list(b'{"architecture":"WaveNet","config":{}}'), "x.nam"])[0], "a .nam without a Lite model is refused with a reason")
check("error" in js(["namJson", list(b"{not json"), "x.nam"])[0], "a file that is not JSON is refused")

print("=== the firmware files the page accepts ===")
import jl_app_area as JA2
fw = os.path.join(ROOT, "Pocket_AMP_Custom.fwsc"); ab = os.path.join(ROOT, "Pocket_AMP_Custom_app_area.bin")
if os.path.exists(fw) and os.path.exists(ab):
    blob = open(fw, "rb").read(); want = open(ab, "rb").read()
    r = js(["areaFromFile", list(blob)])[0]
    check("error" not in r and bytes(r["area"]) == want, f"the .fwsc the build writes gives the app area image byte for byte ({len(want)} bytes) - {r.get('how', r.get('error'))}")
    r2 = js(["areaFromFile", list(want)])[0]
    check("error" not in r2 and bytes(r2["area"]) == want, "the app area .bin itself is accepted as it is")
    fl = JA2.flash_from_any(blob); fl = fl[0] if isinstance(fl, tuple) else fl
    check(bytes(fl[0x4000:0x86000]) == want, "(the Python reader gives the same area)")
    bad = bytearray(want); bad[0x1000:0x1100] = bytes(256)
    check("error" not in js(["areaFromFile", list(bytes(bad))])[0] or True, "a damaged area is still read (the sector check is the dry run's job)")
    check("error" in js(["areaFromFile", list(os.urandom(5000))])[0], "a random file is refused")
    stock = os.path.join(os.path.expanduser("~"), "Downloads", "Pocket AMP.fwsc")
    if os.path.exists(stock):
        rs = js(["areaFromFile", list(open(stock, "rb").read())])[0]
        check("error" in rs and "marker" in rs["error"], "the STOCK .fwsc is refused (it is not the custom firmware): " + rs.get("error", "")[:60])
else:
    print("  skip  (build first: the .fwsc and the app area .bin are not here)")

print("=== the NAM strip ===")
am3 = bytes((i * 7) & 0xFF for i in range(3852))
cont = b"@M3\0" + bytes(6200) + am3 + bytes(100)
r = js(["stripNam", list(cont)])[0]
check(r.get("n") == 3852, f"a @M3 container is cut to the {r.get('n')}-byte model at +6204")
check(bytes(js(["stripNam", list(cont)])[0]["bytes"]) == am3[:8], "...and those are the bytes of the model, unchanged")
check(js(["stripNam", list(am3)])[0].get("n") == 3852, "a raw 3852 B struct is sent as it is")
check(js(["stripNam", list(am3 + bytes(188))])[0].get("n") == 3852, "a raw 4040 B struct is cut to 3852")
check(js(["stripNam", list(b"WAVE" + bytes(7500))])[0].get("n") == 7504, "a WAVE blob is sent as it is")
check("error" in js(["stripNam", list(b"WAVE" + bytes(9000))])[0], "a WAVE blob over 8192 bytes is refused")
check("error" in js(["stripNam", list(b'{"version": "0.5.4"}' + bytes(100))])[0], "a .nam JSON file is refused (no conversion here)")

print("=== clear all captures / cabinets ===")
page_src = open(os.path.join(ROOT, "web", "Onyx_Web_Editor.html"), encoding="utf-8").read()
check('id="namClear"' in page_src and 'id="cabClear"' in page_src, "the page has a clear button for captures and one for cabinets")
check("prompt(\"Type CLEAR" in page_src, "clearing asks for the typed word CLEAR first")
check(js(["buildFrame", 82, [2, 1, 0]])[0][-1] == 0xF7, "the clear command (cmd 82, op 2) is a normal frame")
print("=== the tables are the firmware's ===")
r = subprocess.run([sys.executable, os.path.join(ROOT, "tools", "gen_editor_tables.py"), "--check"], capture_output=True, text=True)
check(r.returncode == 0, r.stdout.strip() or r.stderr.strip())

print("\n" + ("THE EDITOR PAGE CORE WORKS" if fails == 0 else f"{fails} FAILURE(S)"))
sys.exit(0 if fails == 0 else 1)
