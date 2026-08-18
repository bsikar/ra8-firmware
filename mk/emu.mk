# SPDX-License-Identifier: MIT
# Copyright (c) 2026 Brighton Sikarskie
# mk/emu.mk -- ra8_emulator: emu-<app>, profile-<app>, ereader GUI, EIL,
# and the e-reader chrome golden gate. RA8_EMU_DIR + the RA8_EMU*/RA8_PROFILE
# app lists come from the top Makefile.

# Panel descriptor used by emu-<app>: tools/ra8_emulator/panels/<PANEL>.toml.
EMU_PANEL ?= ek_ra8d2
PANEL     ?= $(EMU_PANEL)

.PHONY: $(RA8_EMU) emu-help emu-setup emu-matrix emu-matrix-triage emu-matrix-baseline \
        eil eil-all eil-only ereader-gui \
        ereader-golden ereader-golden-update

# `make emu-<app> [PANEL=<name>]` -- cross-build the app, boot its REAL .elf on
# the Unicorn CPU emulator, live macOS panel window.
$(RA8_EMU_GENERIC): emu-%: %
	$(CMAKE) -B $(RA8_EMU_DIR)/build -S $(RA8_EMU_DIR)
	$(CMAKE) --build $(RA8_EMU_DIR)/build -j
	$(RA8_EMU_DIR)/build/ra8_emulator $(RA8_APP_DIR_$*)/build/$*.elf \
		--panel $(RA8_EMU_DIR)/panels/$(PANEL).toml --view

# The e-reader (apps/stand_alone/ereader) is a two-image TrustZone Debug build;
# ra8_emulator loads the Non-Secure .elf via --ns across a hand-emulated BLXNS
# seam.
RA8_EREADER_EMU_DIR := $(RA8_APP_DIR_ra8d2-ereader)/build-emu
emu-ra8d2-ereader:
	$(CMAKE) -S $(RA8_APP_DIR_ra8d2-ereader) -B $(RA8_EREADER_EMU_DIR) \
		-DCMAKE_TOOLCHAIN_FILE=$(ROOT)/cmake/toolchain-ra8d2.cmake \
		-DCMAKE_BUILD_TYPE=Debug -DCMAKE_EXPORT_COMPILE_COMMANDS=ON
	$(CMAKE) --build $(RA8_EREADER_EMU_DIR) -j
	$(CMAKE) -B $(RA8_EMU_DIR)/build -S $(RA8_EMU_DIR)
	$(CMAKE) --build $(RA8_EMU_DIR)/build -j
	$(RA8_EMU_DIR)/build/ra8_emulator \
		$(RA8_EREADER_EMU_DIR)/ra8d2-ereader.elf \
		--ns $(RA8_EREADER_EMU_DIR)/ra8d2-ereader_ns.elf \
		--panel $(RA8_EMU_DIR)/panels/$(PANEL).toml --view

# The dual-core demo: single M85 .elf with an embedded Cortex-M33 .cpu1_image.
RA8_DUALCORE_EMU_DIR := $(RA8_APP_DIR_dualcore_mailbox)/build-emu
emu-dualcore_mailbox:
	$(CMAKE) -S $(RA8_APP_DIR_dualcore_mailbox) -B $(RA8_DUALCORE_EMU_DIR) \
		-DCMAKE_TOOLCHAIN_FILE=$(ROOT)/cmake/toolchain-ra8d2.cmake \
		-DCMAKE_BUILD_TYPE=Debug -DCMAKE_EXPORT_COMPILE_COMMANDS=ON
	$(CMAKE) --build $(RA8_DUALCORE_EMU_DIR) -j
	$(CMAKE) -B $(RA8_EMU_DIR)/build -S $(RA8_EMU_DIR)
	$(CMAKE) --build $(RA8_EMU_DIR)/build -j
	$(RA8_EMU_DIR)/build/ra8_emulator \
		$(RA8_DUALCORE_EMU_DIR)/dualcore_mailbox.elf \
		--panel $(RA8_EMU_DIR)/panels/$(PANEL).toml --view

# The two-image TrustZone ThreadX demo (Secure + Non-Secure via --ns).
RA8_TZ_THREADX_DEMO_EMU_DIR := $(RA8_APP_DIR_tz_threadx_demo)/build-emu
emu-tz_threadx_demo:
	$(CMAKE) -S $(RA8_APP_DIR_tz_threadx_demo) -B $(RA8_TZ_THREADX_DEMO_EMU_DIR) \
		-DCMAKE_TOOLCHAIN_FILE=$(ROOT)/cmake/toolchain-ra8d2.cmake \
		-DCMAKE_BUILD_TYPE=Debug -DCMAKE_EXPORT_COMPILE_COMMANDS=ON
	$(CMAKE) --build $(RA8_TZ_THREADX_DEMO_EMU_DIR) -j
	$(CMAKE) -B $(RA8_EMU_DIR)/build -S $(RA8_EMU_DIR)
	$(CMAKE) --build $(RA8_EMU_DIR)/build -j
	$(RA8_EMU_DIR)/build/ra8_emulator \
		$(RA8_TZ_THREADX_DEMO_EMU_DIR)/tz_threadx_demo.elf \
		--ns $(RA8_TZ_THREADX_DEMO_EMU_DIR)/tz_threadx_demo_ns.elf \
		--panel $(RA8_EMU_DIR)/panels/$(PANEL).toml --view

# `make profile-<app>` -- run headless under the ra8_emulator profiler; opens a
# local interactive flamechart GUI (ra8_emulator_profile.html) on exit.
MODE         ?= full
PROFILE_ARGS ?=
STOP_PC      ?=
.PHONY: emu-ra8d2-ereader emu-dualcore_mailbox emu-tz_threadx_demo
.PHONY: $(RA8_PROFILE)
$(RA8_PROFILE): profile-%: %
	$(CMAKE) -B $(RA8_EMU_DIR)/build -S $(RA8_EMU_DIR)
	$(CMAKE) --build $(RA8_EMU_DIR)/build -j
	RA8_EMU_PROFILE=$(MODE) RA8_EMU_STOP_PC=$(STOP_PC) RA8_EMU_MAX_CHUNKS=8000000 \
		$(RA8_EMU_DIR)/build/ra8_emulator $(RA8_APP_DIR_$*)/build/$*.elf $(PROFILE_ARGS)
	@if [ -f ra8_emulator_profile.html ]; then \
		echo "  opening flamechart GUI: ra8_emulator_profile.html"; \
		( command -v open >/dev/null 2>&1 && open ra8_emulator_profile.html ) \
		  || ( command -v xdg-open >/dev/null 2>&1 && xdg-open ra8_emulator_profile.html ) \
		  || echo "  (open ra8_emulator_profile.html in a browser)"; \
	fi

# `make ereader-gui` -- the full hybrid e-reader GUI: baked + SD books on a live
# window, --fast-sd by default (FAST_SD=0 to opt out).
EREADER_SD_IMG   := $(ROOT)/build/ereader_sd.img
EREADER_SD_DIR   ?= $(ROOT)/apps/stand_alone/ereader/content/compiled
EREADER_SD_COUNT ?= 5
FAST_SD          ?= 1
_EREADER_FAST    := $(if $(filter-out 0,$(FAST_SD)),--fast-sd,)
ereader-gui: ereader_shelf
	$(CMAKE) -B $(RA8_EMU_DIR)/build -S $(RA8_EMU_DIR)
	$(CMAKE) --build $(RA8_EMU_DIR)/build -j
	$(CMAKE) -B $(ROOT)/tools/mkbookimg/build -S $(ROOT)/tools/mkbookimg
	$(CMAKE) --build $(ROOT)/tools/mkbookimg/build -j
	@mkdir -p $(ROOT)/build
	@set -e; shopt -s nullglob; \
		all=("$(EREADER_SD_DIR)"/*.rabook); \
		books=("$${all[@]:0:$(EREADER_SD_COUNT)}"); \
		if [ $${#books[@]} -eq 0 ]; then echo "no *.rabook in $(EREADER_SD_DIR)"; exit 1; fi; \
		echo "  SD card: $${#books[@]} book(s) from $(EREADER_SD_DIR)"; \
		"$(ROOT)/tools/mkbookimg/build/mkbookimg" "$(EREADER_SD_IMG)" "$${books[@]}"
	$(RA8_EMU_DIR)/build/ra8_emulator $(RA8_APP_DIR_ereader_shelf)/build/ereader_shelf.elf \
		--sd $(EREADER_SD_IMG) $(_EREADER_FAST) \
		--panel $(RA8_EMU_DIR)/panels/$(PANEL).toml --view

# `make emu-help` -- usage, the PANEL knob, and the ra8_emulator flag surface.
emu-setup:
	bash scripts/emu/setup_macos.sh

emu-help:
	@echo "make emu-setup                         install macOS emulator dependencies"
	@echo "make emu-<app> [PANEL=<name>]  -- boot an app's REAL .elf on the ra8_emulator"
	@echo "                                  Unicorn CPU emulator (tools/ra8_emulator)"
	@echo ""
	@echo "  PANEL=<name>   display descriptor tools/ra8_emulator/panels/<name>.toml (default ek_ra8d2)"
	@echo ""
	@echo "Examples:"
	@echo "  make emu-blink                       live board view (LEDs, USB/UART/IRQ sidebar)"
	@echo "  make emu-ereader_ui                  e-reader chrome UI preview"
	@echo "  make emu-lcd_color_cycle PANEL=ek_ra8d2"
	@echo ""
	@echo "Profile an app (Ozone-style flamechart; opens a local interactive GUI on exit):"
	@echo "  make profile-<app>                       boot flamechart -> opens ra8_emulator_profile.html"
	@echo "  make profile-ereader_ui PROFILE_ARGS=\"--click 250 250\"   profile opening a book"
	@echo "  make profile-<app> MODE=1                cheap flat wall-time sampler (no flamechart)"
	@echo "  make profile-<app> STOP_PC=0x...         stop the profile at an exact PC"
	@echo ""
	@echo "Run ra8_emulator directly for headless / scripted use (tools/ra8_emulator/README.md):"
	@echo "  cd tools/ra8_emulator && cmake -B build -S . && cmake --build build -j"
	@echo "  ./build/ra8_emulator <app.elf>                  headless boot + MMIO report"
	@echo "  ./build/ra8_emulator <app.elf> --view           live macOS window"
	@echo "  ./build/ra8_emulator <app.elf> --ppm out.ppm    write the composite frame"
	@echo "  ./build/ra8_emulator <app.elf> --sd <img> | --sd-new 64:fat32 [--save-sd out.img]"
	@echo ""
	@echo "Headless run-bounding env vars: RA8_EMU_MAX_CHUNKS, RA8_EMU_WALL_S,"
	@echo "  RA8_EMU_IDLE_STOP=N, RA8_EMU_USB_STOP=N, RA8_EMU_STOP_ON='<substr>'"

# `make emu-matrix` -- build + boot EVERY ek_ra8d2 example on the emulator and
# report a per-app boot/fault/halt/truncated table + coverage %. Runs one worker
# per core; MATRIX_JOBS=1 forces serial (the verdicts are identical either way).
emu-matrix:
	bash scripts/emu/matrix.sh $(APPS)

# `make emu-matrix-triage` -- group the last sweep's failures by cause, so the
# burn-down has tranches rather than a bare total.
emu-matrix-triage:
	bash scripts/emu/matrix_triage.sh

# `make emu-matrix-baseline` -- lock in progress after burning failures down.
# The emulator-matrix gate ratchets against this baseline: growth fails,
# shrinking is free, and the end state is an empty baseline.
emu-matrix-baseline:
	bash scripts/emu/matrix.sh $(APPS) || true
	python3 scripts/checks/matrix_ratchet.py --update

# EIL (emulator-in-the-loop): boot EVERY hw_validated/hil app headless and
# assert its hil.conf expectation, in parallel. No board required.
eil eil-all:
	bash scripts/emu/eil_all.sh $(if $(EIL_JOBS),-j $(EIL_JOBS),)

eil-only:
	@test -n "$(APP)" || { echo "usage: make eil-only APP=<app>"; exit 2; }
	bash scripts/emu/eil_all.sh --only $(APP)

# E-reader chrome golden-image regression gate (issue #84).
EREADER_GOLDEN_ELF := $(RA8_APP_DIR_ereader_ui)/build/ereader_ui.elf
EREADER_GOLDEN_DIR := $(ROOT)/tests/golden/ereader_chrome
ereader-golden: ereader_ui
	$(CMAKE) -B $(RA8_EMU_DIR)/build -S $(RA8_EMU_DIR)
	$(CMAKE) --build $(RA8_EMU_DIR)/build -j
	python3 scripts/gen/ereader_golden.py check \
		--elf $(EREADER_GOLDEN_ELF) --emulator $(RA8_EMU_DIR)/build/ra8_emulator \
		--golden-dir $(EREADER_GOLDEN_DIR) --out-dir /tmp/ereader_golden_out

ereader-golden-update: ereader_ui
	$(CMAKE) -B $(RA8_EMU_DIR)/build -S $(RA8_EMU_DIR)
	$(CMAKE) --build $(RA8_EMU_DIR)/build -j
	python3 scripts/gen/ereader_golden.py update \
		--elf $(EREADER_GOLDEN_ELF) --emulator $(RA8_EMU_DIR)/build/ra8_emulator \
		--golden-dir $(EREADER_GOLDEN_DIR)
