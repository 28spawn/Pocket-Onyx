# DSP benchmark (runs from RAM, never touches flash)

Measures, on a real Pocket Amp, what the heavy DSP costs per sample, and whether the AC791N's
single-precision FPU makes it cheaper than the firmware's fixed-point (Q21/Q18) code.

| What | How |
|---|---|
| Core clock in the ROM mode | 64 dependent integer adds per loop (1 cycle each), timed with Timer4 (24 MHz crystal) |
| FPU | Float add and multiply latency (dependent chains), float add throughput (4 independent sums) |
| `base` | `pa_wavenet_step_sample()`, the firmware's code today |
| `fast` | Bit-exact rewrite: packed weights, 3 independent accumulators. Checked bit for bit on the device |
| `fast6` | 6 accumulators, reordered additions. Max difference reported |
| Biquads, Q21 | The amp's 4-band tone stack exactly as `pa_audio_core.c` runs it (the firmware's `q21_mul`/`sat32`) |
| Biquads, float | The same 4 filters in float, plus a run with denormal state (and whether the FPU flushes denormals to zero) |
| Cab, direct | A 1,024-tap IR through the firmware's partitioned Q18 convolution (`fx_cab_ir.c`'s loop, software `pa_hw_accel_fir_part64`) |
| Cab, FFT | The same IR through the float FFT convolution prototype (`fft_conv.c`); outputs compared with the direct path |
| GRU | The stock-amp engine, `nam_gru_run_block()`, with the matrix-vector work in software |
| DRV | Each of the 6 drive models: the firmware's own `fx_drive.c` (4x oversampled, ADAA on RAT / OCD / BIG MUFF), in 64-sample blocks |
| MOD | Each of the 7 mod types: the firmware's own `fx_mod.c`, through `run_stereo()` as the device runs it |

`hw_accel.c` is compiled in its software form (`PA_HOST_SIM`), and the build refuses an image
that contains the accelerator's register address. So the benchmark never touches the
unverified `0x01EE0400` accelerator: the GRU and cab numbers are what the device costs once
the accelerator has timed out and the firmware falls back to software.

Results come back over USB and, in a second short run, are drawn on the LCD.

## Run it

1. Put the unit in the **ROM download mode** (`UBOOT1.00`): the soft key on a stock unit
   (`tools/enter_download_mode.py --frame 7D`) or the USB_KEY dongle. See
   `TEST-PROTOCOL.md` section 4.
2. Build and run, from an Administrator terminal on Windows:
   ```bash
   python custom-fw/bench/build_bench.py
   ```
   ```bash
   python custom-fw/tools/run_bench.py
   ```
3. To put the measured DRV / MOD cost into the CPU meter's model (`include/pa_dsp_cost.h`,
   used by the device, the emulator and the WASM build alike), add `--update-cost`:
   ```bash
   python custom-fw/tools/run_bench.py --update-cost
   ```
   It writes the worst model / type per 64-sample block, rounded up to 500 cycles, and
   stamps the line with the date and the benchmark image's hash. It refuses if any DRV / MOD
   output was not finite. Only those two lines change.
4. Power-cycle the unit afterwards. It boots its normal firmware; nothing was written.

## Why it is safe

- It is uploaded to RAM at `0x01C02000`, where JieLi's own loaders run. It uses only the
  ROM's memory-write, memory-read and jump commands. The flash loader is **never** uploaded,
  so no flash command exists on the device while it runs.
- The upload is read back and compared before it runs.
- A tiny probe run measures the speed first. The real run is sized to about a second, well
  inside the ROM's 5 s USB command timeout, and the LCD is drawn in a separate run.
- If anything hangs, power-cycle: RAM is lost, the flash is unchanged.

## Reading the numbers

- **Budget:** 8,163 cycles per sample is **all** of one core at 360 MHz (44.1 kHz), before
  the cab, EQ, delay and reverb.
- Everything here runs from SRAM. In the firmware the code runs from XIP flash unless it is
  put in `.ram_text`, so the firmware will be at least this slow.
- Cycles per sample hardly depend on the clock the ROM mode happens to run at; the
  microseconds per sample do.
- DRV / MOD run over at most 1,024 samples (the image's RAM is 256 KB); every mod type
  and drive model runs the same code path at any knob setting, so mid-knob settings are
  timed. OCD and BIG MUFF call `logf` per 4x sample (their antialiasing), so they show
  what the toolchain's `libm` costs.
- `--update-cost` uses these SRAM numbers. If the firmware runs the effects from XIP flash,
  the real load is higher: the device's own CPU meter (`test_cpu_meter.c`) is the final word.

## Files

| File | |
|---|---|
| `bench_entry.S` | Entry (own 8 KB stack), timing loops |
| `bench_main.c` | The benchmark, result block, LCD output |
| `bench_dsp.c` | Biquads, cab and GRU measurements |
| `bench_fx.c/.h` | DRV per model, MOD per type (runs the firmware's `fx_drive.c` / `fx_mod.c`) |
| `fft_conv.c/.h` | The float FFT convolution prototype (uniformly partitioned overlap-save, 128-point real FFT) |
| `test_fft_conv.c` | Host proof that the prototype matches direct convolution and the firmware's Q18 cab (gate check U-05) |
| `bench.ld` | Links everything to SRAM at `0x01C02000` (about 255 KB of 256 KB used) |
| `build_bench.py` | Builds `out/bench.bin` + `out/bench.json` (addresses, sha256) |
| `../tools/run_bench.py` | Uploads, runs, reads back, reports, saves `qa/reports/bench_a2lite_*.json`; `--update-cost` writes `include/pa_dsp_cost.h` |
| `../test/test_wavenet_fast.c` | Host proof that `fast` is bit-exact (gate check U-04) |
