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
# re-creating a second home for check logic. scripts/utils/check_ci_parity.py
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
  "build-cross|slow|cross-build every example app"
  "sg-offsets|slow|NSC SG-veneer slot offsets in the linked secure ELF"
  "stack-usage|slow|aggregate -fstack-usage frames"
  "docs|slow|Doxygen warning gate + authored-diagram render check"
  "board-sim-smoke|slow|board_sim boot smoke over the example apps"
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

# --- ci-parity ------------------------------------------------------------
# The backstop for the whole scheme: workflow-as-driver still drifts if
# someone pastes a raw `run:` check into the YAML. This gate refuses that.
# A `( set -e )` subshell, not a `{ }` body: run_gate_capture disables errexit
# around the CALL, so a `{ }` gate with several commands reports only its LAST
# command's status and swallows everything before it. Every other multi-command
# gate here is already a subshell; this one was the exception, which is exactly
# why suite_errexit_selftest's `return 1` below could not fail the gate.
gate_ci_parity() (
  set -e
  require_python_mod yaml "pip install pyyaml (the CI runners ship it)"
  suite_errexit_selftest
  python3 scripts/utils/check_ci_parity.py --selftest
  python3 scripts/utils/check_ci_parity.py
)

# Assert that run_suite() still reports FAIL for a gate that fails PART-WAY
# through its body -- not merely one whose last command fails.
#
# This is not hypothetical. run_suite once invoked gates as
# `if run_one_gate "$name"; then`, and calling a function from an `if`
# condition suppresses ERREXIT *into the callee*: the `set -e` inside every
# `gate_*()` subshell went inert, so only each gate's final command decided
# PASS/FAIL. gate_pre_commit_checks runs a dozen checkers in sequence and only
# the last one counted; two of the others were dying on an uncaught exception
# while the gate reported PASS.
#
# A suite runner that cannot fail is worse than no suite runner, because it
# manufactures the exact "local green, CI red" this whole file exists to
# prevent. So the property is asserted on every run rather than remembered.
suite_errexit_selftest() {
  local out rc=0
  # A stand-in gate shaped like a real one: `set -e` subshell, failing command
  # in the MIDDLE, more commands after it.
  gate_ra8_errexit_probe() (
    set -e
    false
    echo "probe body continued past the failure"
  )
  # Drive the gate through run_gate_capture -- the SAME dispatch run_suite
  # uses. Reimplementing the call here would test a copy of the runner rather
  # than the runner, which is the very mistake this file is about.
  #
  # Redirect to a temp file rather than `out="$(run_gate_capture ...)"`:
  # command substitution runs the callee in a SUBSHELL, so the RA8_GATE_RC it
  # sets would never reach us (and under `set -u` reading it would abort).
  local probe_log
  probe_log="$(mktemp "${TMPDIR:-/tmp}/ra8-errexit-probe.XXXXXXXX")"
  run_gate_capture ra8-errexit-probe >"$probe_log" 2>&1
  rc="$RA8_GATE_RC"
  out="$(cat "$probe_log")"
  rm -f "$probe_log"
  unset -f gate_ra8_errexit_probe

  if [[ "$rc" -eq 0 ]]; then
    echo "ERROR: ci.sh suite runner self-test FAILED." >&2
    echo "       A gate that fails mid-body was reported as success (rc=0)." >&2
    echo "       ERREXIT is being suppressed into gate bodies -- almost" >&2
    echo "       certainly because a caller invokes the gate from an \`if\`" >&2
    echo "       condition, a && chain, or a \`!\` negation. Call it as a plain" >&2
    echo "       command with \`set +e\` around the CALL only." >&2
    echo "       Probe output was: $out" >&2
    return 1
  fi
  echo "ci.sh: suite-runner errexit self-test OK (mid-body failure propagates)."
}

# Assert that the commit-message gates can still tell real history from a
# synthetic snapshot -- in BOTH directions.
#
# Shaped after suite_errexit_selftest above, and for the same reason: #348 was
# a detector that had quietly stopped seeing its subject while still printing a
# green line. A guard nobody has watched fire is worth nothing, so the property
# is re-proved on every run rather than remembered.
#
# Direction 1 (fires): a `git init` + one-commit repo, built exactly the way
#   run_suite_on_snapshot builds its snapshot, must be REJECTED.
# Direction 2 (stays silent): a repo with real history must be ACCEPTED --
#   including the legal-but-tricky shape that a naive "the range must span more
#   than one commit" rule would wrongly reject, namely a detached HEAD with no
#   upstream, where ci_commit_range legitimately resolves to a range spanning
#   exactly ONE commit. It also asserts the resolved range is RESOLVABLE in the
#   history repo, which is the failure mode of the rejected "export
#   RA8_CI_COMMIT_RANGE into the snapshot" fix (SHAs the object store lacks).
commit_range_selftest() (
  # A SUBSHELL, not a { } body: this probe flips errexit while driving the
  # guard, and the gates that call it run `set -uo pipefail` WITHOUT -e.
  # A { } body would leave errexit enabled in the caller after returning,
  # silently changing how the rest of the gate behaves.
  set -uo pipefail
  require_cmd git || exit 1
  local tmp fake real range span
  tmp="$(mktemp -d "${TMPDIR:-/tmp}/ra8-range-probe.XXXXXXXX")"

  # --- Direction 1: the broken input the guard exists to catch. -----------
  fake="$tmp/snapshot"
  mkdir -p "$fake"
  printf 'snapshot\n' >"$fake/file.txt"
  git -C "$fake" init --quiet
  git -C "$fake" add -A
  git -C "$fake" -c user.email=ci@localhost -c user.name=ci \
    commit --quiet --no-verify -m "ci.sh snapshot of HEAD"

  if ci_require_real_history "$fake" 2>/dev/null; then
    rm -rf "$tmp"
    echo "ERROR: ci.sh commit-range self-test FAILED (direction 1)." >&2
    echo "       A synthetic one-commit snapshot was ACCEPTED as real" >&2
    echo "       history. The #348 false-green guard has stopped firing:" >&2
    echo "       the commit-message gates would again report PASS having" >&2
    echo "       scanned only 'ci.sh snapshot of HEAD'." >&2
    exit 1
  fi

  # --- Direction 2: legal-but-tricky input that must NOT be rejected. -----
  real="$tmp/real"
  mkdir -p "$real"
  git -C "$real" init --quiet
  local n
  for n in 1 2 3; do
    printf 'rev %s\n' "$n" >"$real/file.txt"
    git -C "$real" add -A
    git -C "$real" -c user.email=ci@localhost -c user.name=ci \
      commit --quiet --no-verify -m "test: commit $n"
  done
  # Detach HEAD so there is no upstream: ci_commit_range must fall through to
  # HEAD~1..HEAD, a range spanning exactly one commit. That is an ordinary
  # single-commit push, and rejecting it would be a false failure.
  git -C "$real" checkout --quiet --detach HEAD

  if ! ci_require_real_history "$real" 2>/dev/null; then
    rm -rf "$tmp"
    echo "ERROR: ci.sh commit-range self-test FAILED (direction 2)." >&2
    echo "       A repository with three real commits was REJECTED as" >&2
    echo "       having no history. The guard is over-firing and would" >&2
    echo "       block ordinary pushes." >&2
    exit 1
  fi

  # The resolved range must name objects the history repo actually has, and
  # must cover at least the tip commit.
  #
  # GITHUB_EVENT_PATH / GITHUB_SHA are cleared deliberately. On a runner they
  # ARE set, and ci_commit_range would then resolve the real PR's head/base
  # SHAs -- which do not exist in this throwaway repo -- so the probe would
  # fail on every CI run while passing locally. The property under test is the
  # local fallback chain, so pin the inputs to it.
  range="$(RA8_CI_HISTORY_REPO="$real" RA8_CI_COMMIT_RANGE="" \
    GITHUB_EVENT_PATH="" GITHUB_SHA="" ci_commit_range)"
  span="$(git -C "$real" rev-list --count "$range" 2>/dev/null)" || span=""
  if [[ -z "$span" || "$span" -lt 1 ]]; then
    rm -rf "$tmp"
    echo "ERROR: ci.sh commit-range self-test FAILED (direction 2, range)." >&2
    echo "       Resolved range '$range' does not resolve in the history" >&2
    echo "       repo, so the gates would scan nothing. This is the" >&2
    echo "       failure mode of exporting a host range into a snapshot" >&2
    echo "       whose object store lacks those commits." >&2
    exit 1
  fi

  rm -rf "$tmp"
  echo "ci.sh: commit-range self-test OK (rejects a synthetic snapshot," \
    "accepts real history; tricky one-commit span resolved to '$range')."
)

# --- ascii ----------------------------------------------------------------
# Every first-party root. fix-encoding.py skips third_party and any non-text
# extension, so vendored assets (the doxygen-awesome theme under docs/,
# datasheets, fonts, epubs) are exempt automatically.
gate_ascii() (
  set -e
  for dir in src libs tests examples port scripts tools docs; do
    python3 scripts/utils/fix-encoding.py --check "$dir"
  done
)

# --- copyright ------------------------------------------------------------
gate_copyright() (
  set -e
  local files=() line
  while IFS= read -r line; do files+=("$line"); done < <(
    git ls-files '*.c' '*.h' '*.cpp' '*.hpp' '*.cmake' '*.sh' '*.py' 'CMakeLists.txt'
  )
  python3 scripts/utils/check-copyright.py "${files[@]}"
)

# --- since ----------------------------------------------------------------
gate_since() (
  set -e
  local files=() line
  while IFS= read -r line; do files+=("$line"); done < <(
    git ls-files 'libs/ra8_*/inc/*.h'
  )
  python3 scripts/utils/check-since-version.py "${files[@]}"
)

# --- hil-sil-parity -------------------------------------------------------
# SIM==HIL: re-derives each harness's app discovery from hil_all.sh /
# sil_all.sh and fails if a hil/ app has no hil.conf, sits outside
# sil_all.sh's run set, or declares a HIL_MODE board_sim cannot check.
# Hardware-free, so an added HIL app cannot escape SIM coverage.
gate_hil_sil_parity() {
  python3 scripts/utils/check_hil_sil_parity.py
}

# --- no-ai-attribution ----------------------------------------------------
gate_no_ai_attribution() {
  python3 scripts/utils/check_no_ai_attribution.py
}

# --- no-ai-attribution-commits --------------------------------------------
# The file scanner cannot see commit messages -- trailers live in git
# metadata, not tracked files. Run the real scripts/git/commit-msg gate (the
# single source of truth for the brand list) over the push/PR range.
# CHECK_ONLY=ai runs just the attribution half, so a stray legacy term is
# attributed to -- and fails -- the inclusive-terminology gate instead.
gate_no_ai_attribution_commits() (
  set -uo pipefail
  local range rc=0 sha f hook_out repo
  # Prove the guard still tells real history from a snapshot, THEN prove this
  # run has real history to read. Both before the scan: a detector that has
  # stopped seeing its subject must not get to print a green line first.
  commit_range_selftest || return 1
  repo="$(ci_history_repo)"
  ci_require_real_history "$repo" || return 1
  range="$(ci_commit_range)"
  echo "Scanning commit messages in: $range (history repo: $repo)"
  for sha in $(git -C "$repo" rev-list "$range"); do
    f="$(mktemp)"
    git -C "$repo" log -1 --format=%B "$sha" >"$f"
    if ! hook_out="$(CHECK_ONLY=ai bash scripts/git/commit-msg "$f" 2>&1)"; then
      echo "$hook_out"
      echo "::error::Commit $sha carries a forbidden trailer in its message"
      rc=1
    fi
    rm -f "$f"
  done
  return $rc
)

# --- inclusive-terminology ------------------------------------------------
gate_inclusive_terminology() {
  python3 scripts/utils/check_inclusive_terminology.py
}

# --- inclusive-terminology-commits ----------------------------------------
gate_inclusive_terminology_commits() (
  set -uo pipefail
  local range repo
  # See gate_no_ai_attribution_commits: self-test, then real-history guard,
  # then the scan.
  commit_range_selftest || return 1
  repo="$(ci_history_repo)"
  ci_require_real_history "$repo" || return 1
  range="$(ci_commit_range)"
  echo "Scanning commit messages in: $range (history repo: $repo)"
  git -C "$repo" log "$range" --format=%B |
    python3 scripts/utils/check_inclusive_terminology_commits.py
)

# --- format ---------------------------------------------------------------
gate_format() (
  set -e
  local cf
  cf="$(pick_clang_format)"
  CLANG_FORMAT="$cf" bash scripts/format_code.sh --check --verbose
)

# --- pre-commit-checks ----------------------------------------------------
# The check_*.py gate suite. Each entry runs in its default mode -- the same
# way scripts/git/pre-commit invokes it.
gate_pre_commit_checks() (
  set -e
  python3 scripts/utils/check_obsolete_standards.py
  python3 scripts/utils/check_world_tags.py --strict
  python3 scripts/utils/check_mcdc_block.py
  # --all asks it to enumerate src/ + libs/ rather than read staged files.
  python3 scripts/utils/check_no_dynamic_alloc.py --all
  python3 scripts/utils/check_no_ai_attribution.py
  # C23 nullptr-only in first-party code. Vendor macros UX_NULL / TX_NULL /
  # FX_NULL / NX_NULL are exempted.
  python3 scripts/utils/check_no_null.py --all
  # NASA P10 Rule 4 -- every function fits in <=60 lines. Independent of the
  # clang-tidy compile-db, so it covers cross-compiled TUs the host tidy build
  # never sees (ThreadX/USBX/NetX/HAL register code).
  python3 scripts/utils/check_function_size.py
  # Maintainability cap -- no single .c/.h over 1000 lines. Complements the
  # per-function Rule 4 gate, which a god-file of short bodies can pass while
  # still being unreviewable.
  python3 scripts/utils/check_file_size.py
  # A header under a src/ directory is module-private and must be named
  # *_internal.h. A non-internal src/ header is a misfiled public interface
  # (belongs in inc/) or an unmarked private one.
  python3 scripts/utils/check_header_file_placement.py
  # The EK-RA8D2 pinout is a board fact owned by libs/ra8_board_ek_ra8d2.
  # Forbid the (port << 8 | pin) idiom in examples so the USB-pin duplication
  # #251 fixed (identical pins copy-pasted across 29 apps) cannot come back.
  python3 scripts/utils/check_example_board_pins.py
  # ra8_core is the foundation lib: it must depend on nothing above itself.
  python3 scripts/utils/check_core_layering.py
  # Every first-party source file ends in a trailing newline. Complements
  # .clang-format InsertNewlineAtEOF (C/C++ only) by covering scripts and
  # config-as-code.
  python3 scripts/utils/check_final_newline.py
  # No magic numbers. clang-tidy's readability-magic-numbers only sees files
  # in the host compile-db (no example main.c, no ARM-only #ifdef paths),
  # which is how ra8_delay_ms(500U) slipped past CI.
  python3 scripts/utils/check_magic_numbers.py
  # C23 [[...]] attribute syntax tree-wide (GNU __attribute__((...)) is
  # rejected except for interrupt / cmse_nonsecure_entry / cmse_nonsecure_call,
  # which clang has no portable [[gnu::]] spelling for).
  python3 scripts/utils/check_no_gnu_attribute.py
  # No silent ra8_err_t discards at TrustZone boot boundaries. A C23
  # (void)-cast silences [[nodiscard]] by ISO rule, so -Werror can never catch
  # a discarded ra8_cgc_init() right before a BLXNS (#191).
  python3 scripts/utils/check_tz_boundary_discard.py
  # Ban the numbered session-bookkeeping tags from comments and docs.
  python3 scripts/utils/check_no_wave_references.py
  # Every RA8_NSC_VENEER declared in ra8_nsc.h must have a definition -- a
  # decl with no def advertises an NS->S trust-boundary entry point that does
  # not exist.
  python3 scripts/utils/check_nsc_veneer_defs.py
  # Every insecure placeholder-crypto body (deterministic TRNG, forgeable
  # key-import MAC, plain-SRAM key vault, non-cryptographic RSIP key-wrap)
  # must sit behind the RA8_INSECURE_STUB_CRYPTO / RA8_SIMULATOR_MODE guard
  # with a fail-closed #else, so a release image that forgot to swap in real
  # crypto fails closed instead of shipping the stub (#180).
  python3 scripts/utils/check_stub_crypto_guarded.py
  # No function may exist only to satisfy the linker. Two narrowly-calibrated
  # rules: SHADOW (a do-nothing second definition of a symbol implemented for
  # real elsewhere -- the tools/*/webp_stub.c case, which made both host tools
  # advertise WebP and fail at runtime) and CANNED (an unsupported-error return
  # that discards every argument). Legitimate no-ops -- platform alternatives,
  # vtable/ISR callbacks, the fail-closed crypto #else above, MMIO handlers
  # returning module state -- are outside both rules by construction. Hardware
  # that does not exist yet is waived only by TODO(<named missing part>).
  # --selftest runs first and asserts the detector both fires and stays silent
  # on the right inputs, so a detector that quietly stopped matching cannot
  # pass as clean.
  python3 scripts/utils/check_no_silent_stubs.py --selftest
  python3 scripts/utils/check_no_silent_stubs.py
  # A HAL peripheral driver must not guard bare CPU asm
  # (wfi/dsb/isb/nop/cpsie/cpsid/reset-spin) on RA8_SIMULATOR_MODE -- those
  # route through libs/ra8_hal/inc/ra8_hw_intrinsics.h +
  # tests/mocks/ra8_host_asm_stub.c so the driver stays branch-free and
  # coverage lands on the shipping path (#293).
  python3 scripts/utils/check_no_driver_asm_guard.py
  # The in-tree line-number citation ban: reference a symbol, never a file
  # plus line number, since line numbers rot.
  python3 scripts/utils/check_line_citations.py
  # Per-app SystemInit boot init-order audit.
  python3 scripts/utils/audit_init_order.py
  # Every newly added compound boolean decision must arrive with MC/DC
  # vectors. This is the local counterpart of the mcdc gate's baseline
  # comparison: catching the gap at the decision keeps the baseline from
  # sliding in the first place. It was in the hand-maintained local suite but
  # never in the workflow, which is the same drift in the other direction --
  # now that the workflow calls this gate, both sides run it.
  python3 scripts/utils/check_new_compound_has_mcdc.py
  # OSHWA inclusive-terminology gate over first-party sources.
  python3 scripts/utils/check_inclusive_terminology.py
  # MAXIMUM-documentation gate: every function -- including statics -- carries
  # the full Doxygen tag set.
  python3 scripts/utils/doxy_audit.py --check
  # ... and for aggregate members: every enum value, struct/union member, and
  # macro across the first-party tree carries a doc comment.
  python3 scripts/utils/doxy_audit.py --members --check
  # Every hw_validated/hil app must be instrumented (a probed counter +
  # HIL_MODE=jlink_memprobe) or explicitly HIL_FAULT_EXPECTED -- a bare
  # HIL_MODE=alive proves nothing.
  python3 scripts/utils/check_hil_alive_policy.py
  # Reject explicit integer casts inside TEST_ASSERT_EQ arguments. The macro
  # widens both args to int64_t, so an outer (int)/(uint32_t) cast is
  # redundant and latently buggy (a (int) cast on a uint32_t enum truncates
  # before the widening).
  python3 scripts/utils/check_assert_casts.py tests/*.c
)

# --- annotations ----------------------------------------------------------
# check_annotations.py walks the AST via the libclang Python bindings and
# enforces the ra8_* annotation rules (docs/ANNOTATIONS.md).
#
# The import probe is load-bearing: check_annotations.py EXITS 0 when libclang
# is missing, so without the probe a strict gate reports nothing and passes.
# That is strictly worse than not running it at all.
gate_annotations() (
  set -e
  require_python_mod clang.cindex \
    "CI installs libclang==18.1.1; add it to .devcontainer/Dockerfile too."
  # Regression-test the checker itself before trusting its verdict.
  python3 scripts/utils/check_annotations.py --selftest
  python3 scripts/utils/check_annotations.py --check
)

# --- doc-attachment -------------------------------------------------------
# doxy_audit.py (run inside pre-commit-checks) asks only whether a block is
# PRESENT. A block attached to the wrong symbol SATISFIES that: paste one block
# twice and measured coverage rises while one symbol silently loses its
# documentation and another gains a duplicate. This gate asks the other
# question -- does the block describe the thing it sits on.
gate_doc_attachment() (
  set -e
  require_python_mod clang.cindex \
    "CI installs libclang==18.1.1; add it to .devcontainer/Dockerfile too."
  # Regression-test the checker itself, in BOTH directions, before trusting its
  # verdict: every defect class must fire, and the legal-but-tricky forms
  # (@copydoc, the CLAUDE.md definition-site one-liner, macro-generated
  # declarations, documented //#define options) must not.
  python3 scripts/utils/check_doc_attachment.py --selftest
  python3 scripts/utils/check_doc_attachment.py --check
)

# --- lint-py-shell --------------------------------------------------------
# --require: fail (never skip) when a tool is missing. These gates fail on ANY
# finding -- there is no grandfathering.
gate_lint_py_shell() (
  set -e
  python3 scripts/utils/check_ruff.py --require
  python3 scripts/utils/check_shell.py --require
)

# --- cite-check -----------------------------------------------------------
# cite-VALIDATION pass: every existing HUM cite must parse and point at a real
# chapter/page. The complementary cite-COVERAGE pass (--require-cites: does
# every MMIO access HAVE a cite?) surfaces a large libs/ra8_hal backlog and is
# not yet gate-clean, so it is deliberately not wired blocking.
gate_cite_check() {
  python3 scripts/utils/cite_check.py --strict
}

# --- roadmap-stats --------------------------------------------------------
gate_roadmap_stats() {
  if [[ -f docs/ROADMAP.md ]]; then
    python3 scripts/utils/roadmap_stats.py --check
  else
    echo "no docs/ROADMAP.md -- skipping roadmap_stats"
  fi
}

# --- sbom -----------------------------------------------------------------
# Supply-chain provenance gate. Fails when the committed CycloneDX SBOM
# (docs/sbom/ra8-firmware.cdx.json) is stale or the vendored libs/third_party/
# tree drifted from the registry -- an uncatalogued SOUP directory, or a
# version macro that disagrees with the recorded version.
gate_sbom() {
  python3 scripts/utils/gen_sbom.py --check
}

# --- nsc-cmse -------------------------------------------------------------
# Compiles every libs/ra8_nsc TU under -mcmse with -Wall -Wextra -Werror. The
# warning flags are load-bearing: a bare -fsyntax-only run is what let a
# veneer attribute clash go unnoticed. No app links the comms/eth veneers, so
# only this gate would catch an over-4-arg cmse_nonsecure_entry regression.
gate_nsc_cmse() (
  set -e
  use_pinned_arm_toolchain
  require_arm_gcc_m85
  bash scripts/utils/check_nsc_cmse.sh
)

# --- cppcheck -------------------------------------------------------------
# cppcheck 2.13 (Ubuntu 24.04) is finicky about the --suppressions-list
# parser; convert each non-comment, non-blank line into an explicit
# --suppress= flag, the syntax every version accepts. examples/host/* are
# macOS-only dev tools (C23 nullptr + AppKit), not cross-compiled firmware.
gate_cppcheck() (
  set -e
  require_cmd cppcheck
  local apps=() dir line
  if [[ -d examples ]]; then
    for dir in examples/*/*/ examples/*/*/*/ examples/*/*/*/*/; do
      dir="${dir%/}"
      case "$dir" in examples/host/*) continue ;; esac
      [[ -f "$dir/main.c" ]] && apps+=("$dir")
    done
  fi
  local suppress_args=()
  while IFS= read -r line; do
    line="${line%$'\r'}"
    line="${line## }"
    line="${line%% }"
    [[ -z "$line" ]] && continue
    case "$line" in \#*) continue ;; esac
    suppress_args+=("--suppress=$line")
  done <.cppcheck-suppressions
  cppcheck --enable=warning,style,performance,portability \
    --error-exitcode=1 \
    "${suppress_args[@]}" \
    --inline-suppr \
    -i libs/third_party \
    --std=c23 \
    src libs "${apps[@]}"
)

# --- misra ----------------------------------------------------------------
# misra_check.sh (cppcheck misra.py addon) over libs/ src/ port/, then
# misra_ratchet.py compares per-file-per-rule finding counts against
# .github/misra-baseline.txt. `make cppcheck` is NOT a substitute: different
# rule set, no addon, no baseline, so a new MISRA finding sails through it.
gate_misra() (
  set -e
  require_cmd cppcheck
  bash scripts/utils/misra_check.sh
  python3 scripts/utils/misra_ratchet.py --check
)

# --- tidy -----------------------------------------------------------------
gate_tidy() {
  bash scripts/clang_tidy.sh --check --verbose
}

# --- unit-tests -----------------------------------------------------------
gate_unit_tests() (
  set -e
  # build_tests.sh defaults to `${CMAKE:-cmake}` and run_tests.sh drives ctest.
  # Without a guard an absent toolchain surfaces as a bare "command not found"
  # deep in a build log; name the missing dependency at the gate boundary
  # instead. A gate must never be able to report "nothing to run" as success.
  require_cmd cmake "apt-get install -y cmake"
  require_cmd ctest "ships with cmake; check the cmake install"
  bash tests/build_tests.sh
  bash tests/run_tests.sh
)

# --- ubsan ----------------------------------------------------------------
# The whole host suite rebuilt under -fsanitize=undefined in its own tree with
# UBSAN_OPTIONS=halt_on_error=1, so any undefined behaviour is a hard test
# failure. Pin the compiler like the coverage gate does: the ambient `gcc`
# changes with the runner image, and -Wconversion findings are
# compiler-version-specific.
gate_ubsan() (
  set -e
  require_cmd gcc-13 "the UBSan gate pins gcc-13 to match CI"
  CC=gcc-13 CXX=g++-13 make ubsan
)

# --- coverage -------------------------------------------------------------
gate_coverage() (
  set -e
  require_cmd gcovr
  bash scripts/coverage.sh --gate
)

# --- coverage-report ------------------------------------------------------
# The plain statement + branch flow from docs/COVERAGE.md, ratcheted against
# .github/coverage-baseline.txt. Independent of the MC/DC pipeline.
# --in-container is a no-op marker that skips the script's macOS branch.
gate_coverage_report() (
  set -e
  require_cmd gcovr
  bash scripts/utils/coverage_report.sh --in-container
  python3 scripts/utils/check_coverage.py
)

# --- mcdc -----------------------------------------------------------------
# clang-18 source-based coverage with -fcoverage-mcdc, gated against
# .github/mcdc-baseline.txt so coverage can never regress.
#
# RA8_MCDC_THRESHOLD=0 disables mcdc_report.sh's own per-file gate; the
# project-wide baseline comparison below is the actual quality bar.
gate_mcdc() (
  set -e
  set -o pipefail
  require_cmd clang-18 "the MC/DC gate pins clang-18 to match CI"

  CC=clang-18 CXX=clang++-18 RA8_MCDC_THRESHOLD=0 \
    bash scripts/utils/mcdc_report.sh --in-container | tee mcdc-output.log

  local summary="build/mcdc-report/summary.txt"
  local baseline_file="${MCDC_BASELINE_FILE:-.github/mcdc-baseline.txt}"
  if [[ ! -f "$summary" ]]; then
    echo "FAIL: MC/DC summary not produced at $summary" >&2
    return 1
  fi
  if [[ ! -f "$baseline_file" ]]; then
    echo "FAIL: baseline file $baseline_file missing" >&2
    return 1
  fi
  local baseline total_line measured drop
  baseline="$(tr -d '[:space:]' <"$baseline_file")"
  total_line="$(grep -E '^TOTAL' "$summary" | tail -1 || true)"
  if [[ -z "$total_line" ]]; then
    echo "FAIL: no TOTAL row in $summary" >&2
    tail -40 "$summary" || true
    return 1
  fi
  # Last percentage column on the TOTAL row is the MC/DC %.
  measured="$(echo "$total_line" | grep -oE '[0-9]+\.[0-9]+%' | tail -1 | tr -d '%')"
  if [[ -z "$measured" ]]; then
    echo "FAIL: could not parse MC/DC % from TOTAL row: $total_line" >&2
    return 1
  fi
  printf 'Measured MC/DC: %s%%   Baseline: %s%%\n' "$measured" "$baseline"
  drop="$(awk -v m="$measured" -v b="$baseline" 'BEGIN{print (m+0 < b+0) ? 1 : 0}')"
  if [[ "$drop" -eq 1 ]]; then
    echo "FAIL: MC/DC coverage ${measured}% dropped below baseline ${baseline}%"
    echo ""
    echo "      Either add MC/DC test vectors for the new compound decisions"
    echo "      OR reduce the decision count. Do NOT lower the baseline file"
    echo "      to make this pass."
    echo ""
    echo "      scripts/utils/check_new_compound_has_mcdc.py is supposed to"
    echo "      catch this locally; if a regression reached CI, either the hook"
    echo "      was bypassed or a citation in an existing test drifted out of"
    echo "      the +/- 25-line tolerance window. See docs/MCDC.md."
    return 1
  fi
  echo "PASS: MC/DC coverage holds the baseline."
)

# --- cache-bench ----------------------------------------------------------
# #147/#160: builds + runs cache_bench (the SLRU decision record), reader_vmem
# (drives the real ra8_vmem with a reader workload and emits a
# cache_bench-consumable trace) and glyph_bench (sweeps the real glyph atlas),
# re-confirming SLRU on the captured reader trace on every push. clang-18
# accepts the C23 typed-enum / nullptr syntax the bench tools and ra8_mem use.
gate_cache_bench() (
  set -e
  require_cmd clang-18 "the cache-bench gate pins clang-18 to match CI"
  CC=clang-18 make bench-cache
)

# --- build-cross ----------------------------------------------------------
# #178: RA8_STRICT_TOOLCHAIN=1 promotes toolchain-ra8d2.cmake's version
# mismatch warning to a hard error, so a runner with a skewed arm-gcc fails
# loudly instead of silently shipping version-divergent miniz codegen.
gate_build_cross() (
  set -e
  use_pinned_arm_toolchain
  require_cmd arm-none-eabi-gcc
  RA8_STRICT_TOOLCHAIN=1 bash scripts/build_all_examples.sh
)

# --- sg-offsets -----------------------------------------------------------
# The only automated guard that the NSC Secure-Gateway veneer slot offsets in
# the linked SECURE ELF still match the k_sg_off_* enum ns_main.c reaches them
# by (ld emits the 8-byte stubs in ascending symbol order, so a rename or
# reorder silently shifts the slots). Reads the build-cross output:
# tz_nsc_cgc_usb is the app that binds all three ra8_nsc_cgc_* veneers. Its NS
# image and every non-TZ app carry no veneers and the checker skips them.
gate_sg_offsets() (
  set -e
  local elf
  elf="$(find examples -type f -name 'tz_nsc_cgc_usb.elf' | head -n 1)"
  if [[ -z "$elf" ]]; then
    echo "check_sg_offsets: tz_nsc_cgc_usb secure ELF not found -- run the" >&2
    echo "                  build-cross gate first (this gate reads its output)." >&2
    return 1
  fi
  echo "check_sg_offsets: inspecting $elf"
  python3 scripts/utils/check_sg_offsets.py "$elf"
)

# --- stack-usage ----------------------------------------------------------
# Every app is compiled with -fstack-usage (cmake/ra8_warnings.cmake), so
# build-cross left a per-object .su file next to each object. Aggregate them
# project-wide and fail on any first-party frame over 2048 B, any `dynamic`
# (VLA/alloca) frame -- NASA P10 Rule 3 -- or any critical-path module
# (ra8_isr/ra8_check/ra8_err/ra8_mpu/ra8_cgc/ra8_pfs) over 256 B.
gate_stack_usage() (
  set -e
  if [[ -z "$(find . -name '*.su' -print -quit)" ]]; then
    echo "stack_usage_check: no .su files found -- run the build-cross gate" >&2
    echo "                   first (this gate reads its output)." >&2
    return 1
  fi
  python3 scripts/utils/stack_usage_check.py --strict
)

# --- docs -----------------------------------------------------------------
# --gate builds the single top-level Doxyfile with the project-pinned doxygen
# (downloaded + sha256-verified by provision_doxygen.sh on first use, cached
# under build/tools/ after) and writes the warning log. Using the same pinned
# version as docs-publish keeps this gate and the published site in lockstep.
gate_docs() (
  set -e
  # graphviz is a hard dependency, not a nice-to-have: build_docs.sh degrades to
  # text-only output when `dot` is absent, and doxygen then warns on every
  # author-written diagram block, which this gate reports as a failure. Without
  # this check that surfaces as a dozen confusing warnings about the .md files
  # rather than the one true cause. Fail on the real reason instead.
  require_cmd dot
  bash scripts/build_docs.sh --gate
  local log="build/docs-gate/doxygen-warnings.log"
  if [[ ! -f "$log" ]]; then
    echo "FAIL: doxygen warning log not produced at $log" >&2
    return 1
  fi
  # Filter known-benign Doxygen warnings:
  #   - "for \ref command" -- Doxygen treats Markdown links in README.md as
  #     \ref directives; targets outside INPUT "fail" to resolve but render.
  #   - "multiple documentation sections" / "from the argument list of" --
  #     @retval / @param present in both the public header (canonical) and the
  #     .c definition. Cosmetic, no output impact.
  local relevant_warnings
  relevant_warnings="$(grep "warning:" "$log" |
    grep -v "for .ref command" |
    grep -v "multiple documentation sections" |
    grep -v "from the argument list of " |
    grep -v "multiple @param documentation sections" |
    grep -v "has multiple documentation sections" |
    grep -v "tag INCLUDE_PATH:" |
    grep -v "is not a readable file or directory" |
    grep -v "found more than one .mainpage comment block" |
    grep -v "End of list marker found without any preceding list items" |
    grep -v "Invalid list item found" |
    grep -v "Found unknown command" |
    grep -v "explicit link request to" |
    grep -v "argument '.*' of command @param is not found" |
    grep -v "found documented return type for .* that does not return anything" |
    grep -v "Problems running latex" || true)"
  if [[ -n "$relevant_warnings" ]]; then
    echo "Doxygen reported warnings:"
    echo "$relevant_warnings"
    return 1
  fi
  # A clean warning log does NOT mean the diagrams rendered. Doxygen drops an
  # authored diagram silently in several ways (HAVE_DOT=NO, a dot layout that
  # produces an empty SVG, a block doxygen never parsed), and the page still
  # publishes HTTP 200 with its prose intact. This counts what actually reached
  # the generated HTML and compares it against the source. It runs here, inside
  # the docs gate, because this is where the built HTML it inspects exists.
  python3 scripts/utils/check_doc_diagrams.py --html build/docs-gate/html
)

# --- board-sim-smoke ------------------------------------------------------
# board_sim (tools/board_sim) boots the real cross-compiled .elf on an
# emulated Cortex-M with the RA8D2 peripheral space modelled, asserting each
# image reaches its main loop without faulting (no invalid opcode, no unmapped
# access) and is not parked in a panic/fault halt.
gate_board_sim_smoke() (
  set -e
  use_pinned_arm_toolchain
  bash scripts/board_sim_smoke.sh
)

# --- board-sim-io-fabric --------------------------------------------------
# ra8_io fabric (#155) end-to-end: every storage backend driven through the
# same VFS API (block device -> ra8_fs FAT format/mount -> VFS mkdir + nested
# file round-trip), plus the format registry, the LRU sector cache, and the
# DEFLATE stream, asserted by each demo's PASS banner. Covers RAM/SRAM,
# external SDRAM, SD-over-SPI, native SDHI, OSPI NOR (erase-before-write) and
# on-chip MRAM (program/erase).
gate_board_sim_io_fabric() (
  set -e
  use_pinned_arm_toolchain
  bash scripts/board_sim_smoke.sh \
    ra8_io_demo ra8_io_sdram_demo ra8_io_compress_demo \
    ra8_io_sd_demo ra8_io_sdhi_demo ra8_io_xspi_demo ra8_io_mram_demo \
    ra8_io_fsfmt_demo ra8_io_cache_demo
)

# --- sil-integration ------------------------------------------------------
# The hardware-free mirror of the bench HIL suite: every app under
# examples/ek_ra8d2/hw_validated/hil/ booted in board_sim headless and checked
# against the SAME per-app hil.conf the real board is checked against.
# ENFORCING -- a board_sim modelling gap or a firmware regression fails here
# rather than being logged and ignored.
gate_sil_integration() (
  set -e
  use_pinned_arm_toolchain
  # ereader_shelf compiles against a COMMITTED generated MRAM library header.
  # The bake is not reproducible across architectures (libjpeg SIMD decode
  # rounding differs x86_64 vs Apple silicon at the same Pillow version), so
  # the fixture is tracked; assert it is present so a future re-gitignore
  # fails loudly here instead of as a confusing app FAIL.
  test -s examples/ek_ra8d2/hw_validated/hil/ereader_shelf/library.h
  bash scripts/sil_all.sh -j "$(cpu_count)"
)

# --- mcdc-delta-base (manual) ---------------------------------------------
# Builds the BASE branch's MC/DC summary in a throwaway worktree so the PR
# comment can show a per-file delta. Informational, never blocking: the
# workflow marks the step continue-on-error and an empty summary just renders
# the PR column alone.
gate_mcdc_delta_base() (
  set -e
  local base_ref="${RA8_MCDC_BASE_REF:-main}"
  local tree
  tree="$(mktemp -d "${TMPDIR:-/tmp}/ra8-mcdc-base.XXXXXXXX")"
  rm -rf "$tree"
  git worktree add "$tree" "origin/${base_ref}"
  (
    cd "$tree"
    CC=clang-18 CXX=clang++-18 RA8_MCDC_THRESHOLD=0 \
      bash scripts/utils/mcdc_report.sh --in-container
  ) || echo "base-branch MC/DC build failed -- the delta will be PR-only"
  if [[ -f "$tree/build/mcdc-report/summary.txt" ]]; then
    cp "$tree/build/mcdc-report/summary.txt" base-summary.txt
  else
    : >base-summary.txt
  fi
  git worktree remove --force "$tree" || rm -rf "$tree"
)

# --- osv-scan (manual) ----------------------------------------------------
# Two legs (scripts/utils/osv_scan.sh): the SBOM purl leg, and the commit leg
# that resolves GIT-range advisories for the git-vendored C/C++ SOUP. Exits 1
# on any finding. Downloads a version-pinned, sha256-verified scanner, so it
# needs the network and is scheduled weekly rather than run per push.
gate_osv_scan() (
  set -e
  require_cmd curl
  local version="${OSV_SCANNER_VERSION:?set OSV_SCANNER_VERSION}"
  local sha256="${OSV_SCANNER_SHA256:?set OSV_SCANNER_SHA256}"
  # The scan is only as good as the SBOM it reads; refuse to scan a stale one.
  python3 scripts/utils/gen_sbom.py --check
  curl -fsSL -o osv-scanner \
    "https://github.com/google/osv-scanner/releases/download/v${version}/osv-scanner_linux_amd64"
  echo "${sha256}  osv-scanner" | sha256sum -c -
  chmod +x osv-scanner
  bash scripts/utils/osv_scan.sh --scanner ./osv-scanner --output-dir osv-report
)

# --- fuzz-sweep (manual) --------------------------------------------------
# Deep-runs every registered harness (the RA8_FUZZ_TARGETS registry in
# tests/fuzz/CMakeLists.txt, consumed through run_fuzz.sh --all) with a real
# per-target budget. --list parses the registry and cross-checks it against
# the tests/fuzz/fuzz_ra8_*.c sources, so registry drift fails before the
# budget is spent.
gate_fuzz_sweep() (
  set -e
  set -o pipefail
  local budget="${RA8_FUZZ_SECONDS:-600}"
  case "$budget" in
    '' | *[!0-9]*)
      echo "invalid RA8_FUZZ_SECONDS: '$budget'" >&2
      return 1
      ;;
  esac
  if [[ "$budget" -eq 0 ]]; then
    echo "RA8_FUZZ_SECONDS must be >= 1 (0 means 'no limit' to libFuzzer)" >&2
    return 1
  fi
  echo "Registered harnesses:"
  bash scripts/utils/run_fuzz.sh --list
  # Compiler probe: the same trivial -fsanitize=fuzzer link run_fuzz.sh
  # performs during auto-selection, done explicitly so a de-provisioned runner
  # is diagnosed in seconds instead of after hours.
  local probe found="" cand
  probe="$(mktemp -d)"
  printf 'int LLVMFuzzerTestOneInput(const unsigned char* d, unsigned long n);\nint LLVMFuzzerTestOneInput(const unsigned char* d, unsigned long n) { (void)d; (void)n; return 0; }\n' >"$probe/p.c"
  for cand in clang clang-22 clang-21 clang-20 clang-19 clang-18 clang-17; do
    if command -v "$cand" >/dev/null 2>&1 &&
      "$cand" -fsanitize=fuzzer -o "$probe/p" "$probe/p.c" >/dev/null 2>&1; then
      found="$cand"
      break
    fi
  done
  rm -rf "$probe"
  if [[ -z "$found" ]]; then
    echo "FAIL: no clang on this runner can link -fsanitize=fuzzer." >&2
    echo "      Install clang-<N> + libclang-rt-<N>-dev (the same packages" >&2
    echo "      the mcdc gate's profile runtime comes from)." >&2
    return 1
  fi
  echo "libFuzzer-capable compiler: $found"
  bash scripts/utils/run_fuzz.sh --all "$budget" 2>&1 | tee fuzz-nightly.log
)

# --- hil-all (manual) -----------------------------------------------------
# Drives scripts/hil_all.sh, which auto-discovers every app under
# examples/ek_ra8d2/hw_validated/hil/ and verifies each via its hil.conf
# manifest. Needs the bench EK-RA8D2 attached to the Pi 5 runner.
gate_hil_all() (
  set -e
  require_cmd arm-none-eabi-gcc
  bash scripts/hil_all.sh --list
  # hil_all.sh builds them itself, but doing it explicitly first gives clearer
  # logs when a build (not a flash) fails.
  local apps=() line
  while IFS= read -r line; do apps+=("$line"); done < <(
    find examples/ek_ra8d2/hw_validated/hil -mindepth 1 -maxdepth 1 -type d \
      -exec basename {} \;
  )
  make -j"$(cpu_count)" "${apps[@]}"
  bash scripts/hil_all.sh --skip-build
)

# --- docs-publish (manual) ------------------------------------------------
# Builds the Doxygen HTML and force-pushes it to the orphan gh-pages branch.
# Not a pass/fail quality gate -- registered so ci-parity can bind the publish
# workflow's step and no unreviewed `run:` body hides inside it.
gate_docs_publish() (
  set -e
  # Same hard dependency as the docs gate, and it matters more here: without
  # `dot`, build_docs.sh degrades to text-only output and this gate would
  # force-push a diagram-free site over the live one, succeeding the whole way.
  # The publish path is exactly where a silent degradation does the damage.
  require_cmd dot
  make docs
  # Verify the site about to be published actually contains its diagrams,
  # against the real output tree `make docs` just wrote.
  python3 scripts/utils/check_doc_diagrams.py --html build/docs/html
  bash scripts/publish_docs.sh
)

# ===========================================================================
# REGISTRY PLUMBING
# ===========================================================================

gate_fn_name() {
  printf 'gate_%s\n' "${1//-/_}"
}

registry_names() {
  local row
  for row in "${RA8_GATE_REGISTRY[@]}"; do
    printf '%s\n' "${row%%|*}"
  done
}

# Machine-readable dump consumed by scripts/utils/check_ci_parity.py. Also
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
  return $rc
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
  return $rc
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
# matching scripts/test-docker.sh. podman on macOS uses its own VM instead.
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
