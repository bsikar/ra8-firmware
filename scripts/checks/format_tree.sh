#!/usr/bin/env bash
# SPDX-License-Identifier: MIT
# Copyright (c) 2026 Brighton Sikarskie
#
# scripts/checks/format_tree.sh -- run EVERY first-party formatter over the
# tree: C (clang-format + comment pass), Go (gofmt), Python (ruff format),
# shell (shfmt), CMake (cmake-format), justfiles (just --fmt).
#
# The `format` / `check` just recipes and the CI `format` gate both drive this
# script. Every language's file list comes from ONE seam definition
# (run_scope below): the format lanes, the --list-files mode, and the selftest
# all read it, so the three cannot drift from each other. lint-coverage asks
# this script (not the lint checkers) which files are formatted -- formatting
# is owned here alone, and a language this script stopped formatting would go
# red there rather than passing silently in a lint gate that no longer checks
# it.
#
# A missing formatter is a HARD FAILURE, never a silent skip: a language the
# toolchain cannot format is a language CI cannot be checking, and a script
# that quietly drops a language is the #296/#332/#358 collapse defect wearing
# a different name. The per-language dirty/canonical proofs live in each
# formatter's own both-direction selftest (lint_selftest.sh for cmake-format);
# this script's selftest proves the orchestration contract (missing tool fails,
# scopes stay live, the argument grammar is closed, --list-files reports
# exactly what the lanes would format).
#
# gofmt -l and shfmt -l LIST dirty files on stdout and still exit 0, so the
# check lanes for those two languages judge by output, not by exit code.
#
# Usage:
#     format_tree.sh             # format every language in place
#     format_tree.sh --check     # exit 1 on the first language with any diff
#     format_tree.sh --selftest  # prove the orchestration contract
#     format_tree.sh --list-files go|python|shell|cmake|just
#                                # print the files the lanes would format

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "$SCRIPT_DIR/../.." && pwd)"

CHECK_ONLY=0

# require_tool fails loudly when a formatter is absent; the CI and recipe path
# must never degrade to formatting a subset of the tree's languages.
require_tool() {
  local tool="$1"
  if ! command -v "$tool" >/dev/null 2>&1; then
    echo "format_tree: ${tool} not found -- see docs/TOOLCHAIN.md for the pinned version." >&2
    exit 1
  fi
}

# run_scope executes the --list-files seam for one language on stdout. This is
# the SINGLE definition of "which files does the format gate cover": the lanes
# below (via language_scope), the --list-files mode, and the selftest all read
# it. A scope answer must not depend on tool presence -- formatters are
# required by the lanes, never by the question of what would be formatted.
run_scope() {
  case "$1" in
    go) python3 "$SCRIPT_DIR/check_go.py" --list-files ;;
    python) python3 "$SCRIPT_DIR/check_ruff.py" --list-files ;;
    shell) python3 "$SCRIPT_DIR/check_shell.py" --list-files ;;
    cmake) python3 "$SCRIPT_DIR/lint_targets.py" cmake ;;
    just) python3 "$SCRIPT_DIR/check_justfiles.py" --list-files ;;
    *)
      echo "format_tree: unknown language '$1' (expected go|python|shell|cmake|just)" >&2
      exit 2
      ;;
  esac
}

# language_scope writes one language's scope to a temp file and refuses an
# empty result: a collapsed scope must not read as a clean pass.
language_scope() {
  local label="$1"
  local out="/tmp/ra8-fmt-${label}.list"
  if ! run_scope "$label" >"$out" 2>/dev/null; then
    echo "format_tree: ${label} scope command failed" >&2
    exit 1
  fi
  if [ ! -s "$out" ]; then
    echo "format_tree: ${label} scope is empty; refusing to report success." >&2
    exit 1
  fi
}

# run_check runs a formatter in --check semantics. `list_test` formatters
# report diffs on stdout with exit 0; `exitcode_test` formatters exit non-zero
# on a diff. Both are failures here.
run_check() {
  local label="$1" mode="$2"
  shift 2
  echo "  check  ${label}"
  local out
  out="$(mktemp)"
  if ! "$@" >"$out" 2>&1; then
    cat "$out" >&2
    rm -f "$out"
    return 1
  fi
  if [ "$mode" = "list" ] && [ -s "$out" ]; then
    echo "format_tree: ${label} files need formatting:" >&2
    cat "$out" >&2
    rm -f "$out"
    return 1
  fi
  rm -f "$out"
  return 0
}

run_format() {
  local label="$1"
  shift
  echo "  format ${label}"
  "$@"
}

selftest() {
  local failures=0 empty_dir lang n

  # Must-fire: an absent formatter must hard-fail, never skip its language.
  empty_dir="$(mktemp -d)"
  if (PATH="$empty_dir" require_tool does-not-exist-format-tree-probe) 2>/dev/null; then
    echo "selftest: FAILED -- missing formatter did not fail the run." >&2
    failures=1
  fi
  # Must-stay-quiet: a formatter that exists (bash itself) must be accepted.
  if ! (require_tool bash); then
    echo "selftest: FAILED -- a present formatter was refused." >&2
    failures=1
  fi
  rm -rf "$empty_dir"

  # Argument grammar is closed: an unknown flag is a usage error, not a run.
  if "$SCRIPT_DIR/format_tree.sh" --nonsense >/dev/null 2>&1; then
    echo "selftest: FAILED -- an unknown flag was accepted." >&2
    failures=1
  fi

  # Non-vacuity: every language scope seam reports live files today, through
  # the same run_scope the lanes and --list-files both read. Each checker's
  # own floor deepens the same guarantee; this proves the orchestrator is
  # actually wired to the seams.
  for lang in go python shell cmake just; do
    n="$(run_scope "$lang" 2>/dev/null | sed '/^$/d' | wc -l | tr -d ' ')"
    if [ -z "$n" ] || [ "$n" -eq 0 ] 2>/dev/null; then
      echo "selftest: FAILED -- ${lang} scope collapsed." >&2
      failures=1
    fi
  done

  # The --list-files grammar is closed too: an unknown language, or a missing
  # one, is a usage error, not a scope answer.
  if "$SCRIPT_DIR/format_tree.sh" --list-files nonsense >/dev/null 2>&1; then
    echo "selftest: FAILED -- --list-files accepted an unknown language." >&2
    failures=1
  fi
  if "$SCRIPT_DIR/format_tree.sh" --list-files >/dev/null 2>&1; then
    echo "selftest: FAILED -- --list-files accepted a missing language." >&2
    failures=1
  fi

  if [ "$failures" -eq 0 ]; then
    echo "format_tree.sh --selftest: OK (missing tool fails, present tool passes, grammar closed, scopes live, list mode closed)."
    return 0
  fi
  return 1
}

case "${1:-}" in
  --selftest)
    selftest
    exit $?
    ;;
  --check)
    CHECK_ONLY=1
    shift
    [ $# -eq 0 ] || {
      echo "usage: format_tree.sh [--check|--selftest|--list-files LANG]" >&2
      exit 2
    }
    ;;
  --list-files)
    shift
    [ $# -eq 1 ] || {
      echo "usage: format_tree.sh --list-files go|python|shell|cmake|just" >&2
      exit 2
    }
    run_scope "$1"
    exit $?
    ;;
  "")
    ;;
  *)
    echo "usage: format_tree.sh [--check|--selftest|--list-files LANG]" >&2
    exit 2
    ;;
esac

cd "$REPO_ROOT"

# --- C: clang-format + comment pass (the `format` gate's own script) -----
if [ -z "${CLANG_FORMAT:-}" ] && ! command -v clang-format-22 >/dev/null 2>&1; then
  # format_code.sh enforces the pinned major itself; pre-empt with a clear
  # message only when nothing is even configured.
  echo "format_tree: no pinned clang-format-22 found on PATH (or CLANG_FORMAT unset)." >&2
  exit 1
fi
if [ "$CHECK_ONLY" -eq 1 ]; then
  run_check C exitcode bash "$SCRIPT_DIR/format_code.sh" --check || exit 1
else
  run_format C bash "$SCRIPT_DIR/format_code.sh"
fi

# --- Go -------------------------------------------------------------------
require_tool gofmt
language_scope go
mapfile -t go_files </tmp/ra8-fmt-go.list
if [ "$CHECK_ONLY" -eq 1 ]; then
  run_check go list gofmt -l "${go_files[@]}" || exit 1
else
  run_format go gofmt -w "${go_files[@]}"
fi
require_tool ruff
language_scope python
mapfile -t python_files </tmp/ra8-fmt-python.list
if [ "$CHECK_ONLY" -eq 1 ]; then
  run_check python exitcode ruff format --check "${python_files[@]}" || exit 1
else
  run_format python ruff format "${python_files[@]}"
fi
require_tool shfmt
language_scope shell
mapfile -t shell_files </tmp/ra8-fmt-shell.list
if [ "$CHECK_ONLY" -eq 1 ]; then
  run_check shell list shfmt -i 2 -ci -l "${shell_files[@]}" || exit 1
else
  run_format shell shfmt -i 2 -ci -w "${shell_files[@]}"
fi

# --- CMake ----------------------------------------------------------------
require_tool cmake-format
language_scope cmake
mapfile -t cmake_files </tmp/ra8-fmt-cmake.list
if [ "$CHECK_ONLY" -eq 1 ]; then
  run_check cmake exitcode cmake-format --check "${cmake_files[@]}" || exit 1
else
  run_format cmake cmake-format -i "${cmake_files[@]}"
fi

# --- justfiles ------------------------------------------------------------
require_tool just
language_scope just
while IFS= read -r justfile; do
  [ -n "$justfile" ] || continue
  if [ "$CHECK_ONLY" -eq 1 ]; then
    run_check "just:${justfile}" exitcode just --unstable --fmt --check --justfile "$REPO_ROOT/$justfile" || exit 1
  else
    run_format "just:${justfile}" just --unstable --fmt --justfile "$REPO_ROOT/$justfile"
  fi
done </tmp/ra8-fmt-just.list

rm -f /tmp/ra8-fmt-go.list /tmp/ra8-fmt-python.list /tmp/ra8-fmt-shell.list \
  /tmp/ra8-fmt-cmake.list /tmp/ra8-fmt-just.list
if [ "$CHECK_ONLY" -eq 1 ]; then
  echo "format_tree: all languages formatted"
else
  echo "format_tree: formatted all languages"
fi
