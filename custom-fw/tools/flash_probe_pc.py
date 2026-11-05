#!/usr/bin/env python3
"""
custom-fw/tools/flash_probe_pc.py - what did the READ-ONLY flash controller probe (src/board/flash_ram.c) find? Reads the result block from the RUNNING unit over the USB-MIDI diagnostic channel.
Nothing is written to the unit.

The probe runs once at boot (interrupts off, core 1 not started) ONLY with '<' alone held at power-on (since pack 168; before, it was compiled out). Any other boot skips it. Run this after that boot, before the next power cycle clears the RAM block.

    python custom-fw/tools/flash_probe_pc.py        exit 0 = the probe ran, read the id and the three reads back and restored the controller; 1 = it did not run or did not finish
"""
import struct
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
import ble_matrix_pc as m
import jl_ram_read as J


class _Live:
    """the Rom interface of jl_ram_read.py (read(addr, n)), over the diagnostic channel"""
    def __init__(self, unit):
        self.u = unit

    def read(self, addr, n):
        out = b""
        while len(out) < n:
            k = min(256, n - len(out))
            out += bytes(self.u.ram(addr + len(out), k))
        return out


def main():
    u = m.Unit("Custom MIDI")
    print("BUILD", u.build_id())
    rom = _Live(u)
    p = struct.unpack("<8I", rom.read(0x01C7D5D0, 32))
    if p[0] != 0x50454C42:
        print("no BLEP pointer block: this build has no Bluetooth status block, so the probe block cannot be found")
        return 1
    st = J.read_ble_status(rom, p)
    ptr = st.get("FL_PTR", 0)
    if ptr <= 0x1000:
        print("the status block carries no probe pointer")
        return 1
    J._print_flash_probe(rom, ptr)
    w = struct.unpack("<24I", rom.read(ptr, 96))
    rc = w[1] - (1 << 32) if w[1] & 0x80000000 else w[1]
    # The viewer above compares the first read with the STOCK image; the unit runs the custom pack, so compare it with the area that was flashed (--area), and the other two with the backup.
    area = None
    for i, a_ in enumerate(sys.argv):
        if a_ == "--area" and i + 1 < len(sys.argv):
            area = Path(sys.argv[i + 1]).read_bytes()[:16]
    got0 = b"".join(w[12 + j].to_bytes(4, "little") for j in range(4))
    if area is not None:
        print(f"    flash 0x004000 against the area that was flashed: {'EQUAL, the raw read of the app area is right' if got0 == area else 'DIFFERENT: ' + area.hex(' ')}")
    sfc_ok = (w[4] & 0x7FFFFFFF) == (w[5] & 0x7FFFFFFF)        # bit 31 is the controller's BUSY status at the instant of the read, not a setting
    print(f"    SFC_CON back as it was (bit 31 = busy status ignored): {'yes' if sfc_ok else 'NO'}")
    print(f"    SPI0_CON back as it was: {'yes' if w[6] == w[7] else 'NO'}")
    io_ok = w[8] == w[9]
    print(f"    IOMAP_CON0 back as it was: {'yes' if io_ok else 'NO: %#x before, %#x after (the give-back sets bit 5 whatever it was; fix: restore the saved value)' % (w[8], w[9])}")
    ok = (w[0] == 0x52504C46 and rc == 0 and w[2] == 0x856014 and (w[11] & 0xFF) == 3 and sfc_ok and w[6] == w[7] and io_ok
          and (area is None or got0 == area))
    print("VERDICT:", "PASS: the probe read the chip and gave the controller back exactly as it was" if ok else "NOT PASSED (see the lines above)")
    # ---- step 2: the one-sector write test (flash_ram.c pa_flashfn_wtest), present when the boot was '<' + '>' ----
    x = struct.unpack("<40I", rom.read(ptr, 160))[24:]
    if x[0] == 0x54574C46:
        RCS = {0: "programmed, verified, erased, blank again", -1: "refused: sector outside the allowed range", -2: "cache never idle (nothing changed)", -3: "continuous-read mode (refused, nothing changed)",
               -4: "SFC busy never cleared (nothing changed)", -10: "the sector was NOT blank (refused, nothing changed)", -11: "write enable did not latch (nothing changed)", -12: "the program timed out",
               -13: "the page read back wrong", -14: "the erase's write enable did not latch", -15: "the erase timed out", -16: "the sector is NOT blank after the erase", -17: "a transfer timed out"}
        wrc = x[1] - (1 << 32) if x[1] & 0x80000000 else x[1]
        if x[1] == 0xFFFFFFF0:
            print(f"  WRITE TEST: THE PREVIOUS RUN DID NOT FINISH, it stopped at stage {x[2] & 0xFF:#04x}; it was skipped this time. The sector may need a look (read 0xBC000..0xBCFFF).")
        elif x[1] in (0xFFFFFFC0, 0xFFFFFFE0):
            print(f"  WRITE TEST: not run, code {x[1]:#010x} (0xFFFFFFC0 = the read-only probe did not pass first, 0xFFFFFFE0 = the RAM copy is not usable): nothing was written")
        else:
            print(f"  WRITE TEST (sector 0xBC000 only): result {wrc} = {RCS.get(wrc, '?')}; stage {x[2] & 0xFF:#04x} (0x20 = finished and restored); the whole test took {x[15]} us")
            print(f"    before: {x[3]} of 4096 bytes not 0xFF; after the write enable SR1 {x[4] & 0xFF:#04x} (bit 1 = WEL, wanted)")
            print(f"    program: {x[5] / 24.0:.0f} us, SR1 after {x[7] & 0xFF:#04x}; read back {256 - x[6]} of 256 bytes right, first four {x[10]:#010x} (want 0x6f68615a = 5A 61 68 6F)")
            print(f"    erase: {x[8] / 24.0 / 1000.0:.1f} ms; chip still busy when it was handed back to XIP: {'YES' if x[14] else 'no'}; after the erase {x[9]} of 4096 bytes not 0xFF")
            print(f"    status at the end: SR1 {x[11] & 0xFF:#04x} SR2 {x[12] & 0xFF:#04x} (SR1 bits 0 and 1 must be 0)")
            wok = (wrc == 0 and x[3] == 0 and x[6] == 0 and x[9] == 0 and x[14] == 0 and (x[11] & 3) == 0 and (x[2] & 0xFF) == 0x20)
            print("WRITE TEST VERDICT:", "PASS: one page programmed and read back, the sector erased and blank again, the controller handed back" if wok else "NOT PASSED (see the lines above)")
            ok = ok and wok
    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main())
