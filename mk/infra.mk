# mk/infra.mk -- the rig itself: dev box, CI runner hosts, the bench, the vault.
#
# Thin wrappers over scripts/dev/infra.sh so provisioning is discoverable from
# `make help` rather than being a bare `ansible-playbook` invocation known only
# to whoever wrote the role. What each machine IS and how much of it CI may use
# is declared in infra/fleet.yml; the runbooks are docs/CI_FLEET.md and the
# estate as a whole is docs/INFRASTRUCTURE.md.
#
# Copyright (c) 2026 Brighton Sikarskie
# SPDX-License-Identifier: MIT

.PHONY: infra-help infra-setup infra-list infra-doctor infra-status \
        infra-check infra-apply infra-remove infra-scale infra-show

infra-help:
	@echo "INFRASTRUCTURE -- the machines the project runs on"
	@echo ""
	@echo "  ONE declaration: infra/fleet.yml. A machine is a block in it; how"
	@echo "  much of a machine CI may use is a number in that block."
	@echo ""
	@echo "  make infra-list          what is declared, and how it is sized"
	@echo "  make infra-show HOST=x   one machine in full, with its derived vars"
	@echo "  make infra-doctor        can THIS machine drive infra at all?"
	@echo "  make infra-status        what every host is running, right now"
	@echo ""
	@echo "  make infra-setup         first-run onboarding: inventory + credentials"
	@echo "  make infra-check HOST=x  DRY RUN -- report what would change, change nothing"
	@echo "  make infra-apply HOST=x  converge that machine to the declaration"
	@echo "  make infra-remove HOST=x tear down (classes whose roles implement it)"
	@echo ""
	@echo "  make infra-scale HOST=x N=n   change how many runners a host is"
	@echo "                                running RIGHT NOW. Shrinking DRAINS:"
	@echo "                                an instance is stopped only once it"
	@echo "                                is idle, never while it holds a job."
	@echo ""
	@echo "  HOST is a machine from 'make infra-list':"
	@echo "    k3s-pve | truenas | win-ci | dev | star"
	@echo ""
	@echo "  infra-list / infra-show / infra-doctor / infra-status are READ-ONLY"
	@echo "  and safe at any time. The rest are not -- dry-run first."
	@echo ""
	@echo "  Add a machine, retune one, declare quiet hours, remove one:"
	@echo "    docs/CI_FLEET.md"

# First-run onboarding for a fresh clone: prerequisites, the generated
# inventory and the token. See infra/README.md.
infra-setup:
	@bash $(ROOT)/infra/bootstrap.sh

infra-list:
	@bash $(ROOT)/scripts/dev/infra.sh list

infra-doctor:
	@bash $(ROOT)/scripts/dev/infra.sh doctor

infra-status:
	@bash $(ROOT)/scripts/dev/infra.sh status

infra-show:
	@if [ -z "$(HOST)" ]; then \
	  echo "usage: make infra-show HOST=<host>   ('make infra-list' for the hosts)" >&2; \
	  exit 2; \
	fi
	@python3 $(ROOT)/scripts/dev/fleet.py show "$(HOST)"

# make infra-check HOST=truenas -- always do this before infra-apply.
infra-check:
	@if [ -z "$(HOST)" ]; then \
	  echo "usage: make infra-check HOST=<host>   ('make infra-list' for the hosts)" >&2; \
	  exit 2; \
	fi
	@bash $(ROOT)/scripts/dev/infra.sh check "$(HOST)"

infra-apply:
	@if [ -z "$(HOST)" ]; then \
	  echo "usage: make infra-apply HOST=<host>   ('make infra-list' for the hosts)" >&2; \
	  exit 2; \
	fi
	@bash $(ROOT)/scripts/dev/infra.sh apply "$(HOST)"

infra-remove:
	@if [ -z "$(HOST)" ]; then \
	  echo "usage: make infra-remove HOST=<host>  ('make infra-list' for the hosts)" >&2; \
	  exit 2; \
	fi
	@bash $(ROOT)/scripts/dev/infra.sh remove "$(HOST)"

# A LIVE capacity change: no re-provisioning, no downtime for the instances
# that stay. Shrinking drains -- it waits for an instance to go idle and stops
# it only then, because the runner cancels its in-flight job on SIGTERM.
infra-scale:
	@if [ -z "$(HOST)" ] || [ -z "$(N)" ]; then \
	  echo "usage: make infra-scale HOST=<host> N=<instances>" >&2; \
	  exit 2; \
	fi
	@bash $(ROOT)/scripts/dev/infra.sh scale "$(HOST)" "$(N)"
