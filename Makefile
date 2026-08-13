# SPDX-License-Identifier: MIT
# Copyright (c) 2026 Brighton Sikarskie
#
# Top-level Makefile for ra8-firmware.
#
# This file is the WIRING layer: it defines the shared variables (app discovery,
# tool directories, tool paths) and the `help` target, then `include`s the
# per-area rule modules under mk/. The rules themselves live in:
#
#   mk/apps.mk     firmware app discovery + build (`make <app>`, apps, clean)
#   mk/emu.mk      ra8_emulator (emu-<app>, profile-<app>, ereader-gui, eil)
#   mk/tools.mk    host developer tools (media_dl, ra8_viewer, `make tools`, ...)
#   mk/hil.mk      hardware: flash/debug/ozone (J-Link), remote Pi HIL, OpenOCD
#   mk/quality.mk  format/tidy/cppcheck/test/coverage/misra/scan-build/ci/...
#   mk/docs.mk     docs/dashboard/app-sizes/audit-init
#
# Each app lives in examples/<tier>/<app>/ with its own main.c, boot files, and
# CMakeLists.txt; the tier dir is organisational, so `make <app>` works
# regardless of tier. Run `make help` for the grouped target reference.
#
# Common targets:
#   make            -- build the default app ($(RA8_DEFAULT_APP))
#   make <app>      -- build a cross-compiled firmware app, e.g. `make blink_hal`
#   make apps       -- list every discovered firmware app
#   make emu-<app>  -- boot an app's REAL .elf on the Unicorn CPU emulator
#   make tools      -- build every host developer tool
#   make help       -- grouped reference of every top-level target
#
#

SHELL := /bin/bash

# `make` with no target builds the default app.
.DEFAULT_GOAL := default

# --- shared paths + tools ----------------------------------------------------
ROOT              := $(abspath .)
TESTS_DIR         := $(ROOT)/tests
TESTS_BUILD       := $(TESTS_DIR)/build
TESTS_BUILD_COV   := $(TESTS_DIR)/build-cov
TESTS_BUILD_UBSAN := $(TESTS_DIR)/build-ubsan
# Legacy tidy build dir kept so `bash scripts/checks/clang_tidy.sh` keeps working.
TIDY_BUILD        := $(ROOT)/build/tidy

CMAKE        ?= cmake
CLANG_FORMAT ?= clang-format
DOXYGEN      ?= doxygen
ARM_SIZE     ?= arm-none-eabi-size

# Host tool directories (referenced by mk/emu.mk, mk/tools.mk, and clean).
RA8_EMU_DIR  := $(ROOT)/tools/ra8_emulator
MEDIA_DL_DIR   := $(ROOT)/tools/media_dl
RA8_VIEWER_DIR := $(ROOT)/tools/rabook_viewer

# --- git hooks: active for every clone, every make invocation ----------------
# A fresh clone has no hooks in .git/hooks, so the commit-msg / pre-commit /
# pre-push gates would be silently skipped. Point core.hooksPath at the tracked
# scripts/git/ hooks on every make run -- idempotent, prints only the first time.
_RA8_HOOKS_MSG := $(shell $(ROOT)/scripts/git/install-hooks.sh 2>/dev/null)
$(if $(_RA8_HOOKS_MSG),$(info $(_RA8_HOOKS_MSG)))

# Default app -- override on the command line, e.g. `make RA8_DEFAULT_APP=blink_hal`.
RA8_DEFAULT_APP ?= ra8d2-ereader

# --- app discovery -----------------------------------------------------------
# Every examples/<tier>/<app>/ (up to 4 deep) with a main.c. RA8_APPS holds the
# bare names; RA8_APP_DIR_<app> resolves each to its full per-app directory.
_RA8_APP_MAINS := $(wildcard $(ROOT)/examples/*/*/main.c) \
                 $(wildcard $(ROOT)/examples/*/*/*/main.c) \
                 $(wildcard $(ROOT)/examples/*/*/*/*/main.c)
RA8_APPS       := $(sort $(notdir $(patsubst %/main.c,%,$(_RA8_APP_MAINS))))
$(foreach m,$(_RA8_APP_MAINS),$(eval RA8_APP_DIR_$(notdir $(patsubst %/main.c,%,$m)) := $(patsubst %/main.c,%,$m)))

# Register the main e-reader application target manually (src/app, not examples/).
RA8_APPS       += ra8d2-ereader
RA8_APP_DIR_ra8d2-ereader := $(ROOT)/src/app

# Per-app shorthand target lists.
RA8_FLASH := $(addprefix flash-,$(RA8_APPS))
RA8_DEBUG := $(addprefix debug-,$(RA8_APPS))
RA8_OZONE := $(addprefix ozone-,$(RA8_APPS))
RA8_MONITOR := $(addprefix monitor-,$(RA8_APPS))
RA8_EMU   := $(addprefix emu-,$(RA8_APPS))
# The e-reader / dual-core / tz-threadx runs need dedicated Debug recipes (see
# mk/emu.mk); drop them from the generic single-image emulator rule.
RA8_EMU_GENERIC := $(filter-out emu-ra8d2-ereader emu-dualcore_mailbox emu-tz_threadx_demo,$(RA8_EMU))
RA8_PROFILE := $(addprefix profile-,$(RA8_APPS))

# hw_validated apps -- smoke + stack-usage sweeps run over this set only.
_EK_APP_MAINS := $(wildcard $(ROOT)/examples/ek_ra8d2/hw_validated/*/main.c) \
                 $(wildcard $(ROOT)/examples/ek_ra8d2/hw_validated/*/*/main.c)
EK_APPS       := $(sort $(notdir $(patsubst %/main.c,%,$(_EK_APP_MAINS))))

# clangd reads build/compile_commands.json; regenerate only when a CMake input
# changes. Only hand-written source CMakeLists.txt are real inputs (exclude
# transient build/ TryCompile files that races would otherwise glob in).
RA8_COMPILE_COMMANDS := $(ROOT)/build/compile_commands.json
_RA8_CMAKE_INPUTS := $(ROOT)/CMakeLists.txt $(wildcard $(ROOT)/cmake/*.cmake) \
	$(shell find $(ROOT)/examples -name CMakeLists.txt -not -path '*/build/*' 2>/dev/null)

.PHONY: help hooks all

help:
	@echo "ra8-firmware make targets   ($(words $(RA8_APPS)) firmware apps -- 'make apps' for the list)"
	@echo ""
	@echo "BUILD"
	@echo "  make                   build the default app ($(RA8_DEFAULT_APP))"
	@echo "  make <app>             cross-compile one firmware app, e.g. make blink"
	@echo "  make build-all         cross-compile every firmware app (CI's cross-build job)"
	@echo "  make clean             remove every app build dir, tests/build, and tool builds"
	@echo "  make compile_commands  regenerate build/compile_commands.json for clangd"
	@echo ""
	@echo "RUN / PREVIEW / EMULATE  (no board needed -- see 'make apps')           [make emu-help]"
	@echo "  make emu-setup              one-command macOS emulator dependency setup"
	@echo "  make emu-<app> [PANEL=ek_ra8d2]  boot an app's REAL .elf on the Unicorn CPU"
	@echo "                             emulator, live panel/UI window (tools/ra8_emulator)"
	@echo "  make ereader-gui           full hybrid e-reader GUI: baked + SD books on a live window"
	@echo "  make eil [EIL_JOBS=N]      EIL: boot EVERY hil app in ra8_emulator headless + assert"
	@echo "  make emu-matrix            boot EVERY example in ra8_emulator (the coverage matrix)"
	@echo "  make emu-matrix-triage     group the last sweep's failures by cause"
	@echo "  make emu-matrix-baseline   re-baseline the matrix ratchet after burning debt down"
	@echo "                             its hil.conf expectation, in parallel (no board). CI-gated."
	@echo ""
	@echo "READER TOOLS  (host-native, not firmware -- macOS)                      [make tools-help]"
	@echo "  make tools             build every compiled host tool"
	@echo "  make media_dl          build the comic/manga/manhwa downloader CLI (tools/media_dl)"
	@echo "  make dl ARGS='...'     build + run the downloader with ARGS (e.g. --format cbz)"
	@echo "  make test-media_dl     build + run the downloader unit tests (ctest)"
	@echo "  make test-integration  pack synthetic pages in EVERY format + view each (end-to-end gate)"
	@echo "  make viewer            build the native reader viewer (tools/rabook_viewer)"
	@echo "  make view FILE=<doc>   open a document in the viewer (arrows page; HEADLESS=1 dumps a PPM)"
	@echo ""
	@echo "HARDWARE -- flash / debug / monitor (board on THIS machine)  [make flash-help / debug-help / ozone-help / monitor-help]"
	@echo "  make flash-<app>       build + flash an app  (e.g. make flash-blink)"
	@echo "  make debug-<app>       build + gdb via J-Link"
	@echo "  make ozone-<app>       build + open in Ozone"
	@echo "  make monitor           open live serial console stream for attached board"
	@echo "  make monitor-<app>     build + flash <app>, then open live serial console"
	@echo ""
	@echo "HIL -- hardware-in-the-loop (board on the Pi rig, driven over SSH)        [make hil-help]"
	@echo "  make hil               full HIL suite from this machine (build+flash+verify)"
	@echo "  make hil-flash APP=<app>     build + flash to the Pi-attached board"
	@echo ""
	@echo "AGENT WORKSPACES -- isolated checkouts on a shared verification box  (Linux)"
	@echo "  make ws-new NAME=x     isolated git-worktree workspace (build output stays in the container)"
	@echo "  make ws-free NAME=x    release it"
	@echo "  make ws-list / ws-doctor     what exists / environment check"
	@echo "  make ci-status         CI verdict from the shared monitor (costs no GitHub API quota)"
	@echo ""
	@echo "QUALITY / CI -- gate bodies live ONLY in scripts/ci.sh; CI runs the same ones"
	@echo "  make ci                run ALL CI gates in the Linux devcontainer (before every push)"
	@echo "  make ci-fast           same, minus the slow misra + clang-tidy + coverage gates"
	@echo "  make ci-native         run ALL gates natively -- no container runtime (the Linux path)"
	@echo "  make ci-native-fast    same, minus the slow gates"
	@echo "  make ci-list           print the gate registry (name / speed class / description)"
	@echo "  make ci-gate GATE=<n>  run exactly ONE gate -- the invocation CI itself uses"
	@echo "  make ci-gate-container GATE=<n>  the same gate, but inside the toolchain image"
	@echo "  make format / check    run clang-format in place / --dry-run"
	@echo "  make tidy / cppcheck   run clang-tidy / the cppcheck gate"
	@echo "  make ascii / version   encoding check / @since-tag check"
	@echo "  make test              host-compile + run unit tests (tests/build/)"
	@echo "  make test-cov / ubsan  MC/DC coverage / UBSan host runs"
	@echo "  make coverage / mcdc   lcov HTML / DO-178C Level B MC/DC report"
	@echo "  make misra[-check]     MISRA-C 2012 audit [+ baseline ratchet]"
	@echo "  make scan-build / iwyu clang static analyzer / include-what-you-use"
	@echo "  make fuzz / bench      libFuzzer smoke / host microbenchmarks"
	@echo "  make stack-usage       build EVM apps + aggregate -fstack-usage report"
	@echo ""
	@echo "DOCS / REPORTS"
	@echo "  make docs              generate doxygen HTML into build/docs/html/"
	@echo "  make docs-push         build + publish doxygen HTML to the gh-pages branch"
	@echo "  make dashboard         regenerate docs/ROADMAP_DASHBOARD.md + docs/badges/"
	@echo "  make app-sizes         summarise per-app .text/.data/.bss footprints"
	@echo "  make audit-init        per-app init-order audit (docs/INIT_ORDER_AUDIT.md)"
	@echo ""
	@echo "INFRASTRUCTURE -- the machines themselves (dev box, runners, bench)   [make infra-help]"
	@echo "  make infra-list        host classes, their playbooks and roles"
	@echo "  make infra-doctor      can THIS machine drive infra at all?"
	@echo "  make infra-status      what is deployed across the estate, right now (read-only)"
	@echo "  make infra-check HOST=x  DRY RUN a provision; infra-apply does it for real"
	@echo "  make infra-ssh-config  make THIS machine a control node (~/.ssh aliases)"
	@echo "  make infra-setup       first-run onboarding (inventory + credentials)"
	@echo "                         the whole estate: docs/INFRASTRUCTURE.md"
	@echo ""
	@echo "DEV SETUP / DISCOVERY"
	@echo "  make hooks             (re)install the tracked git hooks (auto-runs on every make)"
	@echo "  make apps              list every discovered firmware app"
	@echo "  make mcp               self-test the MCP dev server (tools/mcp; see .mcp.json)"
	@echo "  make help              this grouped reference"

# `make hooks` -- (re)install the tracked git hooks for this clone (also runs
# automatically on every make invocation via install-hooks.sh above).
hooks:
	@$(ROOT)/scripts/git/install-hooks.sh
	@echo "git hooks active: core.hooksPath = $$(git config core.hooksPath)"

# `make all` -- the local pre-commit meta-target.
all: format tidy test default

# --- rule modules ------------------------------------------------------------
include $(ROOT)/mk/apps.mk
include $(ROOT)/mk/emu.mk
include $(ROOT)/mk/tools.mk
include $(ROOT)/mk/hil.mk
include $(ROOT)/mk/quality.mk
include $(ROOT)/mk/docs.mk
include $(ROOT)/mk/workspace.mk
include $(ROOT)/mk/infra.mk
