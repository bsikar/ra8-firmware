#!/bin/bash -p
# SPDX-License-Identifier: MIT
# Copyright (c) 2026 Brighton Sikarskie
# SHEBANG-SECURITY: -p blocks BASH_ENV and exported-function startup injection.
#
# rot_provision.sh -- root-of-trust signing-key ceremony.
#
# Generates a NIST P-256 root keypair, writes the PRIVATE key out of the repo
# tree, emits the PUBLIC key as the s_rot_root_pubkey C initialiser, and (with
# --patch) provisions it into libs/ra8_dfu/src/ra8_rot.c. The private key never
# enters git; it is what tools/rot/src/rot_sign.py uses to sign every launched image.
#
# The signing key is the single anchor of the secure-boot chain of trust: if it
# is lost, no new image can be signed for the provisioned public key (you must
# re-key and re-flash). BACK IT UP -- the script prints how.
#
# Usage:
#   /bin/bash -p scripts/secrets/rot_provision.sh              # keygen only (prints header)
#   /bin/bash -p scripts/secrets/rot_provision.sh --patch      # also write s_rot_root_pubkey
#   /bin/bash -p scripts/secrets/rot_provision.sh --key <path> --patch
#   /bin/bash -p scripts/secrets/rot_provision.sh --patch --store
#   /bin/bash -p scripts/secrets/rot_provision.sh --store --backend local
#
# --store persists the key into the versioned, tagged key store
# (scripts/secrets/rot_keystore.py): OpenBao when reachable, else a local directory.
# For routine re-keying, prefer `scripts/secrets/rot_keystore.py rekey --patch`, which
# backs up the outgoing key, generates + tags a new one, and installs it.
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
  python3 "${REPO_ROOT}/tools/rot/src/rot_sign.py" keygen --key "${KEY_FILE}" --pubkey-c "${HEADER_TMP}"
  chmod 600 "${KEY_FILE}"

  FINGERPRINT="$(openssl ec -in "${KEY_FILE}" -pubout 2>/dev/null | openssl dgst -sha256 | awk '{print $NF}')"

  # 2. Optionally provision the public key into ra8_rot.c.
  if [[ "${PATCH}" -eq 1 ]]; then
    python3 "${REPO_ROOT}/tools/rot/src/rot_patch_pubkey.py" "${ROT_C}" "${HEADER_TMP}"
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
  python3 tools/rot/src/rot_sign.py sign --key ${KEY_FILE} --image app.bin --out app.signed.bin --img-version N
EOF
else
  [[ "$-" == *p* ]]
fi
