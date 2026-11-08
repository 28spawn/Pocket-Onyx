#!/usr/bin/env python3
"""
custom-fw/tools/watch_boot_buttons.py - Interactive USB PnP watcher for button boot testing.
Monitors JieLi / Pocket Amp VID/PIDs (app: 3654:7756 on V1.05, 4C4A:C755 on V1.03) and reports enumeration state changes in real time.
"""
import json
import subprocess
import sys
import time

PS_CMD = (
    "Get-PnpDevice -PresentOnly | "
    "Where-Object { $_.InstanceId -match 'VID_4C4A|VID_4D4A|VID_3654|UBOOT|AC79' } | "
    "Select-Object InstanceId, FriendlyName, Class, Status | "
    "ConvertTo-Json -Compress"
)

def get_devices():
    try:
        p = subprocess.run(
            ["powershell", "-NoProfile", "-Command", PS_CMD],
            capture_output=True,
            timeout=10,
            text=True
        )
        out = p.stdout.strip()
        if not out:
            return []
        data = json.loads(out)
        if isinstance(data, dict):
            return [data]
        return data
    except Exception as e:
        return []

def classify(devs):
    if not devs:
        return "DISCONNECTED", []
    text = " ".join(((d.get("FriendlyName") or "") + " " + (d.get("InstanceId") or "")).upper() for d in devs)
    
    if "8057" in text or "UBOOT" in text:
        return "UBOOT_ROM_DOWNLOAD", devs
    if "4155" in text:
        return "OTA_LOADER", devs
    if "C755" in text or "VID_3654" in text:
        return "NORMAL_APP_MODE", devs
    return "UNKNOWN_JIELI_MODE", devs

def main():
    print("=" * 65)
    print("  Pocket Amp Boot-Button Combination Monitor")
    print("=" * 65)
    print("Watching for USB device connect/disconnect events...\n")
    
    last_mode = None
    plug_cycle = 0
    
    while True:
        devs = get_devices()
        mode, cur_devs = classify(devs)
        
        if mode != last_mode:
            ts = time.strftime("%H:%M:%S")
            if mode == "DISCONNECTED":
                print(f"[{ts}] [DISCONNECTED] Device unplugged.")
                print("       -> Hold your target button (VOL-, PARA, HOME, etc.) and reconnect USB.\n")
            elif mode == "NORMAL_APP_MODE":
                plug_cycle += 1
                print(f"[{ts}] [CYCLE #{plug_cycle} CONNECTED] Normal Application Mode (VID:PID 3654:7756 on V1.05, 4C4A:C755 on V1.03)")
                for d in cur_devs:
                    print(f"       * {d.get('Class', '?'):10} {d.get('FriendlyName', '-')} [{d.get('InstanceId', '')}]")
                print("       Result: Standard Bootloader (did not enter DFU/ROM mode with this button).")
                print("       -> Unplug USB to test next button combination.\n")
            elif mode == "UBOOT_ROM_DOWNLOAD":
                plug_cycle += 1
                print(f"[{ts}] [CYCLE #{plug_cycle} CONNECTED] *** ROM DOWNLOAD MODE (UBOOT) DETECTED! ***")
                for d in cur_devs:
                    print(f"       * {d.get('Class', '?'):10} {d.get('FriendlyName', '-')} [{d.get('InstanceId', '')}]")
                print("       SUCCESS: Hardware button triggered ROM Download Mode!\n")
            elif mode == "OTA_LOADER":
                plug_cycle += 1
                print(f"[{ts}] [CYCLE #{plug_cycle} CONNECTED] *** OTA LOADER MODE DETECTED (VID:PID 4D4A:4155) ***")
                for d in cur_devs:
                    print(f"       * {d.get('Class', '?'):10} {d.get('FriendlyName', '-')} [{d.get('InstanceId', '')}]")
                print("       SUCCESS: Hardware button triggered OTA Loader Mode!\n")
            else:
                plug_cycle += 1
                print(f"[{ts}] [CYCLE #{plug_cycle} CONNECTED] Other JieLi device state: {mode}")
                for d in cur_devs:
                    print(f"       * {d.get('Class', '?'):10} {d.get('FriendlyName', '-')} [{d.get('InstanceId', '')}]")
                print("\n")
            
            last_mode = mode
            sys.stdout.flush()
            
        time.sleep(0.5)

if __name__ == "__main__":
    try:
        main()
    except KeyboardInterrupt:
        print("\nMonitor stopped.")

# SPDX-License-Identifier: GPL-3.0-only
# Copyright (C) 2026 The Onyx Authors (@28spawn)
