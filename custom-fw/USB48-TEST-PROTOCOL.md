# USB audio 48 kHz test protocol

Checks the fixes of branch `experimental/usb-48k-fixes` (Onyx 1.02): per-direction rates, the capture-ring drift control, the 48 kHz playback margin. Tool: `custom-fw/tools/usb48_verify_pc.py`. Nothing is written to the unit.

## Set-up
1. Firmware 1.02 flashed, unit powered from the PC over USB-C, enumerated as "Custom MIDI" (audio + MIDI). Build id is read from the unit and printed first.
2. The tone plays into the unit's USB speakers (default -30 dBFS): **lower anything plugged into the unit's output first.**
3. Close other programs that use the unit's audio. Windows keeps its shared-mode mixer out of the way when the test opens the endpoints in WASAPI exclusive mode (`--exclusive`), where the test sets the rate.

## Tests
| id | command | what it isolates |
|---|---|---|
| T1 | `usb48_verify_pc.py --rate 48000 --exclusive --seconds 30` | both directions at 48 kHz: tone to the unit, recording back (the unit's loopback mixes the playback into the recording) |
| T2 | `usb48_verify_pc.py --rate 48000 --exclusive --record-only --seconds 20` | the recording direction alone (the capture ring) |
| T3 | `usb48_verify_pc.py --rate 44100 --exclusive --record-only --seconds 20` | the same at 44.1 kHz, the A/B |

## Criteria (a run passes only if all hold)
- the stream runs in real time (97-103 % of the wall clock) and the PC reports no callback over/underflow
- the unit served the rate: frames per packet = rate / 1000 (48.00 / 44.10)
- capture ring re-centres + starvations <= 1 in the run (the one at stream start)
- playback ring re-centres + starvations <= 1 (T1)
- host frames the unit missed <= 0.1 % of packets
- T1 only: worst 10 ms window tone/error >= 35 dB, level within 1 dB

## Results, 2026-11-05, build 2611051424 (Onyx 1.02, branch `experimental/usb-48k-fixes`)
| run | frames/packet | host frames missed | underruns | capture re-centres | playback starved | worst window | verdict |
|---|---|---|---|---|---|---|---|
| T1 30 s (48k) | 48.00 | 294 | 106 | 25 | 45 | -21 dB (level spread 65 dB) | FAIL |
| T1 repeat 20 s | 48.00 | 105 | 21 | 2 | 16 | -24 dB | FAIL |
| T2 20 s (48k) | 48.00 | 131 | 2 | 12 | - | - | FAIL |
| T3 20 s (44.1k) | 44.10 | 102 | 4 | 8 | - | - | FAIL |

What passed: the unit serves 48 kHz (and 44.1 kHz) correctly, real time, no PC flags. Timer interrupt: always on time (every gap < 350 us), longest 105 us.

What failed: the recording ring still re-centres 8-25 times in 20-30 s, at 44.1 kHz as much as at 48 kHz, and the playback ring starves 16-45 times. The cause is a missed-host-frame rate of 5-10 per second, far above the 0.7 per second the first measurement showed. At that rate the ring gains ~44 frames per miss (about 290 frames per second) and the controller (clamped to ~130 frames per second) cannot drain it.

## Results, 2026-11-05, build 2611051449 (Onyx 1.03: + drop-on-miss in the recording ring)
| run | host frames missed | capture re-centres | capture starved | playback starved | worst window | verdict |
|---|---|---|---|---|---|---|
| T2 20 s (48k, record only) | 139 | **0** (was 12) | 1 (stream start) | - | - | capture criteria PASS; missed-frames criterion FAIL |
| T3 20 s (44.1k, record only) | 87 | **0** (was 8) | 1 | - | - | capture criteria PASS; missed-frames criterion FAIL |
| T1 30 s (48k, both directions) | 138 | 4 | 15 | 21 | -21 dB (level spread 72 dB) | FAIL |
| T1 15 s (48k, both directions) | 64 (48 dropped) | - | - | - | -17 dB | FAIL |

What the fix did: with the recording alone the ring now stays bounded (fill 136..250 frames, was 105..492) and never re-centres, at 48 kHz and 44.1 kHz.

What it did not: the host still misses 0.4-0.7 % of the frames (cause not found), and with playback running as well the recording still has dropouts (up to -21 dB windows): the playback ring starves 16-45 times per 30 s, and in the two-way run the capture ring also starved 15 times and re-centred 4. The timer interrupt was held off for >= 1.5 ms ten times in that run (histogram bucket), all other gaps < 350 us; the audio interrupt peaks at 26,251 ticks (about 1.1 ms of its 1.45 ms budget), no overruns.
