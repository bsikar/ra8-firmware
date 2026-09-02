# SPDX-License-Identifier: MIT
# Copyright (c) 2026 Brighton Sikarskie
#

set working-directory := ".."

# Show agent workspace help menu
default:
    @echo "WS - Isolated Git Agent Workspaces & CI Status"
    @echo ""
    @echo "COMMANDS:"
    @echo "  just workspace::new <name>                  Create isolated worktree workspace"
    @echo "  just workspace::new <name> ref=<commit>     Create workspace branched from git ref"
    @echo "  just workspace::free <name>                 Delete and release an agent workspace"
    @echo "  just workspace::list                        List all active agent workspaces"
    @echo "  just workspace::reap                        Prune stale/orphaned workspaces"
    @echo "  just workspace::doctor                      Check workspace environment sanity"
    @echo "  just workspace::status                      Query CI status from shared monitor"
    @echo "  just workspace::quota                       Query remaining GitHub API quota"
    @echo ""

# Create isolated agent git-worktree workspace on shared verification box
new name ref="":
    #!/bin/bash -p
    set -euo pipefail
    extra=()
    [ -n "{{ ref }}" ] && extra=("{{ ref }}")
    /bin/bash -p scripts/dev/agent_workspace.sh create "{{ name }}" "${extra[@]}"

# Release and delete an agent workspace
free name:
    /bin/bash -p scripts/dev/agent_workspace.sh release "{{ name }}"

# List all active agent workspaces
list:
    /bin/bash -p scripts/dev/agent_workspace.sh list

# Check environment for agent workspace functionality
doctor:
    /bin/bash -p scripts/dev/agent_workspace.sh doctor

# Reap stale workspaces
reap force="0":
    #!/bin/bash -p
    set -euo pipefail
    extra=()
    [ "{{ force }}" = "1" ] && extra=(--force)
    /bin/bash -p scripts/dev/agent_workspace.sh reap "${extra[@]}"

# Query CI verdict from the shared monitor (costs zero GitHub API quota)
status sha="":
    #!/bin/bash -p
    set -euo pipefail
    extra=()
    [ -n "{{ sha }}" ] && extra=(--sha "{{ sha }}")
    /bin/bash -p scripts/ci/monitor.sh status "${extra[@]}"

# Query GitHub Actions API quota usage
quota:
    /bin/bash -p scripts/ci/monitor.sh quota
