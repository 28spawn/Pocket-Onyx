# custom-fw/Makefile — Host Simulation & pi32v2 Cross-Compilation Build System
#
# Usage:
#   make test    — Build and run native PC verification suites
#   make cross   — Cross-compile all firmware objects for JieLi AC791N (pi32v2)
#   make fw      — Link app.elf, extract app.bin, and package Pocket_AMP_Custom.fwsc

BOARD ?= pocket_amp
BLE   ?= 0

# Only a ported board can be built (include/boards/). blackbox is not: built today it would be the Pocket AMP's firmware, wrong pins and wrong flash sectors on a BlackBox.
ifneq ($(BOARD),pocket_amp)
  $(error BOARD=$(BOARD) cannot be built: only pocket_amp is ported (blackbox is not yet, see include/boards/board_blackbox.h))
endif
BOARD_DEF = -DBOARD_POCKET_AMP=1
BLE_DEF = -DPA_BLE=$(BLE)

HOST_CC   ?= cc
HOST_CFLAGS = -O2 -Wall -Wextra -Iinclude -DPA_HOST_SIM=1 $(BOARD_DEF)

# Auto-detect JieLi toolchain on Windows or Linux
ifeq ($(OS),Windows_NT)
  JIELI_ROOT ?= $(or $(JL_TOOLCHAIN), $(HOME)/tools/jieli/C$$/JL/pi32)
  PI32_CC    ?= $(JIELI_ROOT)/bin/clang.exe
  PI32_LD    ?= $(JIELI_ROOT)/bin/pi32v2-ld.exe
  PI32_OC    ?= $(JIELI_ROOT)/bin/llvm-objcopy.exe
  PI32_INC   ?= $(JIELI_ROOT)/pi32v2-include
  PI32_LIB   ?= $(JIELI_ROOT)/pi32v2-lib/r3
  PYTHON     ?= python
else
  JIELI_ROOT ?= /opt/jieli/common
  PI32_CC    ?= $(JIELI_ROOT)/bin/clang
  PI32_LD    ?= $(JIELI_ROOT)/bin/pi32v2-ld
  PI32_OC    ?= $(JIELI_ROOT)/bin/objcopy
  PI32_INC   ?= $(JIELI_ROOT)/include
  PI32_LIB   ?= $(JIELI_ROOT)/lib
  PYTHON     ?= python3
endif

PI32_CFLAGS = -target pi32v2 -mcpu=r3 -O2 -Wall -ffreestanding -fno-builtin -isystem $(PI32_INC) -Iinclude -Isrc/board/ble_platform -Ithird_party/optimist_ble/src -Ithird_party/optimist_ble/hal $(BOARD_DEF) $(BLE_DEF)

DSP_SRCS = \
	src/dsp/activation.c \
	src/dsp/hw_accel.c \
	src/dsp/tuner.c \
	src/dsp/drums_synth.c \
	src/nam/nam_engine.c \
	src/nam/pa_wavenet.c

FX_SRCS = \
	src/dsp/pa_biquad.c \
	src/fx/fx_graph.c \
	src/fx/fx_dynamics.c \
	src/fx/fx_drive.c \
	src/fx/fx_cab_ir.c \
	src/fx/fx_eq.c \
	src/fx/fx_mod.c \
	src/fx/fx_delay.c \
	src/fx/fx_reverb.c

SYS_SRCS = \
	src/sys/cpu_meter.c \
	src/sys/preset.c \
	src/sys/settings.c \
	src/sys/sysex.c \
	src/sys/smp_cpu1.c \
	src/sys/pa_pool.c \
	src/sys/pa_user_nam.c \
	src/sys/stock_assets.c \
	src/sys/pa_audio_core.c \
	src/sys/pa_input.c \
	src/sys/pa_diag.c \
	src/sys/pa_ota.c

BOARD_SRCS = \
	src/board/entry.S \
	src/board/audio_alnk.c \
	src/board/gpio_scan.c \
	src/board/spi_nor.c \
	src/board/st7735s.c \
	src/board/ui_5x7.c \
	src/board/ui_app.c \
	src/board/usb_ble.c \
	src/board/ble_midi.c \
	src/board/ble_radio.c \
	src/board/main.c

ALL_FW_SRCS = $(DSP_SRCS) $(FX_SRCS) $(SYS_SRCS) $(BOARD_SRCS)

.PHONY: all test cross fw clean

all: test

test:
	$(HOST_CC) $(HOST_CFLAGS) test/test_reclaim.c $(FX_SRCS) src/dsp/hw_accel.c src/sys/pa_pool.c src/sys/cpu_meter.c -o build_test_reclaim
	./build_test_reclaim
	$(HOST_CC) $(HOST_CFLAGS) test/test_dsp_chain.c \
		$(DSP_SRCS) $(FX_SRCS) src/sys/pa_pool.c src/sys/cpu_meter.c -lm -o build_test_dsp_chain
	./build_test_dsp_chain
	$(HOST_CC) $(HOST_CFLAGS) test/test_full_firmware.c \
		$(DSP_SRCS) $(FX_SRCS) src/sys/cpu_meter.c src/sys/preset.c src/sys/settings.c \
		src/sys/pa_pool.c src/sys/pa_user_nam.c src/sys/stock_assets.c \
		src/sys/pa_audio_core.c src/sys/pa_input.c src/sys/pa_diag.c src/sys/pa_ota.c \
		src/board/spi_nor.c src/board/ui_app.c src/board/usb_ble.c src/board/ble_midi.c \
		-lm -o build_test_full_fw
	./build_test_full_fw
	$(HOST_CC) $(HOST_CFLAGS) -DPA_POOL_TEST=1 test/test_robustness.c \
		src/sys/preset.c src/sys/pa_pool.c src/board/spi_nor.c -o build_test_robustness
	./build_test_robustness
	$(HOST_CC) $(HOST_CFLAGS) test/test_sysex_read.c src/sys/sysex.c -o build_test_sysex_read
	./build_test_sysex_read
	$(HOST_CC) $(HOST_CFLAGS) test/test_comp.c src/fx/fx_dynamics.c -lm -o build_test_comp
	./build_test_comp
	$(HOST_CC) $(HOST_CFLAGS) test/test_gate.c src/fx/fx_dynamics.c -lm -o build_test_gate
	./build_test_gate
	$(HOST_CC) $(HOST_CFLAGS) test/test_drive.c src/fx/fx_drive.c -lm -o build_test_drive
	./build_test_drive
	$(HOST_CC) $(HOST_CFLAGS) test/test_eq.c src/fx/fx_eq.c -lm -o build_test_eq
	./build_test_eq
	$(HOST_CC) $(HOST_CFLAGS) test/test_mod.c src/fx/fx_mod.c -lm -o build_test_mod
	./build_test_mod
	$(HOST_CC) $(HOST_CFLAGS) test/test_drum_clip.c -lm -o build_test_drum_clip
	./build_test_drum_clip
	$(HOST_CC) $(HOST_CFLAGS) test/test_reverb_modes.c src/fx/fx_reverb.c -lm -o build_test_reverb_modes
	./build_test_reverb_modes
	$(HOST_CC) $(HOST_CFLAGS) test/test_mix_limiter.c -lm -o build_test_mix_limiter
	./build_test_mix_limiter
	$(HOST_CC) $(HOST_CFLAGS) test/test_ble_midi.c src/board/ble_midi.c -o build_test_ble_midi
	./build_test_ble_midi
	$(HOST_CC) $(HOST_CFLAGS) test/test_wavenet_fast.c src/nam/pa_wavenet.c src/dsp/hw_accel.c src/nam/pa_wavenet_fast.c -lm -o build_test_wavenet_fast
	./build_test_wavenet_fast

cross:
	@for f in $(ALL_FW_SRCS); do \
		echo "[PI32V2 CC] $$f"; \
		$(PI32_CC) $(PI32_CFLAGS) -c $$f -o /dev/null; \
	done
	@echo "[+] All $(words $(ALL_FW_SRCS)) firmware modules cross-compiled cleanly for pi32v2!"

fw:
	@mkdir -p build_pi32
	@for f in $(ALL_FW_SRCS); do \
		echo "[PI32V2 CC] $$f"; \
		$(PI32_CC) $(PI32_CFLAGS) -c $$f -o build_pi32/$$(basename $$f .c | sed 's/\.S$$//').o || exit 1; \
	done
	$(PI32_LD) -T app.ld -o build_pi32/app.elf build_pi32/*.o -L $(PI32_LIB) -lc -lm -lcompiler_rt
	$(PI32_OC) -O binary -j .text build_pi32/app.elf build_pi32/app.bin
	$(PYTHON) tools/pack_ufw.py --in-bin build_pi32/app.bin --out-ufw build_pi32/app.ufw --out-fwsc ../../Pocket_AMP_Custom.fwsc --template-fwsc "../../Pocket AMP.fwsc"
	@echo "[+] Successfully built build_pi32/app.bin and Pocket_AMP_Custom.fwsc!"

clean:
	rm -f build_test_* *.o build_pi32/*.o build_pi32/*.elf build_pi32/*.bin build_pi32/*.ufw

