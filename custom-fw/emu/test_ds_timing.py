# SPDX-License-Identifier: GPL-3.0-only
# Copyright (C) 2026 The Onyx Authors (@28spawn)
import sounddevice as sd
import numpy as np
import time

in_dev = 13
out_dev = 17

xruns = []
buffer_deltas = []
last_time = None

def cb(indata, outdata, frames, time_info, status):
    global last_time
    t = time.perf_counter()
    if last_time is not None:
        buffer_deltas.append((t - last_time) * 1000.0)
    last_time = t
    if status:
        xruns.append((t, str(status)))
    outdata[:] = indata

s = sd.Stream(device=(in_dev, out_dev), samplerate=44100, blocksize=256, channels=(2, 2), dtype='float32', callback=cb)
s.start()
time.sleep(2.0)
s.stop()
s.close()

print(f"Callbacks: {len(buffer_deltas)}")
print(f"Xruns (PortAudio status flags): {len(xruns)}")
for t, st in xruns[:10]:
    print("  Xrun:", st)

if buffer_deltas:
    print(f"Callback interval: expected {256/44.1:.2f} ms")
    print(f"  min: {min(buffer_deltas):.2f} ms, avg: {sum(buffer_deltas)/len(buffer_deltas):.2f} ms, max: {max(buffer_deltas):.2f} ms")
    spikes = [d for d in buffer_deltas if d > 12.0 or d < 1.0]
    print(f"  Severe timing jitter / missed deadlines (>12ms or <1ms): {len(spikes)} spikes out of {len(buffer_deltas)}")
