#!/usr/bin/env python3
"""The flash controller probe (src/board/flash_ram.c) is READ-ONLY and is entered through the right address; the driver operation (step 3) sends only the allowed commands and writes only inside the map windows.

1. The only SPI NOR commands it sends are the reads: 0x9F (JEDEC id), 0x05 / 0x35 (status registers), 0x0B (fast read). Never write enable (0x06), program (0x02), erase (0x20 / 0x52 / 0xD8 / 0xC7 / 0x60),
   write status (0x01 / 0x31 / 0x11) or anything else: a command byte in the source that is not one of the four fails this test.
2. It never writes the flash pins' owner in any way but the documented take-over / give-back (SFC_CON, SPI0_CON, IOMAP bit 5): no other store to a peripheral.
3. pa_flash_probe_boot() calls the probe by ITS address, not the start of its SRAM section (the first hardware run entered fl_cs: the section starts with whichever function the linker puts first), and
   refuses to call it if that address is outside the section.
"""
import re
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
src = (ROOT / "src/board/flash_ram.c").read_text(encoding="utf-8").replace("\r", "")
body = src[src.index("FLASHFN int pa_flashfn_probe"):src.index("FLASHFN static void fl_addr3")]      # the READ-ONLY probe alone; the driver operation is checked separately below
fails = 0


def check(ok, msg):
    global fails
    print(("  ok   " if ok else "  FAIL ") + msg)
    if not ok:
        fails += 1


sent = set(int(m.group(1), 16) for m in re.finditer(r"fl_tx\(\s*(0x[0-9A-Fa-f]+)u?\s*,", body))
check(sent == {0x9F, 0x05, 0x35, 0x0B, 0xFF}, f"the bytes sent are the reads 9F / 05 / 35 / 0B and the 0xFF dummy byte after a fast read's address (found {sorted(hex(x) for x in sent)})")
# every fl_tx call in the probe has a literal command or an address byte expression, never a variable command
calls = re.findall(r"fl_tx\(([^,]+),", body)
cmds = [c for c in calls if re.fullmatch(r"\s*0x[0-9A-Fa-f]+u?\s*", c)]
others = [c.strip() for c in calls if c not in cmds]
check(all(re.fullmatch(r"\(a >> (16|8)\) & 0xFFu|a & 0xFFu", o) for o in others), f"the other bytes sent are the 3 address bytes ({others})")
check("0x02u" not in "".join(cmds) and "0x06u" not in "".join(cmds) and "0x20u" not in "".join(cmds), "no program / write-enable / erase command")
stores = re.findall(r"\b(FL_[A-Z0-9_]+)\s*(?:=|\|=|&=)[^=]", body)
allowed = {"FL_SFC_CON", "FL_SPI0_CON", "FL_IOMAP0", "FL_SPI0_BUF"}
check(set(stores) <= allowed, f"the registers it stores to are only SFC_CON / SPI0_CON / SPI0_BUF / IOMAP_CON0 (found {sorted(set(stores))})")
check("FL_IOMAP0 = (FL_IOMAP0 & ~0x20u) | (io & 0x20u);" in body and "FL_IOMAP0 |= 0x20u;" not in body, "the IOMAP pin-owner bit is given back as it was saved, not set unconditionally")
boot = src[src.index("void pa_flash_probe_boot(void)" + chr(10) + "{"):src.index("/* XIP side of the driver.")]
check("= &pa_flashfn_probe;" in boot, "the probe is called through its own address")
check("(void *)_ff_start;" not in boot.split("pa_flashfn_probe;")[0].split("int (*volatile fn)")[-1], "...not through the start of its section")
check("0xFFFFFFD0u" in boot, "an empty copy (all zeros / all ones) is never jumped into")
check("0xFFFFFFE0u" in boot, "an address outside the SRAM section is refused")

# ---- step 3: the generic driver operation (pa_flashfn_op) ----
wt = src[src.index("FLASHFN int pa_flashfn_op"):src.index("/* XIP side of the driver.")]
helpers = src[src.index("FLASHFN static void fl_addr3"):src.index("FLASHFN int pa_flashfn_op")]
wsent = set(int(m.group(1), 16) for m in re.finditer(r"fl_tx\(\s*(0x[0-9A-Fa-f]+)u?\s*,", wt + helpers))
check(wsent == {0x06, 0x04, 0x02, 0x20, 0x05, 0x0B, 0xFF}, f"the driver sends only: write enable 06, write disable 04, page program 02, sector erase 20, status read 05, fast read 0B, dummy FF (found {sorted(hex(x) for x in wsent)})")
every = set(int(m.group(1), 16) for m in re.finditer(r"fl_tx\(\s*(0x[0-9A-Fa-f]+)u?\s*,", src))
check(not (every & {0x01, 0x31, 0x11, 0xC7, 0x60, 0xD8, 0x52, 0xB9, 0xAB, 0x75, 0x7A}), "no write-status, chip-erase, block-erase, power-down or suspend command anywhere in the file")
# the driver's own write window equals pa_flash_map.h's (the RAM function cannot include the header, so the numbers are compared here)
hdr = (ROOT / "include/pa_flash_map.h").read_text(encoding="utf-8")
def hv(name):
    return int(re.search(r"#define\s+" + name + r"\s+(0x[0-9A-Fa-f]+)u", hdr).group(1), 16)
ghdr = (ROOT / "include/pa_flash_guard.h").read_text(encoding="utf-8")
W = r"\(\(\(o\) >= (0x[0-9A-Fa-f]+)u\) && \(\(o\) < (0x[0-9A-Fa-f]+)u\) && \(\(l\) <= 0x[0-9A-Fa-f]+u - \(o\)\)\)"
m = re.search(r"#define PA_FLD_WIN_OK\(o, l\) \(" + W + r" \|\| " + W + r"\)", ghdr)
check(bool(m) and tuple(int(x, 16) for x in m.groups()) == (hv("PA_FLASH_RW3_START"), hv("PA_FLASH_RW3_END"), hv("PA_FLASH_OTA_START"), hv("PA_FLASH_OTA_END")), "the driver's erase / program windows are exactly the map's two (our window and the update staging area; PA_FLD_WIN_OK in pa_flash_guard.h)")
check(hv("PA_FLASH_RW3_START") == 0x86000 and hv("PA_FLASH_RW3_END") == 0xB4000 and hv("PA_FLASH_OTA_START") == 0xB8000 and hv("PA_FLASH_OTA_END") == 0xBD000 and "PA_FLASH_RW1" not in hdr and "PA_FLASH_RW2" not in hdr, "our window is 0x86000..0xB3FFF, the staging area 0xB8000..0xBCFFF, and nothing else is writable: the rest of the stock's VM, BTIF / USER / the factory bundle, its saved presets and its save record are never written")
check("pa_fld_args_ok(op, off, (uint32_t)(uintptr_t)buf, n)" in wt and "buf < PA_FLD_SRAM_LO" in ghdr, "the operation checks its arguments with pa_fld_args_ok (windows, page, a buffer in SRAM: the flash cannot be read while the controller is in command mode)")
check(wt.count("fl_tx(0x02u") == 1 and wt.count("fl_tx(0x20u") == 1, "one page program and one sector erase in the driver")
check("(void)fl_wait_idle(48000000u, &extra, &err);" in wt, "the chip is waited for (up to 2 s more) before the flash goes back to XIP")
check("FL_IOMAP0 = (FL_IOMAP0 & ~0x20u) | (io & 0x20u);" in wt, "the IOMAP bit is given back as it was saved")
check("fl_guard_blocked()) return -8;" in src and "g[1] = 2u;" in ghdr and "pa_flash_dev_boot_done" in open(str(ROOT / "src/board/main.c"), encoding="utf-8").read(), "the boot guard: a boot-time flash operation that hung the last boot disables flash I/O for the session, and the main loop start clears the in-progress mark")
# the instruction audit of the built section: every call stays inside it (no libcall into the flash that is switched off)
import subprocess, shutil
elf = ROOT / "build_pi32" / "pocket_amp_custom.elf"
od = Path.home() / "tools/jieli/C$/JL/pi32/bin/llvm-objdump.exe"
if elf.exists() and od.exists():
    out = subprocess.run([str(od), "-d", "-j", ".flashfn", str(elf)], capture_output=True, text=True).stdout
    names = set(re.findall(r"^([A-Za-z_][A-Za-z0-9_]*):", out, re.M))
    tgt = set(re.findall(r"call\s+-?\d+\s+<([A-Za-z0-9_]+)", out))
    check(bool(names) and tgt <= names, f"every call in the built section goes to a function inside it ({sorted(tgt - names)} outside)")
    check(not re.search(r"(div|mod|rem)|__[a-z]+[0-9]", out), "no division / libcall in the built section")
else:
    print("  skip the instruction audit check (no built ELF / objdump)")
print("RESULT: " + ("FAILED" if fails else "ALL PASSED"))
sys.exit(1 if fails else 0)
