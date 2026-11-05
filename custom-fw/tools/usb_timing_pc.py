#!/usr/bin/env python3
"""
custom-fw/tools/usb_timing_pc.py - the USB 1 ms service experiment (pack 159, USB-AUDIO-PLAN.md step B): is the recording endpoint served better by a 2 kHz timer interrupt than by the main loop?

For each mode in turn (MIDI CC 91 = main loop only, CC 90 = the timer interrupt also serves the endpoint; the mode is reset by every reboot), --runs times: record from the unit's microphone for
--seconds with usb_rec_check_pc.py while this script reads the unit's counters (SRAM 0x01C73F00, usb_dev.c) and the audio side (second core, governor, block time, overruns) over the diag channel,
and prints the difference between the counters before and after. What matters:
    host frames with no packet (counter 12) per second of streaming      the true miss rate, independent of Windows
    packets queued per second                                            1000 is a perfect service
    the PC check's share of the wall clock                               97-103 % is the gate
    main-loop poll gaps (histogram)                                      why the main loop cannot do it
    the timer interrupt's cost (longest and average, us) and the audio block time and overruns while it runs   what the service costs the amp

    python custom-fw/tools/usb_timing_pc.py [--seconds 10] [--runs 3]          it switches the service mode by MIDI; nothing is stored
"""
import argparse, struct, subprocess, sys, time
from pathlib import Path
sys.path.insert(0, str(Path(__file__).resolve().parent))
import ble_matrix_pc as m

ISOT = 0x01C73F00

def isot(u):
    return struct.unpack("<64I", b"".join(u.ram(ISOT + 64 * i, 64) for i in range(4)))

def audio(u):
    w = struct.unpack("<8I", u.ram(0x01C7D200, 32)); g = struct.unpack("<I", u.ram(0x01C7D4C0, 4))[0]; s = struct.unpack("<6I", u.ram(0x01C7C250, 24))
    return dict(on=w[1], level=g & 0xFF, avg=g >> 8, over=s[3], entries=s[0])

def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--seconds", type=int, default=10); ap.add_argument("--runs", type=int, default=3)
    a = ap.parse_args()
    u = m.Unit("Custom MIDI")
    print("BUILD", u.build_id(), flush=True)
    if isot(u)[0] != 0x544F5349:
        print("no ISOT counters: this build is older than pack 159"); return 2
    labels = ["< .5", "< 1", "< 1.5", "< 2", "< 3", "< 5", "< 10", ">= 10"]
    rows = []
    for mode in (0, 1, 0, 1):
        u.mo.send_message([0xB0, 90 if mode else 91, 127]); time.sleep(0.5)
        for r in range(a.runs):
            b = isot(u); ab = audio(u)
            p = subprocess.Popen([sys.executable, str(Path(__file__).resolve().parent / "usb_rec_check_pc.py"), "--seconds", str(a.seconds)], stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True)
            t0 = time.time(); mid = []
            while p.poll() is None:
                time.sleep(1.0)
                try: mid.append(audio(u))
                except Exception: pass
            out = p.communicate()[0].strip().splitlines()
            e = isot(u); ae = audio(u)
            d = lambda k: e[k] - b[k]
            pk, miss = d(2), d(12)
            dur = max(1.0, time.time() - t0)
            share = next((l.split("(")[1].split(" %")[0] for l in out if "of the wall clock" in l), "?")
            gaps = [e[16 + i] - b[16 + i] for i in range(8)]
            isr_n, isr_sum = d(33), e[36] - b[36]
            row = dict(mode=mode, pk=pk, miss=miss, under=d(4), wait=d(5), share=share, gaps=gaps, isr_n=isr_n, isr_avg=(isr_sum / max(1, isr_n)) / 24.0, isr_max=e[35] / 24.0,
                       by_isr=d(40), by_main=d(41), skipped=d(34), avg=max([x["avg"] for x in mid] or [0]), over=ae["over"] - ab["over"], core1=ae["on"], level=ae["level"], dur=dur)
            rows.append(row)
            print(f"mode {mode} run {r + 1}: PC check {share} % of wall clock | packets/s {pk / dur:6.0f}  host frames with no packet {miss:5d} ({miss / dur:5.0f}/s)  underruns {row['under']}  "
                  f"| by interrupt {row['by_isr']} by main {row['by_main']} skipped {row['skipped']} | interrupt avg {row['isr_avg']:.1f} us max {row['isr_max']:.0f} us "
                  f"| audio: core1 {'on' if row['core1'] else 'OFF'} L{row['level']} worst avg block {row['avg']} us overruns +{row['over']}", flush=True)
            print("    main-loop poll gaps (ms): " + "  ".join(f"{l}:{g}" for l, g in zip(labels, gaps)), flush=True)
    u.mo.send_message([0xB0, 91, 127])
    print("\nSUMMARY (mode 0 = main loop only, 1 = timer interrupt also)")
    for mode in (0, 1):
        rs = [r for r in rows if r["mode"] == mode]
        if rs:
            shares = ", ".join(r["share"] for r in rs)
            pps = ", ".join("%.0f" % (r["pk"] / r["dur"]) for r in rs)
            mps = ", ".join("%.0f" % (r["miss"] / r["dur"]) for r in rs)
            print("  mode %d: PC check %s %% | packets/s %s | frames with no packet per s %s" % (mode, shares, pps, mps))
    return 0

if __name__ == "__main__":
    sys.exit(main())
