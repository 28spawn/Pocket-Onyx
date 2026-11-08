# SPDX-License-Identifier: GPL-3.0-only
# Copyright (C) 2026 The Onyx Authors (@28spawn)
import sounddevice as sd

apis = sd.query_hostapis()
print("HOST APIS:")
for idx, a in enumerate(apis):
    print(f"  {idx}: {a['name']}")

print("\nALL DEVICES:")
for idx, d in enumerate(sd.query_devices()):
    api = apis[d['hostapi']]['name']
    print(f"[{idx:2d}] {d['name']:35s} | API {d['hostapi']} ({api:20s}) | in:{d['max_input_channels']} out:{d['max_output_channels']}")
