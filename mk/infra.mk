# mk/infra.mk -- the rig itself: dev box, CI runner hosts, the bench, the vault.
#
# Thin wrappers over scripts/dev/infra.sh so provisioning is discoverable from
# `make help` rather than being a bare `ansible-playbook` invocation known only
# to whoever wrote the role. The rationale for each host class lives in the role
# it deploys; the estate as a whole is docs/INFRASTRUCTURE.md.
#
# Copyright (c) 2026 Brighton Sikarskie
# SPDX-License-Identifier: MIT

.PHONY: infra-help infra-setup infra-list infra-doctor infra-status \
        infra-check infra-apply infra-remove

infra-help:
	@echo "INFRASTRUCTURE -- provision the machines the project runs on"
	@echo ""
	@echo "  make infra-list          host classes, their playbooks and roles"
	@echo "  make infra-doctor        can THIS machine drive infra at all?"
	@echo "  make infra-status        what is deployed across the estate, right now"
	@echo ""
	@echo "  make infra-setup         first-run onboarding: inventory + credentials"
	@echo "  make infra-check HOST=x  DRY RUN -- report what would change, change nothing"
	@echo "  make infra-apply HOST=x  provision for real"
	@echo "  make infra-remove HOST=x tear down (only classes that implement it)"
	@echo ""
	@echo "  HOST is a class from 'make infra-list', not a hostname:"
	@echo "    dev | k3s | ci-runner | ci-runner-docker | bench"
	@echo ""
	@echo "  infra-list / infra-doctor / infra-status are READ-ONLY and safe to run"
	@echo "  at any time. infra-apply and infra-remove are not -- dry-run first."
	@echo ""
	@echo "  The whole estate, and how to rebuild each machine from nothing:"
	@echo "    docs/INFRASTRUCTURE.md"

# First-run onboarding for a fresh clone: prerequisites, the git-ignored
# inventory and token, then an optional deploy. See infra/README.md.
infra-setup:
	@bash $(ROOT)/infra/bootstrap.sh

infra-list:
	@bash $(ROOT)/scripts/dev/infra.sh list

infra-doctor:
	@bash $(ROOT)/scripts/dev/infra.sh doctor

infra-status:
	@bash $(ROOT)/scripts/dev/infra.sh status

# make infra-check HOST=dev -- always do this before infra-apply.
infra-check:
	@if [ -z "$(HOST)" ]; then \
	  echo "usage: make infra-check HOST=<class>   ('make infra-list' for the classes)" >&2; \
	  exit 2; \
	fi
	@bash $(ROOT)/scripts/dev/infra.sh check "$(HOST)"

infra-apply:
	@if [ -z "$(HOST)" ]; then \
	  echo "usage: make infra-apply HOST=<class>   ('make infra-list' for the classes)" >&2; \
	  exit 2; \
	fi
	@bash $(ROOT)/scripts/dev/infra.sh apply "$(HOST)"

infra-remove:
	@if [ -z "$(HOST)" ]; then \
	  echo "usage: make infra-remove HOST=<class>  ('make infra-list' for the classes)" >&2; \
	  exit 2; \
	fi
	@bash $(ROOT)/scripts/dev/infra.sh remove "$(HOST)"
