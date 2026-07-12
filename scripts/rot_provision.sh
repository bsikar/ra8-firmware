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
#   scripts/rot_provision.sh                     # keygen only (prints header)
#   scripts/rot_provision.sh --patch             # also write s_rot_root_pubkey
#   scripts/rot_provision.sh --key <path> --patch
set -euo pipefail

REPO_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
KEY_FILE="${HOME}/ra8d2-rot-signing-key.pem"
PATCH=0

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
  python3 - "${ROT_C}" "${HEADER_TMP}" <<'PY'
import re
import sys

rot_c, header = sys.argv[1], sys.argv[2]
lines = open(header, encoding="ascii").read().splitlines()
body = "\n".join(line for line in lines if line.strip().startswith("0x"))
new_array = "static const uint8_t s_rot_root_pubkey[k_ra8_rot_pubkey_bytes] = {\n" + body + "\n};"
src = open(rot_c, encoding="ascii").read()
patched, n = re.subn(
    r"static const uint8_t s_rot_root_pubkey\[k_ra8_rot_pubkey_bytes\] = \{.*?\n\};",
    new_array,
    src,
    count=1,
    flags=re.DOTALL,
)
if n != 1:
    sys.exit(f"expected exactly one s_rot_root_pubkey definition, found {n}")
open(rot_c, "w", encoding="ascii").write(patched)
print(f"patched {rot_c}")
PY
  if command -v clang-format-22 >/dev/null 2>&1; then
    clang-format-22 -i "${ROT_C}"
  elif command -v clang-format >/dev/null 2>&1; then
    clang-format -i "${ROT_C}"
  fi
fi

# 3. Report + tell the operator how to back the key up.
cat <<EOF

Root-of-trust key ceremony complete.
  Private key:  ${KEY_FILE}  (mode 0600 -- keep it secret)
  Pubkey SHA-256 fingerprint: ${FINGERPRINT}
  Provisioned into ra8_rot.c:  $([[ "${PATCH}" -eq 1 ]] && echo yes || echo "no (re-run with --patch)")

BACK UP THE PRIVATE KEY NOW so it cannot be lost. For example, into OpenBao:
  bao kv put secret/ra8d2/rot-signing-key pem=@${KEY_FILE} fingerprint=${FINGERPRINT}
(or store it in your password manager). Anyone with this key can sign firmware.

To sign an application image for this key:
  python3 tools/rot_sign.py sign --key ${KEY_FILE} --image app.bin --out app.signed.bin --img-version N
EOF
