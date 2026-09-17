#!/usr/bin/env bash
# SPDX-License-Identifier: MIT
# Copyright (c) 2026 Brighton Sikarskie
# shellcheck shell=bash
#
# WHICH ZIG BUILD ROOTS the macos-host-build gate builds and tests natively,
# and for every root it does NOT, the reason.
#
# The gate carried this list inline, as three hard-coded directory names in a
# `for` loop. Two things were wrong with that. A host root added later is
# silently outside the gate: it takes its default target from
# ra8_build.hostDefaultTargetQuery (check_zig.py's host-target rule makes sure
# of that, #899/#989), so it looks correct from Linux and nothing ever builds
# it on a Mac. And the four roots the gate skips looked like an oversight
# rather than a decision, so the log could not say why a root was absent and a
# reviewer could not tell a deliberate deferral from a forgotten one.
#
# So the coverage lives here, one row per root, every row carrying its reason,
# and `--selftest` fails when a build root exists that this file does not
# mention in either direction. Adding a root is then a deliberate edit: cover
# it, or say in writing why not.
#
# Coverage values:
#   covered   built and tested by the gate on the arm64 macOS runner
#   deferred  knowingly outside the gate today, for the stated reason

# root (relative to the repo)|coverage|reason
RA8_MACOS_HOST_ROOTS=(
  "tools/zig_build|covered|it defines the host target selection, so it is the first root that must build and test natively"
  "apps/host/image_pyramid|covered|a pure-Zig host app needing no toolchain beyond zig, and the root whose Mach-O the gate reads back"
  "tests/zig_abi_fixture|covered|a pure-Zig test root needing no toolchain beyond zig"
  "apps/host/reg_gen|deferred|its generated-header contract resolves a C23 front end at run time and the zig cc fallback leg is unexercised; cover it once the nightly shows which front end the runner resolves (#1035)"
  "apps/host/firmware_pipeline/zig|deferred|it links a cargo-built archive and the macOS workflow provisions no Rust toolchain; a wrong-target archive is now a named refusal rather than a bare undefined symbol (#1141)"
  "tests/abi_chain_fixture|deferred|same cargo-built archive dependency as apps/host/firmware_pipeline/zig"
  "tests/rust_abi_fixture/zig|deferred|same cargo-built archive dependency as apps/host/firmware_pipeline/zig"
)

_ra8_macos_host_row() {
  local root="$1" row
  [ -n "${root}" ] || return 1
  for row in "${RA8_MACOS_HOST_ROOTS[@]}"; do
    if [ "${row%%|*}" = "${root}" ]; then
      printf '%s\n' "${row}"
      return 0
    fi
  done
  return 1
}

# ra8_macos_host_root_coverage <root> -- "covered" or "deferred". Empty (and
# non-zero) for a root this file does not declare, which is the case the
# selftest turns into a failure.
ra8_macos_host_root_coverage() {
  local row rest
  row="$(_ra8_macos_host_row "${1:-}")" || return 1
  rest="${row#*|}"
  printf '%s\n' "${rest%%|*}"
}

# ra8_macos_host_root_reason <root> -- the one-line why, either why it is in
# the gate or why it is not.
ra8_macos_host_root_reason() {
  local row
  row="$(_ra8_macos_host_row "${1:-}")" || return 1
  printf '%s\n' "${row##*|}"
}

# ra8_macos_host_covered_roots -- the roots the gate builds, one per line, in
# declaration order (the definer first, so a broken selection fails fast).
ra8_macos_host_covered_roots() {
  local row rest
  for row in "${RA8_MACOS_HOST_ROOTS[@]}"; do
    rest="${row#*|}"
    [ "${rest%%|*}" = "covered" ] || continue
    printf '%s\n' "${row%%|*}"
  done
}

# ra8_macos_host_deferred_roots -- the roots the gate does NOT build, one
# "root|reason" per line.
ra8_macos_host_deferred_roots() {
  local row rest
  for row in "${RA8_MACOS_HOST_ROOTS[@]}"; do
    rest="${row#*|}"
    [ "${rest%%|*}" = "deferred" ] || continue
    printf '%s|%s\n' "${row%%|*}" "${row##*|}"
  done
}

# ra8_macos_host_announce_coverage -- print the coverage into the gate log, so
# a green run states what it did NOT measure instead of implying the whole
# tree builds on macOS.
ra8_macos_host_announce_coverage() {
  local root line
  printf 'build roots this gate builds and tests natively:\n'
  while IFS= read -r root; do
    printf '  %s -- %s\n' "${root}" "$(ra8_macos_host_root_reason "${root}")"
  done < <(ra8_macos_host_covered_roots)
  printf 'build roots this gate does NOT measure (a green run says nothing about these):\n'
  while IFS= read -r line; do
    printf '  %s -- %s\n' "${line%%|*}" "${line##*|}"
  done < <(ra8_macos_host_deferred_roots)
}

# --- selftest ---------------------------------------------------------------

_ra8_macos_host_repo_root() {
  (cd "$(dirname "${BASH_SOURCE[0]}")/../../.." && pwd)
}

# Every directory in the repo holding a build.zig, relative to the repo root.
# Kept as a seam so the selftest can prove the discovery actually finds
# something before the drift check leans on it.
_ra8_macos_host_discover_roots() {
  local root="$1"
  (
    cd "${root}" || return 1
    find . -name build.zig -not -path './.git/*' -not -path '*/zig-out/*' \
      -not -path '*/.zig-cache/*' -not -path '*/zig-cache/*' \
      -print 2> /dev/null |
      sed -e 's|^\./||' -e 's|/build\.zig$||' |
      sort
  )
}

# Does the gate body actually read this manifest? A hard-coded loop would
# otherwise drift straight back in while every case here still passed.
_ra8_macos_host_gate_consumes() {
  case "$1" in
    *ra8_macos_host_covered_roots*) return 0 ;;
    *) return 1 ;;
  esac
}

_ra8_macos_host_gate_body() {
  awk 'index($0, "gate_macos_host_build()") == 1 {inside = 1} inside {print} inside && /^\)$/ {exit}' \
    "$1"/*.sh
}

ra8_macos_host_roots_selftest() {
  local root row name coverage reason found declared body fails=0

  check() {
    if [ "$1" = "yes" ]; then
      printf 'ok   %s\n' "$2"
    else
      printf 'FAIL %s\n' "$2"
      fails=$((fails + 1))
    fi
  }
  # `yn` answers "did this succeed", `nn` answers "did this fail". Two helpers
  # rather than a leading `!` argument, which is not a command.
  yn() { if "$@" > /dev/null 2>&1; then printf 'yes\n'; else printf 'no\n'; fi; }
  nn() { if "$@" > /dev/null 2>&1; then printf 'no\n'; else printf 'yes\n'; fi; }

  # The lookup, both directions.
  check "$([ "$(ra8_macos_host_root_coverage tools/zig_build)" = covered ] && echo yes || echo no)" \
    "tools/zig_build is covered"
  check "$([ "$(ra8_macos_host_root_coverage apps/host/reg_gen)" = deferred ] && echo yes || echo no)" \
    "apps/host/reg_gen is deferred"
  check "$(nn ra8_macos_host_root_coverage nope/not/a/root)" \
    "an undeclared root has no coverage"
  check "$(nn ra8_macos_host_root_coverage '')" \
    "an empty root name has no coverage"

  # Each list holds only its own kind, and neither is empty: a gate with no
  # covered roots would pass every leg while measuring nothing.
  check "$([ -n "$(ra8_macos_host_covered_roots)" ] && echo yes || echo no)" \
    "at least one root is covered"
  check "$([ -n "$(ra8_macos_host_deferred_roots)" ] && echo yes || echo no)" \
    "the deferred list is reported too"
  found=yes
  while IFS= read -r name; do
    [ "$(ra8_macos_host_root_coverage "${name}")" = covered ] || found=no
  done < <(ra8_macos_host_covered_roots)
  check "${found}" "every entry of the covered list is declared covered"
  found=yes
  while IFS= read -r row; do
    [ "$(ra8_macos_host_root_coverage "${row%%|*}")" = deferred ] || found=no
  done < <(ra8_macos_host_deferred_roots)
  check "${found}" "every entry of the deferred list is declared deferred"

  # Rows must be well formed: a known coverage value, a real reason, no
  # duplicate root. An unrecognised value would otherwise silently drop a
  # root out of both lists.
  found=yes
  for row in "${RA8_MACOS_HOST_ROOTS[@]}"; do
    name="${row%%|*}"
    coverage="$(ra8_macos_host_root_coverage "${name}")"
    reason="$(ra8_macos_host_root_reason "${name}")"
    case "${coverage}" in
      covered | deferred) ;;
      *) found=no ;;
    esac
    [ -n "${name}" ] || found=no
    [ -n "${reason}" ] || found=no
    [ "${reason}" != "${coverage}" ] || found=no
  done
  check "${found}" "every row carries a known coverage value and a reason"
  check "$([ "$(printf '%s\n' "${RA8_MACOS_HOST_ROOTS[@]}" | cut -d'|' -f1 | sort -u | wc -l)" \
    -eq "${#RA8_MACOS_HOST_ROOTS[@]}" ] && echo yes || echo no)" \
    "no root is declared twice"

  root="$(_ra8_macos_host_repo_root)"

  # Prove the discovery looks before the drift check trusts it.
  declared="$(_ra8_macos_host_discover_roots "${root}")"
  check "$([ -n "${declared}" ] && echo yes || echo no)" \
    "the build-root discovery finds roots in this checkout"
  check "$(printf '%s\n' "${declared}" | grep -qx 'tools/zig_build' && echo yes || echo no)" \
    "the discovery finds tools/zig_build"
  check "$(printf '%s\n' "${declared}" | grep -qx 'nope/not/a/root' && echo no || echo yes)" \
    "the discovery does not invent roots"

  # Live drift, both directions. A new build.zig must be declared here, and a
  # declared root must still exist.
  while IFS= read -r name; do
    check "$(yn _ra8_macos_host_row "${name}")" \
      "build root ${name} is declared in this manifest"
  done <<< "${declared}"
  for row in "${RA8_MACOS_HOST_ROOTS[@]}"; do
    name="${row%%|*}"
    check "$([ -f "${root}/${name}/build.zig" ] && echo yes || echo no)" \
      "declared root ${name} still holds a build.zig"
  done

  # The consumption predicate, both directions, then live against the gate.
  check "$(yn _ra8_macos_host_gate_consumes 'for root in $(ra8_macos_host_covered_roots); do')" \
    "a body reading the covered list counts as consuming it"
  check "$(nn _ra8_macos_host_gate_consumes 'for root in tools/zig_build; do')" \
    "a hard-coded loop does not"
  body="$(_ra8_macos_host_gate_body "${root}/scripts/ci/gates")"
  check "$([ -n "${body}" ] && echo yes || echo no)" \
    "gate_macos_host_build has a body under scripts/ci/gates"
  check "$(yn _ra8_macos_host_gate_consumes "${body}")" \
    "gate_macos_host_build takes its roots from this manifest"

  # The announcement must name every root, covered and deferred, or a green
  # log overstates what ran.
  body="$(ra8_macos_host_announce_coverage)"
  found=yes
  for row in "${RA8_MACOS_HOST_ROOTS[@]}"; do
    printf '%s\n' "${body}" | grep -q -- "${row%%|*}" || found=no
  done
  check "${found}" "the announcement names every declared root"

  if [ "${fails}" -ne 0 ]; then
    printf 'macos_host_roots.sh --selftest: %s case(s) FAILED\n' "${fails}" >&2
    return 1
  fi
  printf 'macos_host_roots.sh --selftest: PASS (%s declared root(s), %s covered)\n' \
    "${#RA8_MACOS_HOST_ROOTS[@]}" "$(ra8_macos_host_covered_roots | wc -l | tr -d ' ')"
}

if [[ "${BASH_SOURCE[0]}" == "$0" ]]; then
  case "${1:-}" in
    --selftest) ra8_macos_host_roots_selftest ;;
    *)
      echo "Usage: scripts/ci/lib/macos_host_roots.sh --selftest" >&2
      exit 2
      ;;
  esac
fi
