# mk/workspace.mk -- per-agent isolated workspaces on a shared verification box.
#
# Thin wrappers over scripts/agent_workspace.sh so the lifecycle is discoverable
# from `make help` rather than being tribal knowledge. See
# recon/design/AGENT_VERIFICATION_INFRA.md for the full rationale.
#
# Copyright (c) 2026 Brighton Sikarskie
# SPDX-License-Identifier: MIT

.PHONY: ws-new ws-free ws-list ws-reap ws-doctor ws-timer ci-status ci-quota

# make ws-new NAME=my-task [REF=origin/dev]
ws-new:
	@if [ -z "$(NAME)" ]; then \
	  echo "usage: make ws-new NAME=<name> [REF=<ref>]" >&2; exit 2; \
	fi
	@bash $(ROOT)/scripts/agent_workspace.sh create "$(NAME)" $(if $(REF),"$(REF)",)

# make ws-free NAME=my-task
ws-free:
	@if [ -z "$(NAME)" ]; then \
	  echo "usage: make ws-free NAME=<name>" >&2; exit 2; \
	fi
	@bash $(ROOT)/scripts/agent_workspace.sh release "$(NAME)"

ws-list:
	@bash $(ROOT)/scripts/agent_workspace.sh list

# Normally unnecessary: reaping runs from a systemd timer and on every ws-new.
# This target exists for when you want the space back right now.
ws-reap:
	@bash $(ROOT)/scripts/agent_workspace.sh reap $(if $(FORCE),--force,)

ws-doctor:
	@bash $(ROOT)/scripts/agent_workspace.sh doctor

ws-timer:
	@bash $(ROOT)/scripts/agent_workspace.sh install-timer

# CI status from the shared monitor -- costs no GitHub API quota, unlike
# `gh run watch`. Exit codes: 0 PASS, 1 FAIL, 3 UNKNOWN.
ci-status:
	@bash $(ROOT)/scripts/ci_monitor.sh status

ci-quota:
	@bash $(ROOT)/scripts/ci_monitor.sh quota
