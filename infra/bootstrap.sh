#!/bin/bash -p
# SPDX-License-Identifier: MIT
# Copyright (c) 2026 Brighton Sikarskie
# SHEBANG-SECURITY: -p blocks BASH_ENV and exported-function startup injection.
#
# infra/bootstrap.sh -- one-command setup for a fresh clone.
#
# Walks you from "git clone" to a deployable rig: checks prerequisites, writes
# your (git-ignored) inventory, and stores your GitHub token locally so nothing
# secret ever touches the repo. At the end it offers to converge the declared
# runner-image source host through the fleet dispatcher.
#
#   /bin/bash -p infra/bootstrap.sh    # or:  just infra::setup
#

if [[ "$-" == *p* ]]; then
  unset -v BASH_ENV ENV
  declare -a ra8_startup_env_unset=()
  _ra8_startup_refuse() {
    printf 'error: privileged startup %s\n' "$1" >&2
    exit 1
  }
  ra8_startup_env_done_count=0
  while IFS= read -r -d '' ra8_startup_env_row; do
    ra8_startup_env_name="${ra8_startup_env_row%%=*}"
    case "$ra8_startup_env_name" in
      RA8_STARTUP_ENV_DONE)
        ra8_startup_env_done_count=$((ra8_startup_env_done_count + 1))
        ;;
      BASH_FUNC_*%% | BASH_FUNC_*'()') ra8_startup_env_unset+=(-u "$ra8_startup_env_name") ;;
    esac
  done < <(
    /usr/bin/env -u RA8_STARTUP_ENV_DONE -0 &&
      /usr/bin/printf 'RA8_STARTUP_ENV_DONE=1\0'
  )
  ((ra8_startup_env_done_count == 1)) && [[ "$ra8_startup_env_name" == RA8_STARTUP_ENV_DONE ]] || _ra8_startup_refuse 'environment enumeration was incomplete'
  if ((${#ra8_startup_env_unset[@]})); then
    [[ -z "${RA8_STARTUP_ENV_SCRUBBED-}" ]] || _ra8_startup_refuse 'scrub did not converge'
    ra8_startup_reentry="$0"
    [[ "$ra8_startup_reentry" == */* ]] || _ra8_startup_refuse 'requires a script path'
    if [[ "$ra8_startup_reentry" != /* ]]; then
      ra8_startup_reentry="$PWD/$ra8_startup_reentry"
    fi
    ra8_startup_check="$ra8_startup_reentry"
    while [[ "$ra8_startup_check" != "/" ]]; do
      [[ ! -L "$ra8_startup_check" ]] || _ra8_startup_refuse 'refuses a symlinked path'
      ra8_startup_parent="${ra8_startup_check%/*}"
      [[ -n "$ra8_startup_parent" ]] || ra8_startup_parent="/"
      [[ "$ra8_startup_parent" != "$ra8_startup_check" ]] ||
        _ra8_startup_refuse 'cannot validate its script path'
      ra8_startup_check="$ra8_startup_parent"
    done
    [[ -f "$ra8_startup_reentry" ]] || _ra8_startup_refuse 'refuses a non-regular path'
    if ! exec /usr/bin/env "${ra8_startup_env_unset[@]}" -u BASH_ENV -u ENV \
      -u RA8_STARTUP_ENV_DONE RA8_STARTUP_ENV_SCRUBBED=1 \
      /bin/bash -p -- "$ra8_startup_reentry" "$@"; then
      _ra8_startup_refuse 'could not enter sanitized process'
    fi
  fi
  unset -v ra8_startup_check ra8_startup_env_done_count
  unset -v ra8_startup_env_name ra8_startup_env_row
  unset -v ra8_startup_env_unset ra8_startup_parent ra8_startup_reentry
  unset -v RA8_STARTUP_ENV_DONE
  unset -v RA8_STARTUP_ENV_SCRUBBED
  unset -f _ra8_startup_refuse

  set -euo pipefail

  export BASH_ENV=/dev/null ENV=/dev/null PYTHONNOUSERSITE=1
  unset PYTHONHOME PYTHONPATH RA8_TOOL_VENV
  PATH=/usr/bin:/bin
  export PATH

  if [[ "${1:-}" == "--selftest-boundary" ]]; then
    [[ "$PATH" == "/usr/bin:/bin" && -z "${PYTHONHOME:-}" &&
      -z "${PYTHONPATH:-}" && -z "${RA8_TOOL_VENV:-}" ]] || exit 1
    exit 0
  fi

  ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
  ANSIBLE_DIR="${ROOT}/infra/ansible"
  PRIVATE_DIR="${ANSIBLE_DIR}/private"
  INVENTORY="${ANSIBLE_DIR}/inventory/hosts.ini"
  SECRETS="${PRIVATE_DIR}/secrets.yml"

  say() { printf '\n== %s ==\n' "$1"; }

  say "ra8-firmware rig bootstrap"

  # 1. Prerequisites -----------------------------------------------------------
  command -v python3 >/dev/null 2>&1 || {
    echo "  Python 3.11 through 3.14 is required to bootstrap pinned uv." >&2
    exit 1
  }
  command -v ssh >/dev/null 2>&1 || {
    echo "  OpenSSH is required to reach the declared fleet." >&2
    exit 1
  }
  echo "  synchronizing uv-locked control-host dependencies..."
  /bin/bash -p "${ROOT}/scripts/dev/setup_python.sh" setup
  export PATH
  PATH="$(/bin/bash -p "${ROOT}/scripts/dev/setup_python.sh" --print-path)"
  for cmd in ansible ansible-playbook ansible-galaxy; do
    command -v "${cmd}" >/dev/null 2>&1 || {
      echo "  Locked environment is missing ${cmd}; uv synchronization is incomplete." >&2
      exit 1
    }
  done
  echo "  installing exact Ansible Galaxy collection versions..."
  /bin/bash -p "${ROOT}/scripts/dev/setup_ansible.sh"
  export ANSIBLE_COLLECTIONS_PATH="${ROOT}/.ansible/collections"

  mkdir -p "${PRIVATE_DIR}"

  # 2. Inventory (git-ignored, GENERATED) --------------------------------------
  #
  # Nothing is asked here any more, and nothing is copied from an example. The
  # machines are declared once in infra/fleet.yml and the inventory is derived
  # from it -- so adding a machine is adding a block there, and this file cannot
  # describe a fleet the declaration does not.
  say "Inventory"
  python3 "${ROOT}/scripts/dev/fleet.py" inventory
  echo "  generated from infra/fleet.yml; edit that file, not ${INVENTORY#"${ROOT}"/}"

  # 3. SSH aliases (generated, outside the repo) -------------------------------
  #
  # The declaration carries every machine's real address, so nothing here NEEDS
  # an alias -- but the docs, the runbooks and everyone's fingers say `ssh dev`
  # and `ssh truenas`, and those names used to exist on exactly one laptop. This
  # generates them from the same declaration, so a fresh control node has them
  # too (#526).
  say "SSH aliases"
  python3 "${ROOT}/scripts/dev/fleet.py" ssh-config --install

  # 4. Secrets (git-ignored, never committed) ----------------------------------
  say "Secrets"
  if [ -f "${SECRETS}" ]; then
    echo "  ${SECRETS#"${ROOT}"/} already exists -- leaving it."
  else
    echo "  ARC needs a GitHub token to register runners."
    echo "  Create a fine-grained PAT with 'Administration: read/write' on the repo:"
    echo "    https://github.com/settings/personal-access-tokens"
    read -r -s -p "  Paste PAT (input hidden): " pat
    echo
    if [ -z "${pat}" ]; then
      echo "  No PAT entered -- skipping. Add it later to ${SECRETS#"${ROOT}"/}"
    else
      (
        umask 077
        printf 'ci_runner_github_pat: "%s"\n' "${pat}" >"${SECRETS}"
      )
      unset pat
      echo "  wrote ${SECRETS#"${ROOT}"/} (git-ignored, 0600)"
      echo "  (Prefer OpenBao? Put the token there and set ci_runner_github_pat"
      echo "   via a vault lookup in group_vars/all.yml -- see all.example.yml.)"
    fi
  fi

  # 5. Converge through the declared fleet path --------------------------------
  say "Ready"
  # The runner-image producer is a declaration fact, not a bootstrap-script
  # constant. Resolve it through the same model the operator-facing infra
  # commands use, then let fleet.py derive the play order, inventory group, role
  # variables and transport. Calling ansible-playbook here directly used to
  # bypass all of those contracts and provision only the ARC half of the host.
  runner_image_source="$(
    PYTHONPATH="${ROOT}/scripts/dev" python3 -c \
      'import fleet_model as fm; print(fm.load()["runner_image"]["source_host"])'
  )"
  deploy_cmd=(python3 "${ROOT}/scripts/dev/fleet.py" apply "${runner_image_source}")
  echo "  Converge the declared runner-image source host (${runner_image_source}) with:"
  echo "    ${deploy_cmd[*]}"
  echo
  read -r -p "  Run it now? [y/N] " go
  case "${go}" in
    [yY]*)
      exec "${deploy_cmd[@]}"
      ;;
    *)
      echo "  Skipped. Run the command above when you are ready."
      ;;
  esac
else
  [[ "$-" == *p* ]]
fi
