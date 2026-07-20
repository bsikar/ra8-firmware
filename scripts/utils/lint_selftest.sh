#!/usr/bin/env bash
# SPDX-License-Identifier: MIT
# Copyright (c) 2026 Brighton Sikarskie
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
# Usage: lint_selftest.sh cmake|yaml

set -euo pipefail

MODE="${1:?usage: lint_selftest.sh cmake|yaml}"
REPO_ROOT="$(git rev-parse --show-toplevel)"
TMP="$(mktemp -d)"
trap 'rm -rf "$TMP"' EXIT

fail() {
  echo "SELFTEST FAIL: $*" >&2
  exit 1
}

# actionlint refuses to run outside a git project ("no project was found in
# any parent directories"). That exits NON-ZERO, so a scratch tree without a
# repo makes the "must reject the malformed workflow" half pass for entirely
# the wrong reason -- the selftest would then be as blind as the gate it is
# supposed to protect. Every scratch tree gets a real repo.
init_scratch_repo() {
  git -C "$1" init -q
  git -C "$1" config user.email selftest@invalid
  git -C "$1" config user.name selftest
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
