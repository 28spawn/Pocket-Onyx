# Host verification tools

Build each one on a PC (gcc/clang) together with the firmware sources. `PA_HOST_SIM` replaces the
hardware, and the device audio path (`pa_audio_process_half`) runs unchanged:

    SRCS="src/board/main.c src/board/audio_alnk.c src/board/ui_app.c src/board/ui_5x7.c \
          src/board/usb_ble.c src/board/spi_nor.c src/board/gpio_scan.c src/sys/*.c src/fx/*.c \
          src/dsp/*.c src/nam/*.c"
    gcc -O2 -DPA_HOST_SIM=1 -DPA_EMU_DLL=1 -DPA_POOL_TEST=1 -Iinclude -Isrc/dsp tools/<tool>.c $SRCS -lm -o tool

| tool | purpose |
|---|---|
| preset_click_probe.c | `probe <from> <to> <sine dBFS>`: largest sample step at a preset switch vs. the normal step size |
| ui_screenshot_harness.c | renders screens into a 240x240 RGB565 framebuffer (`*.raw`) |

The DIs are 16-bit mono 44.1 kHz WAVs with a 44-byte header, e.g. `emu/samples/*.wav`.

## The drum tools (generate firmware sources)

| tool | purpose |
|---|---|
| `gen_drum_kit.py` | synthesises the 12 drum voices procedurally from oscillators, noise, and filters into `src/dsp/drum_kit.inc` |
| `gen_drum_patterns.py` | generates the 49 rhythm styles into `src/dsp/drum_patterns.inc` (48 steps to a bar) |

