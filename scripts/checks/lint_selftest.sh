#!/bin/bash -p
# SPDX-License-Identifier: MIT
# Copyright (c) 2026 Brighton Sikarskie
# SHEBANG-SECURITY: -p blocks BASH_ENV and exported-function startup injection.
#
# Both-directions selftest for the two config-as-code gates that drive
# OFF-THE-SHELF tools (lint-cmake, lint-yaml). The gates that drive
# first-party checkers carry their own --selftest instead.
#
# Why this exists: a gate whose tool silently stops matching reports PASS
# forever. `require_cmd` proves the binary is on PATH; it does not prove the
# binary still fires. So before each real run, the gate feeds the tool a
# deliberately malformed file of that type (must FAIL) and a legal-but-tricky
# one (must PASS). Both directions, every run, against the SAME config the
# real check uses.
#
# Usage: lint_selftest.sh [--selftest] cmake|yaml

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

  usage() {
    echo "usage: lint_selftest.sh [--selftest] cmake|yaml" >&2
  }

  case "${1:-}" in
    --selftest)
      [[ "$#" -eq 2 ]] || {
        usage
        exit 2
      }
      MODE="$2"
      ;;
    cmake | yaml)
      [[ "$#" -eq 1 ]] || {
        usage
        exit 2
      }
      MODE="$1"
      ;;
    *)
      usage
      exit 2
      ;;
  esac
  REPO_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
  TMP="$(mktemp -d)"
  trap 'rm -rf "$TMP"' EXIT

  fail() {
    echo "SELFTEST FAIL: $*" >&2
    exit 1
  }

  # shellcheck source=scripts/dev/git_environment.sh
  . "$REPO_ROOT/scripts/dev/git_environment.sh"
  install_sanitized_git_environment

  case "$MODE" in
    cmake)
      # The config lives at the repo root, so run from a subdirectory of it.
      work="$REPO_ROOT/.lint_selftest_tmp"
      rm -rf "$work"
      mkdir -p "$work"
      trap 'rm -rf "$work"' EXIT

      # Deliberately defective: 7-space indent (C0307), a non-conforming
      # variable name (C0103), a line past the column limit (C0301), and a
      # statement missing its COMMENT (C0113). Four independent findings, so
      # the must-fire half survives any single rule being widened later.
      cat >"$work/malformed.cmake" <<'EOF'
if(TRUE)
       set(x 1)
endif()
add_custom_target(a_very_long_target_name_here COMMAND echo one two three four five six seven eight nine ten eleven)
EOF
      if cmake-lint "$work/malformed.cmake" >/dev/null 2>&1; then
        fail "cmake-lint accepted a defective listfile"
      fi
      echo "selftest: cmake-lint rejects a defective listfile OK"

      # Legal-but-tricky: bracket comment, bracket argument, a nested generator
      # expression, and a quoted string holding an unbalanced paren. Variable
      # names follow the tree's private-scope convention (leading underscore),
      # so a clean result here also proves .cmake-format.yaml's name patterns
      # accept the style the tree actually uses.
      cat >"$work/tricky.cmake" <<'EOF'
#[[ A bracket comment
    spanning lines. ]]
set(_msg [==[a bracket arg with ) and ; inside]==])
target_compile_options(tgt PRIVATE $<$<CONFIG:Debug>:-Og>)
set(_paren "unbalanced ( in a string")
EOF
      if ! cmake-lint "$work/tricky.cmake" >/dev/null 2>&1; then
        fail "cmake-lint rejected a legal listfile"
      fi
      echo "selftest: cmake-lint accepts legal-but-tricky input OK"
      ;;

    yaml)
      # yamllint: missing document start + a duplicate key.
      cat >"$TMP/malformed_style.yml" <<'EOF'
a: 1
a: 2
EOF
      if yamllint --strict -c "$REPO_ROOT/.yamllint.yaml" \
        "$TMP/malformed_style.yml" >/dev/null 2>&1; then
        fail "yamllint accepted a duplicate-key document with no ---"
      fi
      echo "selftest: yamllint rejects a malformed document OK"

      # Legal-but-tricky: a folded scalar, a flow sequence, and an `on:` key
      # YAML 1.1 would call boolean.
      cat >"$TMP/ok.yml" <<'EOF'
---
name: good
on:
  push:
    branches: [main]
note: >-
  one folded
  paragraph
EOF
      if ! yamllint --strict -c "$REPO_ROOT/.yamllint.yaml" \
        "$TMP/ok.yml" >/dev/null 2>&1; then
        fail "yamllint rejected a legal document"
      fi
      echo "selftest: yamllint accepts a legal-but-tricky document OK"
      ;;

    *)
      fail "unknown mode '$MODE' (expected cmake or yaml)"
      ;;
  esac
else
  [[ "$-" == *p* ]]
fi
