#!/usr/bin/env python3
"""
custom-fw/tools/enter_download_mode.py - find out where the STOCK firmware's
"enter upgrade" command takes the device (TEST-PROTOCOL.md section 4.3, route
"stock_enter_upgrade").

This is an EXPERIMENT, not a recovery tool. It answers one question: when a stock
Pocket Amp receives the frame sent to start an update, what does it turn
into on USB?

  * a "UBOOT" mass-storage-style device  -> the JieLi ROM download mode.
    jl-uboot-tool can then READ the whole flash over USB (no opening the unit).
  * a 4D4A:4155 MIDI/HID device           -> JieLi's OTA loader, which waits for an
    image to be served over USB MIDI. Not usable for dumping.
  * nothing new                           -> the frame was ignored (the stock may need
    identification handshake query first).

The script writes nothing to the device. It sends two frames:
  1. the OTA query  F0 00 32 45 00 00 00 40 7F F7  (a read: the device returns its ID)
  2. "enter upgrade" F0 22 24 35 7F F7              (only with --enter --i-understand)
     The FM-1 (same AC791N, same chip key 0x980F) is verified to drop into the ROM
     download mode on F0 22 24 35 7D F7: it leaves the bus ~21 ms later and comes back
     as 4C4A:8057 "WL82 UBOOT1.00" ~1 s after that. --frame 7D sends that variant.

What (2) can do, honestly: the stock firmware may store an "update pending" flag in its
own settings before it reboots, and the device may then come back in upgrade mode on
every power-up until an update completes. The way out is the stock firmware updater with the stock
"Pocket AMP.fwsc" (stock over stock). Have that ready before you use --enter, and only
do it on a unit with a stock firmware you can re-flash.

    python custom-fw/tools/enter_download_mode.py --list
    python custom-fw/tools/enter_download_mode.py --query
    python custom-fw/tools/enter_download_mode.py --enter --i-understand --wait 30 --jl-tool <path to jl-uboot-tool>

Windows only (it watches Windows Plug and Play). Needs python-rtmidi.
"""
from __future__ import annotations

import argparse
import datetime as dt
import json
import subprocess
import sys
import time
from pathlib import Path

TOOLS = Path(__file__).resolve().parent
FW = TOOLS.parent
ROOT = FW.parent
REPORTS = FW / "qa" / "reports"

OTA_QUERY = [0xF0, 0x00, 0x32, 0x45, 0x00, 0x00, 0x00, 0x40, 0x7F, 0xF7]
OTA_ENTER = {"7F": [0xF0, 0x22, 0x24, 0x35, 0x7F, 0xF7],   # OTA upgrade frame
             "7D": [0xF0, 0x22, 0x24, 0x35, 0x7D, 0xF7]}   # verified on the FM-1 (lunar-modulator)
UBOOT_VIDPID = "VID_4C4A&PID_8057"                          # WL82 UBOOT1.00 (FM-1, and the Pocket Amp via the Transporter)
NAME_HINTS = ("pocket", "m-vave", "mvave", "jieli", "jl ", "uboot", "ac79")
JL_VIDS = ("VID_4D4A", "VID_4C4A", "VID_3654")   # 3654:7756 = Pocket Amp app from V1.05


# ---- USB devices (Windows PnP) ---------------------------------------------------------
def pnp_devices() -> dict[str, dict]:
    ps = ("Get-PnpDevice -PresentOnly | Select-Object InstanceId,FriendlyName,Class,Status | "
          "ConvertTo-Json -Compress")
    p = subprocess.run(["powershell", "-NoProfile", "-Command", ps], capture_output=True, timeout=60)
    try:
        data = json.loads(p.stdout.decode("utf-8", errors="replace") or "[]")
    except ValueError:
        return {}
    if isinstance(data, dict):
        data = [data]
    return {d["InstanceId"]: d for d in data if d.get("InstanceId")}


def interesting(d: dict) -> bool:
    iid = (d.get("InstanceId") or "").upper()
    name = (d.get("FriendlyName") or "").lower()
    return any(v in iid for v in JL_VIDS) or any(h in name for h in NAME_HINTS)


def describe(d: dict) -> str:
    iid = d.get("InstanceId") or ""
    vidpid = ""
    up = iid.upper()
    if "VID_" in up and "PID_" in up:
        vidpid = up.split("VID_")[1][:4] + ":" + up.split("PID_")[1][:4]
    return f"{d.get('Class') or '?':14} {vidpid:10} {d.get('FriendlyName') or '-'}   [{iid}]"


def classify(new: list[dict]) -> str:
    text = " ".join(((d.get("FriendlyName") or "") + " " + (d.get("InstanceId") or "")).upper() for d in new)
    classes = {(d.get("Class") or "").lower() for d in new}
    if "UBOOT" in text or UBOOT_VIDPID in text or ("diskdrive" in classes and any(v in text for v in JL_VIDS)):
        return "rom_download"
    if "VID_4D4A&PID_4155" in text:
        return "ota_loader"
    if new:
        return "unknown"
    return "nothing"


# ---- MIDI ------------------------------------------------------------------------------
def midi_ports():
    import rtmidi
    return rtmidi.MidiOut().get_ports(), rtmidi.MidiIn().get_ports()


def pick(ports: list[str], want: str | None) -> str | None:
    if want:
        for p in ports:
            if want.lower() in p.lower():
                return p
        return None
    for p in ports:
        if any(h in p.lower() for h in ("pocket", "m-vave", "mvave")):
            return p
    return None


def send_and_listen(frame: list[int], port_hint: str | None, seconds: float) -> tuple[str | None, list[bytes]]:
    import rtmidi
    outs, ins = midi_ports()
    out_name = pick(outs, port_hint)
    if out_name is None:
        raise SystemExit("no Pocket Amp MIDI output found. Ports here: " + (", ".join(outs) or "none")
                         + "\nPass --port <part of the name>.")
    got: list[bytes] = []
    mi = None
    in_name = pick(ins, port_hint)
    if in_name:                         # open the input BEFORE sending, so no reply is missed
        mi = rtmidi.MidiIn()
        mi.ignore_types(sysex=False, timing=True, active_sense=True)
        mi.open_port(ins.index(in_name))
        mi.set_callback(lambda msg, _d: got.append(bytes(msg[0])))
    mo = rtmidi.MidiOut()
    mo.open_port(outs.index(out_name))
    mo.send_message(frame)
    time.sleep(seconds)
    for h in (mi, mo):
        try:
            if h:
                h.close_port()
        except Exception:
            pass
    return out_name, got


def show_reply(replies: list[bytes]) -> dict:
    if not replies:
        print("  no reply")
        return {}
    r = replies[0]
    print(f"  reply ({len(r)} B): {r.hex(' ')}")
    text = "".join(chr(b) if 32 <= b < 127 else "." for b in r)
    print(f"  text: {text}")
    return {"reply_hex": r.hex(), "reply_text": text}


# ---- main ------------------------------------------------------------------------------
def main() -> int:
    ap = argparse.ArgumentParser(description="Find where the stock 'enter upgrade' command takes the device")
    ap.add_argument("--list", action="store_true", help="list MIDI ports and JieLi-looking USB devices")
    ap.add_argument("--query", action="store_true", help="send the OTA query (a read) and print the ID block")
    ap.add_argument("--enter", action="store_true", help="send 'enter upgrade' and watch what appears")
    ap.add_argument("--i-understand", action="store_true",
                    help="required with --enter: the device may stay in upgrade mode until re-flashed with stock")
    ap.add_argument("--port", help="part of the MIDI port name (default: the first 'Pocket'/'M-VAVE' port)")
    ap.add_argument("--frame", choices=sorted(OTA_ENTER), default="7D",
                    help="which 'enter upgrade' variant to send (7D is the FM-1's verified soft key)")
    ap.add_argument("--wait", type=float, default=30.0, help="seconds to watch USB after --enter")
    ap.add_argument("--jl-tool", help="path to jl-uboot-tool, to run its jldevfind.py if a ROM device appears")
    a = ap.parse_args()

    if sys.platform != "win32":
        print("this script watches Windows Plug and Play; on Linux use 'lsusb' before and after instead")
        return 2
    if not (a.list or a.query or a.enter):
        ap.print_help()
        return 2

    if a.list:
        outs, ins = midi_ports()
        print("MIDI outputs:", ", ".join(outs) or "none")
        print("MIDI inputs: ", ", ".join(ins) or "none")
        print("\nUSB devices that look like JieLi / Pocket Amp:")
        for d in pnp_devices().values():
            if interesting(d):
                print("  " + describe(d))
        return 0

    if a.query or a.enter:
        print("OTA query (read only):")
        port, replies = send_and_listen(OTA_QUERY, a.port, 1.5)
        print(f"  port: {port}")
        info = show_reply(replies)
        if a.query and not a.enter:
            return 0 if replies else 1

    if a.enter and not a.i_understand:
        print("\n--enter needs --i-understand. Before you add it:\n"
              "  * the unit runs STOCK firmware and you have 'Pocket AMP.fwsc' ready;\n"
              "  * battery charged, USB straight into the PC, no hub;\n"
              "  * if it then comes back in upgrade mode on every power-up, re-flash with the stock .fwsc.")
        return 2

    before = pnp_devices()
    print(f"\nsending 'enter upgrade' ({a.frame} variant) on {port} ...")
    _, replies2 = send_and_listen(OTA_ENTER[a.frame], a.port, 0.5)
    seen: dict[str, dict] = {}
    gone: set[str] = set()
    t_end = time.time() + a.wait
    while time.time() < t_end:
        now = pnp_devices()
        for k, d in now.items():
            if k not in before and k not in seen:
                seen[k] = d
                print(f"  +{time.time() - (t_end - a.wait):5.1f}s  appeared: {describe(d)}")
        for k in before:
            if k not in now and k not in gone and interesting(before[k]):
                gone.add(k)
                print(f"  +{time.time() - (t_end - a.wait):5.1f}s  went away: {describe(before[k])}")
        time.sleep(1.0)

    kind = classify(list(seen.values()))
    verdict = {
        "rom_download": "ROM download mode: jl-uboot-tool can READ the flash. Use only 'read' until two "
                        "identical dumps are stored (TEST-PROTOCOL.md 4.2).",
        "ota_loader": "JieLi OTA loader (4D4A:4155): it waits for an image over USB MIDI. Not usable for "
                      "dumping. Power-cycle; if it stays in this mode, re-flash with the stock .fwsc.",
        "unknown": "Something new appeared, but not a known JieLi mode. Look at the list above.",
        "nothing": "Nothing new appeared: the stock ignored the frame (it may need updater's "
                   "authentication first). Power-cycle and try the other --frame variant before "
                   "marking the route 'failed'.",
    }[kind]
    print(f"\nresult: {kind}\n  {verdict}")

    if kind == "rom_download" and a.jl_tool:
        tool = Path(a.jl_tool)
        finder = tool / "jldevfind.py" if tool.is_dir() else tool
        print(f"\nrunning {finder} ...")
        subprocess.run([sys.executable, str(finder)], cwd=str(finder.parent))

    REPORTS.mkdir(parents=True, exist_ok=True)
    stamp = dt.datetime.now().strftime("%Y%m%d-%H%M%S")
    out = REPORTS / f"download_mode_{stamp}.json"
    out.write_text(json.dumps({
        "date": stamp, "port": port, "frame": a.frame, "query": info, "enter_replies": [r.hex() for r in replies2],
        "appeared": list(seen.values()), "went_away": [before[k] for k in gone], "result": kind,
    }, indent=2) + "\n", encoding="utf-8")
    print(f"\nevidence: {out.relative_to(ROOT)}")
    status = "reachable" if kind == "rom_download" else "failed"
    vp = next((describe(d).split()[1] for d in seen.values() if "VID_" in (d.get("InstanceId") or "").upper()), "")
    print("record it:\n  python custom-fw/tools/qa_recovery.py route stock_enter_upgrade "
          f"--status {status} --evidence {out.relative_to(ROOT).as_posix()}" + (f" --vid-pid {vp}" if vp else ""))
    print("  ('reachable' means the mode can be entered; the route becomes 'proven' only after a full restore drill)")
    return 0


if __name__ == "__main__":
    sys.exit(main())

# SPDX-License-Identifier: GPL-3.0-only
# Copyright (C) 2026 The Onyx Authors (@28spawn)
