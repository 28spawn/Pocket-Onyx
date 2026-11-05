#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-only
# Copyright (C) 2026 Leo Kuroshita (@kurogedelic), Hugelton Instruments (pa_pkg_make.py of the Pendel project, from which the package layout and these helpers are taken)
# Changes for Onyx: the app is placed at its own length (the area ends before the user store, as the stock layout does), the loader is Onyx's, the package is named by the release.
"""Build the Onyx update package (.fwsc) for the Pocket AMP with NO vendor (M-VAVE) material in it - the way the Pendel project builds its own:

  flash.bin  head [0, 0x4000): flash header (JieLi SDK default values), JLFS entries, the SDK SPL (uboot.boot, Apache-2.0), an isd_config blob of our own that decodes to the chip key. The head is never
             written by the Onyx loader: it only has to look like a package head.
             app area [0x4000, ...): app_area_head, the Onyx app.bin, the SDK cfg_tool.bin and eq_cfg_hw.bin, the region descriptors, SFC block-encoded with the chip key. It ends before 0x86000
             (the user store), so the area's CRC never covers stored captures or cabinets.
  ota.bin    the Onyx update loader (loader/onyx_loader.c), LZ4-wrapped as the SPL loads it

The layout values (offsets, sizes, flags, descriptor fields) are facts of the Pocket AMP's flash layout and the SDK file formats: VM [0xA5000, 0xBD000), BTIF 0xBD000, OTP 0xBE000, USR [0xBF000, 0xFF000),
key_mac 0xFF000. The three SDK files (Apache-2.0, from the JieLi AC79 SDK: cpu/wl82/tools/uboot.boot, cfg_tool.bin, cfg/eq_cfg_hw.bin) are not in the repository: tools/get_sdk_files.py fetches them; ship
LICENSES/Apache-2.0.txt with every package.

  onyx_pkg_make.py APP.bin LOADER.bin OUT.fwsc --sdk DIR [--product "Pocket AMP_193"]
Standard library only.
"""
import argparse
import os
import struct
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
import lz4blk  # noqa: E402

FLASH_SIZE = 0xA5000                # flash.bin: the head and the app area, up to VM
AREA_END_MAX = 0x86000              # the app area must end before the user store
KEY = 0x980F
LOADER_LOAD = 0x01C0A800            # where the SPL puts the loader
LOADER_NAME = b"usb_hid_ota.bin"    # the file name the SPL looks for
LOADER_MAX = 0x5000 - 0x200         # the staging area (5 x 4 KiB) less its heads
FLASH_HDR = bytes.fromhex("20022f104cc900f00f001010a4ff4e5d0b41e0cd781dc856417fb367cf9f")
UFW_CHIP = b"AC791N"
SDK = None


def crc16(data, crc=0):
    for b in data:
        crc ^= b << 8
        for _ in range(8):
            crc = ((crc << 1) ^ 0x1021) if crc & 0x8000 else crc << 1
        crc &= 0xFFFF
    return crc


def enc(buf, off, size, key=0xFFFF):
    """JieLi SFC stream encoding (in place)"""
    for i in range(size):
        buf[off + i] ^= key & 0xFF
        key = ((key << 1) ^ (0x1021 if key & 0x8000 else 0)) & 0xFFFF


def sfc(buf, off, size, base, key):
    """SFC block encoding: per 32-byte block, key mixed with the block address"""
    for i in range(0, size, 32):
        enc(buf, off + i, min(size - i, 32), key ^ ((off + i - base) >> 2))


def chipkey_decode(d):
    """chip key from the 32-byte isd_config blob"""
    thesum = sum(d[:16]) & 0xFF
    thesum = 0xAA if thesum >= 0xE0 else 0x55 if thesum <= 0x10 else thesum
    return sum(1 << i for i in range(16) if (d[16 + i] ^ d[15 - i]) < thesum)


def entry(dcrc, offset, size, flags, resvd, index, name, fill=0xFF):
    if isinstance(name, bytes):                     # 16 raw bytes: name + the packer's trailing fields
        nm = name
    else:
        nm = name.encode() + b"\0"
        nm = nm + bytes([fill]) * (16 - len(nm))
    body = struct.pack("<HIIBBH16s", dcrc, offset & 0xFFFFFFFF, size & 0xFFFFFFFF, flags, resvd, index, nm)
    return struct.pack("<H", crc16(body)) + body


def key_blob(key):
    """32 bytes that chipkey_decode() turns into `key` (+ CRC16)"""
    d = bytearray([0x10] * 16) + bytearray(16)        # sum 0 -> threshold 0x55
    for i in range(16):
        d[16 + i] = d[15 - i] ^ (0x00 if key >> i & 1 else 0xFF)
    assert chipkey_decode(bytes(d)) == key
    return bytes(d) + struct.pack("<H", crc16(d))


def sdk_file(rel):
    path = Path(SDK or os.environ.get("ONYX_SDK_FILES", "")) / rel
    try:
        return path.read_bytes()
    except OSError as e:
        raise SystemExit(f"onyx_pkg_make: {path}: {e.strerror}. Run tools/get_sdk_files.py (it fetches the three Apache-2.0 SDK files) and pass --sdk <that folder>.")


def flash_image(app, key):
    spl = sdk_file("uboot.boot")                         # SDK SPL, BANKCB-encoded as in the SDK
    cfg_tool = sdk_file("cfg_tool.bin")
    eq = sdk_file("cfg/eq_cfg_hw.bin")
    isd = key_blob(key) + b"[ONYX]\r\n"
    f = bytearray(b"\xFF" * FLASH_SIZE)
    # ---- head
    spl_off = 0xA0
    isd_off = spl_off + len(spl)
    assert isd_off + len(isd) <= 0x4000
    f[spl_off:spl_off + len(spl)] = spl
    f[isd_off:isd_off + len(isd)] = isd
    top = (entry(crc16(spl), spl_off, len(spl), 0x00, 0x00, 0, "uboot.boot") +
           entry(crc16(isd), isd_off, len(isd), 0x02, 0x80, 0, "isd_config.ini") +
           entry(0xFFFF, 0x4000, 0xFFFFFFFF, 0x81, 0xFF, 0, "app_dir_head") +
           entry(0xFFFF, 0xFF000, 0x1000, 0x12, 0x01, 0xFFFF, "key_mac"))
    hdr = struct.pack("<H", crc16(FLASH_HDR)) + FLASH_HDR
    head = bytearray(hdr + top)
    for o in range(0, len(head), 32):                   # header and each JLFS entry: ENC 0xFFFF
        enc(head, o, 32)
    f[0:len(head)] = head
    # ---- app area (plain first, then SFC): the app at its own length, padded to a 32-byte block
    app = app + b"\xFF" * (-len(app) % 32)
    blk = 0x120 + len(app) + len(cfg_tool)              # app_area_head block size
    area = bytearray(b"\xFF" * blk)
    area[0x120:0x120 + len(app)] = app
    area[0x120 + len(app):blk] = cfg_tool
    ents = [entry(crc16(app), 0x120, len(app), 0x82, 0xFF, 0, "app.bin"),
            entry(crc16(cfg_tool), 0x120 + len(app), len(cfg_tool), 0x82, 0xFF, 0, "cfg_tool.bin"),
            # region descriptors; the bytes after the names are SDK packer fields (VM size, "CODE", "AUTO")
            entry(0xFFFF, 0xA5000, 0x18000, 0x12, 0x81, 0, bytes.fromhex("564d00ffffffffffffffffff00800000")),
            entry(0xFFFF, 0, FLASH_SIZE, 0x92, 0x82, 0, bytes.fromhex("5052435400ffffffffffffff434f4445")),
            entry(0xFFFF, 0xBD000, 0x1000, 0x92, 0x81, 0, bytes.fromhex("4254494600ffffffffffffff4155544f")),
            entry(0xFFFF, 0xBE000, 0x1000, 0x92, 0x81, 0, bytes.fromhex("4f545000ffffffffffffffff4155544f")),
            entry(0xFFFF, 0xBF000, 0x40000, 0x92, 0x81, 1, "USR")]
    area[32:32 + len(b"".join(ents))] = b"".join(ents)
    area[0:32] = entry(crc16(area[32:blk]), 0x02000120, blk, 0x83, 0xFF, 0, "app_area_head")
    # nested cfg directory right after the block: SDK default EQ table
    eq_e = entry(crc16(eq), 0x40, len(eq), 0x82, 0xFF, 1, "eq_cfg_hw.bin")
    cfg_body = eq_e + eq
    cfg = entry(crc16(cfg_body), 0x20, 0x20 + len(cfg_body), 0x83, 0xFF, 1, "cfg") + cfg_body
    region = bytearray(area + cfg)
    if 0x4000 + len(region) > AREA_END_MAX:
        raise SystemExit(f"the app area would end at 0x{0x4000 + len(region):X}: it must end before 0x{AREA_END_MAX:X} (the user store); the app is {len(app)} B, at most {AREA_END_MAX - 0x4000 - 0x120 - len(cfg_tool) - len(cfg)} B fit")
    sfc(region, 0, len(region), 0, key)
    f[0x4000:0x4000 + len(region)] = region
    return bytes(f)


def ldr_head(dcrc, a, b, attr, name):
    body = struct.pack("<HIIBBH16s", dcrc, a, b, attr, 0, 0, name)
    return struct.pack("<H", crc16(body)) + body


def wrap_loader(image):
    """ota.bin = outer JLFS head (32 B) + inner head (32 B, load address) +
    blocks [clen u32][dlen u32][LZ4 block], dlen 4096 except the last"""
    if not image or len(image) > 0x14000:
        raise SystemExit("loader: image empty or too big")
    blocks = bytearray()
    for i in range(0, len(image), 4096):
        raw = image[i:i + 4096]
        c = lz4blk.compress(raw)
        if len(c) > 4096 or lz4blk.decompress(c) != raw:
            raise SystemExit(f"loader: block {i // 4096} does not compress below 4096 B")
        blocks += struct.pack("<II", len(c), len(raw)) + c
    inner = ldr_head(crc16(image), len(image), LOADER_LOAD, 0, LOADER_NAME)
    body = inner + bytes(blocks)
    ota = ldr_head(crc16(body), 0x20, len(body), 0x41, LOADER_NAME) + body
    if len(ota) - 0x20 > LOADER_MAX:
        raise SystemExit(f"loader: ota.bin {len(ota)} B does not fit the staging area")
    return ota


def ufw(flash, ota, product):
    files = [(0, "flash.bin", flash), (100, "ota.bin", ota)]
    off, ents = 0x400, []
    for i, (typ, name, d) in enumerate(files):
        off = (off + 0xFF) & ~0xFF
        e = bytearray(0x50)
        struct.pack_into("<HHHHIII", e, 0, typ, i, crc16(d), 0, off, len(d), (len(d) + 31) & ~31)
        e[0x40:0x40 + len(name)] = name.encode()
        ents.append((e, off, d))
        off += len(d)
    total = off
    hdr = bytearray(0x40)
    struct.pack_into("<IH", hdr, 4, total, len(files))
    struct.pack_into("<HHH", hdr, 10, 0x0004, 0x0200, 0)     # header fields as written by the SDK packer
    hdr[16:16 + len(UFW_CHIP)] = UFW_CHIP
    lst = bytearray()
    for e, _, _ in ents:
        x = bytearray(e)
        enc(x, 0, 0x50)
        lst += x
    struct.pack_into("<H", hdr, 2, crc16(lst))
    struct.pack_into("<H", hdr, 0, crc16(hdr[2:0x40]))
    enc(hdr, 0, 0x40)
    logical = bytearray(b"\xFF" * total)
    logical[0:0x40] = hdr
    logical[0x40:0x40 + len(lst)] = lst
    for e, o, d in ents:
        logical[o:o + len(d)] = d
    # .fwsc: one marker byte after each of the first 20 0x2F-byte blocks (the identity)
    if len(product) > 20 or any((ord(c) + i + 1) & 0xFF == 0x7D for i, c in enumerate(product)):
        raise SystemExit(f"product {product!r}: at most 20 characters, none encoding to the unused marker")
    raw = bytearray()
    for i in range(20):
        ch = product[i] if i < len(product) else None
        m = (ord(ch) + i + 1) & 0xFF if ch else 0x7D
        raw += logical[i * 0x2F:(i + 1) * 0x2F] + bytes([m])
    raw += logical[20 * 0x2F:]
    return bytes(raw)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("app", type=Path)
    ap.add_argument("loader", type=Path)
    ap.add_argument("out", type=Path)
    ap.add_argument("--product", default="Pocket AMP_193", help="the package identity: model + _ + number; hosts match the model \"Pocket AMP\"")
    ap.add_argument("--key", type=lambda s: int(s, 0), default=KEY)
    ap.add_argument("--sdk", type=Path, required=True, help="folder with uboot.boot, cfg_tool.bin and cfg/eq_cfg_hw.bin (tools/get_sdk_files.py)")
    a = ap.parse_args()
    global SDK
    SDK = a.sdk
    ota = wrap_loader(a.loader.read_bytes())
    pkg = ufw(flash_image(a.app.read_bytes(), a.key), ota, a.product)
    a.out.write_bytes(pkg)
    print(f"package {a.out}: {len(pkg)} B, identity {a.product}, loader {len(ota)} B, chip key {a.key:#06x}")


if __name__ == "__main__":
    main()
