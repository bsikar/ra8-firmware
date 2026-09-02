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

  # actionlint refuses to run outside a git project ("no project was found in
  # any parent directories"). That exits NON-ZERO, so a scratch tree without a
  # repo makes the "must reject the malformed workflow" half pass for entirely
  # the wrong reason -- the selftest would then be as blind as the gate it is
  # supposed to protect. Every scratch tree gets a real repo.
  init_scratch_repo() {
    "$RA8_TRUSTED_GIT" -C "$1" init -q
    "$RA8_TRUSTED_GIT" -C "$1" config user.email selftest@invalid
    "$RA8_TRUSTED_GIT" -C "$1" config user.name selftest
  }

  case "$MODE" in
    cmake)
      # The config lives at the repo root, so run from a subdirectory of it.
      work="$REPO_ROOT/.lint_selftest_tmp"
      rm -rf "$work"
      mkdir -p "$work"
      trap 'rm -rf "$work"' EXIT

      # Deliberately misformatted: 7-space indent, no dangling paren, a line
      # well past the 100-column limit.
      cat >"$work/malformed.cmake" <<'EOF'
if(TRUE)
       set(x 1)
endif()
add_custom_target(a_very_long_target_name_here COMMAND echo one two three four five six seven eight nine ten eleven)
EOF
      if cmake-format --check "$work/malformed.cmake" >/dev/null 2>&1; then
        fail "cmake-format accepted a misformatted listfile"
      fi
      echo "selftest: cmake-format rejects a misformatted listfile OK"

      # Legal-but-tricky: bracket comment, bracket argument, a nested generator
      # expression, and a quoted string holding an unbalanced paren. Variable
      # names follow the tree's private-scope convention (leading underscore),
      # so a clean result here also proves .cmake-format.yaml's name patterns
      # accept the style the tree actually uses.
      cat >"$work/tricky.cmake.in" <<'EOF'
#[[ A bracket comment
    spanning lines. ]]
set(_msg [==[a bracket arg with ) and ; inside]==])
target_compile_options(tgt PRIVATE $<$<CONFIG:Debug>:-Og>)
set(_paren "unbalanced ( in a string")
EOF
      cmake-format "$work/tricky.cmake.in" >"$work/tricky.cmake"
      if ! cmake-format --check "$work/tricky.cmake" >/dev/null 2>&1; then
        fail "cmake-format is not idempotent on legal input"
      fi
      if ! cmake-lint "$work/tricky.cmake" >/dev/null 2>&1; then
        fail "cmake-lint rejected a legal listfile"
      fi
      echo "selftest: cmake-format/cmake-lint accept legal-but-tricky input OK"
      ;;

    yaml)
      # Two defects, both in the class behind #357: an `on:` trigger GitHub does
      # not recognise (so the workflow silently never runs) and an expression
      # referencing a context property that does not exist (so the step reads
      # empty forever). An earlier revision of this fixture used
      # `github.event.<unknown>` and `fetch-depth: not-a-number`, BOTH of which
      # actionlint legitimately accepts -- it passed only because actionlint was
      # exiting non-zero for want of a git project. Pin defects it truly rejects.
      cat >"$TMP/malformed.yml" <<'EOF'
name: broken
on: pushh
jobs:
  a:
    runs-on: ubuntu-latest
    steps:
      - run: echo "${{ github.nonexistent_ctx }}"
EOF
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

      # actionlint must reject a bad workflow expression. Run it on a scratch
      # tree so the repo's own .github is not in scope.
      mkdir -p "$TMP/wf/.github/workflows"
      init_scratch_repo "$TMP/wf"
      cp "$TMP/malformed.yml" "$TMP/wf/.github/workflows/bad.yml"
      if (cd "$TMP/wf" && actionlint >/dev/null 2>&1); then
        fail "actionlint accepted a workflow with an invalid expression"
      fi
      echo "selftest: actionlint rejects an invalid workflow OK"

      # Legal-but-tricky: folded expression, a matrix, a self-hosted label the
      # repo config declares, and an `on:` key YAML 1.1 would call boolean.
      mkdir -p "$TMP/ok/.github/workflows"
      init_scratch_repo "$TMP/ok"
      cp "$REPO_ROOT/.github/actionlint.yaml" "$TMP/ok/.github/actionlint.yaml"
      cat >"$TMP/ok/.github/workflows/good.yml" <<'EOF'
---
name: good
on:
  push:
    branches: [main]
jobs:
  a:
    runs-on: [self-hosted, Linux, X64]
    if: >-
      github.event_name != 'pull_request'
      || github.event.pull_request.head.repo.full_name == github.repository
    strategy:
      matrix:
        n: [1, 2]
    steps:
      - uses: actions/checkout@v4
      - run: echo "${{ matrix.n }}"
EOF
      if ! yamllint --strict -c "$REPO_ROOT/.yamllint.yaml" \
        "$TMP/ok/.github/workflows/good.yml" >/dev/null 2>&1; then
        fail "yamllint rejected a legal workflow"
      fi
      if ! (cd "$TMP/ok" && actionlint >/dev/null 2>&1); then
        fail "actionlint rejected a legal workflow"
      fi
      echo "selftest: yamllint/actionlint accept a legal-but-tricky workflow OK"
      ;;

    *)
      fail "unknown mode '$MODE' (expected cmake or yaml)"
      ;;
  esac
else
  [[ "$-" == *p* ]]
fi
