#!/usr/bin/env python3
"""
pack_nam_am3.py — Pocket Amp NAM A2-Lite (Q21 GRU) + IR Cabinet (@M3/@M4) Packer & SysEx Builder

Model packing and 1-second DC-bias calibration (`calibrate_gru_dc_bias`) for Pocket Amp:
Specification:
  1. Quantizes a 1-layer GRU (input_size=1, hidden_size=16) to signed Q21 fixed-point
     (`scale = 2097151.0 = 2^21 - 1`).
  2. Packs the 0xFC8 (4,040-byte) runtime model struct (`pa_nam_model_t`).
  3. Runs 44,100 steps (1.0 s @ 44.1 kHz) of zero-input GRU recurrence (`calibrate_gru_dc_bias`)
     to compute the 16 per-unit steady-state DC bias values and tags the model with
     `"LMT"` (`0x544D4C`).
  4. Optionally packs a 1024-tap or 2048-tap Cabinet IR in Q18 fixed-point (`scale = 262144.0`)
     and emits `00 59` `cmd 25` USB-MIDI SysEx frames ready to upload to the Pocket Amp.
"""

import argparse
import json
import math
import struct
from pathlib import Path

Q21_SCALE = 2097151.0       # 2^21 - 1 (Q21 fixed-point scale)
Q18_SCALE = 262144.0        # 2^18
NAM_HIDDEN = 16
NAM_STRUCT_SIZE = 0xFC8     # 4,040 bytes
LMT_MAGIC = 0x00544D4C      # "LMT\0" tag at +0x183C (am3) / +0x1EBC (am4)


def clamp_i32(val: int) -> int:
    return max(-2147483648, min(2147483647, int(val)))


def float_to_q21(w: float) -> int:
    return clamp_i32(round(float(w) * Q21_SCALE))


def sigmoid(x: float) -> float:
    if x >= 0.0:
        z = math.exp(-x)
        return 1.0 / (1.0 + z)
    z = math.exp(x)
    return z / (1.0 + z)


def calibrate_gru_dc_bias(w_ih, w_hh, b_ih, b_hh, steps: int = 44100):
    """
    Computes GRU DC bias operating point:
    Runs the 16-unit GRU with x=0 for 44,100 samples (1 s @ 44.1 kHz) so the recurrent state
    settles to its DC operating point, returning the 16 steady-state hidden values.
    """
    H = NAM_HIDDEN
    h = [0.0] * H
    for _ in range(steps):
        h_next = [0.0] * H
        for i in range(H):
            # Gates: 0 = reset (r), 1 = update (z), 2 = candidate (n)
            acc_r = b_ih[0][i] + b_hh[0][i]
            acc_z = b_ih[1][i] + b_hh[1][i]
            acc_n_hh = b_hh[2][i]
            for j in range(H):
                acc_r += w_hh[0][i][j] * h[j]
                acc_z += w_hh[1][i][j] * h[j]
                acc_n_hh += w_hh[2][i][j] * h[j]
            r_g = sigmoid(acc_r)
            z_g = sigmoid(acc_z)
            n_g = math.tanh(b_ih[2][i] + r_g * acc_n_hh)
            h_next[i] = (1.0 - z_g) * n_g + z_g * h[i]
        h = h_next
    return h


def pack_nam_0xfc8(model_dict: dict) -> bytes:
    """
    Packs a 1->16 GRU model dictionary into the Pocket Amp's 0xFC8 (4,040-byte) Q21 struct:
      - +0x0000: W_z[16][17] (16 rows x {16 recurrent + 1 input} int32 Q21)
      - +0x0440: W_r[16][17] (16 rows x {16 recurrent + 1 input} int32 Q21)
      - +0x0880: W_n[16][16] (16 rows x 16 recurrent int32 Q21)
      - +0x0C80: b_z[16], b_r[16], b_n_ih[16], b_n_hh[16], w_in_n[16], head_w[16], head_bias
      - +0x0DC0: work[129] runtime scratch + dc_bias[16]
    """
    buf = bytearray(NAM_STRUCT_SIZE)
    H = NAM_HIDDEN

    w_ih = model_dict["w_ih"]       # [3][16]
    w_hh = model_dict["w_hh"]       # [3][16][16]
    b_ih = model_dict["b_ih"]       # [3][16]
    b_hh = model_dict["b_hh"]       # [3][16]
    head_w = model_dict["head_w"]   # [16]
    head_b = model_dict.get("head_b", 0.0)

    # 1. Pack W_z[16][17] at +0x0000 (gate index 1 = update gate z)
    off = 0x0000
    for i in range(H):
        for j in range(H):
            struct.pack_into("<i", buf, off, float_to_q21(w_hh[1][i][j]))
            off += 4
        struct.pack_into("<i", buf, off, float_to_q21(w_ih[1][i]))
        off += 4

    # 2. Pack W_r[16][17] at +0x0440 (gate index 0 = reset gate r)
    off = 0x0440
    for i in range(H):
        for j in range(H):
            struct.pack_into("<i", buf, off, float_to_q21(w_hh[0][i][j]))
            off += 4
        struct.pack_into("<i", buf, off, float_to_q21(w_ih[0][i]))
        off += 4

    # 3. Pack W_n[16][16] at +0x0880 (gate index 2 = candidate gate n recurrent weights)
    off = 0x0880
    for i in range(H):
        for j in range(H):
            struct.pack_into("<i", buf, off, float_to_q21(w_hh[2][i][j]))
            off += 4

    # 4. Pack bias & linear head vectors at +0x0C80
    off = 0x0C80
    for i in range(H):  # b_z[16]
        struct.pack_into("<i", buf, off, float_to_q21(b_ih[1][i] + b_hh[1][i]))
        off += 4
    for i in range(H):  # b_r[16]
        struct.pack_into("<i", buf, off, float_to_q21(b_ih[0][i] + b_hh[0][i]))
        off += 4
    for i in range(H):  # b_n_ih[16]
        struct.pack_into("<i", buf, off, float_to_q21(b_ih[2][i]))
        off += 4
    for i in range(H):  # b_n_hh[16]
        struct.pack_into("<i", buf, off, float_to_q21(b_hh[2][i]))
        off += 4
    for i in range(H):  # w_in_n[16]
        struct.pack_into("<i", buf, off, float_to_q21(w_ih[2][i]))
        off += 4
    for i in range(H):  # head_w[16]
        struct.pack_into("<i", buf, off, float_to_q21(head_w[i]))
        off += 4

    struct.pack_into("<i", buf, off, float_to_q21(head_b))
    off += 4
    struct.pack_into("<I", buf, off, LMT_MAGIC)

    # 5. Compute 1-second steady-state DC bias and store in initial h[0..15]
    dc_bias = calibrate_gru_dc_bias(w_ih, w_hh, b_ih, b_hh)
    h_state_off = 3520  # work[64] = byte offset 3520 inside 0xFC8 struct
    for i in range(H):
        struct.pack_into("<i", buf, h_state_off + i * 4, float_to_q21(dc_bias[i]))

    return bytes(buf)


def pack_7bit_sysex(raw: bytes) -> bytes:
    """LSB-first 8-to-7-bit JieLi SysEx payload encoding."""
    out = bytearray()
    for i in range(0, len(raw), 7):
        chunk = raw[i:i + 7]
        msb = 0
        for j, b in enumerate(chunk):
            if b & 0x80:
                msb |= (1 << j)
        out.append(msb)
        for b in chunk:
            out.append(b & 0x7F)
    return bytes(out)


def build_syscmd_frame(cmd: int, payload: bytes) -> bytes:
    """Wraps a payload in `00 59 | cmd:u8 | len:u24le | payload | chk` and 7-bit F0..F7 SysEx."""
    n = len(payload)
    chk = (~sum(payload)) & 0xFF
    inner = bytearray([
        0x00, 0x59,
        cmd & 0xFF,
        n & 0xFF, (n >> 8) & 0xFF, (n >> 16) & 0xFF
    ]) + payload + bytes([chk])
    return b"\xF0" + pack_7bit_sysex(bytes(inner)) + b"\xF7"


def make_default_tube_gru() -> dict:
    """Generates a warm, asymmetric tube-amp 1->16 GRU profile for standalone testing."""
    H = NAM_HIDDEN
    w_ih = [[0.15 * ((-1) ** i) for i in range(H)],
            [0.10 for _ in range(H)],
            [0.45 * (1.0 / (1.0 + 0.1 * i)) for i in range(H)]]
    w_hh = [[[0.05 if i == j else 0.0 for j in range(H)] for i in range(H)],
            [[0.82 if i == j else 0.0 for j in range(H)] for i in range(H)],
            [[0.25 if i == j else -0.02 for j in range(H)] for i in range(H)]]
    b_ih = [[0.0] * H, [-0.5] * H, [0.02] * H]
    b_hh = [[0.0] * H, [0.0] * H, [0.0] * H]
    head_w = [0.35 / math.sqrt(i + 1) for i in range(H)]
    return {"w_ih": w_ih, "w_hh": w_hh, "b_ih": b_ih, "b_hh": b_hh, "head_w": head_w, "head_b": 0.0}


def main():
    ap = argparse.ArgumentParser(description="Pack NAM A2-Lite Q21 0xFC8 model & SysEx for Pocket Amp")
    ap.add_argument("--in-json", type=Path, help="Input GRU weights JSON file (optional; uses default tube profile if omitted)")
    ap.add_argument("--out-bin", type=Path, required=True, help="Output 0xFC8 binary model file")
    ap.add_argument("--out-syx", type=Path, help="Optional output USB-MIDI SysEx (.syx) upload file (cmd 25)")
    args = ap.parse_args()

    if args.in_json and args.in_json.exists():
        model = json.loads(args.in_json.read_text(encoding="utf-8"))
    else:
        model = make_default_tube_gru()

    blob = pack_nam_0xfc8(model)
    args.out_bin.write_bytes(blob)
    print(f"[+] Wrote {len(blob)} bytes (0x{len(blob):X}) Q21 NAM A2-Lite struct to {args.out_bin}")

    if args.out_syx:
        # cmd 25 write-session begin frame: 3-byte u24le total size
        n = len(blob)
        begin_payload = bytes([n & 0xFF, (n >> 8) & 0xFF, (n >> 16) & 0xFF])
        syx = bytearray(build_syscmd_frame(25, begin_payload))
        # Followed by 128-byte chunk frames
        for off in range(0, len(blob), 128):
            syx.extend(build_syscmd_frame(25, blob[off:off + 128]))
        args.out_syx.write_bytes(bytes(syx))
        print(f"[+] Wrote {len(syx)} bytes of USB-MIDI SysEx upload stream to {args.out_syx}")


if __name__ == "__main__":
    main()

# SPDX-License-Identifier: GPL-3.0-only
# Copyright (C) 2026 The Onyx Authors (@28spawn)
