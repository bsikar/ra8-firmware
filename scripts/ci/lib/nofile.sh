#!/usr/bin/env bash
# SPDX-License-Identifier: MIT
# Copyright (c) 2026 Brighton Sikarskie
# shellcheck shell=bash
#
# Canonical file-descriptor budget for containerised builds. The coverage host
# test link reopens more than Docker's default soft limit of 1024 object files,
# so lowering build parallelism cannot repair it: one linker process alone can
# cross the limit.

RA8_NOFILE_TARGET="${RA8_NOFILE_TARGET:-65536}"

ra8_nofile_validate_target() {
  [[ "$1" =~ ^[1-9][0-9]*$ ]] || {
    echo "ERROR: RA8_NOFILE_TARGET must be a positive integer (got '$1')." >&2
    return 1
  }
}

# Return the requested limit bounded to a numeric hard ceiling. `unlimited`
# means there is no lower ceiling to apply.
ra8_nofile_bounded_target() {
  local requested="$1" hard="$2"
  ra8_nofile_validate_target "$requested" || return 1
  if [[ "$hard" == "unlimited" ]]; then
    printf '%s\n' "$requested"
  elif [[ "$hard" =~ ^[1-9][0-9]*$ ]]; then
    ((requested <= hard)) && printf '%s\n' "$requested" || printf '%s\n' "$hard"
  else
    echo "ERROR: unreadable hard nofile limit '$hard'." >&2
    return 1
  fi
}

# Return success only when both reported limits are numeric and meet the target.
ra8_nofile_limits_meet_target() {
  local target="$1" soft="$2" hard="$3"
  ra8_nofile_validate_target "$target" || return 1
  [[ "$soft" =~ ^(0|[1-9][0-9]*)$ && "$hard" =~ ^(0|[1-9][0-9]*)$ ]] || return 1
  ((soft >= target && hard >= target))
}

# Docker, Podman, and nerdctl accept this common run-argument spelling.
ra8_nofile_explicit_container_run_args() {
  local target="$1"
  ra8_nofile_validate_target "$target" || return 1
  printf '%s\n' --ulimit "nofile=$target:$target"
}

# Parse exactly two non-empty output lines: the default soft and hard limits.
ra8_nofile_parse_probe() {
  local output="$1" soft hard
  [[ "$output" == *$'\n'* ]] || return 1
  soft="${output%%$'\n'*}"
  hard="${output#*$'\n'}"
  [[ -n "$soft" && -n "$hard" && "$hard" != *$'\n'* ]] || return 1
  printf '%s\n%s\n' "$soft" "$hard"
}

# Probe an image without mounts or an explicit limit. Omit --ulimit only when
# both defaults are readable numeric values that already meet the target.
ra8_nofile_container_run_args() {
  local image="${1:-}" probe limits soft hard
  (($# == 0)) || shift
  local runtime=("$@")
  ra8_nofile_validate_target "$RA8_NOFILE_TARGET" || return 1
  [[ -n "$image" && "${#runtime[@]}" -gt 0 ]] || {
    echo "ERROR: no image/runtime supplied for the nofile probe." >&2
    return 1
  }

  if probe="$("${runtime[@]}" run --rm --read-only "$image" /bin/bash -c \
    'ulimit -Sn; ulimit -Hn')" && limits="$(ra8_nofile_parse_probe "$probe")"; then
    soft="${limits%%$'\n'*}"
    hard="${limits#*$'\n'}"
    if ra8_nofile_limits_meet_target "$RA8_NOFILE_TARGET" "$soft" "$hard"; then
      echo "==> container default nofile: soft=$soft hard=$hard; explicit --ulimit omitted" >&2
      return 0
    fi
    echo "WARNING: container default nofile is not a sufficient numeric pair" \
      "(soft=$soft hard=$hard; required=$RA8_NOFILE_TARGET); retaining --ulimit." >&2
  else
    echo "WARNING: container default nofile probe was unreadable; retaining --ulimit." >&2
  fi
  ra8_nofile_explicit_container_run_args "$RA8_NOFILE_TARGET"
}

# Raise the current shell's soft limit for a producer invoked inside a caller-
# created container. A hard ceiling below the known-safe target is a clear
# transport error, not a reason to rediscover EMFILE halfway through linking.
ra8_raise_nofile_soft_limit() {
  local hard bounded current
  hard="$(ulimit -Hn)"
  bounded="$(ra8_nofile_bounded_target "$RA8_NOFILE_TARGET" "$hard")" || return 1
  current="$(ulimit -Sn)"
  if [[ "$bounded" -lt "$RA8_NOFILE_TARGET" ]]; then
    echo "ERROR: hard nofile limit $hard is below required $RA8_NOFILE_TARGET." >&2
    echo "       Run the container with --ulimit nofile=$RA8_NOFILE_TARGET:$RA8_NOFILE_TARGET." >&2
    return 1
  fi
  if [[ "$current" != "unlimited" && "$current" -lt "$bounded" ]]; then
    ulimit -Sn "$bounded"
  fi
  echo "==> file-descriptor budget: soft=$(ulimit -Sn) hard=$hard (required $RA8_NOFILE_TARGET)"
}

ra8_nofile_selftest() {
  local explicit=$'--ulimit\nnofile=65536:65536' got
  [[ "$(ra8_nofile_bounded_target 65536 unlimited)" == "65536" ]] || return 1
  [[ "$(ra8_nofile_bounded_target 65536 524288)" == "65536" ]] || return 1
  [[ "$(ra8_nofile_bounded_target 65536 1024)" == "1024" ]] || return 1
  ! ra8_nofile_bounded_target 0 524288 >/dev/null 2>&1 || return 1
  ra8_nofile_limits_meet_target 65536 524288 524288 || return 1
  ! ra8_nofile_limits_meet_target 65536 unlimited 524288 || return 1
  ! ra8_nofile_limits_meet_target 65536 65535 524288 || return 1
  ! ra8_nofile_limits_meet_target 65536 524288 65535 || return 1
  [[ "$(ra8_nofile_parse_probe $'524288\n524288')" == $'524288\n524288' ]] || return 1
  ! ra8_nofile_parse_probe $'524288\nunlimited\nextra' >/dev/null || return 1
  [[ "$(ra8_nofile_explicit_container_run_args 65536)" == "$explicit" ]] || return 1

  got="$(RA8_NOFILE_TARGET=65536 ra8_nofile_container_run_args image \
    ra8_nofile_selftest_adequate 2>/dev/null)"
  [[ -z "$got" ]] || return 1
  got="$(RA8_NOFILE_TARGET=65536 ra8_nofile_container_run_args image \
    ra8_nofile_selftest_unreadable 2>/dev/null)"
  [[ "$got" == "$explicit" ]] || return 1
  got="$(RA8_NOFILE_TARGET=65536 ra8_nofile_container_run_args image \
    ra8_nofile_selftest_low 2>/dev/null)"
  [[ "$got" == "$explicit" ]] || return 1
  got="$(RA8_NOFILE_TARGET=65536 ra8_nofile_container_run_args image \
    ra8_nofile_selftest_failed 2>/dev/null)"
  [[ "$got" == "$explicit" ]] || return 1
  echo "nofile.sh --selftest: PASS (bounded, adequate-default, and fail-safe directions)"
}

ra8_nofile_selftest_adequate() { printf '524288\n524288\n'; }
ra8_nofile_selftest_unreadable() { printf 'unlimited\n524288\n'; }
ra8_nofile_selftest_low() { printf '1024\n524288\n'; }
ra8_nofile_selftest_failed() { return 1; }

if [[ "${BASH_SOURCE[0]}" == "$0" ]]; then
  case "${1:-}" in
    --selftest) ra8_nofile_selftest ;;
    *)
      echo "Usage: scripts/ci/lib/nofile.sh --selftest" >&2
      exit 2
      ;;
  esac
fi
