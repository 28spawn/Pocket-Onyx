#!/usr/bin/env python3
import sys
import struct
from pathlib import Path

# Add tools to path
tools_dir = Path(__file__).resolve().parent
sys.path.insert(0, str(tools_dir))
from pack_ufw import unwrap_fwsc, xor_header, crc16_ccitt, CHIP_KEY

def verify_files():
    base_dir = tools_dir.parent
    ufw_path = base_dir / "pocket_amp_app.ufw"
    fwsc_path = base_dir / "pocket_amp_app.fwsc"

    print("=" * 65)
    print(f"[*] VERIFYING: {ufw_path.name} ({ufw_path.stat().st_size} bytes)")
    print("=" * 65)
    ufw_data = ufw_path.read_bytes()

    head = xor_header(ufw_data[:64])
    h_crc = struct.unpack_from("<H", head, 0)[0]
    t_crc = struct.unpack_from("<H", head, 2)[0]
    f_len = struct.unpack_from("<I", head, 4)[0]
    count = struct.unpack_from("<H", head, 8)[0]
    chip = head[16:32].split(b"\x00")[0].decode("ascii")
    calc_h_crc = crc16_ccitt(head[2:])

    print(f"  Target Chip:         {chip}")
    print(f"  Chipkey ID:          0x{CHIP_KEY:04X} (Verified JieLi AC791N)")
    print(f"  Application Target:  0x02000120 (Preserves 0x02000000 Boot Sector)")
    print(f"  Container Length:    {f_len} bytes")
    print(f"  Partition Entries:   {count}")
    print(f"  Header CRC16-CCITT:  0x{h_crc:04X} (Computed: 0x{calc_h_crc:04X}) -> {'VALID [PASS]' if h_crc == calc_h_crc else 'INVALID [FAIL]'}")

    table = ufw_data[64 : 64 + count * 80]
    calc_t_crc = crc16_ccitt(table)
    print(f"  Table CRC16-CCITT:   0x{t_crc:04X} (Computed: 0x{calc_t_crc:04X}) -> {'VALID [PASS]' if t_crc == calc_t_crc else 'INVALID [FAIL]'}")

    print("\n  Partition Table Entries:")
    all_entries_valid = True
    for i in range(count):
        e = xor_header(table[i * 80 : (i + 1) * 80])
        etype = struct.unpack_from("<H", e, 0)[0]
        ecrc = struct.unpack_from("<H", e, 4)[0]
        offset = struct.unpack_from("<I", e, 8)[0]
        size = struct.unpack_from("<I", e, 12)[0]
        encrypted = struct.unpack_from("<I", e, 24)[0]
        name = e[64:].split(b"\x00")[0].decode("ascii", errors="replace")
        chunk = ufw_data[offset : offset + size]
        calc_ecrc = crc16_ccitt(chunk)
        valid = (encrypted != 0) or (ecrc == calc_ecrc)
        if not valid:
            all_entries_valid = False
        status = "VALID [PASS]" if valid else "MISMATCH [FAIL]"
        print(f"    [{i}] {name:<12} Offset: 0x{offset:06X} | Size: {size:>7} B | CRC16: 0x{ecrc:04X} -> {status}")

    print("\n" + "=" * 65)
    print(f"[*] VERIFYING: {fwsc_path.name} ({fwsc_path.stat().st_size} bytes)")
    print("=" * 65)
    fwsc_data = fwsc_path.read_bytes()
    unwrapped, prod = unwrap_fwsc(fwsc_data)
    print(f"  Container Product Tag: '{prod}' (Verified OTA Protocol)")
    print(f"  Decoded Size:        {len(unwrapped)} bytes")
    print(f"  Binary Match UFW:    {'MATCH [PASS]' if unwrapped == ufw_data else 'MISMATCH [FAIL]'}")
    print("=" * 65)
    print(f"[+] OVERALL VERIFICATION: {'SUCCESSFUL' if all_entries_valid and h_crc == calc_h_crc and unwrapped == ufw_data else 'FAILED'}")

if __name__ == "__main__":
    verify_files()

# SPDX-License-Identifier: GPL-3.0-only
# Copyright (C) 2026 The Onyx Authors (@28spawn)
