# SPDX-License-Identifier: MIT
# Copyright (c) 2026 Brighton Sikarskie
#

set working-directory := ".."

clean_env := "/usr/bin/env -i HOME=" + quote(env('HOME')) + " USER=" + quote(env('USER', '')) + " LOGNAME=" + quote(env('LOGNAME', '')) + " LANG=C.UTF-8 LC_ALL=C.UTF-8 PATH=/usr/bin:/bin PYTHONNOUSERSITE=1 RA8_INFRA_SANITIZED=v1"
infra := clean_env + " /bin/bash --noprofile --norc scripts/dev/infra.sh"

# Show fleet infrastructure help menu
default:
    @echo "INFRA - Fleet Infrastructure & Ansible Management"
    @echo ""
    @echo "COMMANDS:"
    @echo "  just infra::list                            List declared fleet hosts in infra/fleet.yml"
    @echo "  just infra::status                          Check live status of all fleet hosts"
    @echo "  just infra::show <host>                     Inspect a single host declaration"
    @echo "  just infra::doctor                          Verify machine environment can drive infra"
    @echo "  just infra::setup                           First-run onboarding for fresh clone"
    @echo "  just infra::ssh_config                      Generate ~/.ssh/config fleet host aliases"
    @echo "  just infra::ssh_config_preview              Print fleet aliases without installing them"
    @echo "  just infra::fortigate_config_selftest [python] Prove declaration checks both ways"
    @echo "  just infra::fortigate_config_lint [path]    Lint the FortiGate declaration offline"
    @echo "  just infra::fortigate_replay_dry_run [path] [lan] Render its offline replay stream"
    @echo "  just infra::fortigate_bootstrap [path] [tty] Factory-reset and replay the FortiGate"
    @echo "  just infra::fortigate_ap_configure [tty]   Configure the bench AP over the console"
    @echo "  just infra::fortigate_verify [tty]         Run read-only FortiGate/AP verification"
    @echo "  just infra::check <host> [play] [tags]      Dry-run all or one play/tag slice"
    @echo "  just infra::apply <host> [play] [tags]      Apply all or one play/tag slice"
    @echo "  just infra::register_runner <host> <vars>   First-register from a mode-0600 vars file"
    @echo "  just infra::register_hil <vars-file>        First-register the declared HIL listener"
    @echo "  just infra::remove <host> [vars-file]       Tear down a supported host class"
    @echo "  just infra::scale <host> <count>            Scale CI runner count on host"
    @echo ""

# First-run onboarding for fresh clone: inventory + credentials
setup:
    {{ clean_env }} /bin/bash -p infra/bootstrap.sh

# List what is declared in infra/fleet.yml and how it is sized
list:
    {{ infra }} list

# Exercise the sanitized public boundary without requiring a prepared checkout.
_boundary_selftest:
    {{ infra }} --selftest-boundary

# Run environment sanity check to verify this machine can drive infra
doctor:
    {{ infra }} doctor

# Check what every fleet host is running right now
status:
    {{ infra }} status

# Inspect a single machine declaration in full
show host:
    {{ infra }} show "{{ host }}"

# Generate ~/.ssh host aliases from the fleet declaration
ssh_config:
    {{ infra }} ssh-config

alias ssh-config := ssh_config

# Print the generated fleet SSH fragment without changing the user configuration
ssh_config_preview:
    {{ infra }} ssh-config-preview

alias ssh-config-preview := ssh_config_preview

# Prove the FortiGate declaration checker fires and stays quiet without live inputs
fortigate_config_selftest python=".venv/bin/python3":
    {{ clean_env }} {{ quote(python) }} -B -I infra/network/fg_bringup.py --selftest config

alias fortigate-config-selftest := fortigate_config_selftest

# Validate the FortiGate declaration without opening a console or reading secrets
fortigate_config_lint path="infra/network/fortigate-bench.conf":
    {{ clean_env }} .venv/bin/python3 -B -I infra/network/fg_bringup.py config-lint {{ quote(path) }}

alias fortigate-config-lint := fortigate_config_lint

# Render the exact FortiGate replay command stream without contacting hardware
fortigate_replay_dry_run path="infra/network/fortigate-bench.conf" lan="internal":
    {{ clean_env }} .venv/bin/python3 -B -I infra/network/fg_bringup.py replay-dry-run {{ quote(path) }} {{ quote(lan) }}

alias fortigate-replay-dry-run := fortigate_replay_dry_run

# Factory-reset the FortiGate and replay one reviewed declaration
[confirm("Factory-reset and reconfigure the FortiGate? This disrupts the bench network.")]
fortigate_bootstrap path="infra/network/fortigate-bench.conf" tty="":
    #!/bin/bash -p
    set -euo pipefail
    declaration={{ quote(path) }}
    tty={{ quote(tty) }}
    env_args=()
    if [[ -n "$tty" ]]; then
        env_args+=("FG_CONSOLE_TTY=$tty")
    fi
    exec {{ clean_env }} "${env_args[@]}" .venv/bin/python3 -B -I \
        infra/network/fg_bringup.py bootstrap "$declaration"

alias fortigate-bootstrap := fortigate_bootstrap

# Configure the bench AP through the FortiGate console jump
[confirm("Reconfigure the bench AP over the FortiGate console?")]
fortigate_ap_configure tty="":
    #!/bin/bash -p
    set -euo pipefail
    tty={{ quote(tty) }}
    env_args=()
    if [[ -n "$tty" ]]; then
        env_args+=("FG_CONSOLE_TTY=$tty")
    fi
    exec {{ clean_env }} "${env_args[@]}" .venv/bin/python3 -B -I \
        infra/network/fg_bringup.py ap-configure

alias fortigate-ap-configure := fortigate_ap_configure

# Run read-only FortiGate and AP verification over the console
fortigate_verify tty="":
    #!/bin/bash -p
    set -euo pipefail
    tty={{ quote(tty) }}
    env_args=()
    if [[ -n "$tty" ]]; then
        env_args+=("FG_CONSOLE_TTY=$tty")
    fi
    exec {{ clean_env }} "${env_args[@]}" .venv/bin/python3 -B -I \
        infra/network/fg_bringup.py verify

alias fortigate-verify := fortigate_verify

# Dry run -- report what would change on a host without modifying anything
check host play="" tags="":
    #!/bin/bash -p
    set -euo pipefail
    host={{ quote(host) }}
    play={{ quote(play) }}
    tags={{ quote(tags) }}
    args=(check "$host")
    if [ -n "$play" ]; then
        args+=("$play")
    fi
    if [ -n "$tags" ]; then
        args+=(--tags "$tags")
    fi
    exec {{ infra }} "${args[@]}"

# Converge a host machine to the declaration in infra/fleet.yml
[confirm("Are you sure you want to apply infra changes to this host?")]
apply host play="" tags="":
    #!/bin/bash -p
    set -euo pipefail
    host={{ quote(host) }}
    play={{ quote(play) }}
    tags={{ quote(tags) }}
    args=(apply "$host")
    if [ -n "$play" ]; then
        args+=("$play")
    fi
    if [ -n "$tags" ]; then
        args+=(--tags "$tags")
    fi
    exec {{ infra }} "${args[@]}"

# First-register a container runner from a mode-0600 Ansible vars file.
[confirm("Register this host as a GitHub Actions runner?")]
register_runner host vars_file:
    #!/bin/bash -p
    set -euo pipefail
    host={{ quote(host) }}
    vars_file={{ quote(vars_file) }}
    exec {{ infra }} register-runner "$host" "$vars_file"

# First-register the declared dev-box HIL listener from a mode-0600 vars file.
[confirm("Register the declared dev-box HIL listener?")]
register_hil vars_file:
    #!/bin/bash -p
    set -euo pipefail
    vars_file={{ quote(vars_file) }}
    exec {{ infra }} register-hil "$vars_file"

# Tear down a host whose fleet class implements removal
[confirm("Are you sure you want to remove this host's managed infrastructure?")]
remove host vars_file="":
    #!/bin/bash -p
    set -euo pipefail
    host={{ quote(host) }}
    vars_file={{ quote(vars_file) }}
    args=(remove "$host")
    if [ -n "$vars_file" ]; then
        args+=(--vars-file "$vars_file")
    fi
    exec {{ infra }} "${args[@]}"

# Scale how many runners a host is running right now
scale host count:
    {{ infra }} scale "{{ host }}" "{{ count }}"
