# mk/hil.mk -- hardware: local J-Link flash/debug/ozone, remote Pi HIL, OpenOCD.
# The RA8_FLASH/RA8_DEBUG/RA8_OZONE app lists come from the top Makefile.
# Copyright (c) 2026 Brighton Sikarskie
# SPDX-License-Identifier: MIT

.PHONY: $(RA8_FLASH) $(RA8_DEBUG) $(RA8_OZONE) flash-help debug-help ozone-help \
        hil hil-help hil-flash hil-recover hil-flash-retry hil-erase hil-dlm-reset \
        hil-reflash hil-probe hil-find-jlink hil-suite hil-all hil-tapo hil-ppps \
        flash-ocd debug-ocd bench-status bench-selftest

# Local J-Link shorthands (board on THIS machine): build the app, then forward
# to the per-app Makefile (scripts/{flash,debug,ozone}.sh).
$(RA8_FLASH): flash-%: %
	$(MAKE) -C $(RA8_APP_DIR_$*) flash
$(RA8_DEBUG): debug-%: %
	$(MAKE) -C $(RA8_APP_DIR_$*) debug
$(RA8_OZONE): ozone-%: %
	$(MAKE) -C $(RA8_APP_DIR_$*) ozone

# Per-family help (explicit targets win over the flash-%/debug-%/ozone-% rules).
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

# Bench mutual exclusion (#497). One kernel flock on the bench host covers the
# whole target assembly -- board, C6, J-Link, hub ports, Tapo plug -- because
# none of those is physically separable from the others.
bench-status:
	@bash scripts/hil/bench.sh status

# The mechanism, proven rather than asserted. --ssh-death is the one that
# matters: it kills a real ssh CLIENT with SIGKILL and asserts the flock drops,
# because "a dead client releases the lock" is the claim the whole no-stale-lock
# property rests on and ssh does not reap remote payloads in general.
bench-selftest:
	bash scripts/hil/bench.sh selftest --ssh-death

# Remote HIL (board on the Pi rig, driven over SSH). See scripts/hil/.
hil:
	bash scripts/hil/dev.sh

hil-help:
	@echo "HIL -- hardware-in-the-loop (board on the Pi rig, driven over SSH; see scripts/hil/)"
	@echo ""
	@echo "  make bench-status               who holds the bench right now (0 free / 1 held / 3 unknown)"
	@echo "  make bench-selftest             prove the bench lock, including the ssh-death case"
	@echo ""
	@echo "  make hil                        full HIL suite from this machine (build+flash+verify)"
	@echo "  make hil-flash APP=<app>        build + flash to the Pi-attached board"
	@echo "  make hil-recover APP=<app>      recovery flash when the board is wedged"
	@echo "  make hil-flash-retry APP=<app>  power-cycle (uhubctl) then flash"
	@echo "  make hil-erase                  mass-erase the MRAM"
	@echo "  make hil-dlm-reset              recover from OEM_PL0/PL1 lockout"
	@echo "  make hil-probe                  quick J-Link + board diagnostic"
	@echo "  make hil-find-jlink             print connected J-Link serial(s) for .env"
	@echo "  make hil-suite                  run the HIL test suite (on the Pi)"
	@echo "  make hil-all                    run the full HIL suite"
	@echo "  make hil-tapo TARGET=<board|pi> CMD=<status|on|off|cycle>   board/Pi power via Tapo plug"
	@echo "  make hil-ppps CMD=<off|on|cycle [port]>   per-port USB power"

hil-flash:
	@test -n "$(APP)" || { echo "usage: make hil-flash APP=<app>"; exit 2; }
	bash scripts/hil/flash.sh $(APP)

hil-recover:
	@test -n "$(APP)" || { echo "usage: make hil-recover APP=<app>"; exit 2; }
	bash scripts/hil/recover.sh $(APP)

hil-flash-retry:
	@test -n "$(APP)" || { echo "usage: make hil-flash-retry APP=<app>"; exit 2; }
	bash scripts/hil/flash_retry.sh $(APP)

hil-erase:
	bash scripts/hil/erase.sh

hil-dlm-reset:
	bash scripts/hil/dlm_reset.sh

hil-reflash:
	@test -n "$(APP)" || { echo "usage: make hil-reflash APP=<app>"; exit 2; }
	bash scripts/hil/reflash.sh $(APP)

hil-probe:
	bash scripts/hil/probe.sh

hil-find-jlink:
	bash scripts/hil/find_jlink.sh

hil-suite:
	bash scripts/hil/suite.sh

hil-all:
	bash scripts/hil/all.sh

hil-tapo:
	bash scripts/hil/tapo.sh $(or $(TARGET),board) $(or $(CMD),status)

hil-ppps:
	bash scripts/hil/ppps.sh $(or $(CMD),cycle)

flash-ocd:
	@test -n "$(APP)" || { echo "usage: make flash-ocd APP=<app>"; exit 2; }
	$(MAKE) $(APP)
	bash scripts/dev/openocd_flash.sh $(RA8_APP_DIR_$(APP))/build/$(APP).hex

debug-ocd:
	@test -n "$(APP)" || { echo "usage: make debug-ocd APP=<app>"; exit 2; }
	bash scripts/dev/openocd_debug.sh $(RA8_APP_DIR_$(APP))/build/$(APP).elf
