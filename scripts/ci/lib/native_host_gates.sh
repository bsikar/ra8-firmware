#!/usr/bin/env bash
# SPDX-License-Identifier: MIT
# Copyright (c) 2026 Brighton Sikarskie
# shellcheck shell=bash
#
# WHICH GATES MEASURE THE HOST, and therefore must never be routed into the
# pinned Linux devcontainer.
#
# `just quality::gate::run <name>` is the documented way to run one registered
# gate on your own machine, and on macOS it sent EVERY gate into the Linux
# devcontainer: the CI toolchain lives in that image and the low-address host
# tests need Linux, so for almost every gate that is exactly right. It is
# wrong for a gate whose subject IS the host. `macos-host-build` exists to
# observe the native arm64 macOS SDK stub and Mach-O link path (#899); inside
# a Linux container it sees Linux/aarch64, refuses, and returns non-zero, so
# the one gate a Mac owner most needs to run was unreachable through the
# documented entry point. The nightly workflow calls
# `just quality::local::gate macos-host-build` directly and so never hit this.
#
# The list lives here rather than in a justfile so there is ONE place that
# knows it, and so the rule can be selftested: `--selftest` proves the
# predicate and the routing decision in both directions, checks every declared
# gate is really registered in scripts/ci.sh (a renamed gate would otherwise
# leave a dead row that silently routes nothing), and checks each declared
# gate's own body refuses a foreign host, since routing is a convenience and
# the gate body is the actual guard.

# name|required os (uname -s)|required arch (uname -m)|why a container cannot answer it
RA8_NATIVE_HOST_GATES=(
  "macos-host-build|Darwin|arm64|it measures the active macOS SDK's libSystem stub and the native Mach-O link path (#899)"
)

_ra8_native_host_row() {
  local name="$1" row
  [ -n "${name}" ] || return 1
  for row in "${RA8_NATIVE_HOST_GATES[@]}"; do
    if [ "${row%%|*}" = "${name}" ]; then
      printf '%s\n' "${row}"
      return 0
    fi
  done
  return 1
}

# ra8_gate_requires_native_host <gate> -- success when the gate's subject is
# the host itself, so it must run natively wherever it runs at all.
ra8_gate_requires_native_host() {
  _ra8_native_host_row "${1:-}" > /dev/null 2>&1
}

# ra8_native_host_gate_reason <gate> -- the one-line why, for a caller that
# wants to explain its routing. Empty (and non-zero) for any other gate.
ra8_native_host_gate_reason() {
  local row
  row="$(_ra8_native_host_row "${1:-}")" || return 1
  printf '%s\n' "${row##*|}"
}

# ra8_native_host_gate_host <gate> -- the host this gate must run on, as
# "<uname -s>/<uname -m>". Empty (and non-zero) for any other gate.
ra8_native_host_gate_host() {
  local row rest os arch
  row="$(_ra8_native_host_row "${1:-}")" || return 1
  rest="${row#*|}"
  os="${rest%%|*}"
  rest="${rest#*|}"
  arch="${rest%%|*}"
  printf '%s/%s\n' "${os}" "${arch}"
}

# ra8_gate_host_route <gate> <uname -s> -- "native" or "container".
#
# Off macOS nothing changes: Linux runs the exact gate natively, as it always
# did. On macOS the devcontainer stays the default and only a host-measuring
# gate is kept out of it.
ra8_gate_host_route() {
  local name="${1:-}" host_os="${2:-}"
  if [ "${host_os}" != "Darwin" ]; then
    printf 'native\n'
  elif ra8_gate_requires_native_host "${name}"; then
    printf 'native\n'
  else
    printf 'container\n'
  fi
}

# ra8_native_host_gate_announce <gate> -- say why a gate skipped the container,
# so a Mac owner is not left wondering which environment ran. Silent for every
# other gate; never fails the caller.
ra8_native_host_gate_announce() {
  local reason want
  reason="$(ra8_native_host_gate_reason "${1:-}")" || return 0
  want="$(ra8_native_host_gate_host "${1:-}")"
  printf '==> %s runs natively, not in the devcontainer: %s\n' "${1}" "${reason}" >&2
  printf '==> it needs a %s host; the gate body refuses anything else.\n' "${want}" >&2
}

# --- selftest ---------------------------------------------------------------

_ra8_native_host_repo_root() {
  (cd "$(dirname "${BASH_SOURCE[0]}")/../../.." && pwd)
}

# Does scripts/ci.sh register this gate name? Kept as a seam so the selftest
# can prove the lookup actually looks, rather than answering yes to anything.
_ra8_native_host_registered() {
  local name="$1" registry="$2"
  grep -q "\"${name}|" "${registry}"
}

# A declared native-host gate must itself refuse a foreign host: routing is a
# convenience, the body is the guard.
_ra8_native_host_body_guards() {
  case "$1" in
    *'uname -s'*) return 0 ;;
    *) return 1 ;;
  esac
}

_ra8_native_host_gate_body() {
  local name="$1" dir="$2" fn
  fn="gate_$(printf '%s' "${name}" | tr '-' '_')"
  awk -v fn="${fn}()" 'index($0, fn) == 1 {inside = 1} inside {print} inside && /^\)$/ {exit}' \
    "${dir}"/*.sh
}

ra8_native_host_gates_selftest() {
  local root registry gate_dir row name body fails=0

  check() {
    if [ "$1" = "yes" ]; then
      printf 'ok   %s\n' "$2"
    else
      printf 'FAIL %s\n' "$2"
      fails=$((fails + 1))
    fi
  }
  # `yn` answers "did this succeed", `nn` answers "did this fail". Two
  # helpers rather than a leading `!` argument, which is not a command.
  yn() { if "$@" > /dev/null 2>&1; then printf 'yes\n'; else printf 'no\n'; fi; }
  nn() { if "$@" > /dev/null 2>&1; then printf 'no\n'; else printf 'yes\n'; fi; }

  # The predicate, both directions.
  check "$(yn ra8_gate_requires_native_host macos-host-build)" \
    "macos-host-build requires a native host"
  check "$(nn ra8_gate_requires_native_host tidy)" \
    "an ordinary gate (tidy) does not"
  check "$(nn ra8_gate_requires_native_host no-such-gate)" \
    "an unknown gate name does not"
  check "$(nn ra8_gate_requires_native_host '')" \
    "an empty gate name does not"

  # The routing decision, across the host matrix.
  check "$([ "$(ra8_gate_host_route macos-host-build Darwin)" = native ] && echo yes || echo no)" \
    "Darwin + macos-host-build routes native"
  check "$([ "$(ra8_gate_host_route tidy Darwin)" = container ] && echo yes || echo no)" \
    "Darwin + tidy still routes into the devcontainer"
  check "$([ "$(ra8_gate_host_route macos-host-build Linux)" = native ] && echo yes || echo no)" \
    "Linux + macos-host-build routes native"
  check "$([ "$(ra8_gate_host_route tidy Linux)" = native ] && echo yes || echo no)" \
    "Linux + tidy routes native, unchanged"

  # The explanation surface.
  check "$([ -n "$(ra8_native_host_gate_reason macos-host-build)" ] && echo yes || echo no)" \
    "a declared gate carries a reason"
  check "$([ "$(ra8_native_host_gate_host macos-host-build)" = Darwin/arm64 ] && echo yes || echo no)" \
    "macos-host-build declares a Darwin/arm64 host"
  check "$(nn ra8_native_host_gate_reason tidy)" \
    "an ordinary gate carries none"
  check "$([ -z "$(ra8_native_host_gate_announce tidy 2>&1)" ] && echo yes || echo no)" \
    "the announcement is silent for an ordinary gate"
  check "$([ -n "$(ra8_native_host_gate_announce macos-host-build 2>&1)" ] && echo yes || echo no)" \
    "the announcement speaks for a declared gate"

  root="$(_ra8_native_host_repo_root)"
  registry="${root}/scripts/ci.sh"
  gate_dir="${root}/scripts/ci/gates"

  # The registry lookup must actually look, or the drift check below is a
  # rubber stamp.
  check "$(yn _ra8_native_host_registered ci-parity "${registry}")" \
    "the registry lookup finds a known gate"
  check "$(nn _ra8_native_host_registered definitely-not-a-gate "${registry}")" \
    "the registry lookup rejects a made-up gate"

  # The body guard, both directions, before it is used live.
  check "$(yn _ra8_native_host_body_guards 'host_os="$(uname -s)"')" \
    "a body that reads uname -s counts as guarded"
  check "$(nn _ra8_native_host_body_guards 'zig build test')" \
    "a body that does not is unguarded"

  # Live: every declared gate is registered, and its body refuses a foreign
  # host. A renamed or deleted gate fails here instead of routing nothing.
  for row in "${RA8_NATIVE_HOST_GATES[@]}"; do
    name="${row%%|*}"
    check "$(yn _ra8_native_host_registered "${name}" "${registry}")" \
      "declared gate ${name} is registered in scripts/ci.sh"
    body="$(_ra8_native_host_gate_body "${name}" "${gate_dir}")"
    check "$([ -n "${body}" ] && echo yes || echo no)" \
      "declared gate ${name} has a body under scripts/ci/gates"
    check "$(yn _ra8_native_host_body_guards "${body}")" \
      "declared gate ${name} refuses a foreign host itself"
  done

  if [ "${fails}" -ne 0 ]; then
    printf 'native_host_gates.sh --selftest: %s case(s) FAILED\n' "${fails}" >&2
    return 1
  fi
  printf 'native_host_gates.sh --selftest: PASS (%s declared gate(s))\n' \
    "${#RA8_NATIVE_HOST_GATES[@]}"
}

if [[ "${BASH_SOURCE[0]}" == "$0" ]]; then
  case "${1:-}" in
    --selftest) ra8_native_host_gates_selftest ;;
    *)
      echo "Usage: scripts/ci/lib/native_host_gates.sh --selftest" >&2
      exit 2
      ;;
  esac
fi
