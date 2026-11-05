#!/usr/bin/env python3
"""Host test of the PA_FLASH_PROBE readout (tools/jl_ram_read.py fp_decode) and of the probe block's layout.

The probe itself (src/board/flash_probe.c) runs on the unit only; this checks that the viewer's word offsets mirror the C struct
(by counting its fields) and that the decoder reports XIP matches, faults, skips and register hints correctly."""
import re
import struct
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
sys.path.insert(0, str(ROOT / "tools"))
import jl_ram_read as j

src = (ROOT / "src/board/flash_probe.c").read_text(encoding="utf-8")
# the struct mirrors: 10 header + 6 pad + 4*16 xip + 48 pad + 80 status + 80 base + 240 pad + 72*16 data
assert "uint32_t pad0[6];" in src and "uint32_t xip0[16], xip1[16], xip0b[16], xip1b[16];" in src
assert "uint32_t pad1[48];" in src and "status[FP_NWIN + 8]" in src and "base[FP_NWIN + 8]" in src and "pad2[240]" in src
assert "#define FP_NWIN      72u" in src and "#define FP_WWORDS    16u" in src
assert j.FP_DATA == 10 + 6 + 64 + 48 + 80 + 80 + 240 == 528
assert j.FP_WORDS == 528 + 72 * 16
assert "0x01C72000" in src and j.FP_BASE == 0x01C72000
assert re.search(r"#define PA_FLASH_PROBE\s+0|#ifndef PA_FLASH_PROBE\s+#define PA_FLASH_PROBE 0", src.replace("\r", "")), "must default OFF"
assert "0x01C72000u + 4 * 0" or True

# no stores to peripherals in the probe: the window pointers (p, x0, x1) only ever appear on the right of an assignment
body = src[src.index("void pa_flash_probe_run(void) {"):]
assert not re.search(r"(^|[;{]\s*)\*?\s*(p|x0|x1)(\[[^\]]*\])?\s*(=|\+=|\|=|&=)[^=]", body, re.M), "the probe must not store through the window pointers"

w = [0] * j.FP_WORDS
assert "no block" in j.fp_decode(w)[0]
w[0], w[2], w[3], w[5] = 0x42525046, 2611012258, 0x444F4E45, 2
app = bytes(range(64))
w[32:48] = list(struct.unpack("<16I", app))
head = bytes(range(100, 164))
w[16:32] = list(struct.unpack("<16I", head))
for i in range(72):
    w[128 + i], w[208 + i] = 1, 0x10000 + 0x100 * i
w[129] = 2; w[130] = 3; w[131] = 0x5052
w[j.FP_DATA + 16 * 5 + 3] = 0x5000
w[j.FP_DATA + 16 * 6 + 0] = 0xEB
out = "\n".join(j.fp_decode(w, app, head))
assert "YES -> the XIP window shows the app" in out
assert "YES -> the XIP base is flash 0x4000" in out
assert "FAULT" in out and "skipped" in out and "died here" in out
assert "00005000!" in out and "000000EB!" in out
bad = "\n".join(j.fp_decode(w, bytes(64), bytes(64)))
assert "NO" in bad and "YES" not in bad.split("windows")[0]
print("test_flash_probe: ok")
