#!/usr/bin/env python3
"""
custom-fw/bench/build_bench.py - build the RAM-resident A2-Lite WaveNet benchmark.

    python custom-fw/bench/build_bench.py

Output in custom-fw/bench/out/ (git-ignored):
    bench.elf, bench.map   the linked program
    bench.bin              the image uploaded to RAM at 0x01C02000 (code + data, no bss)
    bench.json             the addresses the host needs (entry, config, results) + sha256

It uses the JieLi pi32 toolchain build.ps1 uses (or JIELI_PI32=<root>). The firmware's
own WaveNet sources are compiled unchanged, so the benchmark times the firmware's code.
"""
import hashlib
import json
import os
import re
import subprocess
import sys
from pathlib import Path

BENCH = Path(__file__).resolve().parent
FW = BENCH.parent
OUT = BENCH / "out"

SOURCES = [
    BENCH / "bench_entry.S",
    BENCH / "bench_main.c",
    BENCH / "bench_dsp.c",
    BENCH / "fft_conv.c",
    BENCH / "bench_fx.c",
    FW / "src" / "fx" / "fx_drive.c",
    FW / "src" / "fx" / "fx_mod.c",
    FW / "src" / "nam" / "pa_wavenet.c",
    FW / "src" / "nam" / "pa_wavenet_fast.c",
    FW / "src" / "nam" / "nam_engine.c",
    FW / "src" / "dsp" / "activation.c",
    FW / "src" / "dsp" / "hw_accel.c",
    FW / "src" / "board" / "st7735s.c",
    FW / "src" / "board" / "ui_5x7.c",
]
# hw_accel.c is built in its SOFTWARE form: the benchmark must never write the registers of
# the 0x01EE0400 accelerator, whose protocol has not been verified on a device. The GRU and
# the cab then run their matvec / FIR in software - what the device does once the
# accelerator has timed out.
PER_FILE_FLAGS = {"hw_accel.c": ["-DPA_HOST_SIM=1"]}
SYMBOLS = ["bench_entry", "g_bench_cfg", "g_bench_res", "_image_end", "_bss_start", "_bss_end",
           "bench_stack_top", "bench_main", "pa_wavenet_step_sample", "pa_wavenet_step_sample_fast",
           "pa_wavenet_step_sample_fast6"]


def toolchain() -> Path:
    cands = [os.environ.get("JIELI_PI32", "")]
    ps1 = (FW / "build.ps1").read_text(errors="replace")
    m = re.search(r'^\s*\$pi32Root\s*=\s*"([^"]+)"', ps1, re.M)
    if m:
        cands.append(m.group(1))
    cands.append(r"C:\JL\pi32")
    for c in cands:
        if c and (Path(c) / "bin" / "clang.exe").exists():
            return Path(c)
    sys.exit("JieLi pi32 toolchain not found (set JIELI_PI32)")


def run(cmd):
    p = subprocess.run([str(c) for c in cmd], capture_output=True, text=True)
    if p.returncode != 0:
        sys.exit(f"FAILED: {' '.join(str(c) for c in cmd)}\n{p.stdout}{p.stderr}")
    return p.stdout


def main() -> int:
    root = toolchain()
    b, inc, lib = root / "bin", root / "pi32v2-include", root / "pi32v2-lib" / "r3"
    OUT.mkdir(parents=True, exist_ok=True)
    cflags = ["-target", "pi32v2", "-mcpu=r3", "-O2", "-Wall", "-ffreestanding", "-fno-builtin",
              "-ffunction-sections", "-fdata-sections", "-DPA_ENABLE_RAMFUNC=1",
              "-isystem", inc, "-I", FW / "include", "-I", FW / "src" / "board", "-I", FW / "src" / "dsp",
              "-I", BENCH]
    objs = []
    for src in SOURCES:
        obj = OUT / (src.stem + ".o")
        run([b / "clang.exe", *cflags, *PER_FILE_FLAGS.get(src.name, []), "-c", src, "-o", obj])
        objs.append(obj)
    elf, binf, mapf = OUT / "bench.elf", OUT / "bench.bin", OUT / "bench.map"
    run([b / "pi32v2-ld.exe", "-T", BENCH / "bench.ld", "--gc-sections", "-Map", mapf, *objs,
         "--start-group", lib / "libcompiler_rt.a", lib / "libm.a", lib / "libc.a", "--end-group", "-o", elf])
    # This llvm-objcopy takes one -j per call: extract .text and .data and join them.
    # bench.ld places .data straight after .text (both 4-aligned), and the length check
    # below proves there is no gap.
    parts = []
    for sec in (".text", ".data"):
        part = OUT / f"sec{sec}.bin"
        run([b / "llvm-objcopy.exe", "-O=binary", f"-j={sec}", elf, part])
        parts.append(part.read_bytes())
    binf.write_bytes(b"".join(parts))

    syms = {}
    for line in run([b / "llvm-nm.exe", elf]).splitlines():
        f = line.split()
        if len(f) == 3 and f[2] in SYMBOLS:
            syms[f[2]] = int(f[0], 16)
    missing = [s for s in SYMBOLS if s not in syms]
    if missing:
        sys.exit(f"missing symbols: {missing}")
    image = binf.read_bytes()
    base = 0x01C02000
    if syms["bench_entry"] != base:
        sys.exit(f"bench_entry is at {syms['bench_entry']:#x}, not at the load address {base:#x}")
    if len(image) != syms["_image_end"] - base:
        sys.exit(f"bench.bin is {len(image)} B but the image spans {syms['_image_end'] - base} B")
    # The accelerator's register address must not appear in the image as a 32-bit literal:
    # the software build of hw_accel.c has no code that writes it.
    if (0x01EE0400).to_bytes(4, "little") in image:
        sys.exit("the image contains the accelerator address 0x01EE0400 - refusing (it must run in software)")
    # the float instructions really are hardware ones (no soft-float helpers linked in)
    soft = [s for s in ("__mulsf3", "__addsf3", "__subsf3", "__divsf3", "__muldf3", "__adddf3")
            if re.search(rf"\b{s}\b", run([b / "llvm-nm.exe", elf]))]
    man = {
        "load_address": base,
        "image_size": len(image),
        "image_sha256": hashlib.sha256(image).hexdigest(),
        "symbols": {k: v for k, v in sorted(syms.items())},
        "ram_end": syms["bench_stack_top"],
        "soft_float_helpers_linked": soft,
    }
    (OUT / "bench.json").write_text(json.dumps(man, indent=2) + "\n")
    print(f"bench.bin  {len(image):,} B at {base:#x}, sha256 {man['image_sha256'][:16]}")
    print(f"RAM used   {base:#x}..{syms['bench_stack_top']:#x} ({syms['bench_stack_top'] - base:,} B)")
    print(f"config at  {syms['g_bench_cfg']:#x}, results at {syms['g_bench_res']:#x}")
    print(f"soft-float helpers linked: {soft or 'none'}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
