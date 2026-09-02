#!/bin/bash -p
# SPDX-License-Identifier: MIT
# Copyright (c) 2026 Brighton Sikarskie
# SHEBANG-SECURITY: -p blocks BASH_ENV and exported-function startup injection.
# shellcheck shell=bash
#
# Shell adapter for git_environment.py. Source this file, then call
# install_sanitized_git_environment before the first Git operation in a nested writer.

_RA8_GIT_ENVIRONMENT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"

RA8_TRUSTED_GIT=/usr/bin/git
RA8_TRUSTED_PYTHON=/usr/bin/python3
export RA8_TRUSTED_GIT RA8_TRUSTED_PYTHON

[[ -f "$RA8_TRUSTED_GIT" && ! -L "$RA8_TRUSTED_GIT" && -x "$RA8_TRUSTED_GIT" ]] || {
  echo "ERROR: trusted /usr/bin/git is unavailable or unsafe." >&2
  if [[ "${BASH_SOURCE[0]}" != "$0" ]]; then
    return 1
  fi
  exit 1
}
[[ -x "$RA8_TRUSTED_PYTHON" ]] || {
  echo "ERROR: trusted /usr/bin/python3 is unavailable." >&2
  if [[ "${BASH_SOURCE[0]}" != "$0" ]]; then
    return 1
  fi
  exit 1
}
unalias git python3 2>/dev/null || true
unset -f git python3 2>/dev/null || true
hash -r

_install_git_environment_contract() {
  local mode="$1" contract action name value extra count=0
  if ! contract="$(env -u BASH_ENV -u ENV -u PYTHONHOME -u PYTHONPATH \
    "$RA8_TRUSTED_PYTHON" -I "${_RA8_GIT_ENVIRONMENT_DIR}/git_environment.py" "$mode")"; then
    echo "ERROR: cannot load Git environment contract '$mode'." >&2
    return 1
  fi
  while IFS=$'\t' read -r action name value extra; do
    if [[ ! "$name" =~ ^[A-Z][A-Z0-9_]*$ || -n "${extra:-}" ]]; then
      echo "ERROR: invalid strict Git environment row for '$name'." >&2
      return 1
    fi
    case "$action" in
      unset)
        [[ -z "${value:-}" ]] || {
          echo "ERROR: unset row for '$name' carried a value." >&2
          return 1
        }
        unset "$name"
        ;;
      set)
        export "$name=${value:-}"
        ;;
      *)
        echo "ERROR: invalid strict Git environment action: '$action'." >&2
        return 1
        ;;
    esac
    count=$((count + 1))
  done <<<"$contract"
  if [[ "$count" -eq 0 ]]; then
    echo "ERROR: strict Git environment contract listed no operations." >&2
    return 1
  fi
}

install_sanitized_git_environment() {
  _install_git_environment_contract --shell-contract
}

run_sanitized_git() (
  install_sanitized_git_environment
  "$RA8_TRUSTED_GIT" "$@"
)

# A push happens only after local publication bytes are final. Preserve the
# operator's config/SSH/credential transport, but strip hook-exported repository
# routing so the explicit -C repository remains authoritative.
run_git_network_with_inherited_transport() (
  _install_git_environment_contract --network-shell-contract
  "$RA8_TRUSTED_GIT" "$@"
)
