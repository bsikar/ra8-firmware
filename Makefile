#
# Top-level Makefile for ra8d2-firmware.
#
# Each application lives in its own directory under
# examples/<tier>/<app>/ with its own main.c, boot files
# (vector_table.c, system_init.c, secure_exception.c,
# trustzone_init.c), linker_script.ld, Makefile, and CMakeLists.txt.
# The <tier> layer groups apps by hardware-support category
# (ek_ra8d2/ for the stock EVM, _unsupported/ for apps needing extra
# hardware). The build-target name is just the bare app dir name --
# the tier directory is purely organisational, so `make blink` works
# regardless of which tier blink/ lives in. This top-level Makefile
# auto-discovers them and forwards `make <app>` to the per-app
# Makefile.
#
# Common targets (run `make help` for the full, grouped list):
#
#   make            -- build the default app ($(RA_DEFAULT_APP))
#   make <app>      -- build a cross-compiled firmware app, e.g. `make blink_hal`
#   make apps       -- list every discovered firmware app
#   make help       -- grouped reference of every top-level target
#   make clean      -- remove every app build dir and tests/build
#   make format     -- run clang-format --in-place on the whole tree
#   make check      -- run clang-format --dry-run (no changes)
#   make tidy       -- run clang-tidy against the whole tree
#   make test       -- build + run the host unit tests
#   make docs       -- generate doxygen HTML into build/docs/html/
#   make dashboard  -- regenerate docs/ROADMAP_DASHBOARD.md + docs/badges/
#   make ascii      -- scan for non-ASCII characters
#   make version    -- verify @since tags match the VERSION file
#
# Two ways to "see the UI / firmware run":
#
#   make <app>            cross-compile firmware -> flash it to a board
#   make sim-<app>        boot that REAL .elf on the Unicorn CPU emulator
#                         (tools/board_sim) -- the single simulator: high-
#                         fidelity bring-up + live panel/UI in a macOS window
#
# Copyright (c) 2026 Brighton Sikarskie
# SPDX-License-Identifier: MIT
#

SHELL := /bin/bash

# `make` with no target builds the default app (see `default:` below) -- the
# behaviour the README and the header above document. Set explicitly so it does
# not silently depend on `default:` being the first rule in this file (the
# `help` rule is defined earlier for readability). `make help` lists everything.
.DEFAULT_GOAL := default

ROOT             := $(abspath .)
TESTS_DIR        := $(ROOT)/tests
TESTS_BUILD      := $(TESTS_DIR)/build
TESTS_BUILD_COV  := $(TESTS_DIR)/build-cov
# Legacy tidy build dir kept so `bash scripts/clang_tidy.sh` keeps working.
TIDY_BUILD       := $(ROOT)/build/tidy

CMAKE        ?= cmake
CLANG_FORMAT ?= clang-format
DOXYGEN      ?= doxygen
ARM_SIZE     ?= arm-none-eabi-size

# --- git hooks: active for every clone, every make invocation ----------------
# A fresh clone has no hooks in .git/hooks, so the commit-msg / pre-commit /
# pre-push gates (AI-attribution ban, ASCII, Doxygen, copyright, ...) would be
# silently skipped. Point core.hooksPath at the tracked scripts/git/ hooks on
# every make run -- idempotent, prints only the first time. `make hooks` too.
_RA_HOOKS_MSG := $(shell $(ROOT)/scripts/git/install-hooks.sh 2>/dev/null)
$(if $(_RA_HOOKS_MSG),$(info $(_RA_HOOKS_MSG)))

# Default app -- override on the command line, e.g. `make RA_DEFAULT_APP=blink_hal`.
RA_DEFAULT_APP ?= blink

# Auto-discover apps: every examples/<tier>/<app>/ or
# examples/<tier>/<subtier>/<app>/ dir that contains main.c +
# CMakeLists.txt. RA_APPS holds the bare app names (e.g. "blink");
# RA_APP_DIR_<app> resolves each one to its full per-app directory so
# `make blink` works regardless of which tier/subtier it lives in.
_RA_APP_MAINS := $(wildcard $(ROOT)/examples/*/*/main.c) \
                 $(wildcard $(ROOT)/examples/*/*/*/main.c) \
                 $(wildcard $(ROOT)/examples/*/*/*/*/main.c)
RA_APPS       := $(sort $(notdir $(patsubst %/main.c,%,$(_RA_APP_MAINS))))
$(foreach m,$(_RA_APP_MAINS),$(eval RA_APP_DIR_$(notdir $(patsubst %/main.c,%,$m)) := $(patsubst %/main.c,%,$m)))

# `make flash-<app>` / `debug-<app>` / `ozone-<app>` shorthands -- build the
# app, then run the matching per-app Makefile target (local J-Link board).
RA_FLASH := $(addprefix flash-,$(RA_APPS))
RA_DEBUG := $(addprefix debug-,$(RA_APPS))
RA_OZONE := $(addprefix ozone-,$(RA_APPS))
# `make sim-<app>` -- boot the app's real .elf on the Unicorn CPU emulator
# (tools/board_sim), the single simulator: high-fidelity, runs the genuine
# bring-up + peripheral-driver path, and IS the UI preview for chrome apps.
RA_SIM := $(addprefix sim-,$(RA_APPS))

# We forward each <app> name to the app's own Makefile, so reserve the names
# below from being shadowed by .PHONY targets.
.PHONY: help apps default clean compile_commands format check tidy test test-cov test-docker ctest coverage mcdc misra docs dashboard ascii version smoke stack-usage scan-build scan-build-strict iwyu fuzz bench app-sizes check-annotations all $(RA_APPS)

# hw_validated apps -- smoke test and stack-usage sweeps run over this
# set only, since these are the apps confirmed working on a stock EK-RA8D2.
_EK_APP_MAINS := $(wildcard $(ROOT)/examples/ek_ra8d2/hw_validated/*/main.c) \
                 $(wildcard $(ROOT)/examples/ek_ra8d2/hw_validated/*/*/main.c)
EK_APPS       := $(sort $(notdir $(patsubst %/main.c,%,$(_EK_APP_MAINS))))

help:
	@echo "ra8d2-firmware make targets   ($(words $(RA_APPS)) firmware apps -- 'make apps' for the list)"
	@echo ""
	@echo "BUILD"
	@echo "  make                   build the default app ($(RA_DEFAULT_APP))"
	@echo "  make <app>             cross-compile one firmware app, e.g. make blink"
	@echo "  make build-all         cross-compile every firmware app (CI's cross-build job)"
	@echo "  make clean             remove every app build dir and tests/build"
	@echo "  make compile_commands  regenerate build/compile_commands.json for clangd"
	@echo ""
	@echo "RUN / PREVIEW / SIMULATE  (no board needed -- see 'make apps')          [make sim-help]"
	@echo "  make sim-<app> [PANEL=ek_ra8d2]  boot an app's REAL .elf on the Unicorn CPU"
	@echo "                             emulator, live panel/UI window (tools/board_sim)"
	@echo "                             e.g. make sim-blink, make sim-bedroom_ui_panel"
	@echo ""
	@echo "HARDWARE -- flash / debug (board on THIS machine, local J-Link)  [make flash-help / debug-help / ozone-help]"
	@echo "  make flash-<app>       build + flash an app  (e.g. make flash-blink)"
	@echo "  make debug-<app>       build + gdb via J-Link"
	@echo "  make ozone-<app>       build + open in Ozone"
	@echo "  make flash-ocd APP=<app> / debug-ocd APP=<app>   OpenOCD instead of J-Link"
	@echo ""
	@echo "HIL -- hardware-in-the-loop (board on the Pi rig, driven over SSH)        [make hil-help]"
	@echo "  make hil               full HIL suite from this machine (build+flash+verify)"
	@echo "  make hil-flash APP=<app>     build + flash to the Pi-attached board"
	@echo "  make hil-recover APP=<app>   recovery flash / make hil-flash-retry APP=<app>"
	@echo "  make hil-erase / hil-dlm-reset / hil-probe / hil-suite / hil-all"
	@echo "  make hil-tapo CMD=<status|on|off|cycle>   board power  (hil-ppps for USB)"
	@echo ""
	@echo "QUALITY / CI"
	@echo "  make format            run clang-format in place"
	@echo "  make check             run clang-format --dry-run"
	@echo "  make tidy              run clang-tidy"
	@echo "  make cppcheck          run the cppcheck gate locally"
	@echo "  make ascii             fix-encoding.py --check"
	@echo "  make version           verify @since tags match the VERSION file"
	@echo "  make test              host-compile + run unit tests (tests/build/)"
	@echo "  make test-cov          alias for make mcdc (tests/build-cov/)"
	@echo "  make test-docker       host-compile + run unit tests in Linux container"
	@echo "  make ctest             rerun just ctest (assumes already built)"
	@echo "  make coverage          generate lcov+genhtml HTML coverage report"
	@echo "  make mcdc              generate DO-178C Level B MC/DC report (clang >= 18)"
	@echo "  make misra             run MISRA-C 2012 audit (advisory; see docs/MISRA.md)"
	@echo "  make scan-build        run clang static analyzer over the host test build"
	@echo "  make iwyu              run include-what-you-use over the host test build"
	@echo "  make check-annotations enforce the ra_* annotation-attribute rules"
	@echo "  make fuzz              build + smoke-run libFuzzer harnesses (clang only)"
	@echo "  make bench             build + run host-side performance microbenchmarks"
	@echo "  make stack-usage       build EVM apps + aggregate -fstack-usage report"
	@echo ""
	@echo "DOCS / REPORTS"
	@echo "  make docs              generate doxygen HTML into build/docs/html/"
	@echo "  make dashboard         regenerate docs/ROADMAP_DASHBOARD.md + docs/badges/"
	@echo "  make app-sizes         summarise per-app .text/.data/.bss footprints"
	@echo "  make audit-init        per-app init-order audit (docs/INIT_ORDER_AUDIT.md)"
	@echo ""
	@echo "DEV SETUP / DISCOVERY"
	@echo "  make hooks             (re)install the tracked git hooks (auto-runs on every make)"
	@echo "  make apps              list every discovered firmware app"
	@echo "  make mcp               self-test the MCP dev server (tools/mcp; see .mcp.json)"
	@echo "  make help              this grouped reference"

# `make` with no arg builds the default app.
default: $(RA_DEFAULT_APP)

# `make apps` -- the app catalogue, split by how each app actually runs.
#
# Two families behave differently, so they are listed separately:
#   * FIRMWARE apps      cross-compiled for the EK-RA8D2. Every one can be
#                        built (`make <app>`), flashed (`make flash-<app>`)
#                        and simulated (`make sim-<app>`). Grouped by the
#                        tier dir, which signals hardware-support maturity
#                        (hw_validated = confirmed on a stock EVM).
# Descriptions: firmware apps come from ra_add_app(DESCRIPTION ...) in each
# CMakeLists.txt.
apps:
	@printf '== FIRMWARE apps (%s) -- build: make <app> | flash: make flash-<app> | simulate: make sim-<app>\n' "$(words $(RA_APPS))"
	@for tier_dir in $(ROOT)/examples/*/; do \
		tier=$$(basename "$$tier_dir"); \
		[ "$$tier" = "shared" ] && continue; \
		for main in "$$tier_dir"*/main.c "$$tier_dir"*/*/main.c "$$tier_dir"*/*/*/main.c; do \
			[ -f "$$main" ] || continue; \
			d=$$(dirname "$$main"); \
			app=$$(basename "$$d"); \
			group=$$(dirname "$${d#$(ROOT)/examples/}"); \
			desc=$$(sed -n 's/.*DESCRIPTION "\([^"]*\)".*/\1/p' "$$d/CMakeLists.txt" 2>/dev/null | head -1); \
			printf '%s\t%s\t%s\n' "$$group" "$$app" "$$desc"; \
		done; \
	done | sort | awk -F'\t' '{ if ($$1 != g) { g=$$1; printf "\n  [%s]\n", g } printf "    %-30s %s\n", $$2, $$3 }'
	@printf '\nUI preview: run the e-reader chrome on the emulator, e.g. make sim-ereader_ui [PANEL=ek_ra8d2]   (tools/board_sim)\n'

# Forward `make <app>` to the per-app Makefile so the top-level shorthand
# and `cd examples/<tier>/<app> && make` produce the exact same artifacts.
# The per-app dir is looked up via RA_APP_DIR_<app> so callers don't
# need to know which tier directory the app lives in.
# clangd reads build/compile_commands.json. Regenerate it only when the build
# configuration actually changes (any CMakeLists.txt or cmake/*.cmake), not on
# every app build -- a full top-level reconfigure costs ~2 s. Listing the file
# as a normal (not order-only) prerequisite of each app keeps it in sync: make
# refreshes it before the build when a CMake input is newer, and skips it when
# nothing changed.
RA_COMPILE_COMMANDS := $(ROOT)/build/compile_commands.json
_RA_CMAKE_INPUTS := $(ROOT)/CMakeLists.txt $(wildcard $(ROOT)/cmake/*.cmake) \
	$(shell find $(ROOT)/examples -name CMakeLists.txt 2>/dev/null)

$(RA_APPS): $(RA_COMPILE_COMMANDS)
	$(MAKE) -C $(RA_APP_DIR_$@) build

# Local J-Link shorthands (board plugged into this machine): build the app
# first (via the `%` prereq), then forward to the per-app Makefile, which wraps
# scripts/{flash,debug,ozone}.sh. e.g. `make flash-blink`, `make ozone-blink`.
.PHONY: $(RA_FLASH) $(RA_DEBUG) $(RA_OZONE)
$(RA_FLASH): flash-%: %
	$(MAKE) -C $(RA_APP_DIR_$*) flash
$(RA_DEBUG): debug-%: %
	$(MAKE) -C $(RA_APP_DIR_$*) debug
$(RA_OZONE): ozone-%: %
	$(MAKE) -C $(RA_APP_DIR_$*) ozone

# Per-family help. These are plain explicit targets, so they take precedence
# over the flash-%/debug-%/ozone-% static pattern rules above (which only
# match real app names in $(RA_APPS), never "help").
.PHONY: flash-help debug-help ozone-help
flash-help:
	@echo "make flash-<app>  -- build APP, then flash it to a board on THIS machine via J-Link"
	@echo ""
	@echo "  e.g. make flash-blink"
	@echo ""
	@echo "Alternate flashers / related:"
	@echo "  make flash-ocd APP=<app>     flash via OpenOCD instead of J-Link"
	@echo "  make hil-flash APP=<app>     flash to the Pi-attached board (see make hil-help)"
	@echo "  list apps:  make apps"

debug-help:
	@echo "make debug-<app>  -- build APP, then start a gdb session via J-Link (board on THIS machine)"
	@echo ""
	@echo "  e.g. make debug-blink"
	@echo ""
	@echo "Related:"
	@echo "  make debug-ocd APP=<app>     gdb via OpenOCD instead of J-Link"
	@echo "  make ozone-<app>             open the app in SEGGER Ozone (see make ozone-help)"
	@echo "  list apps:  make apps"

ozone-help:
	@echo "make ozone-<app>  -- build APP, then open it in SEGGER Ozone (board on THIS machine, J-Link)"
	@echo ""
	@echo "  e.g. make ozone-blink"
	@echo ""
	@echo "Related:"
	@echo "  make debug-<app>             plain gdb via J-Link (see make debug-help)"
	@echo "  list apps:  make apps"

$(RA_COMPILE_COMMANDS): $(_RA_CMAKE_INPUTS)
	$(CMAKE) -DCMAKE_TOOLCHAIN_FILE=$(ROOT)/cmake/toolchain-ra8d2.cmake -B $(ROOT)/build $(ROOT)

# Convenience alias: `make compile_commands` forces an up-to-date check.
compile_commands: $(RA_COMPILE_COMMANDS)

clean:
	@for d in $(ROOT)/examples/*/*/main.c $(ROOT)/examples/*/*/*/main.c $(ROOT)/examples/*/*/*/*/main.c; do \
		[ -f "$$d" ] || continue; \
		$(MAKE) -C "$$(dirname $$d)" clean; \
	done
	rm -rf $(TESTS_BUILD) $(TESTS_BUILD_COV) $(TIDY_BUILD)

format:
	bash scripts/format_code.sh

check:
	bash scripts/format_code.sh --check

tidy:
	bash scripts/clang_tidy.sh --check

# `make magic` -- full-tree magic-number gate.  Backstops clang-tidy's
# readability-magic-numbers, which only sees files in the host
# compile-db (no example main.c, no ARM-only #ifdef code paths).
magic:
	python3 scripts/utils/check_magic_numbers.py

test-docker:
	bash scripts/test-docker.sh

test:
	bash $(TESTS_DIR)/build_tests.sh
	bash $(TESTS_DIR)/run_tests.sh

# `make test-cov` is an alias for `make mcdc` -- the coverage build
# tree (tests/build-cov/) lives in parallel with the fast tree
# (tests/build/) so toggling between them is incremental.
test-cov: mcdc

ctest:
	ctest --test-dir $(TESTS_BUILD) --output-on-failure

# `make fuzz` -- build every libFuzzer harness (clang only; opt-in via
# RA_FUZZ=ON) and run each for ~30 seconds as a smoke check. Crash
# artefacts land in tests/build-fuzz/crashes/<target>/. For longer
# fuzz sessions on a single target, use scripts/utils/run_fuzz.sh.
# See docs/FUZZING.md.
FUZZ_BUILD    := $(TESTS_DIR)/build-fuzz
FUZZ_SECONDS  ?= 30
FUZZ_TARGETS  := fuzz_ra_jpeg_sw fuzz_ra_jpeg_sw_block fuzz_ra_epub fuzz_ra_modem_at \
                 fuzz_ra_net_arp fuzz_ra_net_ipv4 fuzz_ra_ble_att fuzz_ra_usb_pal \
                 fuzz_ra_tls fuzz_ra_canfd fuzz_ra_etha fuzz_ra_fs_fat
FUZZ_CC       ?= clang
FUZZ_CXX      ?= clang++

fuzz:
	@command -v $(FUZZ_CC) >/dev/null 2>&1 || { \
	  echo "ERROR: $(FUZZ_CC) not found -- libFuzzer requires clang."; exit 1; }
	$(CMAKE) -S $(TESTS_DIR) -B $(FUZZ_BUILD) \
	    -DRA_FUZZ=ON -DRA_COVERAGE=OFF \
	    -DCMAKE_C_COMPILER=$(FUZZ_CC) \
	    -DCMAKE_CXX_COMPILER=$(FUZZ_CXX) \
	    -DCMAKE_BUILD_TYPE=Debug
	$(CMAKE) --build $(FUZZ_BUILD) --target ra_fuzz_all -j
	@bash scripts/utils/init_fuzz_corpora.sh
	@for t in $(FUZZ_TARGETS); do \
	  echo "==== Running $$t for $(FUZZ_SECONDS)s ===="; \
	  mkdir -p $(FUZZ_BUILD)/crashes/$$t; \
	  $(FUZZ_BUILD)/fuzz/$$t \
	      tests/fuzz/corpus/$$t \
	      -max_total_time=$(FUZZ_SECONDS) \
	      -runs=10000 \
	      -print_final_stats=1 \
	      -artifact_prefix=$(FUZZ_BUILD)/crashes/$$t/ \
	    || { echo "FUZZ FAIL: $$t"; exit 1; }; \
	done
	@echo "All fuzz smoke runs passed."

docs:
	bash scripts/build_docs.sh

dashboard:
	python3 scripts/utils/roadmap_dashboard.py

coverage:
	bash scripts/utils/coverage_report.sh
	python3 scripts/utils/check_coverage.py

mcdc:
	bash scripts/utils/mcdc_report.sh

# MISRA-C 2012 audit (advisory; see docs/MISRA.md). Outputs:
#   build/misra/results.txt  -- one TSV violation per line
#   build/misra/raw.txt      -- raw cppcheck stderr
#   build/misra/misra-raw.txt-- raw misra.py stdout
# Not gated by pre-commit yet.
misra:
	bash scripts/utils/misra_check.sh

.PHONY: cppcheck build-all nsc-cmse-check
# `make cppcheck` -- local parity with the CI cppcheck gate.
cppcheck:
	bash scripts/cppcheck.sh

# `make nsc-cmse-check` -- compile every libs/ra_nsc veneer under -mcmse
# (TrustZone-on) so the Non-Secure-Callable trampolines stay buildable for the
# TZ HIL path (#54); local parity with the CI "NSC veneers (-mcmse)" job.
nsc-cmse-check:
	bash scripts/utils/check_nsc_cmse.sh

# `make build-all` -- cross-compile every firmware app (what CI's
# "Cross-build all apps" job runs); per-app logs in build/build_all_examples/.
build-all:
	bash scripts/build_all_examples.sh

# `make hooks` -- (re)install the tracked git hooks for this clone. This also
# runs automatically on every make invocation (see install-hooks.sh near the
# top); the explicit target is for a one-shot install right after cloning.
.PHONY: hooks
hooks:
	@$(ROOT)/scripts/git/install-hooks.sh
	@echo "git hooks active: core.hooksPath = $$(git config core.hooksPath)"

# `make mcp` -- self-test the Model Context Protocol dev server (tools/mcp).
# The server itself is launched by an MCP client over stdio (see .mcp.json and
# tools/mcp/README.md); this target exercises its dispatcher in-process so the
# protocol surface stays covered without a client or any hardware.
.PHONY: mcp
mcp:
	@python3 $(ROOT)/tools/mcp/ra8d2_mcp.py --selftest

# `make sim-<app> [PANEL=<name>]` -- cross-build the app, then boot its REAL .elf
# (the same binary that flashes to the board) on the Unicorn-based CPU emulator
# (tools/board_sim) and show the emulated panel live in a macOS window, sized by
# the display descriptor tools/board_sim/panels/<PANEL>.toml (default ek_ra8d2 --
# swap it to emulate a different screen). This is the single simulator: high
# fidelity (it exercises the genuine bring-up + peripheral-driver path), and for
# the chrome apps it doubles as the UI preview. Close the window to exit. e.g.
# `make sim-blink`, `make sim-lcd_color_cycle`, `make sim-ereader_ui`.
BOARD_SIM_DIR := $(ROOT)/tools/board_sim
# Panel descriptor used by `sim-<app>`: tools/board_sim/panels/<PANEL>.toml.
# `PANEL=<name>` is the documented knob; `SIM_PANEL` stays accepted as a
# back-compat spelling. Default ek_ra8d2.
SIM_PANEL ?= ek_ra8d2
PANEL     ?= $(SIM_PANEL)
.PHONY: $(RA_SIM)
$(RA_SIM): sim-%: %
	$(CMAKE) -B $(BOARD_SIM_DIR)/build -S $(BOARD_SIM_DIR)
	$(CMAKE) --build $(BOARD_SIM_DIR)/build -j
	$(BOARD_SIM_DIR)/build/board_sim $(RA_APP_DIR_$*)/build/$*.elf \
		--panel $(BOARD_SIM_DIR)/panels/$(PANEL).toml --view

# `make profile-<app>` -- run the app HEADLESS under the board_sim profiler and
# print the hottest functions on exit (Ozone-style). The default per-instruction
# mode gives exact instruction %, count and call count per function; MODE=1 is
# the cheaper wall-time sampler. PROFILE_ARGS passes extra board_sim flags, e.g.
# to profile a specific interaction rather than just boot:
#   make profile-ereader_ui                          # profile cold boot
#   make profile-ereader_ui PROFILE_ARGS="--click 250 250"   # boot + open a book
#   make profile-ereader_ui MODE=1                   # cheap wall-time sampler
# (The live --view window prints its breakdown too, but only after you close it.)
MODE         ?= full
PROFILE_ARGS ?=
RA_PROFILE   := $(addprefix profile-,$(RA_APPS))
.PHONY: $(RA_PROFILE)
$(RA_PROFILE): profile-%: %
	$(CMAKE) -B $(BOARD_SIM_DIR)/build -S $(BOARD_SIM_DIR)
	$(CMAKE) --build $(BOARD_SIM_DIR)/build -j
	BOARD_SIM_PROFILE=$(MODE) BOARD_SIM_IDLE_STOP=4000 BOARD_SIM_MAX_CHUNKS=8000000 \
		$(BOARD_SIM_DIR)/build/board_sim $(RA_APP_DIR_$*)/build/$*.elf $(PROFILE_ARGS)

# `make sim-help` -- usage, the PANEL knob, and the board_sim flag surface.
# Explicit target, so it wins over the sim-% static pattern rule above.
.PHONY: sim-help
sim-help:
	@echo "make sim-<app> [PANEL=<name>]  -- boot an app's REAL .elf on the board_sim"
	@echo "                                  Unicorn CPU emulator (tools/board_sim)"
	@echo ""
	@echo "  PANEL=<name>   display descriptor tools/board_sim/panels/<name>.toml (default ek_ra8d2)"
	@echo ""
	@echo "Examples:"
	@echo "  make sim-blink                       live board view (LEDs, USB/UART/IRQ sidebar)"
	@echo "  make sim-ereader_ui                  e-reader chrome UI preview"
	@echo "  make sim-lcd_color_cycle PANEL=ek_ra8d2"
	@echo ""
	@echo "Profile an app (Ozone-style hot-function breakdown, printed on exit):"
	@echo "  make profile-<app>                       per-instruction % / count / calls"
	@echo "  make profile-ereader_ui PROFILE_ARGS=\"--click 250 250\"   profile opening a book"
	@echo "  make profile-<app> MODE=1                cheap wall-time sampler"
	@echo ""
	@echo "Run board_sim directly for headless / scripted use (tools/board_sim/README.md):"
	@echo "  cd tools/board_sim && cmake -B build -S . && cmake --build build -j"
	@echo "  ./build/board_sim <app.elf>                  headless boot + MMIO report"
	@echo "  ./build/board_sim <app.elf> --view           live macOS window"
	@echo "  ./build/board_sim <app.elf> --ppm out.ppm    write the composite frame"
	@echo "  ./build/board_sim <app.elf> --panel <f.toml> | --size 480x272"
	@echo "  ./build/board_sim <app.elf> --input '<bytes>'      feed the console UART RX"
	@echo "  ./build/board_sim <app.elf> --usb-in '<bytes>'     feed the USB CDC bulk OUT"
	@echo "  ./build/board_sim <app.elf> --sd <img> | --sd-new 64:fat32 [--save-sd out.img]"
	@echo "  ./build/board_sim <app.elf> --trace-sym <fn> [--dump-sym <global>]"
	@echo ""
	@echo "Headless run-bounding env vars: BOARD_SIM_MAX_CHUNKS, BOARD_SIM_WALL_S,"
	@echo "  BOARD_SIM_IDLE_STOP=N, BOARD_SIM_USB_STOP=N, BOARD_SIM_USBH_STOP=N,"
	@echo "  BOARD_SIM_STOP_ON='<substr>'"

# ---------------------------------------------------------------------------
# E-reader chrome golden-image regression gate (issue #84).
#
# board_sim renders the ereader_ui framebuffer deterministically, so the
# Library + Reading chrome can be pinned with checked-in golden images
# (tests/golden/ereader_chrome/). `make ereader-golden` cross-builds the .elf,
# builds board_sim, and compares; `make ereader-golden-update` regenerates the
# goldens after an intentional chrome change. The same comparison runs in CI via
# scripts/board_sim_smoke.sh.
# ---------------------------------------------------------------------------
EREADER_GOLDEN_ELF := $(RA_APP_DIR_ereader_ui)/build/ereader_ui.elf
EREADER_GOLDEN_DIR := $(ROOT)/tests/golden/ereader_chrome
.PHONY: ereader-golden ereader-golden-update
ereader-golden: ereader_ui
	$(CMAKE) -B $(BOARD_SIM_DIR)/build -S $(BOARD_SIM_DIR)
	$(CMAKE) --build $(BOARD_SIM_DIR)/build -j
	python3 scripts/utils/ereader_golden.py check \
		--elf $(EREADER_GOLDEN_ELF) --board-sim $(BOARD_SIM_DIR)/build/board_sim \
		--golden-dir $(EREADER_GOLDEN_DIR) --out-dir /tmp/ereader_golden_out

ereader-golden-update: ereader_ui
	$(CMAKE) -B $(BOARD_SIM_DIR)/build -S $(BOARD_SIM_DIR)
	$(CMAKE) --build $(BOARD_SIM_DIR)/build -j
	python3 scripts/utils/ereader_golden.py update \
		--elf $(EREADER_GOLDEN_ELF) --board-sim $(BOARD_SIM_DIR)/build/board_sim \
		--golden-dir $(EREADER_GOLDEN_DIR)

ascii:
	@for dir in src libs tests; do \
		python3 scripts/utils/fix-encoding.py --check "$$dir" || exit 1; \
	done
	@for d in $(ROOT)/examples/*/*/main.c $(ROOT)/examples/*/*/*/main.c $(ROOT)/examples/*/*/*/*/main.c; do \
		[ -f "$$d" ] || continue; \
		python3 scripts/utils/fix-encoding.py --check "$$(dirname $$d)" || exit 1; \
	done

version:
	@echo "project VERSION: $$(cat VERSION)"
	@python3 scripts/utils/check-since-version.py --all

# ---------------------------------------------------------------------------
# HIL (Hardware-In-the-Loop) tests.
#
# These build the app locally, SCP the hex to the Pi, flash via J-Link,
# and verify expected UART output.  The Pi must be reachable at star.local
# with the EK-RA8D2 wired to its USB ports.  See scripts/hil_run.sh.
#
# Add one target per app as they are validated.  The target name is
# hil-<appname>; `make hil` runs all of them.
# ---------------------------------------------------------------------------
.PHONY: hil hil-help

hil:
	bash scripts/hil_dev.sh

hil-help:
	@echo "HIL -- hardware-in-the-loop (board on the Pi rig, driven over SSH; see scripts/hil_*.sh)"
	@echo ""
	@echo "  make hil                        full HIL suite from this machine (build+flash+verify)"
	@echo "  make hil-flash APP=<app>        build + flash to the Pi-attached board"
	@echo "  make hil-recover APP=<app>      recovery flash when the board is wedged"
	@echo "  make hil-flash-retry APP=<app>  power-cycle (uhubctl) then flash"
	@echo "  make hil-erase                  mass-erase the MRAM"
	@echo "  make hil-dlm-reset              recover from OEM_PL0/PL1 lockout"
	@echo "  make hil-probe                  quick J-Link + board diagnostic"
	@echo "  make hil-suite                  run the HIL test suite (on the Pi)"
	@echo "  make hil-all                    run the full HIL suite"
	@echo "  make hil-tapo CMD=<status|on|off|cycle>   board power via Tapo plug"
	@echo "  make hil-ppps CMD=<off|on|cycle [port]>   per-port USB power"

# Stack-usage proof. Builds every EVM-tier app (each is already
# compiled with -fstack-usage via cmake/ra_warnings.cmake), then runs
# the python aggregator over the resulting .su files. Prints the
# worst-10 stack frames across all apps; exits non-zero if any
# critical-path module (ra_isr, ra_check, ra_err, ra_mpu, ra_cgc,
# ra_pfs) breaches its 256-byte ceiling or contains a `dynamic` frame.
# See docs/STACK_USAGE.md.
stack-usage: $(EK_APPS)
	python3 scripts/utils/stack_usage_check.py --top 10

# Clang Static Analyzer (scan-build). Drives a clean host-test build
# under tests/build-scan/ with the analyzer interposed; emits HTML
# reports under build/scan-build-reports/. See docs/STATIC_ANALYSIS.md
# for the documented baseline of expected findings.
scan-build:
	bash scripts/utils/scan_build.sh

# Strict variant -- exit non-zero on any first-party finding after the
# documented suppressions (HAL MMIO accessor pattern, SOUP, host test
# scaffolding) are applied. This is the CI-gating entry point.
scan-build-strict:
	bash scripts/utils/scan_build.sh --strict

# Annotation-attribute enforcement. Walks the AST of every TU via
# libclang (python3 -m pip install --user --break-system-packages
# libclang) and applies the 19 ra_* rules documented in
# docs/ANNOTATIONS.md. `make check-annotations` runs in CI mode
# (exits non-zero on violation); the bare hook invocation defaults
# to warn-only via the WAVE_0_WARN_ONLY flag at the top of the script.
check-annotations:
	python3 scripts/utils/check_annotations.py --check

# include-what-you-use (IWYU). Reports per-TU include-graph hygiene
# violations: missing direct includes, unnecessary transitive
# includes, and forward-decl opportunities. Warn-only today; see
# scripts/utils/iwyu.sh.
iwyu:
	bash scripts/utils/iwyu.sh

# ---------------------------------------------------------------------------
# `make bench` -- build + run the host-side performance microbenchmarks.
# Opt-in via -DRA_BENCH=ON; lives in its own build tree so the fast
# unit-test build (tests/build/) is never invalidated. Each bench
# binary prints a CSV results row per measurement to stdout.
# See docs/PERFORMANCE.md.
# ---------------------------------------------------------------------------
BENCH_BUILD   := $(TESTS_DIR)/build-bench
BENCH_TARGETS := bench_ra_crc bench_ra_jpeg_sw bench_ra_gfx_text

bench:
	$(CMAKE) -S $(TESTS_DIR) -B $(BENCH_BUILD) \
	    -DRA_BENCH=ON -DRA_COVERAGE=OFF -DRA_MCDC=OFF \
	    -DCMAKE_BUILD_TYPE=Release \
	    -Wno-dev
	$(CMAKE) --build $(BENCH_BUILD) --target ra_bench_all -j
	@echo ""
	@echo "==== ra_bench results ===="
	@for t in $(BENCH_TARGETS); do \
	    echo "---- $$t ----"; \
	    $(BENCH_BUILD)/bench/$$t || { echo "FAIL: $$t"; exit 1; }; \
	done
	@echo "==== ra_bench done ===="

# ---------------------------------------------------------------------------
# `make app-sizes` -- run scripts/utils/app_sizes.py to summarise per-app
# .text/.data/.bss footprints across every built example. If no apps
# have been built yet the script prints a hint and exits cleanly.
# Writes docs/APP_SIZES.md when --write is passed.
# ---------------------------------------------------------------------------
app-sizes:
	python3 scripts/utils/app_sizes.py --write

# ---------------------------------------------------------------------------
# `make audit-init` -- per-app init-order linter. Walks each main.c and
# verifies the canonical CGC -> MSTP -> IOPORT -> peripheral ordering
# is respected. Writes docs/INIT_ORDER_AUDIT.md as a side-effect.
# Warn-only today; pass --strict to fail on first violation.
# ---------------------------------------------------------------------------
.PHONY: audit-init
audit-init:
	python3 scripts/utils/audit_init_order.py --report docs/INIT_ORDER_AUDIT.md

# ---------------------------------------------------------------------------
# Hardware -- remote HIL (board on the Pi rig, driven over SSH from this box)
# plus OpenOCD alternates. App-specific targets take APP=<name>; board-level
# ops take none. The app-specific UART/RTT verification probes are orchestrated
# by `make hil` / hil-suite / hil-all (each app needs its own expected output),
# so they are not exposed as individual targets here.
#
#   make hil-flash APP=blink     build + flash to the Pi-attached board
#   make hil-recover APP=blink   recovery flash when the board is wedged
#   make hil-flash-retry APP=blink  power-cycle (uhubctl) then flash
#   make hil-erase               mass-erase the MRAM
#   make hil-dlm-reset           recover from OEM_PL0/PL1 lockout
#   make hil-probe               quick J-Link + board diagnostic
#   make hil-suite / hil-all     run the HIL test suite (on the Pi)
#   make hil-tapo CMD=cycle      board power via Tapo plug (status|on|off|cycle)
#   make hil-ppps CMD=cycle      per-port USB power (off|on|cycle [port])
#   make flash-ocd APP=blink     flash via OpenOCD instead of J-Link
#   make debug-ocd APP=blink     gdb via OpenOCD
# ---------------------------------------------------------------------------
.PHONY: hil-flash hil-recover hil-flash-retry hil-erase hil-dlm-reset \
        hil-probe hil-suite hil-all hil-tapo hil-ppps flash-ocd debug-ocd

hil-flash:
	@test -n "$(APP)" || { echo "usage: make hil-flash APP=<app>"; exit 2; }
	bash scripts/hil_flash.sh $(APP)

hil-recover:
	@test -n "$(APP)" || { echo "usage: make hil-recover APP=<app>"; exit 2; }
	bash scripts/hil_recover.sh $(APP)

hil-flash-retry:
	@test -n "$(APP)" || { echo "usage: make hil-flash-retry APP=<app>"; exit 2; }
	bash scripts/hil_flash_retry.sh $(APP)

hil-erase:
	bash scripts/hil_erase.sh

hil-dlm-reset:
	bash scripts/hil_dlm_reset.sh

hil-probe:
	bash scripts/hil_probe.sh

hil-suite:
	bash scripts/hil_suite.sh

hil-all:
	bash scripts/hil_all.sh

hil-tapo:
	bash scripts/hil_tapo.sh $(or $(CMD),status)

hil-ppps:
	bash scripts/hil_ppps.sh $(or $(CMD),cycle)

flash-ocd:
	@test -n "$(APP)" || { echo "usage: make flash-ocd APP=<app>"; exit 2; }
	$(MAKE) $(APP)
	bash scripts/openocd_flash.sh $(RA_APP_DIR_$(APP))/build/$(APP).hex

debug-ocd:
	@test -n "$(APP)" || { echo "usage: make debug-ocd APP=<app>"; exit 2; }
	bash scripts/openocd_debug.sh $(RA_APP_DIR_$(APP))/build/$(APP).elf

all: format tidy test default
