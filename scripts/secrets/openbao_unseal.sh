#!/usr/bin/env bash
# SPDX-License-Identifier: MIT
# Copyright (c) 2026 Brighton Sikarskie
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
#   scripts/secrets/openbao_unseal.sh              # unseal using the default key file
#   RA8_BAO_INIT_JSON=/path/to/init.json ...       # ...or one somewhere else
#
# Exit 0 when the vault ends up unsealed (including when it already was).

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
