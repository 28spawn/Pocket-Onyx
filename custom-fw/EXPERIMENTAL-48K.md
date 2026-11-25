# EXPERIMENTAL: a 48 kHz-only firmware

Branch `experimental/48k-codec`. The default build is unchanged: 44.1 kHz (the codec runs at 44,118 Hz, the 24 MHz crystal / 544).

## Why
NAM captures are trained at 48 kHz. Run at 44.1 kHz, every dilated tap spans 8.8 % more time than it was trained for, so a capture's whole
frequency response slides down by 44.1 / 48 (about 1.5 semitones: a 3 kHz presence peak lands near 2.76 kHz). At 48 kHz they play as trained.

## Build
```
powershell -ExecutionPolicy Bypass -File custom-fw/build.ps1 -Rate48k
```
The package goes to `custom-fw/release/Onyx_<version>_48K_EXPERIMENTAL/`. The switch only changes the device image
(`-DPA_SAMPLE_RATE_HZ=48000`); the host suites and the effect matrix still test the 44.1 kHz code.

## What follows the rate
`include/pa_config.h` holds `PA_SAMPLE_RATE_HZ`, `PA_CODEC_RATE_HZ`, `PA_BLOCK_US` (1,333 us at 48 kHz), `PA_BLOCK_TICKS` and `PA_US_SCALE()`.
They drive: the effects' filter design (drive, dynamics, EQ, modulation, cabinet low / high cut, amp tone stack), the reverb's decay law and ambience size,
the slapback range and the delay time shown, the modulation rates (same Hz), the tuner's pitch and hold, the drum tempo, the USB resampler's codec rate,
and every CPU budget (the audio interrupt's overrun limit, the load and stereo governors, core 1's deadline, the WaveNet slow-block guard, the Performance page).
At 44.1 kHz every one of these is exactly the value it had before.

## The codec clock: fixed, found by a probe on the unit
An earlier build probed 128 clock settings at boot. On the unit (2026-11-25) 24 ran: `CLK_CON2` bits 8..11 = 0, 4, 8, 12 give 44,118 Hz (24 MHz / 544); 1, 5, 9, 13 give 48,077 Hz
(25 MHz / 520, 0.16 % above 48 kHz); `CON3` bits 2..4 only halve and quarter those. The firmware now writes source 1, div 0 (`pa_alnk_set_48k_clock`, `src/board/audio_alnk.c`): no probe, no 2.5 s boot delay.
Marker at SRAM 0x01C7DB40: `FX48`, source, div (`python custom-fw/tools/jl_ram_read.py raw 0x01C7DB40 16` in download mode).

## Risks and what is not done
- **The clock source write is unproven beyond this one unit.** Recovery is the usual one (download mode, flash a normal image).
- **The converter may not accept a new clock.** The probe measures the I2S frame rate the chip drives, not whether the external converter locks to it.
  Listen at a low level the first time.
- **44.1 kHz data plays 8.8 % high:** the stock amps (GRU models), the stock and user cabinet IRs and the drum samples were made at 44.1 kHz.
- **Fixed filter tables are not redesigned:** the fizz filter and the WaveNet voicing filters keep their 44.1 kHz coefficients, so their corners sit 8.8 % higher.
  The chorus / flanger / vibrato delay taps and the reverb's comb lengths are in samples, so they are 8.8 % shorter.
- **CPU:** the block is 1,333 us instead of 1,451 us, 8 % less time for the same work. The WaveNet engine (1.36-1.42 ms at 44.1 kHz) will not fit and
  will fall back to the GRU through its own guard; the load governor sheds the drive oversampling and the cabinet partitions sooner.
- **USB audio** keeps its 44.1 kHz descriptor and resamples to the codec; 48 kHz hosts match the codec exactly.
- Nothing here was run on a unit.
