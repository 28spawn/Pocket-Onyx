#!/usr/bin/env python3
"""custom-fw/test/test_wn_block.py - the WaveNet block engine against the float reference, on a real A2-Lite capture.

Converts emu/test_capture.nam (or the .nam given as argv[1]) with emu/nam_loader.py, builds test_wn_block.c with the software accelerator model
(which implements the conv-mode job) and runs it.
"""
import subprocess
import sys
import tempfile
from pathlib import Path

FW = Path(__file__).resolve().parent.parent
sys.path.insert(0, str(FW / "emu"))
from nam_loader import parse_nam_file  # noqa: E402

nam = Path(sys.argv[1]) if len(sys.argv) > 1 else FW / "emu" / "test_capture.nam"
blob, meta = parse_nam_file(nam.read_bytes(), nam.name)
if blob[:4] != b"WAVE":
    sys.exit(f"{nam.name} is not a WaveNet capture")
tmp = Path(tempfile.mkdtemp(prefix="pa_wnblk_"))
(tmp / "w.bin").write_bytes(blob)
exe = tmp / "t.exe"
cmd = ["clang", "-O2", "-Wall", "-Wextra", "-Wno-unused-function", f"-I{FW / 'include'}", "-DPA_HOST_SIM=1",
       str(FW / "test" / "test_wn_block.c"), str(FW / "src" / "nam" / "pa_wavenet.c"), str(FW / "src" / "dsp" / "hw_accel.c"), "-lm", "-o", str(exe)]
r = subprocess.run(cmd, cwd=FW, capture_output=True, text=True)
if r.returncode:
    print(r.stdout, r.stderr[-3000:])
    sys.exit("build failed")
sys.exit(subprocess.run([str(exe), str(tmp / "w.bin")]).returncode)

# SPDX-License-Identifier: GPL-3.0-only
# Copyright (C) 2026 The Onyx Authors (@28spawn)
