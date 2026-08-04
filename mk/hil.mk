# SPDX-License-Identifier: MIT
# Copyright (c) 2026 Brighton Sikarskie
# mk/hil.mk -- hardware: local J-Link flash/debug/ozone, remote Pi HIL, OpenOCD.
# The RA8_FLASH/RA8_DEBUG/RA8_OZONE app lists come from the top Makefile.

.PHONY: $(RA8_FLASH) $(RA8_DEBUG) $(RA8_OZONE) flash-help debug-help ozone-help \
        hil hil-help hil-flash hil-recover hil-flash-retry hil-erase hil-dlm-reset \
        hil-reflash hil-probe hil-find-jlink hil-all hil-c6 hil-tapo hil-ppps \
        flash-ocd debug-ocd bench-status bench-selftest bench-hold bench-free \
        bench-extend bench-take bench-log bench-doctor bench-contention

# WAIT= queues for a bench somebody else holds instead of failing on the spot.
# Every guarded HIL target below inherits it, because ra8_bench_require reads it
# out of the environment. Unset means 0, i.e. fail fast: the right default for a
# human, the wrong one for CI and for an unattended agent, which is why saying
# `WAIT=10m` has to be this cheap.
#
# Passed through VERBATIM -- "10m", "2h", "900s" -- and parsed on the shell side
# by bench_duration(), which already exists and is already exercised by every
# other duration flag. Converting it here with $(shell ... case ... esac) is what
# this line used to do, and it does not work: make ends a $(shell) at the first
# unmatched `)`, so the `*h|*H)` arm terminated the expansion and
# RA8_BENCH_WAIT_S was set to the REST OF THE CASE STATEMENT. The guard then read
# a non-numeric wait, fell back to flock -n, and WAIT= silently did nothing.
ifneq ($(WAIT),)
export RA8_BENCH_WAIT := $(WAIT)
endif

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

# A human hold is a SHELL by default, not a lease: close it and the bench comes
# back, so a laptop that dies cannot leave the board held. FOR= is optional here
# and mandatory for the detached form, which is the only shape nothing but a
# budget bounds.
bench-hold:
	@test -n "$(WHY)" || { echo 'usage: make bench-hold WHY="what you are doing" [FOR=2h]'; exit 2; }
	bash scripts/hil/bench.sh hold --why "$(WHY)" $(if $(FOR),--for $(FOR),) $(if $(DETACHED),--detached,)

bench-free:
	@bash scripts/hil/bench.sh free

bench-extend:
	@test -n "$(FOR)" || { echo "usage: make bench-extend FOR=30m"; exit 2; }
	@bash scripts/hil/bench.sh extend --for $(FOR)

# Preempting somebody is journaled, always. Taking it from a HUMAN additionally
# needs CONFIRM=someone-may-be-at-the-bench, because they may be standing there
# with a probe on a test point and nothing here can see that.
bench-take:
	@test -n "$(WHY)" || { echo 'usage: make bench-take WHY="why you are taking it" [CONFIRM=...]'; exit 2; }
	@bash scripts/hil/bench.sh take --reason "$(WHY)" $(if $(FOR),--for $(FOR),)

bench-log:
	@bash scripts/hil/bench.sh log $(or $(N),25)

bench-doctor:
	@bash scripts/hil/bench.sh doctor

# The lock, proven under REAL contention from several independent machines
# rather than against itself. Needs RA8_BENCH_ACTORS in .env (see .env.example)
# and hardware; the verdict is decided from the bench host's own view of what
# touched the board, not from what the actors claim they did.
bench-contention:
	bash scripts/hil/bench_contention.sh $(if $(PHASE),--phase $(PHASE),) \
	  $(if $(ROUNDS),--rounds $(ROUNDS),) $(if $(VICTIM),--victim $(VICTIM),) \
	  $(if $(WAIT),--wait $(WAIT),)

# Remote HIL (board on the Pi rig, driven over SSH). See scripts/hil/.
hil:
	bash scripts/hil/dev.sh

hil-help:
	@echo "HIL -- hardware-in-the-loop (board on the Pi rig, driven over SSH; see scripts/hil/)"
	@echo ""
	@echo "BENCH -- one actor at a time on the physical board (#497)"
	@echo "  make bench-status               who holds it (exit 0 free / 1 held / 3 unknown)"
	@echo "  make bench-hold WHY=\"...\"        a shell that holds the bench; exit it to release"
	@echo "  make bench-free                 give back your hold"
	@echo "  make bench-extend FOR=30m       push your own budget out"
	@echo "  make bench-take WHY=\"...\"        preempt (journaled; CONFIRM= to take from a human)"
	@echo "  make bench-log [N=25]           tail the audit journal"
	@echo "  make bench-doctor               state dir, perms, inode, clock, sshd bound"
	@echo "  make bench-selftest             prove the lock, including the ssh-death case"
	@echo "  make bench-contention           prove it under real contention from several machines"
	@echo "  ... WAIT=10m                    queue for a busy bench instead of failing at once"
	@echo ""
	@echo "  make hil                        full HIL suite from this machine (build+flash+verify)"
	@echo "  make hil-flash APP=<app>        build + flash to the Pi-attached board"
	@echo "  make hil-recover APP=<app>      recovery flash when the board is wedged"
	@echo "  make hil-flash-retry APP=<app>  power-cycle (uhubctl) then flash"
	@echo "  make hil-erase                  mass-erase the MRAM"
	@echo "  make hil-dlm-reset              recover from OEM_PL0/PL1 lockout"
	@echo "  make hil-probe                  quick J-Link + board diagnostic"
	@echo "  make hil-find-jlink             print connected J-Link serial(s) for .env"
	@echo "  make hil-all                    run the full HIL suite"
	@echo "  make hil-c6 [APP=<app>]         run the ESP32-C6 lane (needs SW4 1=OFF 2=OFF 3=ON 4=OFF"
	@echo "                                  and the C6 harness on J26 -- see the tier README)"
	@echo "  make hil-tapo TARGET=<board|pi|relay> CMD=<status|on|off|cycle>   board/Pi/relay power via Tapo plug"
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

# There is exactly ONE list of HIL apps, and it is the filesystem:
# hil_discover_apps() in scripts/hil/lib/hil_conf.sh, shared with the EIL
# suite. `make hil-suite` used to run a second HIL runner script, which carried a
# SECOND, hand-maintained table of 18 apps against the 151 that are actually
# discoverable -- so the default suite silently tested an eighth of the tree
# and every app added since had to be remembered into a list nobody knew
# existed. That file is deleted; this is the suite.
hil-all:
	bash scripts/hil/all.sh

# The ESP32-C6 lane. Same runner, different tier: these apps need SW4-4 OFF,
# which takes the Arduino and mikroBUS connectors off the board for every app
# in the default pass, so the two cannot share one run of the bench. They are
# also invisible to the EIL suite because ra8_emulator models no ESP32-C6
# (#494) -- which is why they live under hw_validated/c6/ rather than
# hw_validated/hil/, where the parity gate would rightly demand EIL coverage.
hil-c6:
	bash scripts/hil/all.sh --dir examples/ek_ra8d2/hw_validated/c6 $(if $(APP),--only $(APP),)

hil-tapo:
	bash scripts/hil/tapo.sh $(or $(TARGET),board) $(or $(CMD),status)

# No default verb. `make hil-ppps` used to default to `cycle`, so a bare
# invocation -- a tab-completion, a copy-paste, a guess -- cut USB port power
# on a live board. A destructive action does not get to be the default.
hil-ppps:
	@test -n "$(CMD)" || { echo "usage: make hil-ppps CMD=<off|on|cycle> [PORT=<n>]"; \
	  echo "       (no default: 'cycle' cuts power to a port that may be in use)"; exit 2; }
	bash scripts/hil/ppps.sh $(CMD) $(PORT)

flash-ocd:
	@test -n "$(APP)" || { echo "usage: make flash-ocd APP=<app>"; exit 2; }
	$(MAKE) $(APP)
	bash scripts/dev/openocd_flash.sh $(RA8_APP_DIR_$(APP))/build/$(APP).hex

debug-ocd:
	@test -n "$(APP)" || { echo "usage: make debug-ocd APP=<app>"; exit 2; }
	bash scripts/dev/openocd_debug.sh $(RA8_APP_DIR_$(APP))/build/$(APP).elf
