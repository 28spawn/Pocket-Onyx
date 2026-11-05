#!/usr/bin/env python3
"""
custom-fw/tools/usb_diag_trace_pc.py - when diagnostic replies stop, does the unit stop transmitting, or does the PC lose them?

The bench firmware (PA_USB_HEARTBEAT) sends a CC (B0 01..04 7F) every 500 ms by itself. This script sends one 8-byte RAM read per second and logs EVERYTHING that arrives, with times:
  - heartbeats keep arriving while replies stop  -> the unit transmits, the replies are lost in between (reply framing, or the host's sysex handling)
  - heartbeats stop together with the replies    -> the USB transmit path of the unit (or the host's polling of it) stalls

    python -u custom-fw/tools/usb_diag_trace_pc.py [--seconds 40] [--port "Custom MIDI"]

No flash, no download mode. Run it right after the unit boots (replug) so it starts from a clean state.
"""
import argparse
import queue
import struct
import sys
import time
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
import ble_matrix_pc as m  # noqa: E402


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--seconds", type=float, default=40.0)
    ap.add_argument("--port", default="Custom MIDI")
    ap.add_argument("--listen-only", action="store_true", help="send nothing: only count the unit's own heartbeats (isolates unit -> PC from PC -> unit)")
    ap.add_argument("--send", choices=["sysex", "cc"], default="sysex", help="cc: send a harmless short CC (B0 64 00) per second instead of the RAM-read sysex (isolates the host's long-message path)")
    a = ap.parse_args()
    import rtmidi
    mo, mi = rtmidi.MidiOut(), rtmidi.MidiIn()
    mi.ignore_types(sysex=False)
    oi = next((i for i, n in enumerate(mo.get_ports()) if a.port.lower() in n.lower()), None)
    ii = next((i for i, n in enumerate(mi.get_ports()) if a.port.lower() in n.lower()), None)
    if oi is None or ii is None:
        raise SystemExit(f"no MIDI port containing '{a.port}': {mo.get_ports()} / {mi.get_ports()}")
    mo.open_port(oi)
    mi.open_port(ii)
    q: "queue.Queue[tuple[float, list[int]]]" = queue.Queue()
    mi.set_callback(lambda ev, _d=None: q.put((time.time(), ev[0])))
    t0 = time.time()
    req = [0xF0] + list(m.pack_8to7(bytes([0x7D, 0x02]) + struct.pack("<IH", 0x01C7D5B0, 8))) + [0xF7]
    sent = replies = hb = other = send_fail = 0
    last_reply_t = last_hb_t = None
    log = []
    next_send = t0
    while time.time() - t0 < a.seconds:
        now = time.time()
        if now >= next_send and not a.listen_only:
            sent += 1
            next_send += 1.0
            try:
                mo.send_message(req if a.send == "sysex" else [0xB0, 100, 0])
                log.append((now - t0, "TX read #%d" % sent))
            except Exception as e:  # Windows refused the sysex: the request never left the PC
                send_fail += 1
                log.append((now - t0, f"TX read #{sent} FAILED in Windows: {e}"))
        try:
            t, msg = q.get(timeout=0.05)
        except queue.Empty:
            continue
        if msg and msg[0] == 0xF0:
            body = m.unpack_7to8(bytes(msg[1:-1])) if msg[-1] == 0xF7 else b""
            ok = len(body) >= 2 and body[0] == 0x7D and body[1] == 0x82
            replies += ok
            if ok:
                last_reply_t = t - t0
            log.append((t - t0, f"RX sysex len {len(msg)} {'reply' if ok else 'UNRECOGNISED' + ('' if msg[-1] == 0xF7 else ' (no F7)')}"))
            other += (not ok)
        elif msg and (msg[0] & 0xF0) == 0xB0:
            hb += 1
            last_hb_t = t - t0
            log.append((t - t0, f"RX CC {msg[1]} {msg[2]}"))
        else:
            other += 1
            log.append((t - t0, f"RX other {msg}"))
    for ts, line in log:
        print(f"{ts:7.2f}s  {line}")
    exp_hb = a.seconds / 0.5
    print()
    print(f"requests refused by Windows before leaving the PC: {send_fail} of {sent}")
    print(f"reads sent {sent}   replies {replies}   heartbeats {hb} (expected about {exp_hb:.0f})   other/unrecognised {other}")
    print(f"last reply at {last_reply_t if last_reply_t is None else round(last_reply_t, 2)} s   last heartbeat at {last_hb_t if last_hb_t is None else round(last_hb_t, 2)} s")
    if a.send == "cc":
        print("VERDICT (cc mode): " + ("heartbeats ran the whole time while short messages were sent: the stall is specific to sysex" if hb >= exp_hb * 0.9 else "heartbeats stopped even with short messages only: the unit's OUT/IN path stalls, not just sysex"))
    elif replies < sent * 0.9:
        if hb == 0:
            print("VERDICT: no heartbeat arrived at all: the unit's IN pipe was already dead or never started (not a result about replies)")
        elif last_hb_t is not None and (a.seconds - last_hb_t) > 3:
            print("VERDICT: heartbeats stopped as well: the unit's USB transmit path (or the host's polling of it) stalled")
        else:
            print("VERDICT: heartbeats kept arriving while replies were lost: the unit transmits, replies are lost between the firmware's reply and the host callback")
    else:
        print("VERDICT: replies arrive reliably in this run")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
