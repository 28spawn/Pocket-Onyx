# SPDX-License-Identifier: GPL-3.0-only
# Copyright (C) 2026 The Onyx Authors (@28spawn)
"""Polyphase allpass half-band design (elliptic-derived, after L. de Soras' HIIR).
Prints coefficients and verifies an up->down chain numerically."""
import math, sys
import numpy as np


def acc_num(q, order, c):
    i, j, acc = 0, 1, 0.0
    while True:
        t = q ** (i * (i + 1)) * math.sin((i * 2 + 1) * c * math.pi / order) * j
        acc += t; j = -j; i += 1
        if abs(t) <= 1e-100 or i > 200: return acc


def acc_den(q, order, c):
    i, j, acc = 1, -1, 0.0
    while True:
        t = q ** (i * i) * math.cos(i * 2 * c * math.pi / order) * j
        acc += t; j = -j; i += 1
        if abs(t) <= 1e-100 or i > 200: return acc


def design(n, transition):
    k = math.tan((1 - transition * 2) * math.pi / 4) ** 2
    kk = (1 - k * k) ** 0.25
    e = 0.5 * (1 - kk) / (1 + kk)
    e2 = e * e; e4 = e2 * e2
    q = e * (1 + e4 * (2 + e4 * (15 + 150 * e4)))
    order = n * 2 + 1
    out = []
    for idx in range(n):
        c = idx + 1
        num = acc_num(q, order, c) * q ** 0.25
        den = acc_den(q, order, c) + 0.5
        ww = num / den; w2 = ww * ww
        x = math.sqrt((1 - w2 * k) * (1 - w2 / k)) / (1 + w2)
        out.append((1 - x) / (1 + x))
    return out


def response(coefs, f):
    """half-band H(z) = 0.5 (A0(z^2) + z^-1 A1(z^2)) at normalised freq f (of the 2x rate)."""
    z = np.exp(1j * 2 * np.pi * f)
    z2 = z * z
    a0 = np.ones_like(z); a1 = np.ones_like(z)
    for i, a in enumerate(coefs):
        sec = (a + 1 / z2) / (1 + a / z2)
        if i % 2 == 0: a0 = a0 * sec
        else: a1 = a1 * sec
    return 0.5 * (a0 + a1 / z)


if __name__ == "__main__":
    for n, tr, fs2, fpass in ((8, 0.04, 88200, 20000), (6, 0.04, 88200, 20000), (4, 0.20, 176400, 20000), (3, 0.25, 176400, 20000)):
        c = design(n, tr)
        f = np.linspace(0, 0.5, 20001)
        h = np.abs(response(c, f))
        fp = fpass / fs2
        pb = h[f <= fp]
        sb = h[f >= 0.5 - fp]           # images of the passband land here
        print(f"n={n} tr={tr} @ {fs2} Hz: passband ripple {20*np.log10(pb.min()):.4f} dB, "
              f"image rejection {20*np.log10(sb.max()):.1f} dB (stopband from {(0.5-fp)*fs2:.0f} Hz)")
        print("   coefs", ", ".join(f"{x:.10f}f" for x in c))
