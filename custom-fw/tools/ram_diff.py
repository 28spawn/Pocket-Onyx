#!/usr/bin/env python3
"""ram_diff.py - compare SRAM dumps taken with ram_dump.py: what did Bluetooth (or anything else) change?

    python custom-fw/tools/ram_diff.py A.bin B.bin [--base 0x01C00000] [--gap 64] [--top 20]
    python custom-fw/tools/ram_diff.py --fill A.bin             # fill patterns of one dump (stack low-water marks, untouched pools)

Read-only, offline. The SRAM is NOT cleared at boot, so two boots of the same firmware differ in places that mean nothing: take the same state twice (A1, A2)
and compare them first - that is the noise floor. What B has more than the noise is what the state added.

For a pair it prints
  * the words that differ, in total and per 4 KB page (a map: one character a page, ' ' none, '.' a few, 'o' some, '#' most of the page),
  * the biggest changed regions (changes closer than --gap bytes are one region),
  * how much of A's zero/fill-pattern memory became something else in B (memory the new state started to use).
"""
from __future__ import annotations

import argparse
import struct
import sys
from collections import Counter
from pathlib import Path

PAGE = 4096


def load(p: str) -> bytes:
    b = Path(p).read_bytes()
    return b[: len(b) // 4 * 4]


def words(b: bytes):
    return struct.unpack("<%dI" % (len(b) // 4), b)


def fill_runs(w, min_words=32):
    """Runs of one repeated 32-bit value: [(start_word, n_words, value)]"""
    runs, i, n = [], 0, len(w)
    while i < n:
        j = i + 1
        while j < n and w[j] == w[i]:
            j += 1
        if j - i >= min_words:
            runs.append((i, j - i, w[i]))
        i = j
    return runs


def regions(diff_idx, gap_words):
    out, start, last = [], None, None
    for i in diff_idx:
        if start is None:
            start = last = i
        elif i - last <= gap_words:
            last = i
        else:
            out.append((start, last))
            start = last = i
    if start is not None:
        out.append((start, last))
    return out


def cmd_fill(path, base, top):
    w = words(load(path))
    runs = fill_runs(w)
    by = Counter()
    for _, n, v in runs:
        by[v] += n * 4
    print(f"{path}: {len(w) * 4:,} bytes")
    print("  bytes in runs of one repeated 32-bit value (32 words or longer), by value:")
    for v, nb in by.most_common(8):
        print(f"    {v:#010x}  {nb:>9,} B")
    print(f"  the {top} longest runs (address, length, value):")
    for i, n, v in sorted(runs, key=lambda r: -r[1])[:top]:
        print(f"    {base + i * 4:#010x}  {n * 4:>8,} B  {v:#010x}")


def cmd_diff(pa, pb, base, gap, top):
    a, b = words(load(pa)), words(load(pb))
    n = min(len(a), len(b))
    if len(a) != len(b):
        print(f"note: the dumps differ in size ({len(a) * 4:,} / {len(b) * 4:,}); comparing the first {n * 4:,} bytes")
    idx = [i for i in range(n) if a[i] != b[i]]
    print(f"{pa}  ->  {pb}")
    print(f"  {len(idx):,} of {n:,} words differ ({100.0 * len(idx) / n:.2f} %), {len(idx) * 4:,} bytes")
    per_page = Counter(i * 4 // PAGE for i in idx)
    cells = []
    for pg in range((n * 4 + PAGE - 1) // PAGE):
        c = per_page.get(pg, 0)
        cells.append(" " if c == 0 else "." if c < 16 else "o" if c < 256 else "#")
    print("  4 KB page map (' ' none  '.' <16 words  'o' <256  '#' more), 64 pages a line:")
    for row in range(0, len(cells), 64):
        print(f"    {base + row * PAGE:#010x} |{''.join(cells[row:row + 64])}|")
    regs = regions(idx, max(gap // 4, 1))
    regs.sort(key=lambda r: -(r[1] - r[0]))
    print(f"  {len(regs)} changed regions (closer than {gap} B count as one); the {top} biggest:")
    for s, e in regs[:top]:
        print(f"    {base + s * 4:#010x} .. {base + e * 4 + 3:#010x}  {(e - s + 1) * 4:>8,} B")
    # memory that A had as zero / one repeated value in a long run and B uses now
    fa = set()
    for i0, nn, v in fill_runs(a, 64):
        fa.update(range(i0, i0 + nn))
    started = sum(1 for i in idx if i in fa)
    print(f"  of A's fill memory (runs of 64+ equal words, {len(fa) * 4:,} B), {started * 4:,} B hold something else in B")


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("dumps", nargs="+")
    ap.add_argument("--base", type=lambda s: int(s, 0), default=0x01C00000, help="address of the first byte of the dumps")
    ap.add_argument("--gap", type=int, default=64, help="bytes: changes closer than this are one region")
    ap.add_argument("--top", type=int, default=20)
    ap.add_argument("--fill", action="store_true", help="describe the fill patterns of ONE dump instead of comparing two")
    a = ap.parse_args()
    if a.fill:
        for p in a.dumps:
            cmd_fill(p, a.base, a.top)
        return 0
    if len(a.dumps) != 2:
        print("give two dumps to compare (or --fill with one)")
        return 2
    cmd_diff(a.dumps[0], a.dumps[1], a.base, a.gap, a.top)
    return 0


if __name__ == "__main__":
    sys.exit(main())
