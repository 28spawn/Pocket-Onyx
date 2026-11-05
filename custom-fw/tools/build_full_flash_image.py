#!/usr/bin/env python3
"""
Generates a complete 1 MB SPI NOR Flash image (1,048,576 bytes, the size of the real chip, PA_FLASH_SIZE in include/pa_flash_map.h)
from the packaged flash.bin: the MBR, uboot and app.bin at their offsets, everything else erased (0xFF). The emulator's simulated flash is this image.
"""

from pathlib import Path
import sys

tools_dir = Path(__file__).resolve().parent
sys.path.insert(0, str(tools_dir))
from pack_ufw import unwrap_fwsc, xor_header

def build_full_flash():
    custom_fw_dir = tools_dir.parent
    ufw_path = custom_fw_dir / "pocket_amp_app.ufw"
    out_img = custom_fw_dir / "pocket_amp_full_flash.bin"

    # Extract flash.bin from ufw
    ufw_data = ufw_path.read_bytes()
    head = xor_header(ufw_data[:64])
    count = ufw_data[64] # or unpack
    import struct
    count = struct.unpack_from("<H", head, 8)[0]
    table = ufw_data[64 : 64 + count * 80]
    e0 = xor_header(table[:80])
    offset = struct.unpack_from("<I", e0, 8)[0]
    size = struct.unpack_from("<I", e0, 12)[0]
    flash_bin = ufw_data[offset : offset + size]

    # Create a 1 MB image initialized to 0xFF (erased NOR flash state)
    TOTAL_SIZE = 1024 * 1024
    if len(flash_bin) > TOTAL_SIZE:
        raise SystemExit(f"flash.bin is {len(flash_bin):,} bytes, larger than the 1 MB chip")
    image = bytearray(b"\xFF" * TOTAL_SIZE)

    # Place flash.bin at offset 0
    image[0 : len(flash_bin)] = flash_bin

    # Also sync to emu/pocket_amp_flash.bin
    out_img.write_bytes(image)
    (custom_fw_dir / "emu" / "pocket_amp_flash.bin").write_bytes(image)
    print(f"[+] Successfully generated 1 MB SPI Flash image: {out_img} ({len(image):,} bytes)")

if __name__ == "__main__":
    build_full_flash()
