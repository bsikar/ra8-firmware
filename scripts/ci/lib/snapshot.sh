#!/usr/bin/env bash
# SPDX-License-Identifier: MIT
# Copyright (c) 2026 Brighton Sikarskie
# shellcheck shell=bash
#
# scripts/ci/lib/snapshot.sh -- materialise the tree a suite run is gating, and
# run the suite inside it.
#
# SOURCED, NEVER EXECUTED. Split out of scripts/ci.sh because that file is THE
# gate registry: what a gate checks belongs there, and HOW the tree under test
# comes into existence is mechanism, the same way scripts/ci/lib/container.sh
# holds the transport. Splitting it is also what keeps ci.sh under the tree's
# 1000-line cap with room to grow.
#
# It pairs with scripts/ci/lib/abort.sh, which owns that snapshot's LIFECYCLE:
# who may delete it, what happens when the run is signalled, and the guard that
# refuses to dispatch a gate into a tree that is no longer there (#542).
#
# Unlike container.sh this file does reach back for ci.sh's globals and
# functions -- REPO_ROOT, run_suite, run_one_gate. That is deliberate: this is
# the suite runner, not transport beside it, and threading the repo root
# through four call sites would buy an explicit contract that only restates
# where the runner already runs. The one shellcheck directive below records it.
# shellcheck disable=SC2154  # REPO_ROOT / RA8_CI_* come from scripts/ci.sh, the only thing that sources this file.

# Run the suite against a CLEAN snapshot of committed HEAD -- exactly what CI
# checks out. A working tree carries gitignored in-source build dirs whose
# CMake-generated junk makes clang-format / cppcheck / check_magic_numbers
# report failures CI never sees, and stale .gcda from another branch makes
# coverage report bogus "no_working_dir_found" results.
#
# The snapshot dir is created fresh per run and destroyed on exit, so no build
# output can outlive the run that produced it. That is what makes the
# stale-artefact class impossible rather than merely remembered: a coverage run
# can never find a different branch's .gcda here, because "here" did not exist
# a moment ago. Do NOT replace this with a fixed path -- a fixed path is a
# cache, and a cache is exactly the bug.
# Write HEAD's tree into $1 and make it a git repository.
#
# Materialised through a SCRATCH INDEX, not `git archive`. `git archive HEAD |
# tar -x` is not a faithful copy of HEAD, and this file and CLAUDE.md both
# claimed it was ("exactly what CI checks out"). Measured on 2026-07-28 it was
# 441 tracked files short, by two independent mechanisms:
#
#   * `git archive` honours `export-ignore` in the NESTED .gitattributes of
#     vendored trees. The ThreadX family ships one, so 37 files (every
#     `.github/`, `.gitattributes` and `.gitignore` under filex, levelx,
#     netxduo, usbx and threadx) never reached the tarball at all.
#   * the following `git add -A` then respected .gitignore, dropping a further
#     404 tracked files -- the ThreadX `example_build/` IDE projects, which are
#     tracked in HEAD but match an ignore pattern.
#
# Neither is visible: the snapshot looks complete, and every gate that
# enumerates with `git ls-files` silently scanned a smaller tree than CI does.
# Surfaced by the SBOM integrity digest (#538) -- the first gate whose verdict
# depends on the file set being COMPLETE rather than merely large.
#
# `read-tree` + `checkout-index` writes HEAD's tree and nothing else, with no
# attribute filtering and no dependence on the working tree being clean.
# GIT_LFS_SKIP_SMUDGE=1 still emits the content/library epub LFS pointers as-is
# (no gate reads them, so no LFS object or network fetch is needed).
materialise_head_snapshot() {
  local work="$1"
  GIT_LFS_SKIP_SMUDGE=1 GIT_INDEX_FILE="$work.index" \
    git -C "$REPO_ROOT" read-tree HEAD
  GIT_LFS_SKIP_SMUDGE=1 GIT_INDEX_FILE="$work.index" \
    git -C "$REPO_ROOT" checkout-index --all --prefix="$work/"
  rm -f "$work.index"
  # Several gates shell out to git (ls-files, rev-list), so the snapshot needs
  # to be a repository. One synthetic commit of the extracted tree is enough
  # and keeps the snapshot independent of the host object store.
  git -C "$work" init --quiet
  # -f, because HEAD legitimately tracks files that .gitignore matches. Without
  # it those 404 vendored files are dropped from the snapshot's index and the
  # snapshot stops being HEAD -- the second mechanism above.
  git -C "$work" add -A -f
}

# Assert the snapshot's tracked file set is EXACTLY HEAD's.
#
# The suite's whole claim is that a gate run here is the gate run CI performs.
# A snapshot silently short of HEAD does not fail -- it reports success over a
# smaller tree, which is the defect class the gate-honesty epic exists for. So
# the fidelity is DERIVED and compared on every run rather than assumed from
# the extraction method being "obviously" faithful; the method that was
# obviously faithful was short by 441 files.
#
# The reference side is HEAD's TREE, not the host INDEX. `git ls-files` reports
# the index, which is HEAD only in a clean checkout: stage one new file and the
# host reports 13544 against the snapshot's 13543, and the run dies claiming
# the extraction dropped a path it never should have carried. The claim under
# test is "the snapshot is HEAD", so HEAD is what it is compared against; the
# snapshot side stays `ls-files` because that index was just built from HEAD by
# materialise_head_snapshot and is what the gates enumerate.
_snapshot_head_paths() {
  git -C "$REPO_ROOT" ls-tree -r --name-only HEAD
}

snapshot_fidelity_check() {
  local work="$1" head_n snap_n missing extra
  head_n="$(_snapshot_head_paths | wc -l | tr -d ' ')"
  snap_n="$(git -C "$work" ls-files | wc -l | tr -d ' ')"
  if [[ "$head_n" == "$snap_n" ]]; then
    echo "==> snapshot: $snap_n tracked path(s), identical to HEAD" >&2
    return 0
  fi
  missing="$(comm -23 \
    <(_snapshot_head_paths | sort) \
    <(git -C "$work" ls-files | sort) | head -5)"
  extra="$(comm -13 \
    <(_snapshot_head_paths | sort) \
    <(git -C "$work" ls-files | sort) | head -5)"
  echo "ERROR: the snapshot is not HEAD -- $snap_n path(s) vs $head_n." >&2
  echo "       Every gate that enumerates with \`git ls-files\` would scan a" >&2
  echo "       different tree than CI does, and report success over it." >&2
  _snapshot_report_paths "missing" "$missing"
  _snapshot_report_paths "extra" "$extra"
  return 1
}

# Print an indented sample of paths under a heading, or nothing when empty.
_snapshot_report_paths() {
  local label="$1" paths="$2" path
  [[ -z "$paths" ]] && return 0
  echo "       $label (first 5):" >&2
  while IFS= read -r path; do
    echo "         $path" >&2
  done <<<"$paths"
}

# Bring one snapshot into existence, ready for gates: own it (which arms the
# abort traps), materialise HEAD into it, prove it IS HEAD, seal it against
# disappearing later, and give it the single synthetic commit the gates that
# shell out to git need.
#
# Ownership is taken BEFORE materialising, so an abort during the extraction
# still cleans up -- and still EXITS instead of falling through into a suite
# over a half-written tree.
#
# The traps used to be ONE handler for four events:
#
#     trap "rm -rf '$work' '$work.index'" EXIT INT TERM
#
# A handler that does not exit returns to where the shell was interrupted, so a
# signalled run deleted the snapshot and CARRIED ON inside it, inventing a FAIL
# for every gate that came after. scripts/ci/lib/abort.sh is the whole story; do
# not collapse cleanup and abort back into one handler (#542).
prepare_head_snapshot() {
  local work="$1"
  ci_snapshot_own "$work"
  materialise_head_snapshot "$work"
  snapshot_fidelity_check "$work"
  # The snapshot exists and is HEAD. Seal it: from here every gate dispatch is
  # preconditioned on it still being there.
  ci_snapshot_seal
  git -C "$work" -c user.email=ci@localhost -c user.name=ci \
    commit --quiet --no-verify -m "ci.sh snapshot of HEAD" >/dev/null 2>&1 || true
  # That snapshot has ONE synthetic commit and none of the host's objects, so
  # commit messages simply are not in it -- a tree copy carries no history.
  # Point the message-scanning gates back at the real repository; everything
  # else still reads the clean snapshot. Without this the two commit-metadata
  # gates scan "ci.sh snapshot of HEAD" and report PASS, which is #348: the only
  # local enforcement of the attribution-trailer ban and the
  # inclusive-terminology rule, silently reading nothing.
  export RA8_CI_HISTORY_REPO="$REPO_ROOT"
}

# Disable errexit around the gate CALL only -- never `run_suite ... || rc=$?`.
# A `||` chain (like an `if` condition, or `!`) puts the callee into bash's
# inherited "ignoring errors" state, and that state propagates into every nested
# subshell where a plain `set -e` CANNOT clear it: $- shows `e` set while a
# failing command still does not abort. The whole suite then reduces to "did
# each gate's LAST command succeed", so a gate failing part-way -- including
# require_cmd / require_python_mod reporting an absent tool -- reports PASS.
# Measured: lint-py-shell reported PASS on a box with no ruff. This is the same
# discipline run_gate_capture documents, and suite_errexit_selftest is the
# regression test for it.
run_suite_on_snapshot() {
  local fast="$1" only="${2:-}" work rc=0
  if [[ -n "$(git -C "$REPO_ROOT" status --porcelain 2>/dev/null)" ]]; then
    echo "NOTE: working tree is dirty -- ci.sh gates committed HEAD only (like" >&2
    echo "      CI). Commit your changes to have them gated." >&2
  fi
  work="$(mktemp -d "${TMPDIR:-/tmp}/ra8-ci-snapshot.XXXXXXXX")"
  prepare_head_snapshot "$work"
  cd "$work"
  set +e
  # ONE gate, or the suite -- same snapshot, and deliberately inside the same
  # errexit discipline rather than in a second function: a `||` or an `if`
  # around either CALL re-creates the "failed part-way, reported PASS" defect
  # the block above exists to prevent. A branch here cannot.
  if [[ -n "$only" ]]; then
    run_one_gate "$only"
  else
    run_suite "$fast"
  fi
  rc=$?
  set -e
  cd "$REPO_ROOT"
  return "$rc"
}
