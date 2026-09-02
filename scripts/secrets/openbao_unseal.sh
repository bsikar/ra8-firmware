#!/bin/bash -p
# SPDX-License-Identifier: MIT
# Copyright (c) 2026 Brighton Sikarskie
# SHEBANG-SECURITY: -p blocks BASH_ENV and exported-function startup injection.
#
# scripts/secrets/openbao_unseal.sh -- unseal the OpenBao instance after a pod
# or cluster restart.
#
# WHY THIS IS A SCRIPT AND NOT AN ANSIBLE TASK
# --------------------------------------------
# The unseal key is a secret. The openbao role deploys the vault and reports
# that it is sealed; it never handles the key, because a playbook that reads one
# can log one. This runs on the cluster node, in the operator's hands, and reads
# the key from a file the operator owns -- never from this repository.
#
# A Shamir-sealed OpenBao comes up SEALED after every restart. That is by
# design, not a fault, which is why this is a routine step rather than an
# incident.
#
# The key file is the JSON `bao operator init` emitted. It contains the unseal
# keys AND the root token, so it lives at mode 0600 outside any checkout and is
# NEVER committed -- see scripts/secrets/README.md.
#
# Usage:
#   /bin/bash -p scripts/secrets/openbao_unseal.sh  # use the default key file
#   RA8_BAO_INIT_JSON=/path/to/init.json /bin/bash -p scripts/secrets/openbao_unseal.sh
#
# Exit 0 when the vault ends up unsealed (including when it already was).

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

  NAMESPACE="${RA8_BAO_NAMESPACE:-openbao}"
  POD="${RA8_BAO_POD:-openbao-0}"
  INIT_JSON="${RA8_BAO_INIT_JSON:-$HOME/.openbao/init.json}"

  # k3s ships kubectl as a subcommand; a plain kubectl is used when present so
  # this also runs against a non-k3s cluster.
  if command -v kubectl >/dev/null 2>&1; then
    kube() { kubectl "$@"; }
  elif command -v k3s >/dev/null 2>&1; then
    kube() { k3s kubectl "$@"; }
  else
    echo "error: neither kubectl nor k3s is on PATH; run this on the cluster node" >&2
    exit 1
  fi

  # Read one field out of the init JSON. Lists yield their first element, which
  # is what `unseal_keys_b64` is. Nothing else in the file is ever touched, and
  # the value goes straight into a variable rather than the terminal.
  init_field() {
    python3 - "$INIT_JSON" "$1" <<'PY'
import json
import sys

with open(sys.argv[1]) as handle:
    value = json.load(handle)[sys.argv[2]]
print(value[0] if isinstance(value, list) else value)
PY
  }

  seal_state() {
    kube exec -n "$NAMESPACE" "$POD" -- bao status -format=json 2>/dev/null |
      python3 -c 'import json,sys;print(json.load(sys.stdin)["sealed"])' 2>/dev/null ||
      echo unknown
  }

  state="$(seal_state)"
  case "$state" in
    False)
      echo "OpenBao is already unsealed."
      exit 0
      ;;
    unknown)
      echo "error: could not read the seal state of ${POD} in namespace ${NAMESPACE}." >&2
      echo "       Is the pod Running, and is this the cluster node?" >&2
      exit 1
      ;;
  esac

  if [ ! -r "$INIT_JSON" ]; then
    cat >&2 <<EOF
error: no readable unseal key file at ${INIT_JSON}

This file is the JSON that \`bao operator init\` printed when the vault was
first initialised. It holds the Shamir unseal keys and the root token, so it is
kept at mode 0600 outside any checkout and never committed.

If the vault has never been initialised, initialise it first:

    kubectl exec -n ${NAMESPACE} ${POD} -- bao operator init -format=json > ${INIT_JSON}
    chmod 600 ${INIT_JSON}

If it HAS been initialised and this file is gone, the vault's data is
unrecoverable -- that is what a Shamir seal means. See
scripts/secrets/README.md.
EOF
    exit 1
  fi

  key="$(init_field unseal_keys_b64)"
  kube exec -n "$NAMESPACE" "$POD" -- bao operator unseal "$key" >/dev/null
  unset key

  if [ "$(seal_state)" != "False" ]; then
    echo "error: applied the unseal key but ${POD} is still sealed." >&2
    echo "       More key shares may be required than this script applies (one)." >&2
    exit 1
  fi

  echo "Unsealed:"
  kube exec -n "$NAMESPACE" "$POD" -- bao status 2>/dev/null |
    grep -E "Initialized|Sealed|HA Mode" || true
else
  [[ "$-" == *p* ]]
fi
