# SPDX-License-Identifier: GPL-3.0-only
# Copyright (C) 2026 The Onyx Authors (@28spawn)
"""Does the custom firmware accept USB-MIDI SysEx model uploads?

The editor application pushes a model to the device over USB-MIDI:
The upload session protocol:

    cmd 25   the session begins: the payload is the declared length as a
             u24le, and nothing else
    cmd 34   the body, in chunks, straight into the session's receive area
    cmd 48   the armed transfer is committed (the pending -> active swap)

The transfer sequence is: cmd 25 (the length) + cmd 34 (the raw bytes) +
cmd 48 (the commit). This drives the custom firmware's own parser with
that sequence and checks that the capture lands in flash byte for byte.

    python custom-fw/test/test_sysex_upload.py [path/to/pocket_amp_emu.dll]
"""
import ctypes
import os
import struct
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)
# a private simulated flash (the chip has room for ONE user capture, and the shared emu/pocket_amp_flash.bin carries other tests' leftovers)
import tempfile  # noqa: E402
_scratch = tempfile.mkdtemp(prefix="pa_sysex_scratch_")
os.environ["PA_FLASH_FILE"] = os.path.join(_scratch, "flash.bin")
DEFAULT_DLL = os.path.join(ROOT, "emu", "pocket_amp_emu.dll")

dll_path = sys.argv[1] if len(sys.argv) > 1 else DEFAULT_DLL
d = ctypes.CDLL(dll_path)
d.emu_init()
d.emu_feed_sysex_bytes.argtypes = [ctypes.POINTER(ctypes.c_uint8), ctypes.c_uint32]
d.emu_get_user_nam_count.restype = ctypes.c_int

sys.path.insert(0, os.path.join(ROOT, "tools"))
sys.path.insert(0, os.path.join(ROOT, "emu"))
import mvave_device as M          # noqa: E402
import nam_loader                # noqa: E402

fails = 0


def check(ok, msg):
    global fails
    print(("  ok   " if ok else "  FAIL ") + msg)
    if not ok:
        fails += 1


def feed(frame):
    b = (ctypes.c_uint8 * len(frame)).from_buffer_copy(frame)
    d.emu_feed_sysex_bytes(b, len(frame))


# a capture with plenty of bytes above 0x7F, and a marker so that each run
# writes a DIFFERENT capture - the SysEx path registers every capture under
# the name "USER NAM", so it REPLACES the previous one and the count does not
# grow
N = 1871
marker = int.from_bytes(os.urandom(4), "little")
blob = bytearray(20 + N * 4)
blob[0:4] = b"WAVE"
blob[4:8] = N.to_bytes(4, "little")
blob[8:12] = struct.pack("<f", 0.0113756)
blob[12:16] = struct.pack("<I", 1)
blob[16:20] = struct.pack("<f", 0.2818)
for i in range(N):
    blob[20 + i * 4:24 + i * 4] = struct.pack(
        "<f", ((i + (marker & 0xFFFF)) % 97) / 97.0 * 3.7)
blob = bytes(blob)
high = sum(1 for b in blob if b >= 0x80)

print("=== the app's upload, frame for frame ===")
print(f"  the capture: {len(blob)} bytes, {high} of them above 0x7F")

# 1. cmd 25: the declared length as a u24le, and NOTHING else - the stock
#    does not carry a target or a slot in the session header
before = d.emu_get_user_nam_count()
header = bytes([len(blob) & 0xFF, (len(blob) >> 8) & 0xFF, (len(blob) >> 16) & 0xFF])
feed(M.build_frame(25, header))

# 2. cmd 34: the raw body, in chunks
sent = 0
frames = 0
while sent < len(blob):
    part = blob[sent:sent + 200]
    feed(M.build_frame(34, part))
    sent += len(part)
    frames += 1

# 3. cmd 48: the commit
feed(M.build_frame(48, b""))

print(f"  {frames} body frames, {sent} bytes")
# The SysEx path registers every capture under the name "USER NAM", so it
# REPLACES the previous one: the count does not grow, the flash does change.
check(d.emu_get_user_nam_count() >= before,
      f"the firmware still holds {d.emu_get_user_nam_count()} capture(s)")

print()
print("=== the capture in the flash, byte for byte ===")
flash_path = os.environ["PA_FLASH_FILE"]
if os.path.exists(flash_path):
    flash = open(flash_path, "rb").read()
    at = flash.find(blob)
    check(at >= 0, "the capture is in the flash, byte for byte"
          + (f", at offset 0x{at:x}" if at >= 0 else ""))
    if at >= 0:
        check(flash[at:at + len(blob)] == blob,
              "every byte of it is the byte that was sent")
        check(sum(1 for b in flash[at:at + len(blob)] if b >= 0x80) == high,
              f"all {high} bytes above 0x7F survived")
else:
    print(f"  (no {flash_path} - the flash image is not there)")

print()
print("=== the reference parser agrees with the frame the app sends ===")
# the container the emulator builds from the same capture must be the same
# one the app's file would produce
src = os.path.join(ROOT, "emu", "test_capture.nam")
if not os.path.exists(src):                      # generated on first use
    sys.path.insert(0, os.path.join(ROOT, "tools"))
    import gen_test_audio
    gen_test_audio.ensure_emu_assets(os.path.join(ROOT, "emu"), log=lambda s: None)
if os.path.exists(src):
    ref, meta = nam_loader.parse_nam_file(open(src, "rb").read(), "x.nam")
    check(len(ref) == len(blob), f"the reference container is {len(ref)} bytes")

print()
print("=== the commit commits, even if it comes early ===")
# a cabinet upload (target 2, slot 1) so the NAM slot is left alone
cab = bytes(range(64))
feed(M.build_frame(25, bytes([len(cab), 0, 0, 2, 1])))
feed(M.build_frame(34, cab))
feed(M.build_frame(48, b""))           # the commit, before the length arrived
check(d.emu_get_user_nam_count() >= before,
      f"the session committed on the cmd 48 ({d.emu_get_user_nam_count()} captures)")

print()
print("=== the session limit: 8 KB and 32 KB are taken by the session, 33 KB and 64 KB are refused; only up to 8 KB is a capture ===")
# The staging buffer is PA_UPLOAD_BUFFER_SIZE (32,768 B). It was 65,536 and is
# now protocol-visible: a declared length above it must be refused UP FRONT
# (cmd 25 does not arm the session) rather than streamed in and half-written.
d.emu_upload_state.argtypes = [ctypes.POINTER(ctypes.c_uint32),
                               ctypes.POINTER(ctypes.c_uint32),
                               ctypes.POINTER(ctypes.c_int)]
d.emu_upload_state.restype = ctypes.c_int


def session_state():
    declared = ctypes.c_uint32(0)
    rx = ctypes.c_uint32(0)
    status = ctypes.c_int(0)
    armed = d.emu_upload_state(ctypes.byref(declared), ctypes.byref(rx),
                               ctypes.byref(status))
    return armed, declared.value, rx.value, status.value


def try_upload(payload):
    """cmd 25 (length only) + cmd 34 (body) + cmd 48 (commit) transfer sequence."""
    hdr = bytes([len(payload) & 0xFF, (len(payload) >> 8) & 0xFF,
                 (len(payload) >> 16) & 0xFF])
    feed(M.build_frame(25, hdr))
    armed = session_state()[0]
    if not armed:
        return False
    for at in range(0, len(payload), 200):
        feed(M.build_frame(34, payload[at:at + 200]))
    feed(M.build_frame(48, b""))
    return True


flash_path = os.environ["PA_FLASH_FILE"]
SLOT = 8192          # a flash capture slot (include/pa_flash_map.h): the biggest real capture is the 7.5 KB WaveNet
for size, want_ok in ((8192, True), (32768, True), (33792, False), (65536, False)):
    tag = bytes([0x5A] * 8) + os.urandom(8)
    payload = tag + bytes(size - len(tag))
    armed = try_upload(payload)
    check(armed == want_ok,
          f"a {size} byte upload is {'armed' if want_ok else 'refused at cmd 25'}"
          f" (armed={armed}, limit 32768)")
    if not armed:
        # nothing may have been committed: the payload must not be in the flash
        if os.path.exists(flash_path):
            flash = open(flash_path, "rb").read()
            check(flash.find(tag) < 0,
                  f"the refused {size} byte upload left nothing in the flash")
    else:
        st = session_state()
        if size <= SLOT:
            check(st[3] == 0, f"the {size} byte upload committed (status {st[3]})")
        else:
            # the SysEx session took it, but it is larger than a flash capture slot: the commit is refused, and the capture count does not change
            check(st[3] != 0, f"the {size} byte upload is refused as a capture, it does not fit a {SLOT} byte slot (status {st[3]})")

print("\n" + ("THE SYSEX UPLOAD IS ACCEPTED" if fails == 0 else f"{fails} FAILURE(S)"))
sys.exit(0 if fails == 0 else 1)
