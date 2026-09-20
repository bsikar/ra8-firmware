# SPDX-License-Identifier: MIT
# Copyright (c) 2026 Brighton Sikarskie

set working-directory := ".."

default:
    @echo "INFRA (PROXMOX LAB)"
    @echo "  just infra::lab::setup [target]              Bootstrap host packages, pool, and templates"
    @echo "  just infra::lab::check                       Read-only lab/template preflight"
    @echo "  just infra::lab::ci linux                   Provision, run Ansible/CI, clean up"
    @echo "  just infra::lab::ci linux true              Keep one run for debugging"
    @echo "  just infra::lab::list                       List all active lab VMs on Proxmox"
    @echo "  just infra::lab::ssh [target] [cmd]         SSH into lab VM (interactive if multiple)"
    @echo "  just infra::lab::destroy [target]           Tear down preserved lab guest & network"

# Read-only checks: no Terraform apply, guest start, or Proxmox deletion.
check:
    scripts/dev/proxmox_lab_ci.sh check

# The driver owns a saved Terraform plan and a run-marker-guarded cleanup trap.
[confirm("Create a disposable Proxmox lab guest, run its CI, and remove it afterwards?")]
ci profile="linux" keep="false":
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
