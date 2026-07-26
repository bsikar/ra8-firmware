# mk/sim.mk -- board_sim emulator: sim-<app>, profile-<app>, ereader GUI, SIL,
# and the e-reader chrome golden gate. BOARD_SIM_DIR + the RA8_SIM*/RA8_PROFILE
# app lists come from the top Makefile.
# Copyright (c) 2026 Brighton Sikarskie
# SPDX-License-Identifier: MIT

# Panel descriptor used by sim-<app>: tools/ra8_emulator/panels/<PANEL>.toml.
SIM_PANEL ?= ek_ra8d2
PANEL     ?= $(SIM_PANEL)

.PHONY: $(RA8_SIM) sim-help sim-matrix sim-matrix-triage sim-matrix-baseline \
        sil sil-all sil-only ereader-gui \
        ereader-golden ereader-golden-update

# `make sim-<app> [PANEL=<name>]` -- cross-build the app, boot its REAL .elf on
# the Unicorn CPU emulator, live macOS panel window.
$(RA8_SIM_GENERIC): sim-%: %
	$(CMAKE) -B $(BOARD_SIM_DIR)/build -S $(BOARD_SIM_DIR)
	$(CMAKE) --build $(BOARD_SIM_DIR)/build -j
	$(BOARD_SIM_DIR)/build/ra8_emulator $(RA8_APP_DIR_$*)/build/$*.elf \
		--panel $(BOARD_SIM_DIR)/panels/$(PANEL).toml --view

# The e-reader (src/app) is a two-image TrustZone Debug build; board_sim loads
# the Non-Secure .elf via --ns across a hand-emulated BLXNS seam.
RA8_EREADER_SIM_DIR := $(RA8_APP_DIR_ra8d2-ereader)/build-sim
sim-ra8d2-ereader:
	$(CMAKE) -S $(RA8_APP_DIR_ra8d2-ereader) -B $(RA8_EREADER_SIM_DIR) \
		-DCMAKE_TOOLCHAIN_FILE=$(ROOT)/cmake/toolchain-ra8d2.cmake \
		-DCMAKE_BUILD_TYPE=Debug -DCMAKE_EXPORT_COMPILE_COMMANDS=ON
	$(CMAKE) --build $(RA8_EREADER_SIM_DIR) -j
	$(CMAKE) -B $(BOARD_SIM_DIR)/build -S $(BOARD_SIM_DIR)
	$(CMAKE) --build $(BOARD_SIM_DIR)/build -j
	$(BOARD_SIM_DIR)/build/ra8_emulator \
		$(RA8_EREADER_SIM_DIR)/ra8d2-ereader.elf \
		--ns $(RA8_EREADER_SIM_DIR)/ra8d2-ereader_ns.elf \
		--panel $(BOARD_SIM_DIR)/panels/$(PANEL).toml --view

# The dual-core demo: single M85 .elf with an embedded Cortex-M33 .cpu1_image.
RA8_DUALCORE_SIM_DIR := $(RA8_APP_DIR_dualcore_mailbox)/build-sim
sim-dualcore_mailbox:
	$(CMAKE) -S $(RA8_APP_DIR_dualcore_mailbox) -B $(RA8_DUALCORE_SIM_DIR) \
		-DCMAKE_TOOLCHAIN_FILE=$(ROOT)/cmake/toolchain-ra8d2.cmake \
		-DCMAKE_BUILD_TYPE=Debug -DCMAKE_EXPORT_COMPILE_COMMANDS=ON
	$(CMAKE) --build $(RA8_DUALCORE_SIM_DIR) -j
	$(CMAKE) -B $(BOARD_SIM_DIR)/build -S $(BOARD_SIM_DIR)
	$(CMAKE) --build $(BOARD_SIM_DIR)/build -j
	$(BOARD_SIM_DIR)/build/ra8_emulator \
		$(RA8_DUALCORE_SIM_DIR)/dualcore_mailbox.elf \
		--panel $(BOARD_SIM_DIR)/panels/$(PANEL).toml --view

# The two-image TrustZone ThreadX demo (Secure + Non-Secure via --ns).
RA8_TZ_THREADX_DEMO_SIM_DIR := $(RA8_APP_DIR_tz_threadx_demo)/build-sim
sim-tz_threadx_demo:
	$(CMAKE) -S $(RA8_APP_DIR_tz_threadx_demo) -B $(RA8_TZ_THREADX_DEMO_SIM_DIR) \
		-DCMAKE_TOOLCHAIN_FILE=$(ROOT)/cmake/toolchain-ra8d2.cmake \
		-DCMAKE_BUILD_TYPE=Debug -DCMAKE_EXPORT_COMPILE_COMMANDS=ON
	$(CMAKE) --build $(RA8_TZ_THREADX_DEMO_SIM_DIR) -j
	$(CMAKE) -B $(BOARD_SIM_DIR)/build -S $(BOARD_SIM_DIR)
	$(CMAKE) --build $(BOARD_SIM_DIR)/build -j
	$(BOARD_SIM_DIR)/build/ra8_emulator \
		$(RA8_TZ_THREADX_DEMO_SIM_DIR)/tz_threadx_demo.elf \
		--ns $(RA8_TZ_THREADX_DEMO_SIM_DIR)/tz_threadx_demo_ns.elf \
		--panel $(BOARD_SIM_DIR)/panels/$(PANEL).toml --view

# `make profile-<app>` -- run headless under the board_sim profiler; opens a
# local interactive flamechart GUI (board_sim_profile.html) on exit.
MODE         ?= full
PROFILE_ARGS ?=
STOP_PC      ?=
.PHONY: sim-ra8d2-ereader sim-dualcore_mailbox sim-tz_threadx_demo
.PHONY: $(RA8_PROFILE)
$(RA8_PROFILE): profile-%: %
	$(CMAKE) -B $(BOARD_SIM_DIR)/build -S $(BOARD_SIM_DIR)
	$(CMAKE) --build $(BOARD_SIM_DIR)/build -j
	BOARD_SIM_PROFILE=$(MODE) BOARD_SIM_STOP_PC=$(STOP_PC) BOARD_SIM_MAX_CHUNKS=8000000 \
		$(BOARD_SIM_DIR)/build/ra8_emulator $(RA8_APP_DIR_$*)/build/$*.elf $(PROFILE_ARGS)
	@if [ -f board_sim_profile.html ]; then \
		echo "  opening flamechart GUI: board_sim_profile.html"; \
		( command -v open >/dev/null 2>&1 && open board_sim_profile.html ) \
		  || ( command -v xdg-open >/dev/null 2>&1 && xdg-open board_sim_profile.html ) \
		  || echo "  (open board_sim_profile.html in a browser)"; \
	fi

# `make ereader-gui` -- the full hybrid e-reader GUI: baked + SD books on a live
# window, --fast-sd by default (FAST_SD=0 to opt out).
EREADER_SD_IMG   := $(ROOT)/build/ereader_sd.img
EREADER_SD_DIR   ?= $(ROOT)/content/compiled
EREADER_SD_COUNT ?= 5
FAST_SD          ?= 1
_EREADER_FAST    := $(if $(filter-out 0,$(FAST_SD)),--fast-sd,)
ereader-gui: ereader_shelf
	$(CMAKE) -B $(BOARD_SIM_DIR)/build -S $(BOARD_SIM_DIR)
	$(CMAKE) --build $(BOARD_SIM_DIR)/build -j
	$(CMAKE) -B $(ROOT)/tools/mkbookimg/build -S $(ROOT)/tools/mkbookimg
	$(CMAKE) --build $(ROOT)/tools/mkbookimg/build -j
	@mkdir -p $(ROOT)/build
	@set -e; shopt -s nullglob; \
		all=("$(EREADER_SD_DIR)"/*.rabook); \
		books=("$${all[@]:0:$(EREADER_SD_COUNT)}"); \
		if [ $${#books[@]} -eq 0 ]; then echo "no *.rabook in $(EREADER_SD_DIR)"; exit 1; fi; \
		echo "  SD card: $${#books[@]} book(s) from $(EREADER_SD_DIR)"; \
		"$(ROOT)/tools/mkbookimg/build/mkbookimg" "$(EREADER_SD_IMG)" "$${books[@]}"
	$(BOARD_SIM_DIR)/build/ra8_emulator $(RA8_APP_DIR_ereader_shelf)/build/ereader_shelf.elf \
		--sd $(EREADER_SD_IMG) $(_EREADER_FAST) \
		--panel $(BOARD_SIM_DIR)/panels/$(PANEL).toml --view

# `make sim-help` -- usage, the PANEL knob, and the board_sim flag surface.
sim-help:
	@echo "make sim-<app> [PANEL=<name>]  -- boot an app's REAL .elf on the board_sim"
	@echo "                                  Unicorn CPU emulator (tools/ra8_emulator)"
	@echo ""
	@echo "  PANEL=<name>   display descriptor tools/ra8_emulator/panels/<name>.toml (default ek_ra8d2)"
	@echo ""
	@echo "Examples:"
	@echo "  make sim-blink                       live board view (LEDs, USB/UART/IRQ sidebar)"
	@echo "  make sim-ereader_ui                  e-reader chrome UI preview"
	@echo "  make sim-lcd_color_cycle PANEL=ek_ra8d2"
	@echo ""
	@echo "Profile an app (Ozone-style flamechart; opens a local interactive GUI on exit):"
	@echo "  make profile-<app>                       boot flamechart -> opens board_sim_profile.html"
	@echo "  make profile-ereader_ui PROFILE_ARGS=\"--click 250 250\"   profile opening a book"
	@echo "  make profile-<app> MODE=1                cheap flat wall-time sampler (no flamechart)"
	@echo "  make profile-<app> STOP_PC=0x...         stop the profile at an exact PC"
	@echo ""
	@echo "Run board_sim directly for headless / scripted use (tools/ra8_emulator/README.md):"
	@echo "  cd tools/ra8_emulator && cmake -B build -S . && cmake --build build -j"
	@echo "  ./build/ra8_emulator <app.elf>                  headless boot + MMIO report"
	@echo "  ./build/ra8_emulator <app.elf> --view           live macOS window"
	@echo "  ./build/ra8_emulator <app.elf> --ppm out.ppm    write the composite frame"
	@echo "  ./build/ra8_emulator <app.elf> --sd <img> | --sd-new 64:fat32 [--save-sd out.img]"
	@echo ""
	@echo "Headless run-bounding env vars: BOARD_SIM_MAX_CHUNKS, BOARD_SIM_WALL_S,"
	@echo "  BOARD_SIM_IDLE_STOP=N, BOARD_SIM_USB_STOP=N, BOARD_SIM_STOP_ON='<substr>'"

# `make sim-matrix` -- build + boot EVERY ek_ra8d2 example on the emulator and
# report a per-app boot/fault/halt/truncated table + coverage %. Runs one worker
# per core; MATRIX_JOBS=1 forces serial (the verdicts are identical either way).
sim-matrix:
	bash scripts/sim/matrix.sh $(APPS)

# `make sim-matrix-triage` -- group the last sweep's failures by cause, so the
# burn-down has tranches rather than a bare total.
sim-matrix-triage:
	bash scripts/sim/matrix_triage.sh

# `make sim-matrix-baseline` -- lock in progress after burning failures down.
# The board-sim-matrix gate ratchets against this baseline: growth fails,
# shrinking is free, and the end state is an empty baseline.
sim-matrix-baseline:
	bash scripts/sim/matrix.sh $(APPS) || true
	python3 scripts/checks/matrix_ratchet.py --update

# SIL (simulator-in-the-loop): boot EVERY hw_validated/hil app headless and
# assert its hil.conf expectation, in parallel. No board required.
sil sil-all:
	bash scripts/sim/sil_all.sh $(if $(SIL_JOBS),-j $(SIL_JOBS),)

sil-only:
	@test -n "$(APP)" || { echo "usage: make sil-only APP=<app>"; exit 2; }
	bash scripts/sim/sil_all.sh --only $(APP)

# E-reader chrome golden-image regression gate (issue #84).
EREADER_GOLDEN_ELF := $(RA8_APP_DIR_ereader_ui)/build/ereader_ui.elf
EREADER_GOLDEN_DIR := $(ROOT)/tests/golden/ereader_chrome
ereader-golden: ereader_ui
	$(CMAKE) -B $(BOARD_SIM_DIR)/build -S $(BOARD_SIM_DIR)
	$(CMAKE) --build $(BOARD_SIM_DIR)/build -j
	python3 scripts/gen/ereader_golden.py check \
		--elf $(EREADER_GOLDEN_ELF) --board-sim $(BOARD_SIM_DIR)/build/ra8_emulator \
		--golden-dir $(EREADER_GOLDEN_DIR) --out-dir /tmp/ereader_golden_out

ereader-golden-update: ereader_ui
	$(CMAKE) -B $(BOARD_SIM_DIR)/build -S $(BOARD_SIM_DIR)
	$(CMAKE) --build $(BOARD_SIM_DIR)/build -j
	python3 scripts/gen/ereader_golden.py update \
		--elf $(EREADER_GOLDEN_ELF) --board-sim $(BOARD_SIM_DIR)/build/ra8_emulator \
		--golden-dir $(EREADER_GOLDEN_DIR)
