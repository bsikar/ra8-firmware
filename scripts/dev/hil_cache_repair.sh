#!/bin/bash -p
# SPDX-License-Identifier: MIT
# Copyright (c) 2026 Brighton Sikarskie
# SHEBANG-SECURITY: -p blocks BASH_ENV and exported-function startup injection.
#
# Fixed front door for the standalone HIL compiler-cache repair playbook.

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

  ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd -P)"
  PYTHON="${ROOT}/.venv/bin/python3"
  ANSIBLE_PLAYBOOK="${ROOT}/.venv/bin/ansible-playbook"
  mode="${1:-}"

  reject_ansible_environment() {
    local name
    while IFS='=' read -r name _; do
      if [[ "${name}" == ANSIBLE_* ]]; then
        echo "error: inherited ANSIBLE_* environment is not allowed" >&2
        return 2
      fi
    done < <(env)
  }

  selftest_rejection() {
    local key="$1" output rc
    set +e
    output="$(env "${key}=unsafe" "${BASH_SOURCE[0]}" __probe_environment 2>&1)"
    rc=$?
    set -e
    if [ "${rc}" -ne 2 ] || [ "${output}" != "error: inherited ANSIBLE_* environment is not allowed" ]; then
      echo "hil_cache_repair.sh --selftest: ${key} was not rejected fail-closed" >&2
      return 1
    fi
  }

  selftest() {
    selftest_rejection ANSIBLE_HOST_KEY_CHECKING
    selftest_rejection ANSIBLE_ACTION_PLUGINS
    echo "hil_cache_repair.sh --selftest: PASS"
  }

  reject_ansible_environment
  if [ "$#" -eq 1 ] && [ "${mode}" = __probe_boundary ]; then
    [ "${PATH}" = /usr/bin:/bin ] && [ "${BASH_ENV}" = /dev/null ] &&
      [ "${ENV}" = /dev/null ] && [ -z "${PYTHONPATH:-}" ] &&
      [ -z "${PYTHONHOME:-}" ] && [ -z "${RA8_TOOL_VENV:-}" ]
    exit
  fi
  if [ "$#" -eq 1 ] && [ "${mode}" = __probe_environment ]; then
    echo "environment accepted"
    exit 0
  fi
  if [ "$#" -eq 1 ] && [ "${mode}" = --selftest ]; then
    selftest
    exit 0
  fi

  if [ "$#" -ne 1 ] || { [ "${mode}" != check ] && [ "${mode}" != apply ]; }; then
    echo "usage: $0 check|apply" >&2
    exit 2
  fi

  if [ ! -x "${PYTHON}" ] || [ ! -x "${ANSIBLE_PLAYBOOK}" ]; then
    echo "error: locked Ansible environment is absent; run 'just setup-python'" >&2
    exit 2
  fi

  umask 077
  scratch="$(mktemp -d "${TMPDIR:-/tmp}/ra8-hil-cache-repair.XXXXXXXX")"
  inventory="${scratch}/inventory.ini"
  playbook="${scratch}/hil-cache-repair.yml"
  config="${scratch}/ansible.cfg"

  cleanup() {
    rm -f -- "${inventory}" "${playbook}" "${config}"
    rmdir -- "${scratch}"
  }
  trap cleanup EXIT
  trap 'exit 129' HUP
  trap 'exit 130' INT
  trap 'exit 143' TERM

  # Validate first, then derive reachability from the fleet declaration. Keeping
  # inventory in the private temporary directory prevents committed host_vars or
  # group_vars from changing this repair's transport or target.
  "${PYTHON}" "${ROOT}/scripts/checks/check_fleet_declaration.py" >/dev/null
  "${PYTHON}" "${ROOT}/scripts/dev/fleet.py" inventory --stdout >"${inventory}"
  cp "${ROOT}/infra/ansible/playbooks/hil-cache-repair.yml" "${playbook}"
  printf '%s\n' '[defaults]' 'host_key_checking = True' 'retry_files_enabled = False' >"${config}"

  args=("${ANSIBLE_PLAYBOOK}" -i "${inventory}" "${playbook}" --limit dev)
  if [ "${mode}" = check ]; then
    args+=(--check --diff)
  fi

  ANSIBLE_CONFIG="${config}" \
    ANSIBLE_COLLECTIONS_PATH="${ROOT}/.ansible/collections" \
    ANSIBLE_COLLECTIONS_SCAN_SYS_PATH=false \
    PYTHONNOUSERSITE=1 \
    "${args[@]}"
else
  [[ "$-" == *p* ]]
fi
