#!/usr/bin/env python3
"""
custom-fw/tools/jl_app_area.py - Pocket Amp application area: JieLi file table, CRCs, SFC encoding

The app area is flash 0x4000..0xB4FFF (the flash header's `app_dir_head` points at 0x4000).
It is encoded using JieLi SFC block encoding:

    for each 32-byte block at flash address A (A - 0x4000 a multiple of 32):
        key = CHIP_KEY ^ (((A - 0x4000) >> 2) & 0xFFFF)          CHIP_KEY = 0x980F
        byte i of the block ^= low byte of key, then key = key<<1 ^ (0x1021 if bit 15)

Only the bytes the area actually uses are scrambled; the rest up to 0xB5000 is plain 0xFF.

Unpacked layout (all little-endian, 32-byte entries, CRC = CRC16-CCITT/XMODEM, init 0):

    +0x000  entry  app_area_head   size = end of the last file, data_crc = CRC(+0x20 .. end)
    +0x020  entry  app.bin         offset 0x120 (relative to the area), size, data_crc
    +0x040  entry  cfg_tool.bin    offset = right after app.bin, size, data_crc
    +0x060  entries VM, PRCT, BTIF (absolute flash addresses; position-independent)
    +0x0C0  0xFF up to +0x120
    +0x120  app.bin, then cfg_tool.bin, then the 0x80-byte EXT_RESERVED table
            (EXT_RESERVED, USER, USR, SKPST; its CRCs cover only its own bytes)

    entry:  u16 head_crc (CRC of entry bytes 2..31), u16 data_crc, u32 offset, u32 size,
            u8 attr, u8 res, u16 index, char name[16]

Every head and data CRC matches, and selftest verifies partition table consistency.

    python custom-fw/tools/jl_app_area.py inspect <dump.bin | .fwsc>
    python custom-fw/tools/jl_app_area.py selftest <stock .fwsc>
"""
import argparse
import struct
import sys

CHIP_KEY = 0x980F
AREA_START = 0x4000          # flash address of the app area (app_dir_head)
AREA_END = 0xB5000           # VM starts here
TABLE_SIZE = 0x120           # file table; app.bin starts right after it
ENTRY = 32
BLOCK = 32


# ---- CRC16-CCITT (XMODEM), init 0 -----------------------------------------------------
_CRC_TABLE = []
for _i in range(256):
    _c = _i << 8
    for _ in range(8):
        _c = ((_c << 1) ^ 0x1021) if _c & 0x8000 else (_c << 1)
    _CRC_TABLE.append(_c & 0xFFFF)


def crc16(data, crc=0):
    for b in data:
        crc = ((crc << 8) & 0xFFFF) ^ _CRC_TABLE[((crc >> 8) ^ b) & 0xFF]
    return crc


# ---- JieLi SFC block encoding ---------------------------------------------------------
_streams = {}


def _stream(key):
    s = _streams.get(key)
    if s is None:
        out = bytearray(BLOCK)
        k = key
        for i in range(BLOCK):
            out[i] = k & 0xFF
            k = ((k << 1) ^ (0x1021 if k & 0x8000 else 0)) & 0xFFFF
        s = _streams[key] = bytes(out)
    return s


def scramble(data, addr=AREA_START):
    """ENC-scramble (or unscramble: it is its own inverse) bytes that sit at flash `addr`."""
    if (addr - AREA_START) % BLOCK:
        raise ValueError("scramble must start on a 32-byte block of the app area")
    out = bytearray(data)
    for o in range(0, len(out), BLOCK):
        key = (CHIP_KEY ^ (((addr + o - AREA_START) >> 2) & 0xFFFF)) & 0xFFFF
        s = _stream(key)
        n = min(BLOCK, len(out) - o)
        for i in range(n):
            out[o + i] ^= s[i]
    return bytes(out)


# ---- file table ------------------------------------------------------------------------
def parse_entry(e):
    hcrc, dcrc, off, size, attr, res, idx = struct.unpack_from("<HHIIBBH", e, 0)
    return {"head_crc": hcrc, "data_crc": dcrc, "offset": off, "size": size, "attr": attr,
            "res": res, "index": idx, "name": e[16:32].split(b"\0")[0].decode("ascii", "replace"),
            "raw": bytes(e)}


def seal_entry(e):
    """Recompute an entry's head CRC (over its bytes 2..31)."""
    e = bytearray(e)
    struct.pack_into("<H", e, 0, crc16(e[2:ENTRY]))
    return bytes(e)


def entries(table):
    out = []
    for i in range(0, TABLE_SIZE, ENTRY):
        e = table[i:i + ENTRY]
        if e[16:20] == b"\xff\xff\xff\xff" or e == b"\xff" * ENTRY:
            continue
        out.append(parse_entry(e))
    return out


def decode_area(flash):
    """Unpack and parse the app area of a flash image (>= 0xB5000 bytes from flash 0x0)."""
    used_guess = flash[AREA_START:AREA_END]
    plain_head = scramble(used_guess[:TABLE_SIZE])
    ents = entries(plain_head)
    head = next((e for e in ents if e["name"] == "app_area_head"), None)
    if head is None:
        raise ValueError("no app_area_head at 0x4000 - not a JieLi app area (or not this chip key)")
    files_end = head["size"]                       # end of the last file, area-relative
    ext_hdr = scramble(used_guess[:files_end + ENTRY])[files_end:files_end + ENTRY]
    ext = parse_entry(ext_hdr)
    ext_size = ext["size"] if ext["name"] == "EXT_RESERVED" else 0
    used = files_end + ext_size
    plain = scramble(used_guess[:used])
    return {"plain": plain, "entries": entries(plain[:TABLE_SIZE]), "files_end": files_end,
            "ext": plain[files_end:used] if ext_size else b"", "used": used}


def check_area(area):
    """List of problems with a decoded area (empty = every CRC and size is consistent)."""
    p = area["plain"]
    bad = []
    for e in area["entries"]:
        if crc16(e["raw"][2:ENTRY]) != e["head_crc"]:
            bad.append(f"{e['name']}: head CRC")
        if e["name"] == "app_area_head":
            if crc16(p[ENTRY:area["files_end"]]) != e["data_crc"]:
                bad.append("app_area_head: data CRC")
        elif e["name"] in ("app.bin", "cfg_tool.bin"):
            data = p[e["offset"]:e["offset"] + e["size"]]
            if len(data) != e["size"] or crc16(data) != e["data_crc"]:
                bad.append(f"{e['name']}: data CRC")
    ext = area["ext"]
    for i in range(0, len(ext), ENTRY):
        if crc16(ext[i + 2:i + ENTRY]) != struct.unpack_from("<H", ext, i)[0]:
            bad.append(f"EXT_RESERVED entry {i // ENTRY}: head CRC")
    if ext and crc16(ext[ENTRY:]) != struct.unpack_from("<H", ext, 2)[0]:
        bad.append("EXT_RESERVED: data CRC")
    return bad


def file_bytes(area, name):
    e = next(e for e in area["entries"] if e["name"] == name)
    return area["plain"][e["offset"]:e["offset"] + e["size"]]


def tail_bytes(flash, area):
    """The template's bytes after its used area, with anything non-0xFF kept (V1.05 stores a
    6-byte trailer, plain, at 0xB4FF8: `5e 60 2e 96 5e c7`; V1.03 has none). Its meaning is
    unknown - it matched no CRC16 variant over the obvious ranges - so it is carried over
    unchanged and the app may not grow into it."""
    region = bytearray(flash[AREA_START:AREA_END])
    region[:area["used"]] = b"\xff" * area["used"]
    nonff = [i for i in range(area["used"], len(region)) if region[i] != 0xFF]
    return bytes(region), (nonff[0] if nonff else len(region))


def max_app_size(area, flash=None):
    """Largest app.bin that fits before VM (or the trailer), keeping cfg_tool.bin and the EXT table."""
    room = AREA_END - AREA_START
    if flash is not None:
        room = tail_bytes(flash, area)[1]
    cfg = len(file_bytes(area, "cfg_tool.bin"))
    return room - TABLE_SIZE - cfg - len(area["ext"])


def build_area(template_flash, app_bytes):
    """New scrambled app area (bytes for flash 0x4000..0xB4FFF) with `app_bytes` as app.bin.

    Everything else - the other table entries, cfg_tool.bin, the EXT_RESERVED table - is
    taken from the template, re-positioned, and every size and CRC is recomputed.
    """
    area = decode_area(template_flash)
    bad = check_area(area)
    if bad:
        raise ValueError("template app area is inconsistent: " + ", ".join(bad))
    limit = max_app_size(area, template_flash)
    if len(app_bytes) > limit:
        raise ValueError(f"app.bin is {len(app_bytes)} B; the app area holds at most {limit} B")
    cfg = file_bytes(area, "cfg_tool.bin")

    app_off = TABLE_SIZE
    cfg_off = app_off + len(app_bytes)
    files_end = cfg_off + len(cfg)
    table = bytearray(area["plain"][:TABLE_SIZE])
    for i in range(0, TABLE_SIZE, ENTRY):
        e = bytearray(table[i:i + ENTRY])
        name = e[16:32].split(b"\0")[0]
        if name == b"app.bin":
            struct.pack_into("<HII", e, 2, crc16(app_bytes), app_off, len(app_bytes))
        elif name == b"cfg_tool.bin":
            struct.pack_into("<HII", e, 2, crc16(cfg), cfg_off, len(cfg))
        else:
            continue
        table[i:i + ENTRY] = seal_entry(e)
    body = bytes(table[ENTRY:]) + app_bytes + cfg
    head = bytearray(table[:ENTRY])
    if head[16:29] != b"app_area_head":
        raise ValueError("first table entry is not app_area_head")
    struct.pack_into("<H", head, 2, crc16(body))
    struct.pack_into("<I", head, 8, files_end)
    plain = seal_entry(head) + body + area["ext"]
    tail, _ = tail_bytes(template_flash, area)
    return scramble(plain) + tail[len(plain):]


def flash_from_any(blob):
    """Flash bytes from 0x0, from a raw dump / flash.bin, a .ufw or a .fwsc."""
    if blob[16:28] == b"AC791N_STORY":           # the flash header is plain at 0x10
        return blob
    import pack_ufw as P
    for ufw in (P.unwrap_fwsc(blob)[0], blob):   # a .fwsc unwrapped, or a .ufw as is
        if P.xor_header(ufw[:32])[16:22] == b"AC791N":
            try:
                off, size = P.flash_entry(ufw)
            except ValueError:
                continue
            return ufw[off:off + size]
    raise ValueError("not a Pocket Amp dump, flash.bin, .ufw or .fwsc")


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    sub = ap.add_subparsers(dest="cmd", required=True)
    i = sub.add_parser("inspect", help="unpack and check the app area of a dump, flash.bin, .ufw or .fwsc")
    i.add_argument("file")
    i.add_argument("--extract", metavar="APP_BIN", help="write the unpacked app.bin here")
    s = sub.add_parser("selftest", help="verify app area partition table and CRCs")
    s.add_argument("file")
    a = ap.parse_args()
    flash = flash_from_any(open(a.file, "rb").read())
    area = decode_area(flash)
    if a.cmd == "inspect":
        for e in area["entries"]:
            print(f"  {e['name']:14s} offset {e['offset']:#09x} size {e['size']:#08x} attr {e['attr']:#04x}")
        app = file_bytes(area, "app.bin")
        print(f"  app.bin {len(app)} B at flash {AREA_START + TABLE_SIZE:#x}; area uses {area['used']:#x} B; "
              f"room for an app of {max_app_size(area, flash)} B")
        trailer = tail_bytes(flash, area)
        if trailer[1] < AREA_END - AREA_START:
            t0 = AREA_START + trailer[1]
            tbytes = flash[t0:AREA_END].rstrip(b"\xff").hex(" ")
            print(f"  trailer after the area at {t0:#x}: {tbytes} (kept unchanged by the packer)")
        bad = check_area(area)
        print("  CRCs: all consistent" if not bad else "  CRC PROBLEMS: " + ", ".join(bad))
        if a.extract:
            open(a.extract, "wb").write(app)
            print(f"  wrote {a.extract}")
        sys.exit(1 if bad else 0)
    rebuilt = build_area(flash, file_bytes(area, "app.bin"))
    same = rebuilt == flash[AREA_START:AREA_END]
    print(f"selftest: stock app area rebuilt from its own app.bin is "
          f"{'BYTE-IDENTICAL' if same else 'DIFFERENT'} ({len(rebuilt):#x} bytes)")
    sys.exit(0 if same else 1)


if __name__ == "__main__":
    main()

# SPDX-License-Identifier: GPL-3.0-only
# Copyright (C) 2026 The Onyx Authors (@28spawn)
