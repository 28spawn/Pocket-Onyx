#!/usr/bin/env python3
"""
custom-fw/tools/run_bench.py - run the A2-Lite WaveNet benchmark on a real Pocket Amp,
from RAM, in the JieLi ROM download mode. The flash is never touched.

    python custom-fw/bench/build_bench.py                  # once: builds bench/out/bench.bin
    python custom-fw/tools/run_bench.py                     # unit in ROM download mode first
    python custom-fw/tools/run_bench.py --no-lcd            # results over USB only
    python custom-fw/tools/run_bench.py --check             # check the build, no device
    python custom-fw/tools/run_bench.py --update-cost       # also write the DRV / MOD cost
                                                            # into include/pa_dsp_cost.h

How it stays safe:
  * It uses only the ROM's own RAM commands (write memory, read memory, jump). It never
    uploads the flash loader, so no flash command exists on the device while it runs.
  * The unit must be in the ROM mode itself ("UBOOT1.00"), freshly entered: if a loader is
    already running at 0x01C02000 it refuses (overwriting running code would hang it).
  * The upload is read back and compared before anything runs.
  * A first, tiny run measures the speed; the real run is then sized to about one second,
    well inside the 5 s USB command timeout.
  * If the benchmark ever hangs, power-cycle the unit: RAM is lost, the flash is unchanged.

Get into the ROM download mode with the soft key (enter_download_mode.py --frame 7D) on a
stock unit, or with the USB_KEY dongle (TEST-PROTOCOL.md section 4).

Windows: run from an Administrator terminal (raw SCSI access).
"""
from __future__ import annotations

import argparse
import datetime as dt
import hashlib
import json
import os
import struct
import sys
from pathlib import Path

TOOLS = Path(__file__).resolve().parent
FW = TOOLS.parent
OUT = FW / "bench" / "out"
REPORTS = FW / "qa" / "reports"

BUDGET_CYCLES = 8163            # 360 MHz / 44.1 kHz
REF_MHZ = 360.0
TIMER_HZ = 24_000_000
BLOCK = 512
CFG_FMT = "<8s5I"               # bench_cfg_t
RES_FMT = "<8s" + "I" * 51      # bench_res_t (24 words + bench_dsp_res_t's 11 + bench_fx_res_t's 16)
RES_FIELDS = ["magic", "status", "timer_hz", "samples", "reps", "loop_iters", "insns_per_iter",
              "alu", "fadd_dep", "fmul_dep", "fadd_ind",
              "base_min", "fast_min", "fast6_min", "base_sum", "fast_sum", "fast6_sum",
              "fast_mismatches", "fast6_maxdiff_bits", "out_base", "out_fast", "out_fast6",
              "model_bytes", "lcd_drawn", "lcd_spi_timeouts",
              # bench_dsp_res_t
              "bq_q21", "bq_f", "bq_f_denorm", "ftz", "bq_maxdiff_bits",
              "cab_direct", "cab_fft", "cab_maxdiff_bits", "cab_taps", "gru", "gru_out_last",
              # bench_fx_res_t
              "drv0", "drv1", "drv2", "drv3", "drv4", "drv5",
              "mod0", "mod1", "mod2", "mod3", "mod4", "mod5", "mod6",
              "drv_out_last", "mod_out_last", "fx_nonfinite"]
MAGIC = "PABENCH3"
DRV_NAMES = ["TS-808", "KLON", "OCD", "RAT", "BIG MUFF", "FUZZ FACE"]
MOD_NAMES = ["CHORUS", "PHASER", "FLANGER", "TREMOLO", "VIBRATO", "UNIVIBE", "AUTO FILTER"]
COST_H = FW / "include" / "pa_dsp_cost.h"
# every timed test, as (result field, label); each is timed over the same number of samples
TIMED = [("base_min", "A2-Lite WaveNet, firmware code"), ("fast_min", "A2-Lite WaveNet, fast (bit-exact)"),
         ("fast6_min", "A2-Lite WaveNet, fast6"), ("bq_q21", "4 biquads, Q21 (amp tone stack)"),
         ("bq_f", "4 biquads, float"), ("bq_f_denorm", "4 biquads, float, denormal state"),
         ("cab_direct", "cab 1,024 taps, direct Q18 (firmware)"), ("cab_fft", "cab 1,024 taps, float FFT"),
         ("gru", "stock GRU engine (software matvec)")] + \
        [(f"drv{i}", f"DRV {n} (4x oversampled)") for i, n in enumerate(DRV_NAMES)] + \
        [(f"mod{i}", f"MOD {n} (stereo)") for i, n in enumerate(MOD_NAMES)]
STATUS = {0: "did not finish", 1: "done", 0xE001: "bad config block", 0xE002: "Timer4 does not count",
          0xE003: "weights refused", 0xE004: "weights did not pack", 0xE101: "sample count not a multiple of 64",
          0xE201: "fx: sample count not a multiple of 64", 0xE202: "fx: drive did not allocate",
          0xE203: "fx: mod did not allocate"}


def load_build() -> tuple[bytes, dict]:
    man_p, bin_p = OUT / "bench.json", OUT / "bench.bin"
    if not man_p.exists() or not bin_p.exists():
        sys.exit("no benchmark build - run: python custom-fw/bench/build_bench.py")
    man = json.loads(man_p.read_text())
    img = bin_p.read_bytes()
    if hashlib.sha256(img).hexdigest() != man["image_sha256"] or len(img) != man["image_size"]:
        sys.exit("bench.bin does not match bench.json - rebuild")
    if man["soft_float_helpers_linked"]:
        print(f"note: software float helpers linked: {man['soft_float_helpers_linked']} "
              "(the GRU's activation-table setup in activation.c uses double; it runs before timing)")
    return img, man


class RomRam:
    """The ROM's RAM commands, with the wl82 scrambling, and nothing else."""

    def __init__(self, jl_tool: Path, device: str | None):
        if not (jl_tool / "jltech" / "uboot.py").exists():
            sys.exit(f"jl-uboot-tool not found at {jl_tool} (--jl-tool or JL_UBOOT_TOOL)")
        sys.path.insert(0, str(jl_tool))
        import yaml
        from jltech.uboot import JL_MSCDevice, JL_UBOOT
        from jltech.cipher import cipher_bytes, jl_crc_cipher
        self._cipher = lambda b: cipher_bytes(jl_crc_cipher, b)
        chips = yaml.safe_load(open(jl_tool / "data" / "chips.yaml"))
        quirks = (chips.get("wl82", {}).get("uboot1.00", {}) or {}).get("quirks", {}) or {}
        self.scramble = quirks.get("memory-rw-mengli-crypt") is True     # JieLi ROM hardware memory scramble
        if device is None:
            from jldevfind import choose_jl_device
            device = choose_jl_device(venfilter="wl82")
            if device is None:
                sys.exit("no JieLi device in download mode found")
        self.msc = JL_MSCDevice(device)
        self.msc.open()
        vendor, product, _ = self.msc.inquiry()
        if vendor.strip().upper() != "WL82":
            self.close()
            sys.exit(f"the device says '{vendor.strip()}', not WL82 (AC791N)")
        if product.strip() != "UBOOT1.00":
            self.close()
            sys.exit(f"the device is in '{product.strip()}' mode, not the ROM's UBOOT1.00: a loader is "
                     "already running at the benchmark's address. Power-cycle back into download mode.")
        self.rom = JL_UBOOT(self.msc.dev)
        self.path = device

    def write(self, addr: int, data: bytes) -> None:
        for off in range(0, len(data), BLOCK):
            blk = data[off:off + BLOCK]
            self.rom.mem_write(addr + off, self._cipher(blk) if self.scramble else blk)

    def read(self, addr: int, n: int) -> bytes:
        out = bytearray()
        for off in range(0, n, BLOCK):
            k = min(BLOCK, n - off)
            blk = self.rom.mem_read(addr + off, k)
            out += self._cipher(blk) if self.scramble else blk
        return bytes(out)

    def jump(self, addr: int) -> None:
        self.rom.mem_jump(addr, 0)

    def close(self):
        try:
            self.msc.close()
        except Exception:
            pass


def run_once(dev: RomRam, man: dict, samples: int, reps: int, iters: int, lcd: bool) -> dict:
    """lcd=False: measure and store. lcd=True: only draw the stored results (a separate jump,
    so a slow LCD cannot push the measuring run past the 5 s command timeout)."""
    s = man["symbols"]
    cfg = struct.pack(CFG_FMT, b"PABCFG01", samples, reps, iters, 2 if lcd else 0, 12345)
    dev.write(s["g_bench_cfg"], cfg)
    if dev.read(s["g_bench_cfg"], len(cfg)) != cfg:
        raise RuntimeError("the config block did not read back - not running")
    try:
        dev.jump(s["bench_entry"])
    except Exception as e:  # noqa: BLE001
        raise RuntimeError(f"the benchmark did not return ({e}). Power-cycle the unit; the flash is untouched.")
    raw = dev.read(s["g_bench_res"], struct.calcsize(RES_FMT))
    res = dict(zip(RES_FIELDS, struct.unpack(RES_FMT, raw)))
    res["magic"] = res["magic"].decode("ascii", "replace")
    return res


def f32(bits: int) -> float:
    return struct.unpack("<f", struct.pack("<I", bits))[0]


def analyse(r: dict) -> dict:
    if r["magic"] != MAGIC or r["status"] != 1:
        return {"ok": False, "status": STATUS.get(r["status"], hex(r["status"]))}
    sec_per_tick = 1.0 / TIMER_HZ
    alu_instr = r["loop_iters"] * r["insns_per_iter"]
    mhz = alu_instr / (r["alu"] * sec_per_tick) / 1e6                 # 1 dependent add = 1 cycle
    cyc_per_tick = mhz * 1e6 / TIMER_HZ
    n_wn = r["samples"]
    n_dsp = (r["samples"] // 64) * 64 or 64
    a = {"ok": True, "core_mhz": round(mhz, 1),
         "fadd_latency_cycles": round(r["fadd_dep"] / r["alu"], 2),
         "fmul_latency_cycles": round(r["fmul_dep"] / r["alu"], 2),
         "fadd_cycles_4_independent": round(r["fadd_ind"] / r["alu"], 2),
         "ftz": bool(r["ftz"]), "tests": {}}
    for field, label in TIMED:
        n = n_wn if field in ("base_min", "fast_min", "fast6_min") else n_dsp
        if field.startswith(("drv", "mod")):
            n = min(n_dsp, 1024)                 # bench_fx_samples(): the fx runs cap at 1,024
        cps = r[field] * cyc_per_tick / n
        a["tests"][field] = {"label": label, "cycles_per_sample": round(cps),
                             "pct_of_budget_at_360mhz": round(100.0 * cps / BUDGET_CYCLES, 1)}
    for v in ("base", "fast", "fast6"):
        a["tests"][f"{v}_min"]["spread_pct"] = round(
            100.0 * (r[f"{v}_sum"] / r["reps"] - r[f"{v}_min"]) / r[f"{v}_min"], 1)
    t = a["tests"]
    a["fast_bit_exact"] = r["fast_mismatches"] == 0
    a["fast6_max_diff"] = f32(r["fast6_maxdiff_bits"])
    a["bq_q21_vs_float_max_diff"] = f32(r["bq_maxdiff_bits"])
    a["cab_direct_vs_fft_max_diff"] = f32(r["cab_maxdiff_bits"])
    a["speedups"] = {
        "wavenet_fast": round(r["base_min"] / r["fast_min"], 2),
        "wavenet_fast6": round(r["base_min"] / r["fast6_min"], 2),
        "biquads_float_vs_q21": round(r["bq_q21"] / r["bq_f"], 2),
        "denormal_slowdown": round(r["bq_f_denorm"] / r["bq_f"], 2),
        "cab_fft_vs_direct": round(r["cab_direct"] / r["cab_fft"], 2),
    }
    # the per-slot cost the firmware's CPU meter uses: the worst model / type, per 64-sample block
    drv = max(t[f"drv{i}"]["cycles_per_sample"] for i in range(6))
    mod = max(t[f"mod{i}"]["cycles_per_sample"] for i in range(7))
    a["slot_cost_per_block"] = {"DRIVE": drv * 64, "MOD": mod * 64}
    a["fx_nonfinite"] = r["fx_nonfinite"]
    return a


def update_cost(a: dict, sha: str) -> None:
    """Write the measured DRIVE / MOD block cost into include/pa_dsp_cost.h: the worst
    model / type, rounded up to 500 cycles. Everything else in the file is left alone."""
    import math
    import re
    with open(COST_H, encoding="utf-8", newline="") as f:      # keep its line endings
        src = f.read()
    for slot, idx in (("DRIVE", 1), ("MOD", 5)):
        v = int(math.ceil(a["slot_cost_per_block"][slot] / 500.0) * 500)
        pat = re.compile(rf"^(\s*)\d+u,(\s*/\*\s*{idx}\s+{slot}\b)[^\n]*$", re.M)
        new, k = pat.subn(lambda m: f"{m.group(1)}{v}u,{m.group(2)}  (device, {dt.date.today()}, "
                                    f"bench {sha[:8]}) */", src)
        if k != 1:
            sys.exit(f"could not find the {slot} entry in {COST_H}")
        src = new
        print(f"  pa_dsp_cost.h: {slot} = {v} cycles/block")
    with open(COST_H, "w", encoding="utf-8", newline="") as f:
        f.write(src)


def report(r: dict, a: dict) -> None:
    print(f"\n  core clock (ROM mode)  {a['core_mhz']} MHz   (from 64 dependent integer adds = 64 cycles)")
    print(f"  float add latency      {a['fadd_latency_cycles']} cycles")
    print(f"  float mul latency      {a['fmul_latency_cycles']} cycles")
    print(f"  float add, 4 sums      {a['fadd_cycles_4_independent']} cycles each")
    print(f"  denormals              {'flushed to zero (no slowdown possible)' if a['ftz'] else 'kept (see the denormal row)'}\n")
    print(f"  {'test':42} {'cycles/sample':>14} {'% of 1 core @360MHz':>21}")
    for d in a["tests"].values():
        print(f"  {d['label']:42} {d['cycles_per_sample']:>14,} {d['pct_of_budget_at_360mhz']:>20}%")
    s = a["speedups"]
    print(f"\n  WaveNet: fast x{s['wavenet_fast']} ({'bit-exact on the device' if a['fast_bit_exact'] else 'NOT bit-exact: ' + str(r['fast_mismatches']) + ' samples'}),"
          f" fast6 x{s['wavenet_fast6']} (max diff {a['fast6_max_diff']:.3g})")
    print(f"  biquads: float is x{s['biquads_float_vs_q21']} vs Q21 (outputs agree to {a['bq_q21_vs_float_max_diff']:.3g});"
          f" denormal state costs x{s['denormal_slowdown']}")
    print(f"  cab:     FFT is x{s['cab_fft_vs_direct']} vs direct (outputs agree to {a['cab_direct_vs_fft_max_diff']:.3g})")
    c = a["slot_cost_per_block"]
    print(f"  DRV/MOD: worst case {c['DRIVE']:,} / {c['MOD']:,} cycles per 64-sample block"
          f" (pa_dsp_cost.h: --update-cost); non-finite outputs: {a['fx_nonfinite']}")
    print(f"  budget:  {BUDGET_CYCLES} cycles/sample is ALL of one core at 360 MHz, for the whole chain")
    print("  note:    everything ran from SRAM; the flash firmware adds XIP fetch time on top")
    if r["lcd_drawn"] == 1:
        print(f"  LCD: drawn (SPI timeouts: {r['lcd_spi_timeouts']}; non-zero means the panel did not answer)")
    elif r["lcd_drawn"]:
        print(f"  LCD: not drawn (code {r['lcd_drawn']:#x})")


def main() -> int:
    ap = argparse.ArgumentParser(description="Run the RAM-resident A2-Lite benchmark (never touches flash)")
    ap.add_argument("--jl-tool", default=os.environ.get("JL_UBOOT_TOOL") or
                    str(Path.home() / "Downloads" / "jl-uboot-tool-main" / "jl-uboot-tool-main"))
    ap.add_argument("--device", help="download-mode disk, e.g. \\\\.\\E: (default: ask jldevfind)")
    ap.add_argument("--no-lcd", action="store_true", help="do not draw on the LCD")
    ap.add_argument("--target-seconds", type=float, default=1.0, help="length of the real run (max 2.5)")
    ap.add_argument("--check", action="store_true", help="check the build only; no device")
    ap.add_argument("--update-cost", action="store_true",
                    help="write the measured DRIVE / MOD cost into include/pa_dsp_cost.h")
    a = ap.parse_args()

    img, man = load_build()
    s = man["symbols"]
    print(f"benchmark image: {len(img):,} B at {man['load_address']:#x}, RAM up to {man['ram_end']:#x}")
    if a.check:
        print("build OK (no device used)")
        return 0
    target = min(max(a.target_seconds, 0.2), 2.5)

    dev = RomRam(Path(a.jl_tool), a.device)
    try:
        print(f"device: {dev.path} (WL82, UBOOT1.00), scrambled transfers: {dev.scramble}")
        dev.write(man["load_address"], img)
        if dev.read(man["load_address"], len(img)) != img:
            print("the upload did not read back identically - not running it")
            return 1
        print("uploaded and verified")

        probe = run_once(dev, man, samples=64, reps=1, iters=200, lcd=False)
        pa = analyse(probe)
        if not pa["ok"]:
            print(f"probe run failed: {pa['status']}")
            return 1
        # every test runs reps times over the same samples: size the run on their sum
        sec_per_sample = sum(probe[f] for f, _ in TIMED) / TIMER_HZ / 64
        reps = 3
        samples = int(min(2048, max(64, target / (reps * sec_per_sample))))
        samples -= samples % 64
        iters = int(min(200000, max(500, 200 * (0.05 * TIMER_HZ) / max(probe["alu"], 1))))
        print(f"probe: ~{sec_per_sample * 1e6:.1f} us per sample for all tests together -> real run: "
              f"{samples} samples x {reps} reps")

        r = run_once(dev, man, samples=samples, reps=reps, iters=iters, lcd=False)
        an = analyse(r)
        if not an["ok"]:
            print(f"run failed: {an['status']}")
            return 1
        if not a.no_lcd:
            try:
                d = run_once(dev, man, samples=samples, reps=reps, iters=iters, lcd=True)
                r["lcd_drawn"], r["lcd_spi_timeouts"] = d["lcd_drawn"], d["lcd_spi_timeouts"]
            except RuntimeError as e:
                print(f"  LCD draw did not return ({e}); the results below are already safe")
        report(r, an)
        REPORTS.mkdir(parents=True, exist_ok=True)
        p = REPORTS / f"bench_a2lite_{dt.datetime.now():%Y%m%d-%H%M%S}.json"
        p.write_text(json.dumps({"image_sha256": man["image_sha256"], "raw": r, "result": an}, indent=2) + "\n")
        print(f"\n  saved: {p.relative_to(FW.parent)}")
        if an["fx_nonfinite"]:
            print("  DRV/MOD produced non-finite output on the device - pa_dsp_cost.h NOT updated")
            return 2
        if a.update_cost:
            update_cost(an, man["image_sha256"])
        return 0 if an["fast_bit_exact"] else 2
    except RuntimeError as e:
        print(f"\n{e}")
        return 1
    finally:
        dev.close()


if __name__ == "__main__":
    sys.exit(main())
