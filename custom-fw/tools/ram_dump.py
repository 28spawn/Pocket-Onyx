#!/usr/bin/env python3
"""ram_dump.py - save a range of the unit's SRAM to a file, through the ROM download mode (READ ONLY).

Uses the same reader as jl_ram_read.py (the ROM's READ_MEMORY command only: nothing is written to memory or flash, no loader is uploaded). The unit has to be
in the ROM stage (WL82 UBOOT1.00): the SRAM keeps what the firmware left in it across the reset into that mode, which is how every other readout of this project works.

    python custom-fw/tools/ram_dump.py --out "ram_snapshot.bin" --label "baseline, BT off"

    --start / --length   default the whole SRAM, 0x01C00000 .. 0x01C7FFFF (512 KB)
    --jl-tool / --device as for jl_ram_read.py

Writes <out> and <out>.json (label, range, size, sha256, time). Saves memory snapshot to the specified path.

Compare dumps with ram_diff.py.
"""
from __future__ import annotations

import argparse
import datetime as dt
import hashlib
import json
import os
import sys
from pathlib import Path

HERE = Path(__file__).resolve().parent
sys.path.insert(0, str(HERE))

SRAM_LO, SRAM_HI = 0x01C00000, 0x01C80000


def auto_dump(a, out: Path) -> int:
    """Dump in several ROM-stage sessions. On stock V1.05 a read at certain addresses (0x01C01000) causes a session timeout: the unit resets and
    boots its app again. This waits for the app, lets it settle for --settle seconds (the same every cycle, so the state is as repeatable as it can be), sends the 7D frame,
    reads until a read fails, then tries again from the same address once more; a second failure at the same address marks that page unreadable (filled with 0xA5, listed
    in the .json) and the dump goes on after it."""
    import time
    import enter_download_mode as edm
    import jl_ram_read
    jl_ram_read.CHUNK = a.chunk
    from jl_ram_read import Rom
    frame = [0xF0, 0x22, 0x24, 0x35, 0x7D, 0xF7]
    end = a.start + a.length
    data = bytearray([0xA5]) * a.length
    addr, fails, bad, cycles = a.start, {}, [], 0
    while addr < end and cycles < a.max_cycles:
        cycles += 1
        t_w = time.monotonic()
        while True:                                     # wait for the app: its MIDI port
            outs, _ = edm.midi_ports()
            if edm.pick(outs, a.send_7d):
                break
            if time.monotonic() - t_w > 90:
                print("  the unit's MIDI port did not come back within 90 s: power-cycle it by hand and run again")
                return 5
            time.sleep(1.0)
        print(f"  cycle {cycles}: app is up; settling {a.settle:.0f} s, then the 7D frame; next address {addr:#x}")
        time.sleep(a.settle)
        edm.send_and_listen(frame, a.send_7d, 0.0)
        t_sent = time.monotonic()
        rom = Rom(Path(a.jl_tool), a.device)
        try:
            while addr < end:
                k = min(a.step, end - addr)
                try:
                    data[addr - a.start: addr - a.start + k] = rom.read(addr, k)
                except Exception as e:
                    fails[addr] = fails.get(addr, 0) + 1
                    print(f"    read at {addr:#x} failed {time.monotonic() - t_sent:.1f} s after the frame ({fails[addr]}x at this address): {str(e)[:60]}")
                    if fails[addr] >= 2:
                        bad.append((addr, k))
                        print(f"    -> {addr:#x}..{addr + k - 1:#x} is unreadable: skipped")
                        addr += k
                    break
                addr += k
                print(f"\r    read up to {addr:#x} ({addr - a.start:,} / {a.length:,} bytes)", end="", flush=True)
        finally:
            rom.close()
        print()
        if addr < end:
            time.sleep(5.0)                             # the unit resets and boots the app again
    ok = addr >= end
    out.write_bytes(bytes(data))
    meta = {"label": a.label, "start": f"{a.start:#x}", "length": a.length, "sha256": hashlib.sha256(data).hexdigest(),
            "time": dt.datetime.now().isoformat(timespec="seconds"), "mode": "auto", "cycles": cycles, "settle_s": a.settle,
            "unreadable": [f"{s:#x}+{n:#x}" for s, n in bad], "complete": ok}
    Path(str(out) + ".json").write_text(json.dumps(meta, indent=2), encoding="utf-8")
    print(f"  {'done' if ok else 'STOPPED at ' + hex(addr)}: {cycles} cycle(s), {len(bad)} unreadable page(s); wrote {out}  sha256 {meta['sha256'][:16]}")
    return 0 if ok else 6


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--jl-tool", default=os.environ.get("JL_UBOOT_TOOL") or
                    str(Path.home() / "Downloads" / "jl-uboot-tool-main" / "jl-uboot-tool-main"))
    ap.add_argument("--device", help='download-mode disk, e.g. "\\\\.\\PHYSICALDRIVE2" (default: ask jldevfind)')
    ap.add_argument("--start", type=lambda s: int(s, 0), default=SRAM_LO)
    ap.add_argument("--length", type=lambda s: int(s, 0), default=SRAM_HI - SRAM_LO)
    ap.add_argument("--send-7d", metavar="PORT", help="send the 7D enter-upgrade SysEx on the MIDI port whose name contains PORT, then dump at once")
    ap.add_argument("--chunk", type=int, default=256, help="bytes per READ_MEMORY command (the ROM takes at most 512)")
    ap.add_argument("--step", type=lambda s: int(s, 0), default=0x1000, help="bytes per retry unit")
    ap.add_argument("--auto", action="store_true", help="dump in several sessions, skipping pages whose read kills the ROM stage (needs --send-7d)")
    ap.add_argument("--settle", type=float, default=20.0, help="--auto: seconds to let the app run after its MIDI port appears, before each 7D frame")
    ap.add_argument("--max-cycles", type=int, default=40)
    ap.add_argument("--out", required=True)
    ap.add_argument("--label", default="", help="what state the unit was in (written to the .json next to the dump)")
    ap.add_argument("--force", action="store_true", help="force overwrite")
    a = ap.parse_args()

    if not (SRAM_LO <= a.start and a.start + a.length <= SRAM_HI):
        print(f"refused: {a.start:#x}+{a.length:#x} is outside the SRAM {SRAM_LO:#x}..{SRAM_HI - 1:#x}")
        return 2
    out = Path(a.out)
    out.parent.mkdir(parents=True, exist_ok=True)
    if a.auto:
        if not a.send_7d:
            print("--auto needs --send-7d PORT")
            return 2
        return auto_dump(a, out)

    import time
    if a.send_7d:
        # The hardware ROM stage drops off the bus a few seconds after start when no traffic is present:
        # failed at once when a minute passed between entering and dumping), so enter and read in one process with nothing slow in between.
        import enter_download_mode as edm
        name, _ = edm.send_and_listen([0xF0, 0x22, 0x24, 0x35, 0x7D, 0xF7], a.send_7d, 0.0)
        t_sent = time.monotonic()
        print(f"  sent the 7D enter-upgrade frame on {name}; waiting for the ROM stage ...")
    import jl_ram_read
    jl_ram_read.CHUNK = a.chunk
    from jl_ram_read import Rom                                   # (imports pyyaml and the jl-uboot-tool only when a dump is really made)
    rom = Rom(Path(a.jl_tool), a.device)
    if a.send_7d:
        print(f"  ROM stage open {time.monotonic() - t_sent:.1f} s after the frame")
    data = bytearray()
    partial = Path(str(out) + ".partial")
    try:
        # a small test read first: if the ROM stage does not answer, say so before any long read
        try:
            rom.read(a.start, 64)
        except Exception as e:
            print(f"  the ROM stage does not answer a 64-byte read at {a.start:#x}: {e}")
            print("  the unit probably reset or is still settling: power-cycle it, wait for initialization, send the 7D frame again, then dump at once.")
            return 3
        step = a.step
        addr, left = a.start, a.length
        t0 = dt.datetime.now()
        t_enter = t_sent if a.send_7d else None
        while left > 0:
            k = min(step, left)
            for attempt in range(4):
                try:
                    data += rom.read(addr, k)
                    break
                except Exception as e:
                    since = f", {time.monotonic() - t_enter:.1f} s after the 7D frame" if t_enter else ""
                    print(f"\n  read at {addr:#x} failed (attempt {attempt + 1}/4) after {(dt.datetime.now() - t0).total_seconds():.1f} s of reading{since}: {e}")
                    if attempt == 3:
                        partial.write_bytes(bytes(data))
                        print(f"  stopped at {addr:#x}; the {len(data):,} bytes read so far are in {partial}")
                        print("  a failed read can mean the unit reset (the RAM then holds noise, not the state): do NOT use a dump taken after this without a fresh 7D entry.")
                        return 4
                    time.sleep(1.0)
            addr += k
            left -= k
            print(f"\r  read {len(data):>8,} / {a.length:,} bytes", end="", flush=True)
        print(f"\n  {a.length:,} bytes in {(dt.datetime.now() - t0).total_seconds():.0f} s")
    finally:
        rom.close()

    # a running firmware's RAM has runs of zeros; uniformly random bytes mean the RAM was lost (power-up noise) or the read is wrong
    zeros = data.count(0)
    if len(data) >= 65536 and abs(zeros - len(data) / 256) < 4 * (len(data) / 256) ** 0.5:
        print("  WARNING: this dump looks like random noise (zero bytes at exactly the 1-in-256 rate): the unit probably lost its RAM"
              " (reset or power loss) before the read. Do not use it; power-cycle, 7D, dump again.")

    out.write_bytes(bytes(data))
    meta = {"label": a.label, "start": f"{a.start:#x}", "length": a.length, "sha256": hashlib.sha256(data).hexdigest(),
            "time": dt.datetime.now().isoformat(timespec="seconds")}
    Path(str(out) + ".json").write_text(json.dumps(meta, indent=2), encoding="utf-8")
    print(f"  wrote {out}  sha256 {meta['sha256'][:16]}")
    return 0


if __name__ == "__main__":
    sys.exit(main())

# SPDX-License-Identifier: GPL-3.0-only
# Copyright (C) 2026 The Onyx Authors (@28spawn)
