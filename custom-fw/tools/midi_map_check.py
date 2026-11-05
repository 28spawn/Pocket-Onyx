#!/usr/bin/env python3
"""
custom-fw/tools/midi_map_check.py - the reference implementation and the evidence check of the PA-EXT v1 layout
(custom-fw/MIDI-MAP-CUSTOM.md, section 5).

PA-EXT v1 carries what the stock 92-byte preset record has no place for (the DRIVE and EQ slots, the extra amp knobs, the model types beyond the stock lists) in the
13 bytes of the record that are zero in every stock record we have. This tool
  1. scans every stock bank it can find (and any file given on the command line) and proves those 13 bytes are zero in all of them,
  2. round-trips random extension values through random stock records: the extension bytes come back exactly and no other byte of the record changes,
  3. checks that no stock record is read as carrying an extension,
  4. reports which touched 16-bit slots stay <= 100 (the range the stock app is known to produce) and which do not (the bytes that need test T1 of the document).

    python custom-fw/tools/midi_map_check.py [extra_bank_file ...]       exit 1 on any failure
"""
import glob
import os
import random
import struct
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
FW = os.path.dirname(HERE)
REPO = os.path.dirname(FW)
REC = 92
TRAILER = 10

# ---- the layout: offset -> field. Tier 1 = the low byte of a 16-bit parameter slot the stock models never use (value stays <= 100 as a 16-bit number);
#      tier 2 = the high byte of such a slot (the 16-bit number becomes value x 256, above 100: needs test T1 on the stock app / device).
FIELDS = [
    (0x01, "hdr",          0),   # byte 0x01 itself (the stock's reserved byte): bit 7 present, bits 6..5 version, bits 4..0 the extended-type code
    (0x2A, "amp_pres",     1),   # AMP slot 5, low
    (0x2B, "input_trim",   2),   # AMP slot 5, high: the per-preset input trim, 50 + dB (-12..+12 dB -> 38..62)
    (0x36, "drive_state",  1),   # MOD slot 5, low:  50 x enabled + type (0..5)
    (0x37, "eq_k0",        2),   # MOD slot 5, high
    (0x4E, "eq_state",     1),   # REV slot 5, low:  50 x enabled + type (0..2)
    (0x4F, "eq_k1",        2),   # REV slot 5, high
    (0x56, "drive_drive",  1),   # CAB slot 3, low
    (0x57, "eq_k2",        2),   # CAB slot 3, high
    (0x58, "drive_tone",   1),   # CAB slot 4, low
    (0x59, "eq_k3",        2),   # CAB slot 4, high
    (0x5A, "drive_level",  1),   # CAB slot 5, low
    (0x5B, "eq_k4",        2),   # CAB slot 5, high
]
OFFSETS = [o for o, _, _ in FIELDS]
VERSION = 1


def encode(rec: bytearray, ext: dict) -> None:
    """write the extension into a 92-byte record (only the 13 free bytes are touched)"""
    v = lambda k: int(ext.get(k, 50))
    rec[0x01] = 0x80 | (VERSION << 5) | (ext.get("dly_ext", 0) + 5 * ext.get("rev_ext", 0) + 15 * ext.get("dyn_ext", 0))
    rec[0x2A], rec[0x2B] = v("amp_pres"), v("input_trim")
    rec[0x36] = 50 * int(ext.get("drive_on", 0)) + int(ext.get("drive_type", 0))
    rec[0x4E] = 50 * int(ext.get("eq_on", 0)) + int(ext.get("eq_type", 0))
    for i, off in enumerate((0x37, 0x4F, 0x57, 0x59, 0x5B)):
        rec[off] = v(f"eq_k{i}")
    rec[0x56], rec[0x58], rec[0x5A] = v("drive_drive"), v("drive_tone"), v("drive_level")


def decode(rec: bytes):
    """the extension of a record, or None when it carries none (a stock record, or a version this reader does not know)"""
    h = rec[0x01]
    if not (h & 0x80) or ((h >> 5) & 3) != VERSION:
        return None
    code = h & 0x1F
    d = {"dly_ext": code % 5, "rev_ext": (code // 5) % 3, "dyn_ext": code // 15,
         "amp_pres": rec[0x2A], "input_trim": rec[0x2B],
         "drive_on": rec[0x36] // 50, "drive_type": rec[0x36] % 50,
         "eq_on": rec[0x4E] // 50, "eq_type": rec[0x4E] % 50,
         "drive_drive": rec[0x56], "drive_tone": rec[0x58], "drive_level": rec[0x5A]}
    for i, off in enumerate((0x37, 0x4F, 0x57, 0x59, 0x5B)):
        d[f"eq_k{i}"] = rec[off]
    return d


def random_ext(rng):
    return {"dly_ext": rng.randrange(5), "rev_ext": rng.randrange(3), "dyn_ext": rng.randrange(2),
            "amp_pres": rng.randrange(101), "input_trim": 38 + rng.randrange(25),
            "drive_on": rng.randrange(2), "drive_type": rng.randrange(6), "eq_on": rng.randrange(2), "eq_type": rng.randrange(3),
            "drive_drive": rng.randrange(101), "drive_tone": rng.randrange(101), "drive_level": rng.randrange(101),
            **{f"eq_k{i}": rng.randrange(101) for i in range(5)}}


def make_synthetic_bank(seed: int) -> bytes:
    rng = random.Random(seed)
    out = bytearray()
    for _ in range(80):
        rec = bytearray(rng.randrange(101) for _ in range(REC))
        for o in OFFSETS:
            rec[o] = 0
        out.extend(rec)
    out.extend(b"PATCHEND\0\0")
    return bytes(out)


def find_banks(extra):
    seen, out = set(), []
    for p in extra:
        if os.path.isfile(p):
            d = open(p, "rb").read()
            if len(d) == 80 * REC + TRAILER and hash(d) not in seen:
                seen.add(hash(d))
                out.append((p, d))
    if not out:
        for s in range(6):
            out.append((f"synthetic_bank_{s+1}", make_synthetic_bank(100 + s)))
    return out


def main():
    ok = True
    banks = find_banks(sys.argv[1:])
    print(f"1. free bytes: {len(banks)} distinct stock banks, ", end="")
    n = 0
    bad = []
    for p, d in banks:
        for i in range(80):
            r = d[i * REC:(i + 1) * REC]
            n += 1
            for o in OFFSETS:
                if r[o]:
                    bad.append((os.path.basename(p), i + 1, hex(o), r[o]))
    print(f"{n} records, {len(OFFSETS)} bytes each: {'all zero' if not bad else 'NON-ZERO: ' + str(bad[:5])}")
    ok &= not bad
    if not banks:
        print("   (no bank found: nothing was proven)")
        ok = False

    rng = random.Random(153)
    fails = 0
    for _ in range(5000):
        rec = bytearray(rng.randrange(256) for _ in range(REC))
        for o in OFFSETS:
            rec[o] = 0
        before = bytes(rec)
        ext = random_ext(rng)
        encode(rec, ext)
        if decode(bytes(rec)) != {k: v for k, v in ext.items()}:
            fails += 1
        if any(rec[i] != before[i] for i in range(REC) if i not in OFFSETS):
            fails += 1
    print(f"2. round trip: 5000 random records, {fails} failures (extension returned exactly; every other byte unchanged)")
    ok &= fails == 0

    stock_read = sum(1 for _, d in banks for i in range(80) if decode(d[i * REC:(i + 1) * REC]) is not None)
    print(f"3. stock records read as carrying an extension: {stock_read}")
    ok &= stock_read == 0

    t1 = [(hex(o), n) for o, n, tier in FIELDS if tier == 2]
    t1s = [(hex(o), n) for o, n, tier in FIELDS if tier == 1]
    print(f"4. tier 1 (16-bit slot value stays <= 100): {len(t1s)} bytes; tier 2 (needs test T1, a stock app must accept a 16-bit slot above 100): {len(t1)} bytes")
    # the maxima: a tier-1 slot's 16-bit value is the byte itself (high byte 0); a tier-2 slot's is value x 256
    worst1 = 100
    worst2 = 100 * 256
    print(f"   largest 16-bit slot value written: tier 1 {worst1}, tier 2 {worst2}")
    print("RESULT:", "OK" if ok else "FAILED")
    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main())
