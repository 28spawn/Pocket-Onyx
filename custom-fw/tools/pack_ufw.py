#!/usr/bin/env python3
"""
custom-fw/tools/pack_ufw.py — Standalone JieLi AC791N .ufw & M-VAVE .fwsc Packager (QW-5)

Packages a compiled `app.bin` into a flashable JieLi `.ufw` or M-VAVE `.fwsc` firmware update
container using the verified Pocket Amp container parameters
and container specifications:
  - Chip family: b"AC791N"
  - Chipkey:     0x980F (AC791N hardware key)
  - App area:    flash 0x4000..0xB4FFF: JieLi file table, app.bin at 0x4120, cfg_tool.bin and
                 the EXT_RESERVED table after it, all ENC-scrambled with the chip key
                 (jl_app_area.py). The app replaces app.bin; sizes and CRCs are recomputed.
  - .fwsc wrap:  20-byte product marker interleaved every 47 bytes across first 940 bytes
                 using container product encoding: c[i] = (target[i] + i + 1) & 0xFF
"""

import argparse
import math
import os
import sys
import struct

CHIP_KEY = 0x980F
# Largest app.bin that fits the app image (0x4000..0x85FFF: the user store with the owner's captures
# and cabinets starts at 0x86000, include/pa_flash_map.h) with the V1.05 template's file table (0x120),
# cfg_tool.bin (383 B), EXT_RESERVED table (0x80) and the 8 bytes from the trailer.
# jl_app_area.build_area() checks the exact limit for any template.
APP_IMAGE_SIZE = 0x86000 - 0x4000                    # 532,480 B: what a custom pack writes
APP_SLOT_MAX = APP_IMAGE_SIZE - 0x120 - 383 - 0x80 - 8     # 531,673 B
FWSC_TARGET = "Pocket AMP_105"

# Entropy above this means "scrambled" (a plain app measures ~6.7-7.1, the stored app 8.0).
ENTROPY_ENCRYPTED = 7.5


def entropy(data: bytes) -> float:
    """Shannon entropy in bits per byte."""
    if not data:
        return 0.0
    counts = [0] * 256
    for b in data:
        counts[b] += 1
    n = float(len(data))
    e = 0.0
    for c in counts:
        if c:
            p = c / n
            e -= p * math.log2(p)
    return e

CRC16_TABLE = [
    0, 0x1021, 0x2042, 0x3063, 0x4084, 0x50a5, 0x60c6, 0x70e7,
    0x8108, 0x9129, 0xa14a, 0xb16b, 0xc18c, 0xd1ad, 0xe1ce, 0xf1ef
]


def crc16_ccitt(data: bytes, init: int = 0x0000) -> int:
    crc = init
    for b in data:
        crc ^= (b << 8)
        crc = ((crc << 4) ^ CRC16_TABLE[(crc >> 12) & 0xF]) & 0xFFFF
        crc = ((crc << 4) ^ CRC16_TABLE[(crc >> 12) & 0xF]) & 0xFFFF
    return crc


def xor_header(data: bytes, key: int = 0xFFFF) -> bytearray:
    p = bytearray(data)
    k = key
    for i in range(len(p)):
        p[i] ^= (k & 0xFF)
        k = ((k << 1) ^ (0x1021 if (k & 0x8000) else 0)) & 0xFFFF
    return p


def encode_marker(target_name: str) -> bytes:
    """Encodes the 20-byte target product marker:
    c[i] = (target[i] + i + 1) & 0xFF, padded with '}' (ASCII 125).
    Compatible with updater protocol as 'Pocket AMP_105'.
    """
    bname = target_name.encode("ascii")
    out = bytearray(20)
    for i in range(20):
        if i < len(bname):
            out[i] = (bname[i] + i + 1) & 0xFF
        else:
            out[i] = 125  # '}'
    return bytes(out)


def unwrap_fwsc(fwsc_bytes: bytes) -> tuple[bytes, str]:
    image = bytearray(len(fwsc_bytes) - 20)
    product = ""
    ended = False
    for i in range(20):
        image[i * 47 : i * 47 + 47] = fwsc_bytes[i * 48 : i * 48 + 47]
        m = fwsc_bytes[i * 48 + 47]
        if m == 125:
            ended = True
        else:
            c = (m - i - 1) & 0xFF
            product += chr(c)
    image[940:] = fwsc_bytes[960:]
    return bytes(image), product


def wrap_fwsc(ufw_bytes: bytes, target_name: str = FWSC_TARGET) -> bytes:
    marker20 = encode_marker(target_name)
    out = bytearray()
    pos = 0
    for i in range(20):
        chunk = ufw_bytes[pos : pos + 47]
        out.extend(chunk)
        pos += len(chunk)
        out.append(marker20[i])
    out.extend(ufw_bytes[pos:])
    return bytes(out)


def verify_package(fwsc_bytes: bytes) -> bool:
    """Validates package structure and container checksums."""
    ufw_bytes, product = unwrap_fwsc(fwsc_bytes)
    if product != FWSC_TARGET:
        print(f"[-] Verification failed: product identity '{product}' != '{FWSC_TARGET}'")
        return False

    head = xor_header(ufw_bytes[:64])
    h_crc = struct.unpack_from("<H", head, 0)[0]
    t_crc = struct.unpack_from("<H", head, 2)[0]
    f_len = struct.unpack_from("<I", head, 4)[0]
    count = struct.unpack_from("<H", head, 8)[0]
    chip = head[16:32].split(b"\x00")[0].decode("ascii", errors="replace")

    if h_crc != crc16_ccitt(head[2:]):
        print(f"[-] Header CRC mismatch: {h_crc:#x} != {crc16_ccitt(head[2:]):#x}")
        return False
    if f_len != len(ufw_bytes):
        print(f"[-] File length mismatch: {f_len} != {len(ufw_bytes)}")
        return False
    if chip != "AC791N":
        print(f"[-] Chip mismatch: '{chip}' != 'AC791N'")
        return False

    table = ufw_bytes[64 : 64 + count * 80]
    if t_crc != crc16_ccitt(table):
        print(f"[-] Table CRC mismatch: {t_crc:#x} != {crc16_ccitt(table):#x}")
        return False

    for i in range(count):
        e = xor_header(table[i * 80 : (i + 1) * 80])
        entry_type = struct.unpack_from("<H", e, 0)[0]
        entry_crc = struct.unpack_from("<H", e, 4)[0]
        offset = struct.unpack_from("<I", e, 8)[0]
        size = struct.unpack_from("<I", e, 12)[0]
        encrypted = struct.unpack_from("<I", e, 24)[0]
        name = e[64:].split(b"\x00")[0].decode("ascii", errors="replace")
        chunk = ufw_bytes[offset : offset + size]
        if not encrypted and entry_crc != crc16_ccitt(chunk):
            print(f"[-] Entry {i} ({name}) CRC mismatch: {entry_crc:#x} != {crc16_ccitt(chunk):#x}")
            return False

    print(f"[+] Package verification PASSED: valid {chip} container ({count} entries, {len(fwsc_bytes)} B)")
    return True


def flash_entry(ufw_bytes: bytes) -> tuple[int, int]:
    """(offset, size) of the flash.bin entry (entry 0) inside a .ufw image."""
    e0 = xor_header(ufw_bytes[64:144])
    name = e0[64:].split(b"\x00")[0]
    if struct.unpack_from("<H", e0, 0)[0] != 0 or name != b"flash.bin":
        raise ValueError("entry 0 is not flash.bin")
    return struct.unpack_from("<II", e0, 8)


def build_from_template(template_fwsc_bytes: bytes, app_bytes: bytes, require_marker: bool = True) -> bytes:
    if len(app_bytes) > APP_SLOT_MAX:
        raise ValueError(f"app.bin size ({len(app_bytes)} B) exceeds Pocket Amp slot ({APP_SLOT_MAX} B)")

    # if recovery marker is missing, fail-closed gate refuses packaging
    if require_marker and b"RCREC1:PA:" not in app_bytes:
        raise ValueError("SECURITY GATE ABORT: app.bin is missing mandatory RCREC1:PA: recovery marker!")

    ufw_raw, _ = unwrap_fwsc(template_fwsc_bytes)
    image = bytearray(ufw_raw)

    head = xor_header(image[:64])
    count = struct.unpack_from("<H", head, 8)[0]
    table = bytearray(image[64 : 64 + count * 80])

    # Decode entry 0 (flash.bin)
    e0 = xor_header(table[:80])
    entry_type = struct.unpack_from("<H", e0, 0)[0]
    flash_offset = struct.unpack_from("<I", e0, 8)[0]
    flash_size = struct.unpack_from("<I", e0, 12)[0]
    name = e0[64:].split(b"\x00")[0].decode("ascii", errors="replace")
    assert entry_type == 0 and name == "flash.bin"

    # Rebuild the app area (flash 0x4000..0xB4FFF) the way the vendor tool lays it out:
    # file table, app.bin, cfg_tool.bin, EXT_RESERVED, new sizes and CRCs, ENC-scrambled.
    import jl_app_area as A
    if flash_size < A.AREA_END:
        raise ValueError("template flash.bin does not cover the app area")
    flash = bytes(image[flash_offset : flash_offset + flash_size])
    image[flash_offset + A.AREA_START : flash_offset + A.AREA_END] = A.build_area(flash, app_bytes)

    # Recompute CRC16 of flash.bin
    flash_chunk = image[flash_offset : flash_offset + flash_size]
    new_flash_crc = crc16_ccitt(flash_chunk)
    struct.pack_into("<H", e0, 4, new_flash_crc)

    # Re-encode entry 0 and update table
    table[:80] = xor_header(e0)
    image[64 : 64 + count * 80] = table

    # Recompute table CRC and update head
    new_table_crc = crc16_ccitt(table)
    struct.pack_into("<H", head, 2, new_table_crc)
    new_head_crc = crc16_ccitt(head[2:])
    struct.pack_into("<H", head, 0, new_head_crc)

    # Re-encode header
    image[:64] = xor_header(head)

    return bytes(image)


def main() -> None:
    ap = argparse.ArgumentParser(description="Package Pocket Amp app.bin into .ufw and .fwsc")
    ap.add_argument("--in-bin", help="Input raw app.bin binary")
    ap.add_argument("--out-ufw", help="Output .ufw file path")
    ap.add_argument("--out-fwsc", default=None, help="Optional output .fwsc file path")
    ap.add_argument("--out-area", default=None, help="Also write the app AREA image (flash 0x4000..0x85FFF, %d bytes, scrambled as it is in the flash): the file the web editor's Firmware tab and jl_safe_flash.py take" % APP_IMAGE_SIZE)
    ap.add_argument(
        "--template-fwsc",
        help="The stock .fwsc container to inject app.bin into (REQUIRED: without the "
             "vendor's container layout there is no image the device will accept, so "
             "there is no fallback)",
    )
    ap.add_argument(
        "--verify",
        metavar="FWSC",
        help="Verify an existing .fwsc container and exit (no packaging)",
    )
    args = ap.parse_args()

    if args.verify:
        with open(args.verify, "rb") as f:
            ok = verify_package(f.read())
        sys.exit(0 if ok else 1)

    for flag, value in (("--in-bin", args.in_bin), ("--out-ufw", args.out_ufw),
                        ("--template-fwsc", args.template_fwsc)):
        if not value:
            raise SystemExit("%s is required" % flag)

    with open(args.in_bin, "rb") as f:
        app_bytes = f.read()

    if len(app_bytes) > APP_SLOT_MAX:
        raise SystemExit(
            "app.bin is %d bytes, over the app partition's %d (0x%X) - it would run "
            "into the next partition" % (len(app_bytes), APP_SLOT_MAX, APP_SLOT_MAX))

    if not os.path.exists(args.template_fwsc):
        raise SystemExit(
            "the stock template %s does not exist.\n"
            "    It is REQUIRED: the .ufw/.fwsc layout is the vendor's (the JieLi UFW "
            "container with the chip key and the\n"
            "    20-byte product marker), and there is no fallback - a made-up container "
            "cannot be flashed." % args.template_fwsc)

    print("[*] Injecting app.bin into stock template: %s" % args.template_fwsc)
    with open(args.template_fwsc, "rb") as f:
        template_bytes = f.read()
    ufw = build_from_template(template_bytes, app_bytes)

    os.makedirs(os.path.dirname(os.path.abspath(args.out_ufw)), exist_ok=True)
    with open(args.out_ufw, "wb") as f:
        f.write(ufw)
    print("[+] Wrote .ufw container: %s (%d bytes)" % (args.out_ufw, len(ufw)))

    if args.out_fwsc:
        fwsc = wrap_fwsc(ufw)
        os.makedirs(os.path.dirname(os.path.abspath(args.out_fwsc)), exist_ok=True)
        with open(args.out_fwsc, "wb") as f:
            f.write(fwsc)
        print(f"[+] Wrote .fwsc container: {args.out_fwsc} ({len(fwsc)} bytes)")
        verify_package(fwsc)
        if args.out_area:
            import jl_app_area as J
            flash = J.flash_from_any(fwsc)
            flash = flash[0] if isinstance(flash, tuple) else flash
            area = bytes(flash[0x4000:0x4000 + APP_IMAGE_SIZE])
            if len(area) != APP_IMAGE_SIZE or area == bytes(APP_IMAGE_SIZE) or area[:64] == bytes([255]) * 64:
                raise SystemExit("the app area image came out wrong (%d bytes)" % len(area))
            os.makedirs(os.path.dirname(os.path.abspath(args.out_area)), exist_ok=True)
            with open(args.out_area, "wb") as f:
                f.write(area)
            print(f"[+] Wrote app area image: {args.out_area} ({len(area)} bytes, flash 0x4000..0x{0x4000 + APP_IMAGE_SIZE - 1:X}): this is the file for the web editor and jl_safe_flash.py")


if __name__ == "__main__":
    main()

# SPDX-License-Identifier: GPL-3.0-only
# Copyright (C) 2026 The Onyx Authors (@28spawn)
