#!/bin/bash -p
# SPDX-License-Identifier: MIT
# Copyright (c) 2026 Brighton Sikarskie
# SHEBANG-SECURITY: -p blocks BASH_ENV and exported-function startup injection.
# Read-only identity and exact invocation helpers for the Ansible-installed HIL
# privilege boundary. A checkout never installs or updates the root helper;
# trusted Ansible convergence is the only writer.

RA8_HIL_PRIVILEGED_HELPER="/usr/local/libexec/ra8-hil-privileged"
_ra8_hil_privileged_lib_dir="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
_ra8_hil_privileged_manifest="${_ra8_hil_privileged_lib_dir}/ra8-hil-privileged.sha256"

ra8_hil_privileged_expected_identity() {
  local expected
  [[ -r "$_ra8_hil_privileged_manifest" ]] || {
    echo "HIL privileged helper identity manifest is missing" >&2
    return 2
  }
  expected="$(<"$_ra8_hil_privileged_manifest")"
  [[ "$expected" =~ ^[0-9a-f]{64}:[0-9a-f]{64}$ ]] || {
    echo "HIL privileged helper identity manifest is malformed" >&2
    return 2
  }
  printf '%s\n' "$expected"
}

ra8_hil_privileged_policy_local() {
  local iface
  iface="$("$RA8_HIL_PRIVILEGED_HELPER" --policy-interface 2>/dev/null)" || {
    echo "HIL privileged helper policy query failed" >&2
    return 2
  }
  [[ "$iface" =~ ^[A-Za-z][A-Za-z0-9_-]{0,14}$ ]] || {
    echo "HIL privileged helper returned an invalid policy interface" >&2
    return 2
  }
  printf '%s\n' "$iface"
}

ra8_hil_privileged_policy_remote() {
  local host="$1"
  local iface
  iface="$(ssh -o ConnectTimeout=5 -o BatchMode=yes "$host" \
    "/usr/local/libexec/ra8-hil-privileged --policy-interface" 2>/dev/null)" || {
    echo "remote HIL privileged helper policy query failed" >&2
    return 2
  }
  [[ "$iface" =~ ^[A-Za-z][A-Za-z0-9_-]{0,14}$ ]] || {
    echo "remote HIL privileged helper returned an invalid policy interface" >&2
    return 2
  }
  printf '%s\n' "$iface"
}

ra8_hil_privileged_verify_local() {
  local expected actual
  expected="$(ra8_hil_privileged_expected_identity)" || return $?
  [[ -x "$RA8_HIL_PRIVILEGED_HELPER" ]] || {
    echo "HIL privileged helper is not installed" >&2
    return 2
  }
  actual="$("$RA8_HIL_PRIVILEGED_HELPER" --identity 2>/dev/null)" || {
    echo "HIL privileged helper identity query failed" >&2
    return 2
  }
  [[ "$actual" == "$expected" ]] || {
    echo "HIL privileged helper differs from this checkout; run trusted Ansible convergence" >&2
    return 2
  }
}

ra8_hil_privileged_verify_remote() {
  local host="$1"
  local expected actual
  expected="$(ra8_hil_privileged_expected_identity)" || return $?
  actual="$(ssh -o ConnectTimeout=5 -o BatchMode=yes "$host" \
    "/usr/local/libexec/ra8-hil-privileged --identity" 2>/dev/null)" || {
    echo "remote HIL privileged helper identity query failed" >&2
    return 2
  }
  [[ "$actual" == "$expected" ]] || {
    echo "remote HIL privileged helper differs from this checkout; run trusted Ansible convergence" >&2
    return 2
  }
}

ra8_hil_privileged_run_local() {
  sudo -n -- "$RA8_HIL_PRIVILEGED_HELPER" "$@"
}
