# The effect matrix: how every effect is tested

This is the method behind `custom-fw/tools/stress_effects.py`. It exists so the
same test can be re-run after any change to the DSP, and so a failure can be
understood rather than guessed at.

```
python custom-fw/tools/stress_effects.py --dll custom-fw/emu/pocket_amp_emu.dll
python custom-fw/tools/stress_effects.py --types      # every model of every block
python custom-fw/tools\stress_effects.py --quick      # only the single-effect cases
```

Exit code 0 means every case passed.

---

## 1. What runs

The test drives the **real firmware** through the emulator DLL
(`custom-fw/emu/pocket_amp_emu.dll`). The DLL is the firmware's own
`pa_audio_core_process_half()` - the device's audio path, verbatim - so what is
measured here is what the device would produce. Nothing about the DSP is
re-implemented for the test.

Each case:

1. `emu_init()` - a clean chain (the emulator rebuilds everything, so no case
   inherits the previous case's delay lines or reverb tails).
2. The blocks to enable, their knob values, their models and the chain order
   are set.
3. A **warm-up** of a third of the signal is run and discarded.
4. The rest of the signal is measured.

The warm-up matters. The master volume slews, the preset volume glides, the DC
blockers fill up, and the delay and reverb tanks need to charge. Measuring
during that would read the transient, not the effect.

## 2. The signals

| Signal | Level | Why |
|---|---|---|
| sine, 220 Hz | -14 dBFS (0.2) | a steady tone: the level and the spectrum are easy to reason about |
| sine, 220 Hz | -6 dBFS (0.5) | the same tone louder, to see the non-linear stages work harder |
| `rock_riff.wav` | -6 dBFS peak | a real recording, with a real spectrum and real transients |

**The test signal is never DC.** A constant offset is removed over time by the
chain's DC blockers, so a later measurement window reads quieter than an
earlier one and a "the level changed" conclusion is drawn from nothing. (This
is not hypothetical: an early version of this test measured exactly that and
produced a false failure.)

**The noise signal is used for the DC measurements**, because white noise has
no DC and no periodicity, so the mean of any window is a clean reading of the
DC the chain produced.

## 3. The knobs

Each block is measured at a musical default - the values the UI would show for
a sound somebody would actually use:

| Block | Knobs (0..100) |
|---|---|
| DYN | THRESH 50, RATIO 50, ATTACK 50, LEVEL 100 |
| DRIVE | DRIVE 60, TONE 50, LEVEL 80 |
| AMP | GAIN 45, BASS 50, MID 50, TREBLE 55, PRES 50, MASTER 50 |
| CAB | LEVEL 100, LOWCUT 50, HICUT 50 |
| EQ | 100 Hz 50, 400 Hz 50, 1 kHz 50, 3 kHz 50, 8 kHz 50 |
| MOD | RATE 50, DEPTH 50, MIX 30, FDBK 10 |
| DLY | TIME 50, FDBK 15, MIX 10, TONE 50 |
| REV | DECAY 30, LOWCUT 45, TONE 50, MIX 10 |

The knob index is the DSP's own `set_param` id, so what the test sets is what
the device gets.

## 4. The cases

1. **Every effect on its own**, on all three signals.
2. **Every model of every block** (`--types`): 4 dynamics, 6 drives, 20 amps,
   20 cabinets, 7 modulators, 5 delays, 6 reverbs - 68 models in all.
3. **Every pair, in both orders** - 56 cases. A pair that works in one order
   and breaks in the other is exactly the kind of thing this is looking for.
4. **The whole chain, in five orders**, including the two that put the drive
   after the cabinet.
5. **The bypass check**: every block is switched off and on again, and the
   result is compared with the block that was never switched. A block that
   loses its settings when it is bypassed is a bug (see below).

## 5. What is measured, and what passes

### The sanity criteria

Every case has to pass all of these.

| Criterion | Value | Why |
|---|---|---|
| No NaN or infinity | 0 samples | a NaN in the chain is silent, and it propagates |
| Not silent | above -60 dBFS | the chain has to make a sound at all |
| Not too loud | at most +24 dB | a runaway gain is a fault, not a feature |
| Not clipping | at most 5 % of the samples at the rail | a little clipping is a sound; a lot is a fault |
| No DC offset | at most 2 % of the peak | DC is wasted headroom and a click when the output is connected |
| No runaway | the gain may not grow more than 4x | a feedback loop that keeps growing |
| The channels agree | at most 2 % apart | a mono input through a mono chain must come out mono |

**The runaway check is normalised by the input.** A recording that fades in
makes the output's level rise, and that is not a runaway. The test compares the
output's own trend with the input's.

**The channel check only applies to a mono chain.** MOD, DLY and REV are stereo
and legitimately make the two channels differ.

**The level the matrix measures at.** The measurement runs with the **MASTER
backed off 20 dB** (the pot at 2048 = 50 %, i.e. -20 dB on the firmware's
-40..0 dB taper). The matrix measures the *effects*, not the output stage's
overload handling: the chain ends in a limiter and a soft ceiling
(`pa_soft_ceiling_q21`), and the high-gain metal models drive that limiter
into its clamp. The reason is in the findings below.

This was accidental until recently. The emulator's MASTER pot used to start at
the middle of its range (2048), and the firmware's tick reads that pot every
tick, so the matrix silently ran 20 dB down whatever it asked for. The pot
now starts at full scale - the firmware's factory default, and what the
browser build does - and the matrix sets the level it measures at explicitly.

The level law and the output stage are measured on their own, in
`custom-fw/emu/test_level.py`.

### The behaviour criteria

A block that produces a perfectly clean but *unchanged* signal has failed.
Each effect has to do its job:

| Block | What has to be true | How it is checked |
|---|---|---|
| DRIVE | adds gain and distortion | the transfer function at 800 Hz is at least 1.5x, and the THD rises by at least 0.02 |
| AMP | adds gain | the transfer function at 400 Hz is at least 1.5x |
| CAB | filters the top and the bottom | at most 0.35x at 6.4 kHz, and at most 0.9x at 100 Hz |
| EQ | flat at the default | every band within 0.15x of unity |
| DYN | a noise gate closes on a quiet signal | a -46 dBFS sine must come out at least 12 dB quieter than it went in |
| MOD | moves the amplitude | the coefficient of variation of the per-window RMS is at least 0.02 |
| DLY | leaves a tail | the output must keep ringing for 0.6 s after the input stops |
| REV | leaves a tail | the same |

The transfer function is measured on **white noise**, at eight frequencies
from 100 Hz to 10 kHz, and is given as the output's magnitude over the bare
chain's at the same frequency. A single sine cannot show this: a 220 Hz tone
has no 8 kHz content to filter, so a cabinet "passes more 8 kHz than it
received" is true of any filter fed with a 220 Hz tone.

## 6. The order, and why it is the point

The chain order is not cosmetic. The eight blocks run in the order the user
chose, and each one changes what the next one sees:

* A **drive after the cabinet** runs on the cabinet's high-passed output, so
  the drive has far less to work with and distorts much less. It is not
  broken, but it sounds completely different.
* A **reverb before the amp** gets distorted by the amp.
* A **delay before the drive** smears the drive's attack.
* A **cabinet after the stereo blocks** sums L+R to mono, so the stereo image
  is gone. The chain does this deliberately (the comment in `fx_graph.c` says
  so), and the test measures the result rather than pretending it is not
  there.

The whole chain is measured in five orders, and the drive-after-cabinet order
is one of them, because that is the case that has been reported as sounding
wrong.

## 7. What the test found

Running the test found four real faults. All four are fixed, and the test now
passes clean.

| What | The fault | The fix |
|---|---|---|
| **The right channel had no global EQ** | The global 5-band EQ was applied to `buf_L` only, and the two channels shared one filter state. The global EQ is **on by default and is not flat** (+2, -1, 0, +3, -2 dB), so the right channel was audibly wrong on every preset: measured 16 % off the left at 100 Hz and 27 % at 3 kHz. | The EQ filters both channels, each with its own state (`pa_fx.h`, `fx_graph.c`). |
| **A bypass lost the block's settings** | Bypassing a block frees its state (the C7 SRAM reclaim). Switching it back on called `create()`, which restored the **factory defaults** - so the knobs and the model were silently reset. | The chain remembers every value pushed into a block and replays it when the state is created again (`pa_fx.h`, `fx_graph.c`). |
| **Some models left a DC offset on the output** | A non-linear stage rectifies. The AMP's asymmetric clipping and the REVERB's shimmer both turn a symmetric signal into one with a DC component, and the chain's DC blocker sits at its *input*, before the model, so it cannot remove what the model creates. "10ModMetal" left 8.4 % of the peak as DC. | A single-pole DC blocker at the very end of the output path, after the soft ceiling (`pa_audio_core.c`). |
| **The chain's state was not fully initialised** | `pa_fx_chain_init()` left the global-EQ fields unset. The chain is a global on the device (so `.bss` zeroes it) but a **stack local** in the host tests, where the EQ then ran on garbage coefficients. | `pa_fx_chain_init()` now initialises them, and `pa_fx_chain_set_global_eq()` masks the buffer index (`fx_graph.c`). |

### The finding that was not a fault

With the emulator's MASTER pot fixed (it used to start at the middle of its
range, which is -20 dB on the firmware's taper), the matrix for the first
time measured the chain at a level the device actually uses. Three of the
twenty factory amp models then failed the "not clipping" criterion:

| Model | the gain | at the rail | the crest factor |
|---|---|---|---|
| 3USMetal1 | +16.1 dB | 40.6 % | 1.11 |
| 9USMetal2 | +16.1 dB | 39.6 % | 1.11 |
| 10ModMetal | +16.2 dB | 42.7 % | 1.09 |

They are the high-gain **metal** amps, and they are at the rail at *any*
input level - at -26 dBFS they are still 23..36 % at the rail. That is their
own nonlinearity: a crest factor of 1.09..1.16 instead of the 1.41 of a sine
means the wave is already flat-topped **before** the output stage's limiter
sees it. The limiter is then doing exactly what it is there for.

So this is the amp's character, not a fault, and the matrix measures the
chain with 20 dB of headroom so that the other 161 cases are not drowned by
it. The level law and the output stage have their own test
(`custom-fw/emu/test_level.py`).

## 8. Re-running it

The test needs the emulator DLL:

```
powershell -ExecutionPolicy Bypass -File custom-fw/emu/build_dll.ps1
```

The full build (`custom-fw/build.ps1`) builds it too, along with the device
firmware and the WebAssembly build. The host suites are the other half of
the verification:

```
python custom-fw/emu/test_input.py  custom-fw/emu/pocket_amp_emu.dll
python custom-fw/emu/test_editor.py custom-fw/emu/pocket_amp_emu.dll
```

**A new effect must be added to the matrix in the same change that adds it**,
with its knob defaults in the table above and its behaviour criterion in the
table in section 5. A block with no criterion would pass whether it worked or
not, which is the failure mode this test exists to prevent.
