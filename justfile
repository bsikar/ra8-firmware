# SPDX-License-Identifier: MIT
# Copyright (c) 2026 Brighton Sikarskie
#
# Root justfile for ra8-firmware.
# Run `just` or `just --list` for available commands.

set dotenv-load := true
set shell := ["/bin/bash", "-puc"]

export BASH_ENV := "/dev/null"
export ENV := "/dev/null"
export PYTHONHOME := ""
export PYTHONPATH := ""
export PATH := `/bin/bash -p scripts/dev/setup_python.sh --print-path`
export RA8_JUST := env('RA8_JUST', just_executable())
export RA8_MAX_JOBS := env('RA8_MAX_JOBS', `/usr/bin/env -i PATH=/usr/bin:/bin:/usr/sbin:/sbin /bin/bash -p -c 'if [[ -x /usr/bin/nproc ]]; then /usr/bin/nproc; elif [[ -x /usr/sbin/sysctl ]]; then /usr/sbin/sysctl -n hw.ncpu 2>/dev/null || echo 4; elif [[ -x /usr/bin/sysctl ]]; then /usr/bin/sysctl -n hw.ncpu 2>/dev/null || echo 4; else echo 4; fi'`)
export CMAKE_BUILD_PARALLEL_LEVEL := env('CMAKE_BUILD_PARALLEL_LEVEL', RA8_MAX_JOBS)

mod apps 'just/apps.just'
mod libs 'just/libs.just'
mod tests 'just/tests.just'
mod hil 'just/hil.just'
mod quality 'just/ci.just'
mod tools 'just/tools.just'
mod infra 'just/infra.just'
mod workspace 'just/ws.just'
mod work 'just/work.just'
mod docs 'just/docs.just'
mod checks 'just/checks.just'
mod git_hooks "just/hooks.just"

# --- Primary Developer Shortcuts --------------------------------------------

# Default target: show categorized help menu
default:
    @echo ""
    @echo "DOMAIN SUBMODULES (Run any command below to explore its tools):"
    @echo "  just apps                                   Applications and Examples repository"
    @echo "  just libs                                   Firmware libraries"
    @echo "  just tests                                  Host Unit & Integration Tests"
    @echo "  just hil                                    Remote Pi hardware-in-the-loop bench"
    @echo "  just quality                                CI gates, static analysis, and sanitizers"
    @echo "  just tools                                  Desktop utilities and developer tooling"
    @echo "  just docs                                   Doxygen HTML docs and audits"
    @echo "  just workspace                              Isolated git agent workspaces"
    @echo "  just work                                   Plans and canonical task workspaces"
    @echo "  just infra                                  Ansible fleet infrastructure"
    @echo ""
    @echo "REPOSITORY META COMMANDS:"
    @echo "  just build_all                              Build absolutely everything in the repository"
    @echo "  just setup                                  Prepare venv/hooks and pinned compiler image"
    @echo "  just setup-python                           Install only the uv-locked Python environment"
    @echo "  just dev-shell                              Enter the pinned writable development environment"
    @echo "  just checks                                 Pre-commit verification: format, tidy, unit tests"
    @echo "  just hooks                                  Install tracked git hooks into .git/hooks"
    @echo ""

# Prepare the repository-local venv/hooks and the pinned compiler/tool image.

# Python stays repository-local; exact compilers run through `just dev-shell`.
setup: setup-ansible
    /bin/bash -p scripts/ci/devcontainer_image.sh ensure
    @echo "Pinned compilers are ready; enter with: {{ just_executable() }} dev-shell"

# Install only the pinned repository-local Python environment and hooks
setup-python:
    /bin/bash -p scripts/dev/setup_python.sh setup
    "{{ just_executable() }}" hooks

# Install exact Ansible Galaxy collections into this checkout
setup-ansible: setup-python
    /bin/bash -p scripts/dev/setup_ansible.sh

# Enter a writable shell with every pinned compiler, analyzer, and host tool
dev_shell:
    /bin/bash -p scripts/ci/devcontainer_run.sh -- /bin/bash -p

alias dev-shell := dev_shell

# Install immutable-HEAD hook launchers under the shared Git common directory
hooks:
    @/bin/bash -p scripts/git/install-hooks.sh
    @echo "git hooks active: core.hooksPath = $(/usr/bin/git config core.hooksPath)"

# Run the full CI gate suite
ci:
    "{{ just_executable() }}" quality::run

# Build everything in the entire repository: firmware, host apps, board apps, and tests
build_all:
    @echo "==> Building all ARM firmware apps and examples..."
    "{{ just_executable() }}" apps::example::build all
    @echo "==> Building all standalone macOS/Linux host apps..."
    "{{ just_executable() }}" apps::host::build all
    @echo "==> Building all standalone ARM board apps..."
    "{{ just_executable() }}" apps::board::build all
    @echo "==> Building all host unit and integration tests..."
    "{{ just_executable() }}" tests::build

alias build-all := build_all

# Unified search across Apps, Examples, and Tests
search keyword:
    python3 scripts/dev/search.py "{{ keyword }}"
