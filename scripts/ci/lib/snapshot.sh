#!/usr/bin/env bash
# SPDX-License-Identifier: MIT
# Copyright (c) 2026 Brighton Sikarskie
# shellcheck shell=bash
#
# scripts/ci/lib/snapshot.sh -- materialise committed HEAD for ordinary suites
# and run the suite inside it. The pre-commit candidate transport is deliberately
# self-contained in scripts/git/pre-commit and never sources this live file.
#
# SOURCED, NEVER EXECUTED. Split out of scripts/ci.sh because that file is THE
# gate registry: what a gate checks belongs there, and HOW the tree under test
# comes into existence is mechanism, the same way scripts/ci/lib/container.sh
# holds the transport. This file only owns clean committed-HEAD snapshots;
# splitting it also keeps ci.sh under the tree's 1000-line cap.
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
#     `.github/`, `.gitattributes` and `.gitignore` under levelx,
#     netxduo, usbx and threadx) never reached the tarball at all (counts
#     measured while the since-retired FileX snapshot was still vendored).
#   * the following `git add -A` then respected .gitignore, dropping a further
#     404 tracked files -- the ThreadX `example_build/` IDE projects, which are
#     tracked in HEAD but match an ignore pattern.
#
# Neither is visible: the snapshot looks complete, and every gate that
# enumerates with `git ls-files` silently scanned a smaller tree than CI does.
# Surfaced by the SBOM integrity digest (#538) -- the first gate whose verdict
# depends on the file set being COMPLETE rather than merely large.
#
# A fresh private repository reads HEAD's objects through a read-only alternate.
# Its empty local config means committed built-in attributes (including eol)
# retain normal checkout semantics while source/global filter drivers, hooks,
# templates, and fsmonitor helpers do not exist in the materialising repository.
# GIT_LFS_SKIP_SMUDGE=1 leaves the e-reader content-library epub LFS pointers
# as-is (no gate reads them, so no LFS object or network fetch is needed).
install_snapshot_git_environment() {
  local next
  install_sanitized_git_environment

  # The sanitized contract intentionally ignores every host Git config.  When
  # host mode runs as root, explicitly trust only the two read-only repository
  # mounts selected by the container boundary; otherwise Git refuses them on
  # ownership before the snapshot can prove its fidelity.
  next="$GIT_CONFIG_COUNT"
  export "GIT_CONFIG_KEY_${next}=safe.directory"
  export "GIT_CONFIG_VALUE_${next}=$REPO_ROOT"
  next=$((next + 1))
  if [[ -n "${RA8_CI_GIT_COMMON_DIR:-}" ]]; then
    export "GIT_CONFIG_KEY_${next}=safe.directory"
    export "GIT_CONFIG_VALUE_${next}=$RA8_CI_GIT_COMMON_DIR"
    next=$((next + 1))
  fi
  export GIT_CONFIG_COUNT="$next"
}

materialise_head_snapshot() (
  local work="$1" source_head source_objects template expected_tree actual_tree
  install_snapshot_git_environment
  source_head="$("$RA8_TRUSTED_GIT" -C "$REPO_ROOT" -c core.fsmonitor=false \
    -c core.hooksPath=/dev/null rev-parse --verify HEAD)"
  source_objects="$("$RA8_TRUSTED_GIT" -C "$REPO_ROOT" -c core.fsmonitor=false \
    -c core.hooksPath=/dev/null rev-parse --path-format=absolute --git-path objects)"
  expected_tree="$("$RA8_TRUSTED_GIT" -C "$REPO_ROOT" -c core.fsmonitor=false \
    -c core.hooksPath=/dev/null rev-parse "$source_head^{tree}")"

  template="${work}.empty-template"
  mkdir -p "$work" "$template"
  "$RA8_TRUSTED_GIT" -C "$work" -c init.templateDir="$template" init --quiet
  rm -rf -- "$template"
  mkdir -p "$work/.git/objects/info"
  printf '%s\n' "$source_objects" >"$work/.git/objects/info/alternates"
  "$RA8_TRUSTED_GIT" -C "$work" -c core.fsmonitor=false -c core.hooksPath=/dev/null \
    update-ref HEAD "$source_head"
  "$RA8_TRUSTED_GIT" -C "$work" -c core.fsmonitor=false -c core.hooksPath=/dev/null \
    read-tree "$source_head"
  GIT_LFS_SKIP_SMUDGE=1 "$RA8_TRUSTED_GIT" -C "$work" -c core.fsmonitor=false \
    -c core.hooksPath=/dev/null -c core.attributesFile=/dev/null \
    checkout-index --all
  # Several gates shell out to git (ls-files, rev-list), so the snapshot needs
  # a self-contained object store. Force-add tracked ignored paths, then prove
  # the built-in checkout/clean round trip recreates the exact source tree.
  # -f, because HEAD legitimately tracks files that .gitignore matches. Without
  # it those 404 vendored files are dropped from the snapshot's index and the
  # snapshot stops being HEAD -- the second mechanism above.
  "$RA8_TRUSTED_GIT" -C "$work" -c core.fsmonitor=false -c core.hooksPath=/dev/null \
    -c core.attributesFile=/dev/null add -A -f
  actual_tree="$("$RA8_TRUSTED_GIT" -C "$work" -c core.fsmonitor=false \
    -c core.hooksPath=/dev/null write-tree)"
  [[ "$actual_tree" == "$expected_tree" ]] || {
    echo "ERROR: strict snapshot materialisation changed HEAD tree bytes." >&2
    return 1
  }
  # Leave HEAD unborn so prepare_head_snapshot can create one independent root
  # commit instead of reporting that the source commit's tree has no changes.
  "$RA8_TRUSTED_GIT" -C "$work" -c core.fsmonitor=false -c core.hooksPath=/dev/null \
    update-ref -d HEAD
)

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
  "$RA8_TRUSTED_GIT" -C "$REPO_ROOT" ls-tree -r --name-only HEAD
}

snapshot_fidelity_check() (
  install_snapshot_git_environment
  local work="$1" head_n snap_n missing extra
  head_n="$(_snapshot_head_paths | wc -l | tr -d ' ')"
  snap_n="$("$RA8_TRUSTED_GIT" -C "$work" ls-files | wc -l | tr -d ' ')"
  if [[ "$head_n" == "$snap_n" ]]; then
    echo "==> snapshot: $snap_n tracked path(s), identical to HEAD" >&2
    return 0
  fi
  missing="$(comm -23 \
    <(_snapshot_head_paths | sort) \
    <("$RA8_TRUSTED_GIT" -C "$work" ls-files | sort) | head -5)"
  extra="$(comm -13 \
    <(_snapshot_head_paths | sort) \
    <("$RA8_TRUSTED_GIT" -C "$work" ls-files | sort) | head -5)"
  echo "ERROR: the snapshot is not HEAD -- $snap_n path(s) vs $head_n." >&2
  echo "       Every gate that enumerates with \`git ls-files\` would scan a" >&2
  echo "       different tree than CI does, and report success over it." >&2
  _snapshot_report_paths "missing" "$missing"
  _snapshot_report_paths "extra" "$extra"
  return 1
)

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
  (
    install_snapshot_git_environment
    "$RA8_TRUSTED_GIT" -C "$work" -c user.email=ci@localhost -c user.name=ci \
      commit --quiet --no-verify -m "ci.sh snapshot of HEAD" >/dev/null
  )
  # That snapshot has ONE synthetic commit and none of the host's objects, so
  # commit messages simply are not in it -- a tree copy carries no history.
  # Point the message-scanning gates back at the real repository; everything
  # else still reads the clean snapshot. Without this the two commit-metadata
  # gates scan "ci.sh snapshot of HEAD" and report PASS, which is #348: the only
  # local enforcement of the attribution-trailer ban and the
  # inclusive-terminology rule, silently reading nothing.
  export RA8_CI_HISTORY_REPO="$REPO_ROOT"
}

# Reclaim reproducible build trees once the full suite has consumed them.
#
# Static analysis, unit, UBSan, and the gcov gate each leave reproducible
# CMake trees. No later gate consumes them after the boundary table below.
# Keeping every completed tree resident made the suite exceed the shared
# runner's free space before the coverage gate, then again while clang-18
# built MC/DC, even though each gate passed alone.
#
# This is deliberately restricted to the owned, disposable HEAD snapshot.
# `just quality::local::gate <name>` and direct Just recipes run in place and
# retain developers' incremental trees. Exact relative paths plus the owner
# and cwd checks keep this from ever becoming a broad checkout cleanup.
RA8_CI_RECLAIM_BOUNDARIES=(
  "unit-tests"
  "ubsan"
  "coverage-tree"
  "mcdc"
  "cache-bench"
)

# Print the completed build trees whose last consumer precedes the named gate.
# The table is the single definition of their lifetimes; the self-test drives
# every row through the real cleanup helper.
suite_reclaim_targets() {
  case "$1" in
    unit-tests)
      printf '%s\n' build/cppcheck build/misra build/tidy \
        build/tidy-reflow-v2 build/xtidy
      ;;
    ubsan) printf '%s\n' tests/build ;;
    coverage-tree) printf '%s\n' tests/build-ubsan ;;
    mcdc) printf '%s\n' tests/build-cov build/tree-coverage ;;
    cache-bench) printf '%s\n' tests/build-cov build/mcdc-report ;;
    *) return 0 ;;
  esac
}

suite_reclaim_completed_builds() {
  local next_gate="$1" here rel
  local completed_builds=()

  while IFS= read -r line; do
    [[ -n "$line" ]] && completed_builds+=("$line")
  done < <(suite_reclaim_targets "$next_gate")
  [[ "${#completed_builds[@]}" -gt 0 ]] || return 0
  [[ -n "$RA8_CI_SNAPSHOT_DIR" ]] || return 0
  if [[ "$BASHPID" != "$RA8_CI_SNAPSHOT_OWNER" ]]; then
    echo "ERROR: only the snapshot owner may reclaim suite build trees." >&2
    return 1
  fi
  here="$(pwd -P 2>/dev/null || true)"
  if [[ -z "$here" || "$here" != "$RA8_CI_SNAPSHOT_DIR" ]]; then
    echo "ERROR: refusing build-tree cleanup outside the owned snapshot." >&2
    return 1
  fi
  for rel in "${completed_builds[@]}"; do
    if [[ "$rel" == /* || "$rel" == *".."* ]]; then
      echo "ERROR: unsafe suite build lifecycle target: $rel" >&2
      return 1
    fi
    [[ -e "$here/$rel" ]] || continue
    echo "==> suite storage: retiring completed $rel"
    rm -rf -- "${here:?}/$rel" || return 1
  done
}

# Exercise one lifecycle row through the real cleanup helper.
suite_build_lifecycle_boundary_selftest() {
  local probe="$1" boundary="$2" rel rc=0 failures=0
  local targets=()
  while IFS= read -r line; do
    [[ -n "$line" ]] && targets+=("$line")
  done < <(suite_reclaim_targets "$boundary")
  if [[ "${#targets[@]}" -eq 0 ]]; then
    echo "ERROR: lifecycle boundary $boundary has no cleanup targets." >&2
    return 1
  fi
  for rel in "${targets[@]}"; do
    mkdir -p "$probe/$rel"
    touch "$probe/$rel/probe"
  done
  (
    cd "$probe"
    RA8_CI_SNAPSHOT_DIR="$probe"
    RA8_CI_SNAPSHOT_OWNER="$BASHPID"
    suite_reclaim_completed_builds "$boundary"
  ) >/dev/null 2>&1 || rc=$?
  if [[ "$rc" -ne 0 ]]; then
    echo "ERROR: lifecycle boundary $boundary cleanup returned $rc." >&2
    failures=1
  fi
  for rel in "${targets[@]}"; do
    if [[ -e "$probe/$rel" ]]; then
      echo "ERROR: lifecycle boundary $boundary retained $rel." >&2
      failures=1
    fi
  done
  if [[ ! -f "$probe/keep/probe" ]]; then
    echo "ERROR: lifecycle boundary $boundary removed a live path." >&2
    failures=1
  fi
  return "$failures"
}

# Prove every lifecycle boundary in both directions: its completed build trees
# are removed in an owned snapshot while an unrelated path survives; outside a
# snapshot the helper must be a no-op. A row that silently stopped matching
# would otherwise only be rediscovered by another full disk.
suite_build_lifecycle_selftest() {
  local probe boundary failures=0
  probe="$(mktemp -d "${TMPDIR:-/tmp}/ra8-storage-probe.XXXXXXXX")"
  # macOS exposes /var as a symlink to /private/var. The cleanup guard compares
  # against `pwd -P`, so retain that same physical spelling here rather than
  # rejecting the self-test's own, correctly-owned temporary directory.
  probe="$(cd "$probe" && pwd -P)"
  if [[ "${#RA8_CI_RECLAIM_BOUNDARIES[@]}" -lt 5 ]]; then
    echo "ERROR: suite build lifecycle boundary set collapsed below its floor." >&2
    failures=1
  fi
  mkdir -p "$probe/keep"
  touch "$probe/keep/probe"
  for boundary in "${RA8_CI_RECLAIM_BOUNDARIES[@]}"; do
    suite_build_lifecycle_boundary_selftest "$probe" "$boundary" || failures=1
  done

  mkdir -p "$probe/tests/build"
  touch "$probe/tests/build/outside-probe"
  (
    cd "$probe"
    RA8_CI_SNAPSHOT_DIR=""
    RA8_CI_SNAPSHOT_OWNER=""
    suite_reclaim_completed_builds ubsan
  ) >/dev/null 2>&1 || failures=1
  if [[ ! -f "$probe/tests/build/outside-probe" ]]; then
    echo "ERROR: suite build lifecycle modified an in-place checkout." >&2
    failures=1
  fi

  rm -rf -- "$probe"
  if [[ "$failures" -ne 0 ]]; then
    return 1
  fi
  echo "ci.sh: suite build-lifecycle self-test OK (bounded snapshot cleanup)."
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
  if [[ -n "$("$RA8_TRUSTED_GIT" -C "$REPO_ROOT" status --porcelain 2>/dev/null)" ]]; then
    echo "NOTE: working tree is dirty -- ci.sh gates committed HEAD only (like" >&2
    echo "      CI). Commit your changes to have them gated." >&2
  fi
  work="$(mktemp -d "${TMPDIR:-/tmp}/ra8-ci-snapshot.XXXXXXXX")"
  # Keep ownership and the later `pwd -P` safety check in one path namespace.
  # This matters on hosts such as macOS where /var is a symlink.
  work="$(cd "$work" && pwd -P)"
  prepare_head_snapshot "$work"
  cd "$work"
  # From this point until the suite returns, every Git query belongs to the
  # synthetic snapshot. Hook-exported routing would override both this cwd and
  # every `git -C` the gates use, putting the caller's index back in scope.
  install_snapshot_git_environment
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
