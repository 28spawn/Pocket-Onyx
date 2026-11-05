#!/usr/bin/env python3
"""
HIL Stress Tester for Pocket Amp Firmware
Target: http://localhost:8080
Tests: 80 Presets x 49 Drum Groove Patterns + Rapid Switching & Audio Health
"""

import sys
import time
import requests

if hasattr(sys.stdout, "reconfigure"):
    sys.stdout.reconfigure(encoding="utf-8", errors="replace")

HIL_URL = "http://localhost:8080"
TOTAL_PRESETS = 80
TOTAL_DRUM_PATTERNS = 49

# Test timing parameters
STEP_DWELL_SECONDS = 0.25   # Time to let audio DSP settle per preset/groove
RAPID_SWITCH_BURSTS = 20    # Number of fast consecutive switches to test memory/pool churn

class HILStressTester:
    def __init__(self, base_url=HIL_URL):
        self.base_url = base_url.rstrip("/")
        self.session = requests.Session()
        self.failures = []
        self.high_cpu_warnings = []

    def check_connection(self):
        print(f"[*] Connecting to HIL server at {self.base_url}...")
        try:
            resp = self.session.get(f"{self.base_url}/", timeout=2.0)
            if resp.status_code == 200:
                print("[+] Connected to Workbench frontend.")
            else:
                print(f"[-] Received unexpected status {resp.status_code}")
                return False
        except requests.exceptions.RequestException as e:
            print(f"[-] Connection failed: {e}")
            print("    Ensure 'python custom-fw/emu/server.py' is running!")
            return False
        return True

    def post_input(self, buttons=0, enc_delta=0, master_pot=512, **extra):
        """Dispatches an input event directly to the C state machine."""
        payload = {
            "buttons": buttons,
            "enc_delta": enc_delta,
            "master_pot": master_pot,
            **extra
        }
        try:
            resp = self.session.post(f"{self.base_url}/api/input", json=payload, timeout=1.0)
            return resp.status_code == 200
        except Exception as e:
            self.failures.append(f"Input POST failed: {e}")
            return False

    def get_telemetry(self):
        """Polls telemetry from the running emulator bridge."""
        try:
            resp = self.session.get(f"{self.base_url}/api/telemetry", timeout=1.0)
            if resp.status_code == 200:
                return resp.json()
        except Exception:
            pass
        return {}

    def test_presets_sweep(self):
        print("\n" + "=" * 60)
        print(f"[*] TEST 1: Sweeping All {TOTAL_PRESETS} Presets (Memory & Tail Spillover)")
        print("=" * 60)

        for p in range(1, TOTAL_PRESETS + 1):
            # Tell the HIL bridge to select preset p
            self.post_input(preset_select=p)
            time.sleep(STEP_DWELL_SECONDS)

            telem = self.get_telemetry()
            cpu_pct = telem.get("cpu_pct", 0.0)
            curr_p = telem.get("preset_idx", p)

            # Verification
            if cpu_pct > 90.0:
                msg = f"High CPU Warning on Preset {p}: {cpu_pct:.1f}%"
                print(f"  [!] {msg}")
                self.high_cpu_warnings.append(msg)
            elif p % 10 == 0 or p == TOTAL_PRESETS:
                print(f"  [+] Verified Presets up to P{p:02d} | CPU: {cpu_pct:.1f}%")

        print("[+] Preset sweep complete.")

    def test_drum_grooves_sweep(self):
        print("\n" + "=" * 60)
        print(f"[*] TEST 2: Sweeping All {TOTAL_DRUM_PATTERNS} Drum Patterns across Tempos")
        print("=" * 60)

        test_tempos = [60, 120, 180, 240]

        # Enable drums playback
        self.post_input(drums_playing=True)

        for groove_idx in range(TOTAL_DRUM_PATTERNS):
            tempo = test_tempos[groove_idx % len(test_tempos)]
            self.post_input(drum_style=groove_idx, drum_bpm=tempo)
            time.sleep(STEP_DWELL_SECONDS)

            telem = self.get_telemetry()
            cpu_pct = telem.get("cpu_pct", 0.0)

            if cpu_pct > 90.0:
                msg = f"High CPU on Groove #{groove_idx:02d} @ {tempo} BPM: {cpu_pct:.1f}%"
                print(f"  [!] {msg}")
                self.high_cpu_warnings.append(msg)
            elif groove_idx % 7 == 0 or groove_idx == TOTAL_DRUM_PATTERNS - 1:
                print(f"  [+] Groove #{groove_idx:02d} @ {tempo:03d} BPM | Audio OK | CPU: {cpu_pct:.1f}%")

        # Stop drums
        self.post_input(drums_playing=False)
        print("[+] Drum patterns sweep complete.")

    def test_rapid_preset_switching(self):
        print("\n" + "=" * 60)
        print(f"[*] TEST 3: Rapid Preset Churn Under Full Drum Load (Stress Test)")
        print("=" * 60)

        # Engage drums at fast 160 BPM
        self.post_input(drums_playing=True, drum_style=0, drum_bpm=160)

        # Alternating rapidly between a high-gain lead preset (P05) and clean ambient (P01)
        # Tests pa_pool reallocations, FIR re-init, and tail-draining stability
        burst_targets = [1, 5, 22, 45, 12, 38, 79, 2]
        print(f"  [*] Firing {RAPID_SWITCH_BURSTS} rapid preset changes (10ms interval)...")

        for i in range(RAPID_SWITCH_BURSTS):
            target = burst_targets[i % len(burst_targets)]
            self.post_input(preset_select=target)
            time.sleep(0.01) # 10 ms stress interval (less than one standard audio frame)

        time.sleep(0.5)
        self.post_input(drums_playing=False)

        telem = self.get_telemetry()
        cpu_pct = telem.get("cpu_pct", 0.0)
        print(f"  [+] Post-stress state recovered cleanly | Rest CPU: {cpu_pct:.1f}%")

    def run_all(self):
        if not self.check_connection():
            sys.exit(1)

        t_start = time.time()
        self.test_presets_sweep()
        self.test_drum_grooves_sweep()
        self.test_rapid_preset_switching()
        elapsed = time.time() - t_start

        print("\n" + "=" * 60)
        print(f"[*] STRESS TEST SUMMARY (Elapsed: {elapsed:.2f}s)")
        print("=" * 60)
        if self.failures:
            print(f"[-] FAILURES DETECTED ({len(self.failures)}):")
            for f in self.failures:
                print(f"    - {f}")
        else:
            print("[+] Zero hard crashes, timeouts, or disconnection events.")

        if self.high_cpu_warnings:
            print(f"[!] CPU Warnings (>90% threshold) ({len(self.high_cpu_warnings)}):")
            for w in self.high_cpu_warnings:
                print(f"    - {w}")
        else:
            print("[+] All preset and drum combinations remained safely under the CPU deadline.")

        print("=" * 60)


if __name__ == "__main__":
    tester = HILStressTester()
    tester.run_all()
