#!/usr/bin/env python3
"""
custom-fw/tools/wn_render.py - listen to a NAM WaveNet capture on the PC, through the same engine the unit runs

    python custom-fw/tools/wn_render.py "<capture>.nam" "<guitar DI>.wav" out.wav [--gain DB]

The capture is converted with emu/nam_loader.py exactly as pa_import.py does, and the DI is rendered through the firmware's block engine
(src/nam/pa_wavenet.c with the software accelerator model: -80 dB or better against the float reference). Mono 16-bit or 24-bit WAV in, 16-bit out.
Also prints a few numbers that tell a clean tone from a distorted one (crest factor, the share of energy above 2 kHz, the level).
"""
import argparse
import struct
import subprocess
import sys
import tempfile
import wave
from pathlib import Path

import numpy as np

FW = Path(__file__).resolve().parent.parent
sys.path.insert(0, str(FW / "emu"))
from nam_loader import parse_nam_file  # noqa: E402


def read_wav(path):
    with wave.open(str(path), "rb") as w:
        n, ch, sw, sr = w.getnframes(), w.getnchannels(), w.getsampwidth(), w.getframerate()
        raw = w.readframes(n)
    if sw == 2:
        x = np.frombuffer(raw, "<i2").astype(np.float32) / 32768.0
    elif sw == 3:
        b = np.frombuffer(raw, np.uint8).reshape(-1, 3).astype(np.int32)
        v = b[:, 0] | (b[:, 1] << 8) | (b[:, 2] << 16)
        v = np.where(v & 0x800000, v - 0x1000000, v)
        x = v.astype(np.float32) / 8388608.0
    else:
        raise SystemExit(f"unsupported sample width {sw}")
    return x.reshape(-1, ch)[:, 0], sr


def stats(name, x, sr):
    rms = float(np.sqrt(np.mean(x ** 2))) or 1e-12
    crest = float(np.max(np.abs(x)) / rms)
    spec = np.abs(np.fft.rfft(x * np.hanning(len(x)))) ** 2
    f = np.fft.rfftfreq(len(x), 1.0 / sr)
    hi = float(spec[f > 2000].sum() / spec.sum())
    print(f"  {name:8s} rms {20 * np.log10(rms):6.1f} dBFS, peak {20 * np.log10(np.max(np.abs(x)) + 1e-12):6.1f} dBFS, crest {crest:5.2f}, energy above 2 kHz {hi * 100:5.1f} %")


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("nam")
    ap.add_argument("di")
    ap.add_argument("out")
    ap.add_argument("--gain", type=float, default=0.0, help="dB applied to the DI first")
    ap.add_argument("--trim", type=float, default=None, help="override the capture's input trim (linear; the .nam gives e.g. 0.2818)")
    ap.add_argument("--seconds", type=float, default=0.0, help="render only the first seconds")
    a = ap.parse_args()
    blob, meta = parse_nam_file(Path(a.nam).read_bytes(), Path(a.nam).name)
    if blob[:4] != b"WAVE":
        raise SystemExit("not a WaveNet capture")
    x, sr = read_wav(a.di)
    if sr != 44100:
        from scipy.signal import resample_poly
        from math import gcd
        g = gcd(sr, 44100)
        x = resample_poly(x, 44100 // g, sr // g).astype(np.float32)
    if a.seconds:
        x = x[: int(a.seconds * 44100)]
    x = x * (10.0 ** (a.gain / 20.0))
    tmp = Path(tempfile.mkdtemp(prefix="pa_wnr_"))
    (tmp / "w.bin").write_bytes(blob)
    x.astype("<f4").tofile(tmp / "in.f32")
    exe = tmp / "r.exe"
    srcs = [FW / "test" / "wn_render.c", FW / "src" / "nam" / "pa_wavenet.c", FW / "src" / "dsp" / "hw_accel.c"]
    r = subprocess.run(["clang", "-O2", f"-I{FW / 'include'}", "-DPA_HOST_SIM=1", *map(str, srcs), "-lm", "-o", str(exe)], capture_output=True, text=True)
    if r.returncode:
        print(r.stderr[-2000:])
        raise SystemExit("build failed")
    makeup = 10.0 ** (float(meta.get("makeup_db", 0.0) or 0.0) / 20.0)
    r = subprocess.run([str(exe), str(tmp / "w.bin"), str(tmp / "in.f32"), str(tmp / "out.f32"), str(makeup)] + ([str(a.trim)] if a.trim is not None else []), capture_output=True, text=True)
    if r.returncode:
        raise SystemExit(r.stdout + r.stderr)
    y = np.fromfile(tmp / "out.f32", "<f4")
    pk = float(np.max(np.abs(y))) or 1.0
    scale = 0.89 / pk if pk > 0.89 else 1.0
    pcm = np.clip(y * scale * 32767.0, -32768, 32767).astype("<i2")
    with wave.open(a.out, "wb") as w:
        w.setnchannels(1); w.setsampwidth(2); w.setframerate(44100)
        w.writeframes(pcm.tobytes())
    print(f"{Path(a.nam).name} -> {a.out}   (output scaled by {scale:.3f} to avoid clipping)" if scale != 1.0 else f"{Path(a.nam).name} -> {a.out}")
    stats("DI", x, 44100)
    stats("capture", y, 44100)


if __name__ == "__main__":
    main()
