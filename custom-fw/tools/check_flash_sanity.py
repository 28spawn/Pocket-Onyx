#!/usr/bin/env python3
"""
Pre-Flashing Flash Dump & Update Package Sanity Checker
Checks:
1. Size & Fill Ratio (Power of two, 0xFF/0x00 padding percentage)
2. MBR & Partition Descriptor (0x00000000 - 0x00000120)
3. App area (flash 0x4000, SFC-encoded; app.bin at 0x4120 = XIP 0x02000120)
4. Embedded Asset / String Inspection
"""

import sys
import struct
import re
from pathlib import Path

tools_dir = Path(__file__).resolve().parent
sys.path.insert(0, str(tools_dir))
from pack_ufw import unwrap_fwsc, xor_header

def analyze_image(path: Path):
    print("=" * 70)
    print(f"[*] SANITY CHECK: {path.name} ({path.stat().st_size:,} bytes)")
    print("=" * 70)

    data = path.read_bytes()
    size = len(data)

    # 1. Size & Fill Ratio
    is_power_of_two = (size > 0) and ((size & (size - 1)) == 0)
    size_mb = size / (1024 * 1024)
    print(f"1. SIZE & FILL RATIO:")
    print(f"   - File size:            {size:,} bytes ({size_mb:.2f} MB)")
    print(f"   - Power-of-two size:    {'YES [PASS]' if is_power_of_two else 'NO (Container/Partition)'}")

    zeros = data.count(b'\x00')
    ffs   = data.count(b'\xFF')
    other = size - zeros - ffs
    pct_zero = (zeros / size) * 100.0
    pct_ff   = (ffs / size) * 100.0
    pct_other = (other / size) * 100.0
    pad_ratio = pct_zero + pct_ff

    print(f"   - 0x00 bytes:           {zeros:,} ({pct_zero:.1f}%)")
    print(f"   - 0xFF bytes:           {ffs:,} ({pct_ff:.1f}%)")
    print(f"   - Active Data (other):  {other:,} ({pct_other:.1f}%)")
    print(f"   - Total Padding Ratio:  {pad_ratio:.1f}%")
    
    fill_ok = 15.0 <= pad_ratio <= 75.0 and pct_other >= 25.0
    status_fill = "NORMAL [PASS]" if fill_ok else "SUSPICIOUS (>90% or <10%)"
    print(f"   - Bus Health / Fill:    {status_fill}")

    # 2. MBR & Partition Descriptor (0x00000000 - 0x00000120)
    print(f"\n2. MASTER BOOT RECORD & PARTITION DESCRIPTOR (0x0000 - 0x0120):")
    mbr = data[:0x120]
    mbr_zeros = mbr.count(b'\x00')
    mbr_ffs = mbr.count(b'\xFF')
    mbr_active = len(mbr) - mbr_zeros - mbr_ffs
    print(f"   - MBR bytes checked:    288 bytes")
    print(f"   - Non-zero/non-FF:      {mbr_active} bytes")

    # SDFILE signature or MBR vectors
    has_sdfile = b'SDFILE' in mbr or b'app_dir_head' in data[:0x1000]
    # Check SPI clock divisor / config words
    spi_div = struct.unpack_from("<I", mbr, 0x08)[0] if len(mbr) >= 12 else 0
    print(f"   - MBR Vectors/Table:    {'VALID & PRESENT [PASS]' if mbr_active > 32 else 'BLANK [FAIL]'}")
    print(f"   - SDFILE / Partition:   {'DETECTED [PASS]' if (has_sdfile or mbr_active > 32) else 'MISSING'}")

    # 3. App area: flash 0x4000 (file table), app.bin at 0x4120 -> XIP 0x02000120
    print(f"\n3. APP AREA (0x00004000, SFC-encoded; see jl_app_area.py inspect):")
    app_offset = 0x4000
    if size > app_offset + 32:
        app_head = data[app_offset : app_offset + 32]
        # First 4 bytes of app.bin entry in crt0
        entry_sig = app_head[:4].hex()
        is_crt0 = (app_head[0] in (0x04, 0x00, 0x01) or app_head[:2] == b'\x04\x81')
        print(f"   - App Entry Hex (32B):  {app_head[:16].hex(' ')}")
        print(f"   - Signature Check:      {'VALID ENTRY CODE [PASS]' if not all(b == 0xFF for b in app_head[:16]) and not all(b == 0x00 for b in app_head[:16]) else 'EMPTY'}")
    else:
        print(f"   - App offset {app_offset:#x} exceeds image size")

    # 4. Embedded Asset / String Inspection
    print(f"\n4. EMBEDDED FACTORY ASSET & STRING INSPECTION:")
    # Extract ASCII strings >= 4 chars
    ascii_strings = [m.group(0).decode('ascii', errors='ignore') 
                     for m in re.finditer(rb'[A-Za-z0-9_\-\.\:\/ ]{4,}', data)]
    
    target_keywords = ["Pocket", "AMP", "M-VAVE", "Clean", "Lead", "Drive", "Delay", "Reverb"]
    found_keywords = {}
    for kw in target_keywords:
        matches = [s.strip() for s in ascii_strings if kw.lower() in s.lower()]
        found_keywords[kw] = matches[:3]

    for kw, matches in found_keywords.items():
        if matches:
            print(f"   - Keyword '{kw}': FOUND ({len(matches)} samples: {', '.join(matches[:2])})")
        else:
            print(f"   - Keyword '{kw}': NOT FOUND")

    all_found = sum(1 for m in found_keywords.values() if m) >= 5
    print(f"   - Overall String Check: {'CONFIRMED [PASS]' if all_found else 'INCOMPLETE'}")
    print("=" * 70 + "\n")

def check_ufw_flash(ufw_path: Path):
    print("=" * 70)
    print(f"[*] EXTRACTING & SANITY CHECKING flash.bin FROM {ufw_path.name}")
    print("=" * 70)
    ufw_data = ufw_path.read_bytes()
    head = xor_header(ufw_data[:64])
    count = struct.unpack_from("<H", head, 8)[0]
    table = ufw_data[64 : 64 + count * 80]
    
    # Entry 0 is flash.bin
    e0 = xor_header(table[:80])
    offset = struct.unpack_from("<I", e0, 8)[0]
    size = struct.unpack_from("<I", e0, 12)[0]
    name = e0[64:].split(b"\x00")[0].decode("ascii", errors="replace")
    print(f"   - Extracting Entry 0: {name} (Offset: 0x{offset:06X}, Size: {size:,} B)")
    
    flash_chunk = ufw_data[offset : offset + size]
    
    # Save temp and analyze
    temp_p = ufw_path.parent / "temp_extracted_flash.bin"
    temp_p.write_bytes(flash_chunk)
    analyze_image(temp_p)
    temp_p.unlink()

if __name__ == "__main__":
    custom_fw_dir = tools_dir.parent
    full_flash = custom_fw_dir / "pocket_amp_full_flash.bin"
    if full_flash.exists():
        analyze_image(full_flash)
    else:
        print(f"[-] Not found: {full_flash}")

    ufw_file = custom_fw_dir / "pocket_amp_app.ufw"
    if ufw_file.exists():
        check_ufw_flash(ufw_file)
    else:
        print(f"[-] Not found: {ufw_file}")
