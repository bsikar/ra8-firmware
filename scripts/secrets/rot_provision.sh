#!/usr/bin/env bash
# SPDX-License-Identifier: MIT
# Copyright (c) 2026 Brighton Sikarskie
#
# rot_provision.sh -- root-of-trust signing-key ceremony.
#
# Generates a NIST P-256 root keypair, writes the PRIVATE key out of the repo
# tree, emits the PUBLIC key as the s_rot_root_pubkey C initialiser, and (with
# --patch) provisions it into libs/ra8_dfu/src/ra8_rot.c. The private key never
# enters git; it is what tools/rot_sign.py uses to sign every launched image.
#
# The signing key is the single anchor of the secure-boot chain of trust: if it
# is lost, no new image can be signed for the provisioned public key (you must
# re-key and re-flash). BACK IT UP -- the script prints how.
#
# Usage:
#   scripts/secrets/rot_provision.sh                     # keygen only (prints header)
#   scripts/secrets/rot_provision.sh --patch             # also write s_rot_root_pubkey
#   scripts/secrets/rot_provision.sh --key <path> --patch
#   scripts/secrets/rot_provision.sh --patch --store     # also version+tag it in the key store
#   scripts/secrets/rot_provision.sh --store --backend local   # force the no-vault store
#
# --store persists the key into the versioned, tagged key store
# (scripts/secrets/rot_keystore.py): OpenBao when reachable, else a local directory.
# For routine re-keying, prefer `scripts/secrets/rot_keystore.py rekey --patch`, which
# backs up the outgoing key, generates + tags a new one, and installs it.
set -euo pipefail

REPO_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
KEY_FILE="${HOME}/ra8d2-rot-signing-key.pem"
PATCH=0
STORE=0
BACKEND=auto

while [[ $# -gt 0 ]]; do
  case "$1" in
    --key)
      KEY_FILE="$2"
      shift 2
      ;;
    --patch)
      PATCH=1
      shift
      ;;
    --store)
      STORE=1
      shift
      ;;
    --backend)
      BACKEND="$2"
      shift 2
      ;;
    *)
      echo "unknown argument: $1" >&2
      exit 2
      ;;
  esac
done

ROT_C="${REPO_ROOT}/libs/ra8_dfu/src/ra8_rot.c"
HEADER_TMP="$(mktemp)"
trap 'rm -f "${HEADER_TMP}"' EXIT

if [[ -e "${KEY_FILE}" ]]; then
  echo "refusing to overwrite existing key: ${KEY_FILE}" >&2
  echo "(remove it deliberately, or pass --key <new-path>, to re-key)" >&2
  exit 1
fi

# 1. Generate the keypair and the C initialiser via the in-tree signer.
python3 "${REPO_ROOT}/tools/rot_sign.py" keygen --key "${KEY_FILE}" --pubkey-c "${HEADER_TMP}"
chmod 600 "${KEY_FILE}"

FINGERPRINT="$(openssl ec -in "${KEY_FILE}" -pubout 2>/dev/null | openssl dgst -sha256 | awk '{print $NF}')"

# 2. Optionally provision the public key into ra8_rot.c.
if [[ "${PATCH}" -eq 1 ]]; then
  python3 "${REPO_ROOT}/tools/rot_patch_pubkey.py" "${ROT_C}" "${HEADER_TMP}"
  if command -v clang-format-22 >/dev/null 2>&1; then
    clang-format-22 -i "${ROT_C}"
  elif command -v clang-format >/dev/null 2>&1; then
    clang-format -i "${ROT_C}"
  fi
fi

# 2b. Optionally persist the key into the versioned, tagged key store
# (OpenBao when reachable, else a local dir -- see scripts/secrets/rot_keystore.py).
if [[ "${STORE}" -eq 1 ]]; then
  python3 "${REPO_ROOT}/scripts/secrets/rot_keystore.py" --backend "${BACKEND}" \
    store --key "${KEY_FILE}" --note "provision ceremony"
fi

# 3. Report + tell the operator how to back the key up.
cat <<EOF

Root-of-trust key ceremony complete.
  Private key:  ${KEY_FILE}  (mode 0600 -- keep it secret)
  Pubkey SHA-256 fingerprint: ${FINGERPRINT}
  Provisioned into ra8_rot.c:  $([[ "${PATCH}" -eq 1 ]] && echo yes || echo "no (re-run with --patch)")

BACK UP THE PRIVATE KEY NOW so it cannot be lost. Persist it into the
versioned, tagged key store (OpenBao when reachable, else a local dir):
  scripts/secrets/rot_keystore.py store --key ${KEY_FILE}
  scripts/secrets/rot_keystore.py history         # list every stored version + tags
Re-run this ceremony with --store to do that automatically. Anyone with this
key can sign firmware.

To sign an application image for this key:
  python3 tools/rot_sign.py sign --key ${KEY_FILE} --image app.bin --out app.signed.bin --img-version N
EOF
