#!/usr/bin/env python3
"""
custom-fw/tools/usb48_verify_pc.py - does the unit's USB audio stay clean at 48 kHz (and 44.1 kHz for comparison)? Protocol: USB48-TEST-PROTOCOL.md

Plays a 1 kHz tone into the unit's USB speakers while recording its USB microphone in one duplex stream (the unit's loopback, ON by default, mixes the host's playback into the recording), reads the
unit's own counters (ISOT block at SRAM 0x01C73F00, over the MIDI diag channel) before and after, and judges both sides:

  PC side    the recording, cut into 10 ms windows (a whole number of 1 kHz cycles): each window's tone-to-error ratio, its level, and where in time the worst ones are
  unit side  host frames with no packet, underruns, frames per packet (48.0 = the unit really served 48 kHz), capture ring re-centres / starvations, playback ring re-centres / starvations

    python custom-fw/tools/usb48_verify_pc.py [--seconds 30] [--rate 48000] [--exclusive] [--tone-db -30]

Nothing is written to the unit. Exit 0 = every criterion met, 1 = at least one failed, 2 = could not run.
"""
import argparse, struct, sys, time
from pathlib import Path
import numpy as np
import sounddevice as sd
sys.path.insert(0, str(Path(__file__).resolve().parent))
import ble_matrix_pc as m

ISOT = 0x01C73F00


def isot(u):
    return struct.unpack("<64I", b"".join(u.ram(ISOT + 64 * i, 64) for i in range(4)))


def find(devs, name, kind, api):
    ch = "max_input_channels" if kind == "in" else "max_output_channels"
    for i, d in enumerate(devs):
        if name.lower() in d["name"].lower() and d[ch] > 0 and api.lower() in sd.query_hostapis(d["hostapi"])["name"].lower():
            return i
    return None


def analyse(x, rate, tone_db, skip_s=3.0):
    """x: mono float. Windows of 10 ms = rate/100 samples (10 whole 1 kHz cycles for 44.1k and 48k)."""
    n = rate // 100
    t = np.arange(n)
    ph = 2 * np.pi * 1000.0 * t / rate
    tt = (t - n / 2) / (n / 2)
    B = np.stack([np.cos(ph), np.sin(ph), tt * np.cos(ph), tt * np.sin(ph)], 1)
    P = np.linalg.pinv(B)
    out = []
    for w0 in range(int(skip_s * rate), len(x) - n, n):
        seg = x[w0:w0 + n]
        c = P @ seg
        err = seg - B @ c
        amp = float(np.hypot(c[0], c[1]))
        snr = 20 * np.log10(amp / (np.sqrt(np.mean(err ** 2)) * np.sqrt(2) + 1e-12) + 1e-12)
        out.append((w0 / rate, snr, 20 * np.log10(amp + 1e-12)))
    return out


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--seconds", type=float, default=30.0)
    ap.add_argument("--rate", type=int, default=48000, choices=(44100, 48000))
    ap.add_argument("--tone-db", type=float, default=-30.0)
    ap.add_argument("--exclusive", action="store_true", help="WASAPI exclusive mode (the app, not the Windows mixer, sets the rate)")
    ap.add_argument("--device", default="Custom MIDI")
    ap.add_argument("--record-only", action="store_true", help="no playback: isolates the unit-to-host (recording) direction; only the unit counters are judged")
    ap.add_argument("--save", help="save the recording as a WAV here")
    a = ap.parse_args()

    u = m.Unit("Custom MIDI")
    print("BUILD", u.build_id(), flush=True)
    if isot(u)[0] != 0x544F5349:
        print("no ISOT block: cannot read the unit's counters"); return 2

    devs = sd.query_devices()
    api = "WASAPI"
    i_in, i_out = find(devs, a.device, "in", api), find(devs, a.device, "out", api)
    if i_in is None or i_out is None:
        print("no WASAPI input/output named", a.device); return 2
    extra = sd.WasapiSettings(exclusive=a.exclusive)
    amp = 10 ** (a.tone_db / 20)
    state = {"ph": 0, "frames": 0, "flags": []}
    rec = []
    dphi = 2 * np.pi * 1000.0 / a.rate

    def cb(indata, outdata, n, t, status):
        k = np.arange(n) + state["ph"]
        outdata[:, 0] = amp * np.sin(dphi * k)
        outdata[:, 1] = outdata[:, 0]
        state["ph"] += n
        state["frames"] += n
        rec.append(indata[:, 0].copy())
        if status:
            state["flags"].append((round(state["frames"] / a.rate, 2), str(status)))

    b = isot(u)
    t0 = time.perf_counter()
    try:
        if a.record_only:
            def cbi(indata, n, t, status):
                state["frames"] += n; rec.append(indata[:, 0].copy())
                if status: state["flags"].append((round(state["frames"] / a.rate, 2), str(status)))
            with sd.InputStream(device=i_in, samplerate=a.rate, channels=2, dtype="float32", callback=cbi, extra_settings=extra, latency="high"):
                time.sleep(a.seconds)
        else:
            with sd.Stream(device=(i_in, i_out), samplerate=a.rate, channels=2, dtype="float32", callback=cb, extra_settings=(extra, extra), latency="high"):
                time.sleep(a.seconds)
    except Exception as e:
        print("could not open the duplex stream at", a.rate, "Hz:", e); return 2
    wall = time.perf_counter() - t0
    e = isot(u)
    x = np.concatenate(rec)
    if a.save:
        import soundfile as sf
        sf.write(a.save, x, a.rate, subtype="PCM_24")

    w = analyse(x, a.rate, a.tone_db)
    snr = np.array([s for _, s, _ in w]); lvl = np.array([l for _, _, l in w]); tm = np.array([t for t, _, _ in w])
    d = lambda k: e[k] - b[k]
    pk, fr = d(2), d(3)
    fpp = fr / pk if pk else 0.0
    worst = np.argsort(snr)[:5]

    print(f"\n=== {a.rate} Hz, {'exclusive' if a.exclusive else 'shared'}, {a.seconds:.0f} s, tone {a.tone_db:.0f} dBFS ===")
    print(f"PC   frames {state['frames']} in {wall:.1f} s wall ({100 * state['frames'] / a.rate / wall:.1f} %), callback flags {len(state['flags'])}: {state['flags'][:4]}")
    if not a.record_only: print(f"PC   {len(w)} windows: median tone/error {np.median(snr):.1f} dB, worst {snr.min():.1f} dB, level spread {lvl.max() - lvl.min():.2f} dB")
    if not a.record_only: print("PC   worst windows (s, dB):", ", ".join(f"{tm[i]:.1f}s {snr[i]:.1f}" for i in worst))
    print(f"UNIT packets {pk}, frames/packet {fpp:.2f}, host frames missed {d(12)}, underruns {d(4)}, waits {d(5)}")
    print(f"UNIT capture ring: starved {d(51)}, re-centres {d(52)}  (fill {e[53]}..{e[54]} lifetime)")
    print(f"UNIT playback ring: packets {d(46)}, starved {d(47)}, re-centres {d(48)}")
    print(f"DIAG  max usb_sie_lock(): {e[27]} us, global IRQ disabled max: {e[28]} us (count {e[29]})")
    if e[26] > b[26]:
        new_starves = e[26] - b[26]
        log_addr = e[25]
        print(f"DIAG  playback starvation captured: {new_starves} event(s) in run (buffer at {log_addr:#010x}):")
        read_n = min(16, e[26])
        try:
            raw_log = b"".join(u.ram(log_addr + 44 * i, 44) for i in range(read_n))
            for i in range(read_n):
                fb, need, rdi, wr, rx_pkts, rx_bytes, sof, t_rx, t_blk, sie_us, irq_us = struct.unpack("<i10I", raw_log[i*44:(i+1)*44])
                gap_us = (t_blk - t_rx) / 24.0 if t_blk >= t_rx else ((t_blk + 0x100000000 - t_rx) / 24.0)
                print(f"      [{i+1}] pb_fill_before={fb}, pb_need={need}, pb_read={rdi}, pb_write={wr}")
                print(f"          EP3 RX pkts={rx_pkts}, last bytes={rx_bytes}, USB frame/SOF={sof}")
                print(f"          t_last_rx={t_rx}, t_block={t_blk} (time since last pkt: {gap_us:.1f} us)")
                print(f"          max sie_lock={sie_us} us, max irq_off={irq_us} us")
        except Exception as ex:
            print(f"      (could not read starvation entries: {ex})")
    hist = [d(58 + i) for i in range(6)]
    print(f"UNIT service: timer interrupts {d(32)}, served {d(33)}, skipped (main loop in poll) {d(34)}, by interrupt {d(40)}, by main loop {d(41)}, longest interrupt {e[35] / 24.0:.0f} us")
    print(f"UNIT gap between timer interrupts (us) <350/<500/<750/<1000/<1500/>=1500: {hist}")
    if e[14] > b[14]:                                  # 1.05+: the gaps over 1.2 ms between timer interrupts, against the audio interrupt
        print(f"UNIT timer interrupt held off >= 1.2 ms: {e[14] - b[14]} times in the run, longest ever {e[15]} us; last four:")
        for i in range(min(4, e[14])):
            gap_raw, w = e[16 + 2 * i], e[17 + 2 * i]
            end, start = w & 0xFFFF, (w >> 16) & 0x7FFF
            print(f"        gap {gap_raw & 0xFFFF} us, audio breadcrumb {(gap_raw >> 16) & 0xFF:#04x}; audio interrupt entered {end} us before the gap ended, {'and was still running' if w >> 31 else 'and had finished'}; the gap began {start if start < 0x4000 else start - 0x8000} us after its entry")
    print(f"UNIT frame gaps (1 / 2 / 3 / 4+ host frames between packets): {d(8)} / {d(9)} / {d(10)} / {d(11)}")

    want_fpp = a.rate / 1000.0
    crit = [
        ("stream ran in real time (97-103 %)", 0.97 <= state["frames"] / a.rate / wall <= 1.03),
        (f"unit served {a.rate} Hz (frames/packet {want_fpp:.0f} +-0.5)", abs(fpp - want_fpp) <= 0.5),
        ("no PC callback over/underflow flags", len(state["flags"]) == 0),
        ("capture ring re-centres + starvations <= 1 in the run (the one at stream start)", d(51) + d(52) <= 1),
        ("playback ring re-centres + starvations <= 1", a.record_only or d(47) + d(48) <= 1),
    ]
    if not a.record_only:
        crit += [("worst 10 ms window tone/error >= 35 dB", snr.min() >= 35.0), ("level steady within 1 dB", lvl.max() - lvl.min() <= 1.0)]
    crit.append(("host frames missed <= 0.1 % of packets", d(12) <= 0.001 * max(1, pk)))
    print()
    for name, ok in crit:
        print(("  PASS  " if ok else "  FAIL  ") + name)
    ok_all = all(ok for _, ok in crit)
    print("RESULT:", "ALL CRITERIA MET" if ok_all else "FAILED")
    return 0 if ok_all else 1


if __name__ == "__main__":
    sys.exit(main())
