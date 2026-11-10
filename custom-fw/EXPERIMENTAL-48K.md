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

## The codec clock: found by a probe on the unit
No source in this tree says how this chip makes 48 kHz. In a 48 kHz build, before the audio starts, `pa_alnk_clock_probe` (`src/board/audio_alnk.c`) tries
every clock source (`CLK_CON2` bits 8..11, 0..15) with every value of ALNK0 `CON3` bits 2..4 (0..7): 128 settings, at most 20 ms each, the output stage (PA2) off.
It measures the frame rate of each from the receive channel's finished halves and TIMER4, keeps the setting nearest 48,000 Hz if it is within 0.5 %, and
otherwise restores the 44.1 kHz setting.

Read the result in download mode:
```
python custom-fw/tools/jl_ram_read.py raw 0x01C7DB40 192
```
| word | meaning |
|---|---|
| 0 | `CK48` (bytes 43 4B 34 38): the probe ran |
| 1 | settings tried (128) |
| 2 | settings that produced a clock |
| 3 | the chosen setting: source \| div << 4 \| ok << 8 (ok 1 = within 0.5 % of 48 kHz, 0 = the 44.1 kHz setting is back) |
| 4 | the chosen setting's measured rate, Hz |
| 5 | how long the probe took, ms |
| 6..47 | each setting that ran: source \| div << 4 \| rate << 8 |

Send those words back: if a setting is exact, it replaces the probe with a fixed value.

## Risks and what is not done
- **The probe writes clock registers whose other uses are not known.** A setting that stops something the boot needs would hang the unit before the audio
  starts. Recovery is the usual one (download mode, flash a normal image).
- **The converter may not accept a new clock.** The probe measures the I2S frame rate the chip drives, not whether the external converter locks to it.
  Listen at a low level the first time.
- **If no setting is within 0.5 %** (word 3 bit 8 = 0) the unit runs the 44.1 kHz clock with DSP written for 48 kHz: everything is 8.8 % low in pitch and
  time. Do not use such a build for playing.
- **44.1 kHz data plays 8.8 % high:** the stock amps (GRU models), the stock and user cabinet IRs and the drum samples were made at 44.1 kHz.
- **Fixed filter tables are not redesigned:** the fizz filter and the WaveNet voicing filters keep their 44.1 kHz coefficients, so their corners sit 8.8 % higher.
  The chorus / flanger / vibrato delay taps and the reverb's comb lengths are in samples, so they are 8.8 % shorter.
- **CPU:** the block is 1,333 us instead of 1,451 us, 8 % less time for the same work. The WaveNet engine (1.36-1.42 ms at 44.1 kHz) will not fit and
  will fall back to the GRU through its own guard; the load governor sheds the drive oversampling and the cabinet partitions sooner.
- **USB audio** keeps its 44.1 kHz descriptor and resamples to the codec; 48 kHz hosts match the codec exactly.
- Nothing here was run on a unit.
