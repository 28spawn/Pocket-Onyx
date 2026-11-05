# The emulator (`custom-fw/emu`)

The emulator runs the **real firmware** — the same `pa_audio_core_tick()`, the
same input layer, the same DSP — compiled into a native DLL that a host drives
over USB-MIDI-shaped APIs. The browser workbench (`custom-fw/wasm`) is the
same thing in WASM.

```
powershell -ExecutionPolicy Bypass -File custom-fw\emu\run_emu.ps1
```

builds the DLL, starts the ASIO host and serves the UI at <http://localhost:8080>.
Connected ASIO devices are picked up automatically; without one the emulator falls back
to `[Safe Offline] Simulation Only`.

---

## The front-panel buttons

The firmware's input layer (`src/sys/pa_input.c`) turns the **raw level** of
the front-panel buttons into one-tick action pulses:

* the four instant buttons (VOL-, VOL+, `<`, `>`) fire on the **press edge**,
  and repeat after 350 ms while they are held;
* **PARA and HOME fire their CLICK on the RELEASE edge** — a quick press is a
  click, a long one is a hold, and the two are mutually exclusive;
* the HOLD durations are read at the release too: PARA 1 s → "Save to",
  PARA 2 s → "Move block", HOME 1.5 s →
  "Live Stomp Lock";
* a chord consumes the buttons it is made of, so they do not also click.

**A button that is never released never clicks and never holds.** That is why
the emulator has a press/release API instead of a single level setter.

### The API

| call | what it does |
|---|---|
| `emu_ui_tap(mask, hold_ms)` | a **complete gesture**: press, hold, release. One call = one gesture. |
| `emu_ui_press(mask)` | the buttons go down (for a mouse press-and-hold) |
| `emu_ui_release(mask)` | the buttons come up; a contact shorter than the firmware's 20 ms debounce is held to 60 ms so it is not lost |
| `emu_ui_enc(delta)` | the encoder, without touching the buttons |
| `emu_ui_pot(q12)` | the MASTER pot, without touching the buttons |
| `emu_ui_input(mask, enc, pot)` | the old **level** setter — kept for the host tests, which drive a press and a release themselves |

Over HTTP (`POST /api/input`):

```jsonc
{"tap": 1}                  // a tap of HOME
{"hold": 32, "ms": 1200}    // PARA held for 1.2 s
{"press": 1} / {"release": 1}
{"enc_delta": 1}
{"master_pot": 4095}
{"buttons": 1}              // the legacy level api
```

The web UI sends **one gesture per request** and queues the requests, so a
press and its release can never be reordered (the server is threaded) and a
HOLD's duration is enforced by the emulator on the firmware's clock — a
browser timer would be throttled in a background tab, and a 1 s hold could turn
into a 2 s hold and select the wrong action.

### The clock

The audio callback **is** the firmware's clock while the audio is running.
The idle path only folds the host clock in when the callback is *not* running.
When both did it, the firmware's clock ran at about twice the speed — every
timer, the auto-repeat and the HOLD thresholds included — and a button
contact could be swallowed whole before the firmware's debounce saw it.

---

## The level

Two level controls, and they are not the same thing:

| control | where | what it does |
|---|---|---|
| the **MASTER pot** (and the Settings OUTPUT LEVEL) | the front panel / the Settings menu | the master taper: **-40 dB at 0 % .. 0 dB at 100 %** (`pa_audio_core_master_q15`) |
| the **preset volume** (the home screen's VOL-/VOL+) | the home screen | a linear **0..100 %** gain inside the chain, stored per preset (`pa_fx_chain_set_preset_vol`) |

The emulator's MASTER pot starts at **full scale** — the firmware's factory
default. It used to start at 2048 (the middle of the 12-bit range), so the
emulator ran its master at 50 %, which on that taper is **twenty decibels**
of attenuation: the emulator was that much quieter than the browser build and
than the device, while its own UI showed the knob at 100 %.

`test_level.py` checks the pot, the law and the measured input → output gain,
so the emulator cannot quietly go quiet again.

## The tests

```
python custom-fw\emu\test_buttons.py   # the front panel: click, hold, chord, repeat
python custom-fw\emu\test_input.py     # the input model: one tap = one action
python custom-fw\emu\test_editor.py    # the effect window
python custom-fw\emu\test_ir.py        # every cabinet IR in the bank parses
python custom-fw\emu\test_level.py     # the MASTER pot and the output level
```

`test_buttons.py` drives the DLL through the same API the web UI uses and
asserts the effect the firmware documents — the screen it switches to.

---

## What the UI can do

| control | what it does |
|---|---|
| the 5 physical buttons | press/release, so holding them repeats like the real switch |
| the chord buttons | one complete gesture each |
| `🔒 STOMP LOCK` | HOME held 1.5 s |
| `Save (1s)` / `Move (2s)` / `Reset (3s)` | PARA held 1 / 2 / 3 s |
| `● CLICK` | a PARA click |
| the NAM / IR drop zones | load a capture or a cabinet into the running firmware |
| the keyboard | `H` HOME, `-`/`=` VOL-/VOL+, `←`/`→` cursor, `Enter` PARA, `T` tuner, `B` bank, `C` chain, `D` drums, `S` settings, `L` stomp lock, `M` move, `R` reset |

### The host API

`GET /api/state` returns the whole firmware state: the screen, the presets,
the slot types and parameters, the telemetry, the LCD framebuffer and the
scopes. `POST /api/sysex` feeds raw SysEx into the firmware's own parser, and
`POST /api/load_nam` / `POST /api/load_ir` load a capture or a cabinet.

`POST /api/ble_midi` feeds a **BLE-MIDI 1.0 packet** (hex) — the same entry
point a GATT write on the MIDI characteristic would use: the framing is
stripped by the firmware's own BLE-MIDI layer and the bytes land in the same
ring the USB MIDI path uses, so both links drive the device through one
protocol.
