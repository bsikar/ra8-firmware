#!/usr/bin/env bash
# SPDX-License-Identifier: MIT
# Copyright (c) 2026 Brighton Sikarskie
#
# scripts/secrets/openbao_configure.sh -- configure the OpenBao vault's KV
# mount, read-only policy and AppRole for one secret path. Idempotent.
#
# WHY THIS IS A SCRIPT AND NOT AN ANSIBLE TASK
# --------------------------------------------
# It needs the root token and it writes secret VALUES. The openbao role deploys
# the vault and stops there precisely so no playbook ever holds either. This
# runs on the cluster node, in the operator's hands.
#
# NOTHING SECRET IS IN THIS FILE, AND NOTHING CAN BE:
#   * the root token is read from the operator's init JSON (mode 0600, outside
#     any checkout, never committed);
#   * the secret values are read from STDIN as KEY=VALUE lines;
#   * the emitted ROLE_ID / SECRET_ID go to STDOUT for the operator to place in
#     a consumer's ~/.config/hil/openbao.env, and every human-readable status
#     line goes to STDERR so a redirect captures only the credentials.
#
# Usage:
#   scripts/secrets/openbao_configure.sh <secret-path> <policy-name> <role-name> \
#       < values.env  > approle.env
#
#   e.g.  scripts/secrets/openbao_configure.sh hil/tapo hil-tapo-ro hil-tapo
#
# The KEY=VALUE lines on stdin become the fields of the KV secret verbatim, so
# the set of keys is the caller's to choose. Blank lines and #-comments are
# ignored.
#
# Exit 0 when the mount, secret, policy, auth method and role are all in place.

set -euo pipefail
umask 077

SECRET_PATH="${1:?usage: openbao_configure.sh <secret-path> <policy-name> <role-name>}"
POLICY_NAME="${2:?usage: openbao_configure.sh <secret-path> <policy-name> <role-name>}"
ROLE_NAME="${3:?usage: openbao_configure.sh <secret-path> <policy-name> <role-name>}"

NAMESPACE="${RA8_BAO_NAMESPACE:-openbao}"
POD="${RA8_BAO_POD:-openbao-0}"
KV_MOUNT="${RA8_BAO_KV_MOUNT:-secret}"
INIT_JSON="${RA8_BAO_INIT_JSON:-$HOME/.openbao/init.json}"

if command -v kubectl >/dev/null 2>&1; then
  kube() { kubectl "$@"; }
elif command -v k3s >/dev/null 2>&1; then
  kube() { k3s kubectl "$@"; }
else
  echo "error: neither kubectl nor k3s is on PATH; run this on the cluster node" >&2
  exit 1
fi

if [ ! -r "$INIT_JSON" ]; then
  echo "error: no readable root-token file at ${INIT_JSON}." >&2
  echo "       See scripts/secrets/README.md -- it is never kept in this repo." >&2
  exit 1
fi

ROOT_TOKEN="$(
  python3 - "$INIT_JSON" <<'PY'
import json
import sys

with open(sys.argv[1]) as handle:
    print(json.load(handle)["root_token"])
PY
)"

# Run one `bao` command inside the pod with the root token in its environment.
# The token is passed through `env` rather than a shell expansion inside the
# pod so it never lands in the pod's shell history or a process title there.
ex() {
  kube exec -i -n "$NAMESPACE" "$POD" -- \
    env BAO_ADDR=http://127.0.0.1:8200 BAO_TOKEN="$ROOT_TOKEN" "$@"
}

# --- 1. KV v2 mount ---------------------------------------------------------

if ex bao secrets list -format=json |
  python3 -c "import json,sys;sys.exit(0 if '${KV_MOUNT}/' in json.load(sys.stdin) else 1)"; then
  echo "kv: ${KV_MOUNT}/ already mounted" >&2
else
  ex bao secrets enable -path="$KV_MOUNT" kv-v2 >/dev/null
  echo "kv: enabled ${KV_MOUNT}/ (kv-v2)" >&2
fi

# --- 2. The secret itself, from stdin ---------------------------------------

# Collected into an array rather than eval'd into the environment: a value
# containing a space, a quote or a `$` is data, and passing it as one argv
# element keeps it that way.
declare -a fields=()
while IFS= read -r line || [ -n "$line" ]; do
  case "$line" in
    '' | '#'*) continue ;;
    *=*) fields+=("$line") ;;
    *)
      echo "error: stdin line is not KEY=VALUE: ${line%%=*}" >&2
      exit 1
      ;;
  esac
done

if [ "${#fields[@]}" -eq 0 ]; then
  echo "error: no KEY=VALUE lines on stdin; refusing to write an empty secret." >&2
  exit 1
fi

ex bao kv put "${KV_MOUNT}/${SECRET_PATH}" "${fields[@]}" >/dev/null
echo "kv: wrote ${KV_MOUNT}/${SECRET_PATH} (${#fields[@]} keys)" >&2

# --- 3. Read-only policy on exactly that path -------------------------------

ex bao policy write "$POLICY_NAME" - >/dev/null <<POLICY
path "${KV_MOUNT}/data/${SECRET_PATH}" {
  capabilities = ["read"]
}
path "${KV_MOUNT}/metadata/${SECRET_PATH}" {
  capabilities = ["read"]
}
POLICY
echo "policy: ${POLICY_NAME} written (read-only on ${SECRET_PATH})" >&2

# --- 4. AppRole auth, and a role bound to that policy -----------------------

if ex bao auth list -format=json |
  python3 -c "import json,sys;sys.exit(0 if 'approle/' in json.load(sys.stdin) else 1)"; then
  echo "auth: approle already enabled" >&2
else
  ex bao auth enable approle >/dev/null
  echo "auth: enabled approle" >&2
fi

# Short token TTL with a non-expiring secret_id: the consumers are unattended
# bench tools that re-authenticate per run, so a long-lived token would be a
# standing credential for no benefit while a rotating secret_id would need a
# human at the bench every time it expired.
ex bao write "auth/approle/role/${ROLE_NAME}" \
  token_policies="$POLICY_NAME" \
  token_ttl=15m \
  token_max_ttl=1h \
  secret_id_ttl=0 \
  secret_id_num_uses=0 >/dev/null
echo "role: auth/approle/role/${ROLE_NAME} configured" >&2

ROLE_ID="$(ex bao read -field=role_id "auth/approle/role/${ROLE_NAME}/role-id")"
SECRET_ID="$(ex bao write -f -field=secret_id "auth/approle/role/${ROLE_NAME}/secret-id")"

printf 'ROLE_ID=%s\nSECRET_ID=%s\n' "$ROLE_ID" "$SECRET_ID"
