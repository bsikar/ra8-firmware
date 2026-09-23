# SPDX-License-Identifier: MIT
# Copyright (c) 2026 Brighton Sikarskie

set working-directory := ".."

default:
    @echo "INFRA (PROXMOX LAB CI & MANAGEMENT)"
    @echo "  just infra::lab::ci [profile]                Run CI on server & stream live logs (Ctrl+C detaches)"
    @echo "  just infra::lab::start [profile]             Start CI in background on server and return immediately"
    @echo "  just infra::lab::logs [profile]              Stream live CI logs from server (multi-machine / resume)"
    @echo "  just infra::lab::status                      Show status of running background CI jobs on server"
    @echo "  just infra::lab::stop [profile]              Stop and clean up active CI run(s) on server"
    @echo "  just infra::lab::list                        List active lab VMs on Proxmox"
    @echo "  just infra::lab::ssh [target] [cmd]          SSH into lab VM (interactive menu if multiple)"
    @echo "  just infra::lab::destroy [target]            Tear down preserved lab guest & network"
    @echo "  just infra::lab::setup-cache                 Setup apt-cacher-ng & artifact HTTP cache on Proxmox"
    @echo "  just infra::lab::bake-template               Pre-bake ra8-ci container into VM 9001 (~5s CI startup)"
    @echo "  just infra::lab::check                       Read-only lab/template preflight"
    @echo "  just infra::lab::setup [target]              Bootstrap host packages, pool, and templates"
    @echo "  just infra::lab::ci-terraform [profile]      Run CI through the local Terraform/Ansible driver"
    @echo "  just infra::lab::prepare-linux-template      Prepare the Debian lab template"
    @echo "  just infra::lab::prepare-windows-template    Prepare the Windows Server Core lab template"

# Read-only checks: no VM creation or state modifications.
check:
    scripts/dev/proxmox_lab_ci.sh check

# Run disposable CI on Proxmox and stream live logs. Ctrl+C detaches cleanly without killing the run.
ci profile="linux" keep="false":
    #!/bin/bash -p
    keep_flag=""
    if [[ {{ quote(keep) }} == "true" ]]; then
        keep_flag="--keep"
    fi
    python3 scripts/dev/proxmox_lab_manage.py ci {{ quote(profile) }} $keep_flag

# Start disposable CI in the background on Proxmox and exit immediately.
start profile="linux" keep="false":
    #!/bin/bash -p
    keep_flag=""
    if [[ {{ quote(keep) }} == "true" ]]; then
        keep_flag="--keep"
    fi
    python3 scripts/dev/proxmox_lab_manage.py start {{ quote(profile) }} $keep_flag

# Stream live logs from an active or recent Proxmox CI run. Ctrl+C only exits the log view.
logs profile="linux":
    python3 scripts/dev/proxmox_lab_manage.py logs {{ quote(profile) }}

# Show status of running background CI jobs on Proxmox.
status:
    python3 scripts/dev/proxmox_lab_manage.py status

# Stop and tear down an active CI run on Proxmox.
stop profile="all":
    python3 scripts/dev/proxmox_lab_manage.py stop {{ quote(profile) }}

# Run CI via the local Terraform/Ansible driver (synchronous foreground lifecycle)
ci-terraform profile="linux" keep="false":
    #!/bin/bash -p
    set -euo pipefail
    export RA8_LAB_NETWORK_APPROVED="${RA8_LAB_NETWORK_APPROVED:-1}"
    export RA8_LAB_EGRESS_APPROVED="${RA8_LAB_EGRESS_APPROVED:-1}"
    args=(ci --profile {{ quote(profile) }})
    if [[ {{ quote(keep) }} == "true" ]]; then
        args+=(--keep)
    fi
    exec scripts/dev/proxmox_lab_ci.sh "${args[@]}"

# List active or preserved lab VMs on Proxmox
list:
    python3 scripts/dev/proxmox_lab_manage.py list

# SSH into a lab VM (selects automatically if 1, interactive menu if multiple, or specify VMID/run_id/type)
ssh *args:
    #!/bin/bash -p
    exec python3 scripts/dev/proxmox_lab_manage.py ssh "$@"

# Destroy preserved lab guest VM(s) and tear down the lab network
destroy target="all":
    python3 scripts/dev/proxmox_lab_manage.py destroy {{ quote(target) }}

# Prepare the Windows Server 2025 lab template (VM 9011) on Proxmox
[confirm("Build the protected Windows Server 2025 lab template (VM 9011) on Proxmox?")]
prepare-windows-template:
    scripts/dev/prepare_proxmox_lab_windows_template.sh

# Prepare the Debian 12 Linux cloud lab template (VM 9001) on Proxmox
[confirm("Build the protected Debian 12 Linux lab template (VM 9001) on Proxmox?")]
prepare-linux-template:
    scripts/dev/prepare_proxmox_lab_linux_template.sh

# Complete host onboarding: packages, sysctl, pools, storage, and runner templates
[confirm("Configure Proxmox host prerequisites, pools, and provision lab CI templates?")]
setup target="all":
    scripts/dev/setup_proxmox_lab_host.sh {{ quote(target) }}

# Setup apt-cacher-ng (port 3142) and Nginx artifact cache (port 8080) on Proxmox
setup-cache:
    scripts/dev/setup_lab_cache.sh

# Pre-bake the ra8-ci:latest devcontainer directly into template VM 9001 for ~5s startup
[confirm("Bake ra8-ci:latest devcontainer image into Debian template VM 9001 on Proxmox?")]
bake-template:
    scripts/dev/bake_proxmox_lab_template.sh
