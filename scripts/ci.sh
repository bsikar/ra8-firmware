#!/usr/bin/env bash
# SPDX-License-Identifier: MIT
# Copyright (c) 2026 Brighton Sikarskie
#
# scripts/ci.sh -- THE single definition of every CI gate in this repository.
#
# ===========================================================================
# ONE SOURCE OF TRUTH
# ===========================================================================
# Every check CI runs is a shell function in this file, listed in the
# RA8_GATE_REGISTRY table below. The GitHub Actions workflows contain no check
# bodies at all: each gate-bearing step is a thin
#
#     run: bash scripts/ci.sh --gate <name>
#
# driver. The YAML decides only SCHEDULING -- which gates run in which job, on
# which runner, in parallel with what -- and never WHAT a gate does.
#
# That inversion is deliberate. This suite drifted from the workflows FOUR
# separate times: a missing annotation gate plus a missing MISRA ratchet turned
# a green local run into a red push and got dev reverted; agents hand-copied
# gate bodies into throwaway /tmp scripts that stopped mirroring CI the moment
# a gate was added; an audit found 21 checks present in firmware.yml's
# pre-commit job alone and absent here; and a hand re-sync then landed to close
# them. Measured across EVERY workflow just before this rewrite, 26 distinct
# check invocations ran in CI with no local equivalent at all.
#
# Every one of those was the same failure -- the same check written down twice.
# A check written down once cannot disagree with itself. Re-syncing the lists a
# fifth time would only reset the clock; removing the second copy ends it.
#
# The residual hole is someone adding a raw `run:` step straight to a workflow,
# re-creating a second home for check logic. scripts/ci/check_ci_parity.py
# closes it: it parses every workflow and fails when a step runs anything other
# than a registered gate, unless the step is explicitly tagged
# `# ci-parity: infra` -- and infra steps may not invoke checkers. It runs as
# the `ci-parity` gate below, so the guard guards itself.
#
# ---------------------------------------------------------------------------
# ADDING A NEW GATE (the whole procedure)
# ---------------------------------------------------------------------------
#   1. Add one row to RA8_GATE_REGISTRY.
#   2. Write the matching `gate_<name>` function (dashes become underscores).
#   3. Add a step `run: bash scripts/ci.sh --gate <name>` to a workflow.
#
# Steps 1+2 without 3 fail the ci-parity gate ("registered but never
# scheduled"). Step 3 without 1+2 fails it too ("unknown gate"). There is no
# order in which doing half the work passes.
#
# ---------------------------------------------------------------------------
# RUNNING IT
# ---------------------------------------------------------------------------
#   bash scripts/ci.sh --gate <name>   # one gate, in place, natively (CI path)
#   bash scripts/ci.sh --native        # every gate natively, on a HEAD snapshot
#   bash scripts/ci.sh --fast          # skip the slow gates
#   bash scripts/ci.sh --list-gates    # machine-readable registry dump
#   bash scripts/ci.sh                 # containerised (the macOS path)
#
# On Linux the native path IS the CI environment, so `--native` is the
# supported local run and needs no container runtime. The container exists to
# give macOS developers an Ubuntu userland: the format gate pins
# clang-format-22 (Homebrew ships a different major), and the host unit tests
# mmap peripheral RAM with MAP_FIXED below 4 GiB, which macOS arm64 refuses --
# every test SIGKILLs before main() on the Mac. With no container runtime on a
# Linux box this script runs natively rather than refusing; on macOS it
# refuses, because a macOS "pass" would be a lie.
#
# ---------------------------------------------------------------------------
# GATES FAIL LOUDLY ON MISSING TOOLS -- THEY NEVER SKIP
# ---------------------------------------------------------------------------
# A gate whose tool is absent must FAIL, not pass. This repo has been bitten:
# check_annotations.py exits 0 when libclang is missing, so a strict gate
# silently reported nothing. Use require_cmd / require_python_mod below for
# every external dependency, and never let a gate body degrade to a no-op.

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "$SCRIPT_DIR/.." && pwd)"
IMAGE_TAG="ra8-ci:latest"
# Exit status of the most recent run_gate_capture call. Pre-declared so `set -u`
# cannot abort a reader before the first gate has run.
RA8_GATE_RC=0
DOCKERFILE="$REPO_ROOT/.devcontainer/Dockerfile"

# ===========================================================================
# THE GATE REGISTRY -- the single source of truth.
#
# Format: name|speed|description
#
#   speed=fast    always runs in the local suite (seconds to about a minute)
#   speed=slow    skipped by --fast (builds, coverage, whole-tree analysis)
#   speed=manual  never runs in the local suite: needs hardware, a nightly
#                 budget, or the network. Still registered so the ci-parity
#                 guard can bind it to its workflow step.
#
# Order is execution order for the full local suite. A gate that consumes
# another gate's build output (sg-offsets, stack-usage after build-cross)
# must follow it here.
# ===========================================================================
RA8_GATE_REGISTRY=(
  "ci-parity|fast|workflow <-> gate-registry parity guard"
  "ascii|fast|ASCII-only source files"
  "copyright|fast|SPDX + copyright headers"
  "since|fast|Doxygen @since tags on public headers"
  "hil-sil-parity|fast|every HIL app is also exercised in board_sim"
  "no-ai-attribution|fast|attribution ban (tracked files)"
  "no-ai-attribution-commits|fast|attribution ban (commit messages)"
  "inclusive-terminology|fast|OSHWA inclusive terminology (tracked files)"
  "inclusive-terminology-commits|fast|inclusive terminology (commit messages)"
  "format|fast|clang-format dry run"
  "pre-commit-checks|fast|the check_*.py gate suite"
  "annotations|fast|RA8_* annotation attributes (libclang)"
  "doc-attachment|fast|a Doxygen block describes the symbol it is attached to"
  "lint-py-shell|fast|ruff + shellcheck + shfmt"
  "lint-cmake|fast|cmake-format + cmake-lint over every listfile"
  "lint-yaml|fast|yamllint + actionlint over the workflows"
  "lint-make|fast|Makefile structure, headers and portable ROOT"
  "lint-ld|fast|linker-script structure, headers and symbol closure"
  "lint-asm|fast|assembly headers, sections and exported-symbol shape"
  "lint-devcontainer|fast|hadolint over the Dockerfile, zsh -n over the zshrc"
  "lint-coverage|fast|every code file is claimed by a linter and a formatter"
  "cite-check|fast|HUM citation validator (strict)"
  "roadmap-stats|fast|ROADMAP summary stats"
  "sbom|fast|CycloneDX SBOM freshness"
  "nsc-cmse|fast|ra8_nsc veneers compile under -mcmse"
  "cppcheck|slow|cppcheck static analysis"
  "misra|slow|MISRA-C 2012 ratchet"
  "tidy|slow|clang-tidy"
  "unit-tests|slow|host unit tests (ctest)"
  "ubsan|slow|host unit tests under UBSan"
  "coverage|slow|gcovr line/branch gate (90/80)"
  "coverage-report|slow|coverage_report.sh + check_coverage.py ratchet"
  "mcdc|slow|MC/DC coverage against the committed baseline"
  "cache-bench|slow|cache/glyph benchmark toolchain"
  "tools-build|slow|first-party host tools compile, link and test on Linux"
  "build-cross|slow|cross-build every example app"
  "sg-offsets|slow|NSC SG-veneer slot offsets in the linked secure ELF"
  "stack-usage|slow|aggregate -fstack-usage frames"
  "docs|slow|Doxygen warning gate + authored-diagram render check"
  "board-sim-smoke|slow|board_sim boot smoke over the example apps"
  "board-sim-matrix|slow|every example booted in board_sim, ratcheted downward"
  "board-sim-io-fabric|slow|ra8_io fabric demos in board_sim"
  "sil-integration|slow|every HIL app booted in board_sim against its hil.conf"
  "mcdc-delta-base|manual|base-branch MC/DC summary for the PR delta comment"
  "osv-scan|manual|OSV CVE sweep of the vendored SOUP (network, scheduled)"
  "fuzz-sweep|manual|libFuzzer sweep of every harness (nightly budget)"
  "hil-all|manual|hardware-in-the-loop suite on the bench EK-RA8D2"
  "docs-publish|manual|build + force-push the Doxygen site to gh-pages"
)

# ===========================================================================
# HELPERS
# ===========================================================================

# Fail loudly when a required tool is absent. A gate must never silently
# degrade to "nothing to check" -- that reports PASS for work never done.
require_cmd() {
  local tool="$1" hint="${2:-}"
  if ! command -v "$tool" >/dev/null 2>&1; then
    echo "ERROR: required tool '$tool' is not on PATH; this gate cannot run." >&2
    [[ -n "$hint" ]] && echo "       $hint" >&2
    return 1
  fi
}

require_python_mod() {
  local mod="$1" hint="${2:-}"
  if ! python3 -c "import $mod" >/dev/null 2>&1; then
    echo "ERROR: the Python module '$mod' is missing; this gate cannot run." >&2
    [[ -n "$hint" ]] && echo "       $hint" >&2
    return 1
  fi
}

# clang-format is pinned to major 22 project-wide: other majors disagree on
# edge cases and produce diffs CI rejects. Absence is a hard failure, not a
# fallback -- a run under clang-format-18 proves nothing about the gate.
pick_clang_format() {
  if command -v clang-format-22 >/dev/null 2>&1; then
    printf 'clang-format-22\n'
    return 0
  fi
  echo "ERROR: clang-format-22 is not on PATH. It is the project pin; other" >&2
  echo "       majors disagree on edge cases, so a run against them cannot" >&2
  echo "       stand in for the CI gate." >&2
  echo "       Ubuntu: apt-get install clang-format-22 (apt.llvm.org)" >&2
  echo "       macOS:  use the containerised path (bash scripts/ci.sh)" >&2
  return 1
}

cpu_count() {
  if command -v nproc >/dev/null 2>&1; then
    nproc
  elif command -v sysctl >/dev/null 2>&1; then
    sysctl -n hw.ncpu
  else
    echo 4
  fi
}

# Prepend the pinned Arm GNU Toolchain when the runner provisions it under
# /opt, replacing what the workflows used to do with GITHUB_PATH. The apt
# gcc-arm-none-eabi package ships no C++ standard library, so C++ apps
# (ereader_shelf -> ra8_epub + tinyxml2) fail with "fatal error: cstddef";
# the official ARM toolchain under /opt bundles libstdc++.
use_pinned_arm_toolchain() {
  local candidate
  for candidate in \
    "${RA8_ARM_TOOLCHAIN_BIN:-}" \
    /opt/arm-gnu-toolchain-13.3/bin \
    "$HOME/opt/arm-gnu-toolchain-13.3/bin"; do
    if [[ -n "$candidate" && -x "$candidate/arm-none-eabi-gcc" ]]; then
      PATH="$candidate:$PATH"
      export PATH
      return 0
    fi
  done
}

# Fail with the real reason when the arm-gcc on PATH predates Cortex-M85.
#
# -mcpu=cortex-m85 needs arm-gcc 12.3+. An older distro package does not say
# "too old" -- it says `unrecognized -mcpu target: cortex-m85` followed by
# `missing argument to '-march='`, which reads like a broken build script and
# sent a previous run hunting the wrong bug. Name the actual problem instead.
require_arm_gcc_m85() {
  require_cmd arm-none-eabi-gcc
  local version
  version="$(arm-none-eabi-gcc -dumpfullversion 2>/dev/null || echo 0)"
  if ! arm-none-eabi-gcc -mcpu=cortex-m85 -E - </dev/null >/dev/null 2>&1; then
    echo "ERROR: the arm-none-eabi-gcc on PATH ($version) does not know" >&2
    echo "       -mcpu=cortex-m85; this gate cannot run. It needs the pinned" >&2
    echo "       13.3 toolchain (12.3+ minimum)." >&2
    echo "       Looked in: \$RA8_ARM_TOOLCHAIN_BIN," >&2
    echo "                  /opt/arm-gnu-toolchain-13.3/bin," >&2
    echo "                  \$HOME/opt/arm-gnu-toolchain-13.3/bin" >&2
    echo "       Point RA8_ARM_TOOLCHAIN_BIN at its bin/ if it lives elsewhere." >&2
    return 1
  fi
}

# The repository whose HISTORY the commit-message gates read.
#
# Normally the current directory -- but NOT under run_suite_on_snapshot, which
# runs every gate inside a `git archive` snapshot that was turned into a repo
# by `git init`. That snapshot holds exactly ONE synthetic commit
# ("ci.sh snapshot of HEAD") and none of the host's objects, so a gate reading
# history there sees no real commit message at all (#348).
#
# Splitting the two sources is deliberate and is what makes snapshot mode
# still mean something for these gates:
#
#   * the CHECKER SCRIPTS come from the snapshot (cwd), so the suite gates the
#     committed HEAD's version of scripts/git/commit-msg and friends rather
#     than whatever is dirty in the working tree;
#   * the HISTORY comes from the host repo, because commit messages are git
#     metadata that `git archive` cannot carry and no snapshot can synthesise.
#
# Exporting RA8_CI_COMMIT_RANGE into the snapshot instead (the other candidate
# in #348) does not work: the range names SHAs the snapshot's fresh object
# store does not contain, so `git rev-list` dies with "Invalid revision range".
# Making it work would mean importing the host object store into every
# snapshot, which is precisely the independence run_suite_on_snapshot documents
# it wants.
ci_history_repo() {
  printf '%s\n' "${RA8_CI_HISTORY_REPO:-$PWD}"
}

# Number of commits reachable from HEAD in the given repository.
ci_history_depth() {
  git -C "$1" rev-list --count HEAD 2>/dev/null || printf '0\n'
}

# Refuse to scan a repository that has no real history to scan.
#
# This is the #348 guard. A commit-message gate pointed at the synthetic
# one-commit snapshot reports PASS having read nothing but "ci.sh snapshot of
# HEAD" -- the exact "gate that cannot see the thing it audits and says PASS"
# CLAUDE.md bans. Fail loudly instead, the same way require_cmd does for an
# absent tool.
#
# The invariant asserted here is history DEPTH (> 1 commit), not the span of
# the resolved range, even though #348 words it as the latter. A range spanning
# exactly one commit is perfectly legal -- pushing a single commit produces
# `HEAD~1..HEAD` -- so failing on a one-commit span would reject the most
# ordinary push there is. What is never legal is the gate reading a repository
# that HAS no history, and that is the condition that actually distinguishes
# the snapshot from a real checkout. commit_range_selftest asserts both
# directions of exactly this distinction.
ci_require_real_history() {
  local repo="$1" depth
  depth="$(ci_history_depth "$repo")"
  if [[ "$depth" -le 1 ]]; then
    echo "ERROR: this gate reads commit messages, but the repository at" >&2
    echo "       '$repo' contains $depth commit(s) -- there is no real" >&2
    echo "       history here to scan." >&2
    echo "       This is the #348 false-green: a synthetic 'git init'" >&2
    echo "       snapshot has one commit, so the gate would report PASS" >&2
    echo "       having read no real commit message at all." >&2
    echo "       Under the suite runner, RA8_CI_HISTORY_REPO must point at" >&2
    echo "       the host repository (run_suite_on_snapshot exports it)." >&2
    return 1
  fi
}

# The commit range a message-scanning gate should cover.
#
# Derived from the GitHub event payload when running on a runner, so the range
# logic lives here instead of being duplicated as inline expression bash in two
# workflows. Falls back to the local upstream..HEAD, then HEAD~1..HEAD. A
# before..head range never rots the way a hardcoded floor SHA does: a history
# rewrite orphans the floor and silently empties the range, turning the gate
# into a no-op.
#
# Every git query resolves against ci_history_repo(), not the cwd, so the range
# describes the same repository the gates go on to read.
ci_commit_range() {
  if [[ -n "${RA8_CI_COMMIT_RANGE:-}" ]]; then
    printf '%s\n' "$RA8_CI_COMMIT_RANGE"
    return 0
  fi

  local repo head="" base=""
  repo="$(ci_history_repo)"
  if [[ -n "${GITHUB_EVENT_PATH:-}" && -f "${GITHUB_EVENT_PATH}" ]]; then
    head="$(python3 -c '
import json, os
ev = json.load(open(os.environ["GITHUB_EVENT_PATH"]))
pr = ev.get("pull_request") or {}
print((pr.get("head") or {}).get("sha") or os.environ.get("GITHUB_SHA") or "")
' 2>/dev/null || true)"
    base="$(python3 -c '
import json, os
ev = json.load(open(os.environ["GITHUB_EVENT_PATH"]))
pr = ev.get("pull_request") or {}
print((pr.get("base") or {}).get("sha") or ev.get("before") or "")
' 2>/dev/null || true)"
  fi
  [[ -z "$head" ]] && head="${GITHUB_SHA:-HEAD}"

  # A base absent locally (force-push, shallow clone, the all-zero "new
  # branch" sentinel) is unusable -- fall back rather than error out.
  if [[ -z "$base" ]] || ! git -C "$repo" cat-file -e "${base}^{commit}" 2>/dev/null; then
    base="$(git -C "$repo" rev-parse --verify --quiet '@{upstream}' 2>/dev/null || true)"
  fi
  if [[ -z "$base" ]] || ! git -C "$repo" cat-file -e "${base}^{commit}" 2>/dev/null; then
    base="$(git -C "$repo" rev-parse --verify --quiet "${head}~1" 2>/dev/null || true)"
  fi

  if [[ -n "$base" ]]; then
    printf '%s..%s\n' "$base" "$head"
  else
    printf '%s\n' "$head"
  fi
}

# ===========================================================================
# GATE BODIES
#
# Each mirrors exactly one CI step. Nothing here is a copy of anything in the
# YAML, because the YAML has no copy to make.
# ===========================================================================

# ===========================================================================
# GATE BODIES
# ===========================================================================
# Every gate body lives in scripts/ci/gates/*.sh and is sourced here. The
# split is by theme, purely so no single file carries 1100 lines of gate
# bodies; it changes nothing about the architecture. RA8_GATE_REGISTRY above
# remains the ONE list of what gates exist, this file remains the ONE entry
# point, and each gate still has exactly ONE body.
#
# The loop fails loudly on an empty directory rather than proceeding with no
# gates defined -- a suite that silently defines nothing would report every
# gate as an unknown name, or worse, report success having run none.
_RA8_GATE_DIR="${SCRIPT_DIR}/ci/gates"
_ra8_gate_files=("${_RA8_GATE_DIR}"/*.sh)
if [ ! -e "${_ra8_gate_files[0]}" ]; then
  printf 'ci.sh: FATAL -- no gate bodies found under %s\n' "${_RA8_GATE_DIR}" >&2
  exit 2
fi
for _ra8_gate_file in "${_ra8_gate_files[@]}"; do
  # shellcheck source=/dev/null
  . "${_ra8_gate_file}"
done
unset _ra8_gate_file _ra8_gate_files _RA8_GATE_DIR

gate_fn_name() {
  printf 'gate_%s\n' "${1//-/_}"
}

registry_names() {
  local row
  for row in "${RA8_GATE_REGISTRY[@]}"; do
    printf '%s\n' "${row%%|*}"
  done
}

# Machine-readable dump consumed by scripts/ci/check_ci_parity.py. Also
# self-verifies that every registered name has a function behind it, so a
# typo'd registry row is caught here rather than at gate-run time.
list_gates() {
  local row name speed desc rest fn rc=0
  for row in "${RA8_GATE_REGISTRY[@]}"; do
    name="${row%%|*}"
    rest="${row#*|}"
    speed="${rest%%|*}"
    desc="${rest#*|}"
    fn="$(gate_fn_name "$name")"
    if ! declare -F "$fn" >/dev/null 2>&1; then
      echo "ERROR: registry lists gate '$name' but no function $fn() exists." >&2
      rc=1
      continue
    fi
    case "$speed" in
      fast | slow | manual) ;;
      *)
        echo "ERROR: gate '$name' has unknown speed class '$speed'." >&2
        rc=1
        ;;
    esac
    printf '%s\t%s\t%s\n' "$name" "$speed" "$desc"
  done
  return "$rc"
}

run_one_gate() {
  local name="$1" fn
  fn="$(gate_fn_name "$name")"
  if ! declare -F "$fn" >/dev/null 2>&1; then
    echo "ci.sh: unknown gate '$name'. Registered gates:" >&2
    registry_names | sed 's/^/  /' >&2
    return 2
  fi
  "$fn"
}

# THE gate dispatch. Runs one gate and leaves its status in RA8_GATE_RC.
#
# Every caller that needs a gate's status goes through here -- run_suite and
# suite_errexit_selftest alike -- so the self-test exercises the real runner
# instead of a lookalike of it.
#
# Do NOT collapse this into `if run_one_gate ...`. Calling a function from an
# `if` condition (or a `&&` chain, or under `!`) suppresses ERREXIT and that
# suppression extends INTO the callee, silently neutering the `set -e` inside
# every `gate_*()` subshell so only the gate's LAST command decides PASS/FAIL.
# Disable errexit around the CALL only; the callee's own `set -e` then works.
#
#     gate() ( set -e; false; echo reached; )
#     if gate; then echo PASS; else echo FAIL; fi   # prints: reached / PASS
run_gate_capture() {
  local name="$1"
  set +e
  run_one_gate "$name"
  RA8_GATE_RC=$?
  set -e
  return 0
}

# ===========================================================================
# FULL-SUITE RUNNER (native). Executes every registry gate in order and prints
# a PASS/FAIL line per gate.
# ===========================================================================
run_suite() {
  local fast="$1"
  local gate_names=() gate_results=()
  local name speed gate_rc

  while read -r name speed _; do
    if [[ "$speed" == "manual" ]]; then
      continue
    fi
    if [[ "$fast" == "1" && "$speed" == "slow" ]]; then
      continue
    fi
    echo ""
    echo "==================================================================="
    echo "== GATE: $name"
    echo "==================================================================="
    gate_names+=("$name")
    # Dispatch via run_gate_capture -- see the ERREXIT warning on it. Never
    # inline this as `if run_one_gate "$name"; then`.
    run_gate_capture "$name"
    gate_rc="$RA8_GATE_RC"
    if [[ "$gate_rc" -eq 0 ]]; then
      gate_results+=("PASS")
    else
      gate_results+=("FAIL")
    fi
  done < <(list_gates)

  echo ""
  echo "==================================================================="
  echo "== ci.sh summary$([[ "$fast" == "1" ]] && echo "  (--fast: slow gates skipped)")"
  echo "==================================================================="
  local failed=0 idx=0
  while [[ "$idx" -lt "${#gate_names[@]}" ]]; do
    printf '  %-32s %s\n' "${gate_names[$idx]}" "${gate_results[$idx]}"
    [[ "${gate_results[$idx]}" == "FAIL" ]] && failed=1
    idx=$((idx + 1))
  done
  echo "-------------------------------------------------------------------"
  if [[ "$failed" -ne 0 ]]; then
    echo "  RESULT: FAIL"
    return 1
  fi
  echo "  RESULT: PASS"
  return 0
}

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
run_suite_on_snapshot() {
  local fast="$1" work rc=0
  if [[ -n "$(git -C "$REPO_ROOT" status --porcelain 2>/dev/null)" ]]; then
    echo "NOTE: working tree is dirty -- ci.sh gates committed HEAD only (like" >&2
    echo "      CI). Commit your changes to have them gated." >&2
  fi
  work="$(mktemp -d "${TMPDIR:-/tmp}/ra8-ci-snapshot.XXXXXXXX")"
  # shellcheck disable=SC2064
  # Expand $work now: the trap must not depend on the variable surviving.
  trap "rm -rf '$work'" EXIT INT TERM
  # GIT_LFS_SKIP_SMUDGE=1 emits the content/library epub LFS pointers as-is
  # (no gate reads them, so no LFS object or network fetch is needed).
  GIT_LFS_SKIP_SMUDGE=1 git -C "$REPO_ROOT" archive HEAD | tar -x -C "$work"
  # Several gates shell out to git (ls-files, rev-list), so the snapshot needs
  # to be a repository. One synthetic commit of the extracted tree is enough
  # and keeps the snapshot independent of the host object store.
  git -C "$work" init --quiet
  git -C "$work" add -A
  git -C "$work" -c user.email=ci@localhost -c user.name=ci \
    commit --quiet --no-verify -m "ci.sh snapshot of HEAD" >/dev/null 2>&1 || true
  # That snapshot has ONE synthetic commit and none of the host's objects, so
  # commit messages simply are not in it -- `git archive` carries a tree, not
  # history. Point the message-scanning gates back at the real repository;
  # everything else still reads the clean snapshot. Without this the two
  # commit-metadata gates scan "ci.sh snapshot of HEAD" and report PASS,
  # which is #348: the only local enforcement of the attribution-trailer ban
  # and the inclusive-terminology rule, silently reading nothing.
  export RA8_CI_HISTORY_REPO="$REPO_ROOT"
  cd "$work"
  # Disable errexit around the CALL only -- never `run_suite ... || rc=$?`.
  # A `||` chain (like an `if` condition, or `!`) puts the callee into bash's
  # inherited "ignoring errors" state, and that state propagates into every
  # nested subshell where a plain `set -e` CANNOT clear it: $- shows `e` set
  # while a failing command still does not abort. The whole suite then reduces
  # to "did each gate's LAST command succeed", so a gate failing part-way --
  # including require_cmd / require_python_mod reporting an absent tool --
  # reports PASS. Measured: lint-py-shell reported PASS on a box with no ruff.
  # This is the same discipline run_gate_capture documents, and
  # suite_errexit_selftest is the regression test for it.
  set +e
  run_suite "$fast"
  rc=$?
  set -e
  cd "$REPO_ROOT"
  return "$rc"
}

# ===========================================================================
# ARGUMENT PARSING
# ===========================================================================
usage() {
  cat <<'EOF'
usage: bash scripts/ci.sh [--fast] [--native] [--rebuild]
       bash scripts/ci.sh --gate <name>
       bash scripts/ci.sh --list-gates

  --gate <name>  run exactly ONE registered gate, in place, natively.
                 This is what every CI workflow step invokes.
  --list-gates   dump the registry as "name<TAB>speed<TAB>description".
  --native       run the whole suite natively on a clean HEAD snapshot
                 (no container). The supported path on Linux.
  --fast         skip gates whose speed class is slow.
  --rebuild      force a devcontainer image rebuild first (container path).

With no flags: containerised on macOS; native on Linux when no container
runtime is installed. See the header of this file for the design.
EOF
}

fast=0
rebuild=0
native=0
gate=""

while [[ $# -gt 0 ]]; do
  case "$1" in
    --fast) fast=1 ;;
    --native) native=1 ;;
    --rebuild) rebuild=1 ;;
    --list-gates)
      list_gates
      exit $?
      ;;
    --gate)
      shift
      if [[ $# -eq 0 ]]; then
        echo "ci.sh: --gate requires a gate name" >&2
        usage >&2
        exit 2
      fi
      gate="$1"
      ;;
    --gate=*) gate="${1#--gate=}" ;;
    -h | --help)
      usage
      exit 0
      ;;
    *)
      echo "ci.sh: unknown flag '$1'" >&2
      usage >&2
      exit 2
      ;;
  esac
  shift
done

# --- single-gate mode: the CI path ----------------------------------------
# Runs in place (CI already provided a clean checkout) and natively (the
# runner IS the target environment). No container, so this path has no
# container-runtime dependency at all.
if [[ -n "$gate" ]]; then
  cd "$REPO_ROOT"
  run_one_gate "$gate"
  exit $?
fi

# --- in-container re-entry ------------------------------------------------
if [[ "${RA8_CI_INNER:-0}" == "1" ]]; then
  export HOME=/tmp
  git config --global --add safe.directory "$REPO_ROOT" >/dev/null 2>&1 || true
  # When the host tree is a linked worktree its git objects live in the main
  # repo's git dir, bind-mounted separately at its host path (see the
  # RA8_CI_GIT_COMMON_DIR block below). Git checks ownership of THAT directory
  # too, and the container runs as root against files owned by the host user,
  # so it needs its own exemption or discovery fails before `git archive`.
  if [[ -n "${RA8_CI_GIT_COMMON_DIR:-}" ]]; then
    git config --global --add safe.directory "$RA8_CI_GIT_COMMON_DIR" >/dev/null 2>&1 || true
  fi
  run_suite_on_snapshot "${RA8_CI_FAST:-$fast}"
  exit $?
fi

# --- native full-suite mode -----------------------------------------------
if [[ "$native" == "1" ]]; then
  run_suite_on_snapshot "$fast"
  exit $?
fi

# ===========================================================================
# HOST MODE. Build the devcontainer image, then re-enter inside the container.
# ===========================================================================

# podman is preferred (daemonless, and rootless where the kernel allows it, so
# a gate run cannot outlive the user's session); docker via colima is the
# macOS path.
#
# RA8_CONTAINER_RUNTIME may carry arguments, e.g. "sudo podman". That is not
# hypothetical: on a verification box that is itself an unprivileged LXC
# container, rootless podman runs containers fine but cannot BUILD the
# devcontainer -- apt drops privileges to the _apt user and its setgroups(2)
# call is denied inside the nested user namespace. Running podman as root
# inside that LXC sidesteps the nested namespace, and root there is still
# unprivileged on the Proxmox host, so the security boundary is unchanged.
read -r -a RUNTIME_CMD <<<"${RA8_CONTAINER_RUNTIME:-}"
if [[ "${#RUNTIME_CMD[@]}" -eq 0 ]]; then
  for candidate in podman docker nerdctl; do
    if command -v "$candidate" >/dev/null 2>&1; then
      RUNTIME_CMD=("$candidate")
      break
    fi
  done
fi

if [[ "${#RUNTIME_CMD[@]}" -eq 0 ]]; then
  # On Linux the native path is faithful -- the runner is Linux too -- so fall
  # back rather than refusing to run. On macOS it is not: the format gate
  # needs clang-format-22 and the host tests cannot mmap MAP_FIXED below
  # 4 GiB, so a macOS "pass" would be a lie. Refuse there.
  if [[ "$(uname -s)" == "Linux" ]]; then
    echo "==> no container runtime found; running gates NATIVELY on this Linux host."
    echo "    (Linux native == the CI environment. Install podman for isolation:"
    echo "     sudo apt-get install -y podman uidmap)"
    run_suite_on_snapshot "$fast"
    exit $?
  fi
  echo "error: no container runtime on PATH (looked for podman, docker, nerdctl)." >&2
  echo "  macOS cannot run these gates natively: the format gate pins" >&2
  echo "  clang-format-22, and the host tests need MAP_FIXED below 4 GiB," >&2
  echo "  which macOS arm64 refuses. Install a runtime:" >&2
  echo "    brew install colima docker && colima start" >&2
  exit 1
fi

RUNTIME_NAME="$(basename "${RUNTIME_CMD[${#RUNTIME_CMD[@]} - 1]}")"
if ! command -v "${RUNTIME_CMD[0]}" >/dev/null 2>&1; then
  echo "error: container runtime '${RUNTIME_CMD[0]}' is not on PATH." >&2
  exit 1
fi

# On macOS the project uses colima (no Docker Desktop license). Auto-start it,
# matching scripts/ci/test-docker.sh. podman on macOS uses its own VM instead.
if [[ "$(uname -s)" == "Darwin" && "$RUNTIME_NAME" == "docker" ]]; then
  if ! command -v colima >/dev/null 2>&1; then
    echo "error: colima not on PATH. Install: brew install colima" >&2
    exit 1
  fi
  if ! colima status >/dev/null 2>&1; then
    echo "==> starting colima VM (4 CPU, 6 GiB)"
    colima start --cpu 4 --memory 6
  fi
fi

if ! "${RUNTIME_CMD[@]}" info >/dev/null 2>&1; then
  echo "error: '${RUNTIME_CMD[*]}' is installed but not usable." >&2
  if [[ "$RUNTIME_NAME" == "docker" ]]; then
    echo "  docker daemon not reachable (try: colima start)" >&2
  else
    echo "  check '${RUNTIME_CMD[*]} info' output for the underlying failure." >&2
  fi
  exit 1
fi

# Build the devcontainer image. The Dockerfile has no COPY/ADD, so the tiny
# .devcontainer/ directory is a sufficient build context -- do NOT ship the
# multi-GB repo (datasheets, build trees) to the daemon as context.
if [[ "$rebuild" == "1" ]] || ! "${RUNTIME_CMD[@]}" image inspect "$IMAGE_TAG" >/dev/null 2>&1; then
  echo "==> building $IMAGE_TAG from .devcontainer/Dockerfile (runtime: ${RUNTIME_CMD[*]})"
  "${RUNTIME_CMD[@]}" build -t "$IMAGE_TAG" -f "$DOCKERFILE" "$REPO_ROOT/.devcontainer"
else
  echo "==> reusing cached image $IMAGE_TAG (--rebuild / REBUILD=1 to refresh)"
fi

# Linked git worktrees: mount the main repo's git dir too (#334).
#
# In a linked worktree -- an agent workspace from `make ws-new`, or a
# .claude/worktrees/* tree -- `.git` is a FILE holding "gitdir: <path>" that
# points into the MAIN repo's git directory. That directory is outside this
# bind mount, so the in-container `git archive HEAD` saw no repository at all
# and the suite died before its first gate with "fatal: not a git repository"
# followed by "tar: This does not look like a tar archive".
#
# That made the isolation pattern agents are told to use (one workspace each,
# so concurrent runs stop clobbering one another) incompatible with the only
# toolchain-correct way to run the gates, and pushed agents onto native runs
# where clang-tidy / gcovr / shellcheck / shfmt all differ from CI (#333).
#
# Mounting the common git dir at the SAME absolute path it has on the host is
# what makes the pointer resolve identically inside and outside. It stays
# read-only like the worktree itself: `git archive` only reads.
WORKTREE_RUN_ARGS=()
_git_common="$(cd "$REPO_ROOT" && cd "$(git rev-parse --git-common-dir 2>/dev/null || echo .git)" 2>/dev/null && pwd || true)"
case "${_git_common:-}" in
  "" | "$REPO_ROOT"/*) ;; # ordinary checkout: the git dir is already inside the mount
  *)
    echo "==> linked worktree detected; also mounting $_git_common (read-only)"
    WORKTREE_RUN_ARGS=(
      -v "$_git_common":"$_git_common":ro
      -e RA8_CI_GIT_COMMON_DIR="$_git_common"
    )
    ;;
esac

# Persistent compiler cache for the containerised path.
#
# Without this the cache is pointless here: HOME is /tmp inside the container
# and --rm takes it away, so every run would start cold and cmake/ccache.cmake
# would be wiring in a launcher whose cache never survives. A host directory
# mounted read-write is what lets a second `make ci` -- by any agent, from any
# workspace -- hit objects the first one compiled. It cannot carry stale state
# forward: a changed input is simply a different cache key.
#
# CCACHE_BASEDIR + CCACHE_NOHASHDIR are load-bearing, not tuning. Each run
# builds in a fresh mktemp snapshot, so without path normalisation every
# compilation hashes differently and the hit rate is flat zero.
#
# Absent or unwritable cache dir: run without it rather than fail. A cache is
# an optimisation, and a missing one must never turn a gate red.
CCACHE_RUN_ARGS=()
CCACHE_HOST_DIR="${RA8_CCACHE_DIR:-/var/cache/ccache-ra8}"
if [[ -d "$CCACHE_HOST_DIR" && -w "$CCACHE_HOST_DIR" ]]; then
  CCACHE_RUN_ARGS=(
    -v "$CCACHE_HOST_DIR":/ccache
    -e CCACHE_DIR=/ccache
    -e CCACHE_BASEDIR=/
    -e CCACHE_NOHASHDIR=1
    -e "CCACHE_SLOPPINESS=include_file_mtime,include_file_ctime,locale,time_macros"
    -e CCACHE_MAXSIZE="${RA8_CCACHE_MAXSIZE:-20G}"
  )
  echo "==> compiler cache: $CCACHE_HOST_DIR -> /ccache"
fi

echo "==> running CI gates in container (runtime=${RUNTIME_CMD[*]} fast=$fast)"
# The host repo is bind-mounted READ-ONLY: the in-container step extracts a
# clean `git archive HEAD` into a throwaway dir and builds there, so the host
# source tree and its macOS CMake caches are never touched. Run as root so
# that throwaway tree (and its fresh build dirs) is writable.
#
# --rm is load-bearing, not hygiene: the snapshot tree and every build dir the
# gates create live INSIDE the container, so the container exiting is what
# reclaims them. Nothing a gate writes can survive to poison the next run.
#
# CMAKE_BUILD_PARALLEL_LEVEL defaults to 4 (the CI runner's value, so this
# reproduces its timing) but is overridable for a beefier verification box.
exec "${RUNTIME_CMD[@]}" run --rm \
  -u 0:0 \
  -e RA8_CI_INNER=1 \
  -e RA8_CI_FAST="$fast" \
  -e HOME=/tmp \
  -e CMAKE_BUILD_PARALLEL_LEVEL="${CMAKE_BUILD_PARALLEL_LEVEL:-4}" \
  -v "$REPO_ROOT":/workspace:ro \
  ${WORKTREE_RUN_ARGS[@]+"${WORKTREE_RUN_ARGS[@]}"} \
  ${CCACHE_RUN_ARGS[@]+"${CCACHE_RUN_ARGS[@]}"} \
  -w /workspace \
  "$IMAGE_TAG" \
  bash scripts/ci.sh
