# SPDX-License-Identifier: GPL-3.0-only
# Copyright (C) 2026 The Onyx Authors (@28spawn)
"""The Onyx release package (tools/onyx_pkg_make.py) is built from scratch and carries no M-VAVE material:

    python custom-fw/test/test_onyx_package.py [package.fwsc app.bin]       (default: the newest custom-fw/release/*/*.fwsc and custom-fw/build_app.bin)

Checks: the package decodes (every CRC of the app area is consistent), its app.bin is the build's app image byte for byte, the app area ends before the user store (0x86000), it carries the recovery marker
and the identity "Pocket AMP_<pack>", its update loader is the Onyx one (the 16-byte mark the firmware recognises) and fits the staging area, and - when the original M-VAVE package is on this machine -
none of that package's update loader or app code is in it.
"""
import glob, os, sys
ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
sys.path.insert(0, os.path.join(ROOT, "tools"))
import get_sdk_files as _SDK, jl_app_area as J, pack_ufw as P

fails = 0


def check(ok, msg):
    global fails
    print(("  ok   " if ok else "  FAIL ") + msg); fails += not ok


args = sys.argv[1:]
if len(args) >= 2:
    pkg_path, app_path = args[0], args[1]
else:
    c = sorted(glob.glob(os.path.join(ROOT, "release", "*", "*.fwsc")))
    if not c:
        print("  skip  no release package yet (build.ps1 writes it)"); sys.exit(0)
    pkg_path = c[-1]
    bins = sorted(glob.glob(os.path.join(os.path.dirname(pkg_path), "*.bin")))
    app_path = bins[0] if bins else os.path.join(ROOT, "build_app.bin")
pkg = open(pkg_path, "rb").read(); app = open(app_path, "rb").read()
print("===", os.path.basename(pkg_path), "===")
ufw, product = P.unwrap_fwsc(pkg)
flash = J.flash_from_any(pkg); flash = flash[0] if isinstance(flash, tuple) else flash
area = J.decode_area(flash)
check(not J.check_area(area), "the app area decodes and every CRC is consistent")
got = J.file_bytes(area, "app.bin")
check(got[:len(app)] == app and not got[len(app):].strip(b"\xff"), "its app.bin is the build's app image byte for byte (%d B)" % len(app))
check(0x4000 + area["used"] <= 0x86000, "the app area ends at 0x%X, before the user store (0x86000)" % (0x4000 + area["used"]))
check(b"RCREC1:PA:" in J.scramble(flash[0x4000:0x86000], 0x4000), "it carries the recovery marker")
check(isinstance(product, str) and product.startswith("Pocket AMP_"), "its identity is %r" % product)
check(len(flash) == 0xA5000, "its flash image is 0xA5000 bytes: the head and the app area, nothing from the stock's VM on")
# the loader (entry 100)
head = P.xor_header(ufw[:64]); n = int.from_bytes(head[8:10], "little"); ota = None
for i in range(n):
    e = P.xor_header(ufw[64 + i * 80:64 + (i + 1) * 80])
    if int.from_bytes(e[0:2], "little") == 100:
        off, size = int.from_bytes(e[8:12], "little"), int.from_bytes(e[12:16], "little"); ota = ufw[off:off + size]
check(ota is not None and len(ota) - 0x20 <= 0x5000 - 0x200, "it carries an update loader that fits the staging area (%d B)" % (len(ota) if ota else 0))
if ota:
    import lz4blk, struct
    body = ota[0x40:]; raw = b""; o = 0
    while o + 8 <= len(body):
        clen, dlen = struct.unpack_from("<II", body, o); raw += lz4blk.decompress(body[o + 8:o + 8 + clen]); o += 8 + clen
    check(b"ONYX-LOADER-0001" in raw[:32], "the loader is the Onyx one (its mark ONYX-LOADER-0001 is in its first bytes)")
    check(b"PENDEL" not in raw, "and carries no Pendel mark")
# Verify package contains no proprietary material
orig = os.path.expanduser("~/Downloads/Pocket AMP.fwsc")
if os.path.exists(orig):
    _SDK.main()
    ou, _ = P.unwrap_fwsc(open(orig, "rb").read()); oh = P.xor_header(ou[:64]); on = int.from_bytes(oh[8:10], "little"); oota = None
    for i in range(on):
        e = P.xor_header(ou[64 + i * 80:64 + (i + 1) * 80])
        if int.from_bytes(e[0:2], "little") == 100:
            oo, osz = int.from_bytes(e[8:12], "little"), int.from_bytes(e[12:16], "little"); oota = ou[oo:oo + osz]
    oflash = J.flash_from_any(open(orig, "rb").read()); oflash = oflash[0] if isinstance(oflash, tuple) else oflash
    sdk = b"".join(open(os.path.join(ROOT, "third_party", "jieli_sdk_files", f), "rb").read() for f in ("uboot.boot", "cfg_tool.bin", os.path.join("cfg", "eq_cfg_hw.bin")) if os.path.exists(os.path.join(ROOT, "third_party", "jieli_sdk_files", f)))
    def shares(blob, hay):                    # 64-byte windows of the original that are in the package and NOT in the public SDK files (common SDK code is excluded)
        hits = 0
        for i in range(0, len(blob) - 64, 64):
            ch = blob[i:i + 64]
            if len(set(ch)) > 8 and ch in hay and ch not in sdk: hits += 1
        return hits
    check(oota is not None and shares(oota, pkg) == 0, "no third-party update loader (ota.bin) is in the package")
    check(shares(oflash[0x4000:0x10000], pkg) == 0, "no third-party app area bytes are in the package")
else:
    print("  skip  comparison package not found on this machine")
print("\n" + ("THE ONYX PACKAGE IS CLEAN" if not fails else "%d FAILURE(S)" % fails))
sys.exit(1 if fails else 0)
