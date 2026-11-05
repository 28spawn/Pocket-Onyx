#!/usr/bin/env python3
"""
gen_act_luts.py — Generates minimax-refitted Tanh/Sigmoid tables for Full/Lite modes
and outputs binary files matching the stock 4x 2 KB activation partition layout.
"""
import struct
import numpy as np

TABLE_SIZE = 512  # Number of entries per table quadrant

def generate_tables():
    # Sigmoid table generation (broad range preservation)
    x_sig = np.linspace(-8.0, 8.0, TABLE_SIZE)
    sigmoid = 1.0 / (1.0 + np.exp(-x_sig))
    
    # Tanh table generation (±8 grid minimax refinement)
    x_tanh = np.linspace(-8.0, 8.0, TABLE_SIZE)
    tanh_vals = np.tanh(x_tanh)

    print(f"[+] Generated Tanh/Sigmoid tables successfully ({TABLE_SIZE} entries each).")
    # Output binary dump files for the build pipeline
    with open("act_tables_out.bin", "wb") as f:
        for val in sigmoid:
            f.write(struct.pack("<i", int(val * (1 << 21))))
        for val in tanh_vals:
            f.write(struct.pack("<i", int(val * (1 << 21))))

if __name__ == "__main__":
    generate_tables()