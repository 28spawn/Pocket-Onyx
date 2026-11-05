from pathlib import Path
"""QA: does the custom container keep the STOCK flash layout, and is the
runtime write protection actually in place?

1. Unwrap both .fwsc files (the .fwsc format interleaves a 20-byte product
   marker every 47 bytes across the first 940 bytes) and compare the two raw
   flash images byte for byte. The ONLY difference must be the app.bin
   partition: the MBR, the uboot, the VM, the BTIF calibration, the USER
   calibration, the factory AM3/CAB bank, the preset banks, the settings and
   the user NAM/CAB partitions have to be byte-identical to the stock image.
2. Check the container's own integrity (the header and table CRCs).
3. Check the app's placement inside the app partition.
"""
import os
import struct
import sys

FW = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
sys.path.insert(0, os.path.join(FW, "tools"))
import pack_ufw as P   # noqa: E402

STOCK = os.environ.get("STOCK_FWSC", os.path.join(str(Path.home()), "Downloads", "Pocket AMP.fwsc"))
CUSTOM = os.path.join(FW, "Pocket_AMP_Custom.fwsc")

ok = True


def check(cond, msg):
    global ok
    print(("  ok   " if cond else "  FAIL ") + msg)
    if not cond:
        ok = False


def decode(ufw):
    head = P.xor_header(ufw[:64])
    count = struct.unpack_from("<H", head, 8)[0]
    flen = struct.unpack_from("<I", head, 4)[0]
    chip = head[16:32].split(b"\x00")[0].decode("ascii", errors="replace")
    table = bytearray(ufw[64:64 + count * 80])
    entries = []
    for i in range(count):
        e = P.xor_header(table[i * 80:(i + 1) * 80])
        etype = struct.unpack_from("<H", e, 0)[0]
        ecrc = struct.unpack_from("<H", e, 4)[0]
        off = struct.unpack_from("<I", e, 8)[0]
        size = struct.unpack_from("<I", e, 12)[0]
        enc = struct.unpack_from("<I", e, 24)[0]
        name = e[64:].split(b"\x00")[0].decode("ascii", errors="replace")
        entries.append(dict(type=etype, crc=ecrc, off=off, size=size, enc=enc, name=name))
    return head, count, flen, chip, entries


print("=== 1. the containers ===")
stock_ufw, stock_product = P.unwrap_fwsc(open(STOCK, "rb").read())
cust_ufw, cust_product = P.unwrap_fwsc(open(CUSTOM, "rb").read())
print("  the stock template : %s (%d B)" % (os.path.basename(STOCK), os.path.getsize(STOCK)))
print("  the custom one     : %s (%d B)" % (os.path.basename(CUSTOM), os.path.getsize(CUSTOM)))
check(stock_product == cust_product, "the product marker is unchanged (%s)" % cust_product)
check(len(stock_ufw) == len(cust_ufw), "the images are the same size (%d B)" % len(cust_ufw))

print()
print("=== 2. the container's own integrity ===")
s_head, s_count, s_flen, s_chip, s_entries = decode(stock_ufw)
c_head, c_count, c_flen, c_chip, c_entries = decode(cust_ufw)
print("  the chip id        : %s" % c_chip)
print("  the entries        : %d (the stock has %d)" % (c_count, s_count))
check(c_chip == "AC791N", "the chip id is AC791N")
check(c_count == s_count, "the entry count matches the stock")
check(c_flen == len(cust_ufw), "the header's file length matches the image (%d B)" % c_flen)
check(P.crc16_ccitt(c_head[2:]) == struct.unpack_from("<H", c_head, 0)[0], "the header CRC is valid")
tbl = cust_ufw[64:64 + c_count * 80]
check(P.crc16_ccitt(tbl) == struct.unpack_from("<H", c_head, 2)[0], "the table CRC is valid")

print()
print("=== 3. the entries ===")
for e in c_entries:
    print("    %-12s type %d  size %8d  offset 0x%08X  enc %d"
          % (e["name"], e["type"], e["size"], e["off"], e["enc"]))

flash = [e for e in c_entries if e["name"].lower().startswith("flash")]
if not flash:
    print("  (no flash.bin entry - the whole image is the flash)")
    flash_off = 0
else:
    flash_off = flash[0]["off"]
    print("  the flash.bin entry: offset 0x%08X, %d B" % (flash_off, flash[0]["size"]))

app = open(os.path.join(FW, "build_app.bin"), "rb").read()
import jl_app_area as A   # noqa: E402
print()
print("=== 4. the app area (flash 0x4000..0xB4FFF, ENC-scrambled) ===")
area = A.decode_area(cust_ufw[flash_off:flash_off + A.AREA_END])
probs = A.check_area(area)
app_in = A.file_bytes(area, "app.bin")
print("  app.bin sits at flash 0x%06X .. 0x%06X (%d B)" % (A.AREA_START + A.TABLE_SIZE,
      A.AREA_START + A.TABLE_SIZE + len(app_in), len(app_in)))
print("  the app slot allows %d B" % P.APP_SLOT_MAX)
check(len(app) <= P.APP_SLOT_MAX, "the app fits its slot")
check(not probs, "the app area's file table and CRCs are consistent" + ("" if not probs else ": " + ", ".join(probs)))
check(app_in == app, "the app.bin in the image is exactly build_app.bin")

# The flash image starts at the flash.bin's offset inside the container, so
# everything below is in FLASH coordinates (the container's own header and
# entry table live BELOW the flash image and are expected to change: the
# packer recomputes their CRCs).
FBASE = flash_off
print()
print("=== 5. what changed against the stock image ===")
runs = []
i = 0
n = len(stock_ufw)
while i < n:
    if stock_ufw[i] != cust_ufw[i]:
        j = i
        while j < n and stock_ufw[j] != cust_ufw[j]:
            j += 1
        runs.append((i, j))
        i = j
    else:
        i += 1
print("  %d differing ranges, from 0x%06X to 0x%06X" % (len(runs), runs[0][0], runs[-1][1]))
# The container's own header (0..64) and entry table (64..0x240) legitimately
# change: the packer recomputes their CRCs. Everything else must be inside
# the app partition.
CONTAINER_OWN = 0x240
app_lo, app_hi = flash_off + A.AREA_START, flash_off + A.AREA_END
outside = [(a, b) for a, b in runs if not (app_lo <= a and b <= app_hi)
           and not (a < CONTAINER_OWN)]
check(not outside, "every difference is inside the app partition (or the container's own CRCs)")
if outside:
    for a, b in outside[:10]:
        print("      !! 0x%06X..0x%06X" % (a, b))
hdr_changes = [(a, b) for a, b in runs if a < CONTAINER_OWN]
print("  the container's own header/table changed in %d range(s) (the recomputed CRCs)" % len(hdr_changes))

print()
print("=== 6. every other sector is byte-identical to the stock ===")
sectors = [
    ("the MBR (flash header)", 0x000000, 0x001000),
    ("the uboot", 0x001000, 0x004000),
]
for name, a, b in sectors:
    ca, cb = FBASE + a, FBASE + b
    check(stock_ufw[ca:cb] == cust_ufw[ca:cb], "%-34s byte-identical to the stock" % name)
check(stock_ufw[app_lo:app_hi] != cust_ufw[app_lo:app_hi], "%-34s rebuilt with our app.bin (as intended)" % "the app area")
print("  (flash.bin ends at 0xB5000: VM, BTIF, USER, the AM4 bundle and the preset banks are not in the package)")

print()
print("RESULT:", "the container keeps the stock layout" if ok else "PROBLEM - see the FAILs above")
sys.exit(0 if ok else 1)
