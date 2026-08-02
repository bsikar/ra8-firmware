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
#   bash scripts/ci.sh --gate <name> --container   # ...in the toolchain image
#   bash scripts/ci.sh --native        # every gate natively, on a HEAD snapshot
#   bash scripts/ci.sh --fast          # skip the slow gates
#   bash scripts/ci.sh --list-gates    # machine-readable registry dump
#   bash scripts/ci.sh                 # containerised (the macOS path)
#
# The suite's exit status is a THREE-value contract, the same one
# scripts/ci/monitor.sh uses:
#
#   0  PASS      every selected gate passed
#   1  FAIL      a gate failed, or nothing was selected to run
#   3  UNKNOWN   the run stopped being a measurement -- it was signalled, or
#                the snapshot it was gating vanished under it. It prints
#                RESULT: ABORTED and no per-gate FAIL row, because a killed run
#                has no verdict. Never read it as a pass OR as a failure;
#                re-run. scripts/ci/lib/abort.sh has the whole story (#542).
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
# The tag the containerised path boots. Where it comes from, and what stops it
# going stale, is scripts/ci/devcontainer_image.sh -- the one place that knows
# how to build it and how to tell a current image from an old one (#521).
IMAGE_TAG="ra8-ci:latest"
# Exit status of the most recent run_gate_capture call. Pre-declared so `set -u`
# cannot abort a reader before the first gate has run.
RA8_GATE_RC=0

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
  "ci-status-contract|fast|ci-status exit codes: PASS/FAIL/UNKNOWN never conflated"
  "toolchain-parity|fast|pinned host tools match .devcontainer/Dockerfile versions"
  "ascii|fast|ASCII-only source files"
  "copyright|fast|SPDX + copyright headers"
  "since|fast|Doxygen @since tags on public headers"
  "hil-eil-parity|fast|every HIL app is also exercised in ra8_emulator"
  "no-ai-attribution|fast|attribution ban (tracked files)"
  "no-ai-attribution-commits|fast|attribution ban (commit messages)"
  "inclusive-terminology|fast|OSHWA inclusive terminology (tracked files)"
  "inclusive-terminology-commits|fast|inclusive terminology (commit messages)"
  "format|fast|clang-format dry run"
  "pre-commit-checks|fast|the check_*.py gate suite"
  "bench-lock|fast|every bench-touching script takes the bench lock"
  "annotations|fast|RA8_* annotation attributes (libclang)"
  "doc-attachment|fast|a Doxygen block describes the symbol it is attached to"
  "init-order-freshness|fast|committed docs/INIT_ORDER_AUDIT.md matches a fresh regenerate"
  "roadmap-dashboard-freshness|fast|committed docs/ROADMAP_DASHBOARD.md matches a fresh regenerate"
  "lint-py-shell|fast|ruff + shellcheck + shfmt"
  "lint-cmake|fast|cmake-format + cmake-lint over every listfile"
  "lint-yaml|fast|yamllint + actionlint over the workflows"
  "lint-make|fast|Makefile structure, headers and portable ROOT"
  "lint-ld|fast|linker-script structure, headers and symbol closure"
  "lint-asm|fast|assembly headers, sections and exported-symbol shape"
  "lint-devcontainer|fast|hadolint over the Dockerfile, zsh -n over the zshrc"
  "lint-coverage|fast|every code file is claimed by a linter and a formatter"
  "reserved-addrs|fast|address enums never point into a HUM Reserved window"
  "cite-check|fast|HUM citation validator (strict)"
  "hum-register-map|fast|register symbols cross-checked against the HUM register tables"
  "roadmap-stats|fast|ROADMAP summary stats"
  "sbom|fast|CycloneDX SBOM freshness"
  "soup-upstream|fast|vendored SOUP matches the upstream blobs recorded for its pin"
  "nsc-cmse|fast|ra8_nsc veneers compile under -mcmse"
  "cppcheck|slow|cppcheck static analysis"
  "scan-build|slow|clang static analyzer over the host test build"
  "misra|slow|MISRA-C 2012 ratchet"
  "tidy|slow|clang-tidy"
  "unit-tests|slow|host unit tests (ctest)"
  "ubsan|slow|host unit tests under UBSan"
  "coverage|slow|gcovr line/branch gate (90/80)"
  "coverage-report|slow|coverage_report.sh + check_coverage.py ratchet"
  "mcdc|slow|MC/DC coverage against the committed baseline"
  "artefact-freshness|slow|committed MC/DC + doxygen gap docs match a fresh regenerate"
  "cache-bench|slow|cache/glyph benchmark toolchain"
  "tools-build|slow|first-party host tools compile, link and test on Linux"
  "build-cross|slow|cross-build every example app"
  "build-cross-union|slow|the cross-build shards covered every app exactly once"
  "sg-offsets|slow|NSC SG-veneer slot offsets in the linked secure ELF"
  "stack-usage|slow|aggregate -fstack-usage frames"
  "docs|slow|Doxygen warning gate + authored-diagram render check"
  "emulator-smoke|slow|ra8_emulator boot smoke over the example apps"
  "emulator-matrix|slow|every example booted in ra8_emulator, ratcheted downward"
  "emulator-io-fabric|slow|ra8_io fabric demos in ra8_emulator"
  "eil-integration|slow|every HIL app booted in ra8_emulator against its hil.conf"
  "mcdc-delta-base|manual|base-branch MC/DC summary for the PR delta comment"
  "mcdc-delta-render|manual|render the PR MC/DC delta comment body"
  "osv-scan|manual|OSV CVE sweep of the vendored SOUP (network, scheduled)"
  "soup-upstream-refresh|manual|re-fetch every SOUP upstream and re-prove the manifests (network)"
  "fuzz-sweep|manual|libFuzzer sweep of every harness (nightly budget)"
  "runner-clock|manual|no CI runner moved its wall clock under a running job"
  "runner-image-deps|manual|every require_cmd/require_python_mod tool exists in the runner image"
  "hil-all|manual|hardware-in-the-loop suite on the bench EK-RA8D2"
  "bench-lock-selftest|manual|the bench lock proved against the real bench host"
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

# cpu_count() and ra8_max_jobs() -- the ONE canonical bounded-parallelism
# source (#328). Gate bodies derive every `-j` / `-P` width from ra8_max_jobs,
# never a raw nproc, so N gate jobs on one shared box do not each grab all
# cores. The standalone builders / checks / emulator drivers a gate shells out to
# source the same file, so there is a single home for the policy.
# shellcheck source=scripts/ci/lib/parallelism.sh
. "${SCRIPT_DIR}/ci/lib/parallelism.sh"

# use_pinned_tool_path() + require_tool_versions() -- deterministic tool
# resolution (#333). run_one_gate calls use_pinned_tool_path so every gate,
# however the shell was entered (a login shell, a non-interactive `ssh dev`, a
# GitHub Actions step), resolves the SAME pinned binaries; require_tool_versions
# then makes the wrong version fail loudly. One home for the policy, sourced the
# same way as parallelism.sh.
# shellcheck source=scripts/ci/lib/tool_env.sh
. "${SCRIPT_DIR}/ci/lib/tool_env.sh"

# The abort machinery (#542): a run that was KILLED, or whose snapshot vanished
# under it, reports UNKNOWN and stops -- it never invents gate failures against
# a tree that is no longer there. Exit 3, the same "no verdict" code
# scripts/ci/monitor.sh uses. Read that file's header before changing any of
# it; the trap shape in particular is load-bearing.
# shellcheck source=scripts/ci/lib/abort.sh
. "${SCRIPT_DIR}/ci/lib/abort.sh"

# Persistent PINNED-TOOL cache (#326). The docs gate builds with a
# version-pinned doxygen that scripts/builders/provision_doxygen.sh downloads +
# sha256-verifies on first use. Every suite run builds in a fresh mktemp
# snapshot whose build/tools/ is destroyed on exit, so without a persistent
# location that download repeats every run and FAILS outright with no network.
# This is to pinned tools what the ccache mount is to compiled objects: one host
# directory, reused across runs and shared across agents at zero cost.
ra8_tools_cache_host_dir() {
  printf '%s\n' "${RA8_TOOLS_CACHE_DIR:-/var/cache/ra8-tools}"
}

# Point provision_doxygen.sh (and any future pinned-tool provisioner) at that
# persistent directory by exporting RA8_TOOLS_CACHE, for the paths that run a
# gate DIRECTLY on the host (single-gate and native-suite modes). The container
# path sets RA8_TOOLS_CACHE via `-e` against the /toolcache mount instead, so
# leave an already-set value untouched. A cache is an optimisation: an
# unwritable location degrades to the per-build build/tools/ rather than
# failing a gate.
export_tools_cache() {
  [[ -n "${RA8_TOOLS_CACHE:-}" ]] && return 0
  local dir
  dir="$(ra8_tools_cache_host_dir)"
  mkdir -p "$dir" 2>/dev/null || true
  if [[ -d "$dir" && -w "$dir" ]]; then
    export RA8_TOOLS_CACHE="$dir"
    echo "==> pinned-tool cache: $dir (survives the snapshot; docs gate doxygen)" >&2
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

# Refuse to run a ra8_emulator gate on an unpinned Unicorn.
#
# ra8_emulator boots the real firmware .elf on Unicorn, and different Unicorn
# versions decode Armv8.1-M (Helium/MVE) differently, so an unpinned emulator
# makes "same commit, different verdict" structural (#354). This is the
# fail-loud counterpart to require_cmd: the check binds the ACTUAL libunicorn
# ra8_emulator will link and exits non-zero -- with remediation -- when it is not
# the pin, rather than letting a fossil produce an unreproducible green run.
require_pinned_unicorn() {
  bash scripts/checks/check_unicorn_version.sh
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
  # After a FORCE push the event's before-sha was rewritten out of existence,
  # and the @{upstream} fallback resolves to the freshly-pushed head itself --
  # a head..head range spanning nothing, which ci_report_commit_range then
  # rejects. A push event always carries at least its tip commit, so drop the
  # degenerate base and let the head~1 fallback scan that one commit instead.
  # workflow_dispatch deliberately keeps base == head: a manual re-run has
  # nothing new to scan, and rejecting that vacuity is #357's whole point.
  if [[ "${GITHUB_EVENT_NAME:-}" == "push" && -n "$base" ]] &&
    [[ "$(git -C "$repo" rev-parse --verify --quiet "${base}^{commit}" 2>/dev/null)" == "$(git -C "$repo" rev-parse --verify --quiet "${head}^{commit}" 2>/dev/null)" ]]; then
    base=""
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

# Announce the resolved range a message-scanning gate is about to cover, WITH
# the commit count, and reject the vacuous zero-commit case (#357 Path 2).
#
# On workflow_dispatch a manual re-run resolves the range to head..head -- the
# checked-out branch's upstream equals its head, so base == head and the range
# spans zero commits. The gate would then scan nothing and report PASS, so a
# "green" from a hand re-run means "examined nothing", not "history is clean".
# A PR always supplies a real base, and a push resolves to at least head~1..head
# (ci_commit_range drops a base that degenerated to the head after a force
# push), so a zero-commit range is only ever this dispatch degeneracy. Print
# the count in EVERY case so a run that examined commits and found nothing is
# visibly distinct from one that examined none, and FAIL loudly on the zero
# case rather than passing vacuously -- the same fail-on-missing-input
# discipline require_cmd applies to an absent tool.
ci_report_commit_range() {
  local repo="$1" range="$2" count
  count="$(git -C "$repo" rev-list --count "$range" 2>/dev/null || printf '0')"
  echo "Scanning $count commit message(s) in: $range (history repo: $repo)"
  if [[ "${count:-0}" -eq 0 ]]; then
    echo "::error::commit-metadata gate examined 0 commits -- range '$range'" >&2
    echo "       is empty (base resolved equal to head). This is the" >&2
    echo "       workflow_dispatch head..head vacuity (#357): a manual re-run" >&2
    echo "       has nothing new to scan, so a green here would mean 'examined" >&2
    echo "       nothing', not 'history is clean'. Failing loudly instead." >&2
    return 1
  fi
  return 0
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
  # The tree under test must still be there (#542). This is the ONE choke point
  # every dispatch passes through -- the --gate CLI path and run_suite via
  # run_gate_capture both land here -- so no gate has to remember to check, and
  # a vanished snapshot is refused with a named reason instead of being
  # discovered by each remaining gate as a content failure.
  ci_require_tree_intact "$name" || return "$RA8_CI_EXIT_ABORTED"
  # Deterministic tool resolution BEFORE any gate body runs (#333): normalise
  # PATH so a non-login shell resolves the same pinned binaries a login shell
  # does. This is the single choke point every gate passes through -- the
  # --gate CLI path and run_suite (via run_gate_capture) both land here -- so no
  # gate has to remember to do it.
  use_pinned_tool_path
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
# Materialise the registry and HONOUR ITS EXIT STATUS, writing the dump to
# stdout for the caller to consume.
#
# run_suite used to read it as `done < <(list_gates)`. A process substitution's
# exit status is unobservable -- bash discards it and `set -e` never sees it --
# so list_gates' `return 1` was dropped, and since list_gates `continue`s past
# any row whose gate_*() function is missing, such a gate vanished from the
# suite while the run still printed RESULT: PASS. Only the ci-parity gate
# re-reading the registry stood between that and a false green; the runner has
# to be honest on its own (#190). suite_registry_selftest asserts it every run.
registry_dump_or_die() {
  local dump rc=0
  dump="$(list_gates)" || rc=$?
  if [[ "$rc" -ne 0 ]]; then
    echo "" >&2
    echo "ci.sh: the gate registry is INVALID (see the errors above)." >&2
    echo "       Refusing to run a partial suite and report on it." >&2
    return 1
  fi
  printf '%s\n' "$dump"
}

# Print the per-gate PASS/FAIL table and return the suite's verdict. An EMPTY
# selection is never a pass: this loop is bounded by the gate array, so with
# zero gates it never set `failed` and the run printed RESULT: PASS having
# executed nothing -- the shape every gate-honesty defect takes (#190).
print_suite_summary() {
  local fast="$1"
  shift
  local count="$1"
  shift
  local names=("${@:1:count}") results=("${@:count+1}")

  echo ""
  echo "==================================================================="
  echo "== ci.sh summary$([[ "$fast" == "1" ]] && echo "  (--fast: slow gates skipped)")"
  echo "==================================================================="
  if [[ "$count" -eq 0 ]]; then
    echo "  RESULT: FAIL -- no gates were selected to run." >&2
    echo "  A suite that executed nothing has not passed." >&2
    return 1
  fi
  local failed=0 aborted=0 idx=0
  while [[ "$idx" -lt "$count" ]]; do
    printf '  %-32s %s\n' "${names[$idx]}" "${results[$idx]}"
    [[ "${results[$idx]}" == "FAIL" ]] && failed=1
    [[ "${results[$idx]}" == "ABORTED" ]] && aborted=1
    idx=$((idx + 1))
  done
  echo "-------------------------------------------------------------------"
  # An abort outranks everything below it (#542). The rows above it were real
  # measurements and are shown as such, but the RUN has no verdict: it stopped
  # early, and the gates it never reached are unmeasured rather than green.
  # UNKNOWN is a real answer here -- do not read it as either a pass or a fail.
  if [[ "$aborted" -ne 0 ]]; then
    echo "  RESULT: ABORTED -- $(ci_abort_reason)"
    echo "  UNKNOWN (exit $RA8_CI_EXIT_ABORTED): neither a pass nor a fail."
    echo "  The gates listed above ran before the abort; everything after it"
    echo "  was never measured. Re-run to get a verdict."
    return "$RA8_CI_EXIT_ABORTED"
  fi
  if [[ "$failed" -ne 0 ]]; then
    echo "  RESULT: FAIL"
    return 1
  fi
  echo "  RESULT: PASS"
  return 0
}

run_suite() {
  local fast="$1"
  local gate_names=() gate_results=()
  local name speed registry_dump

  registry_dump="$(registry_dump_or_die)" || return 1

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
    # An abort is not a verdict (#542). A signalled run never reaches here at
    # all -- ci_abort_on_signal exits -- so this arm is the other way a run
    # stops being a measurement: the snapshot went away under it. Record it as
    # ABORTED and STOP, because every gate after this one would be reporting on
    # a tree that is not there.
    if ci_aborted; then
      gate_results+=("ABORTED")
      break
    fi
    if [[ "$RA8_GATE_RC" -eq 0 ]]; then
      gate_results+=("PASS")
    else
      gate_results+=("FAIL")
    fi
  done <<<"$registry_dump"

  print_suite_summary "$fast" "${#gate_names[@]}" ${gate_names[@]+"${gate_names[@]}"} \
    ${gate_results[@]+"${gate_results[@]}"}
}

# The snapshot machinery (materialise the tree under test, prove it is HEAD, and
# run the suite inside it). It sits beside abort.sh, which owns that snapshot's
# lifecycle.
# shellcheck source=scripts/ci/lib/snapshot.sh
. "${SCRIPT_DIR}/ci/lib/snapshot.sh"

# ===========================================================================
# ARGUMENT PARSING
# ===========================================================================
usage() {
  cat <<'EOF'
usage: bash scripts/ci.sh [--fast] [--native] [--rebuild]
       bash scripts/ci.sh --gate <name> [--container]
       bash scripts/ci.sh --list-gates

  --gate <name>  run exactly ONE registered gate, in place, natively.
                 This is what every CI workflow step invokes.
  --container    with --gate: run that gate INSIDE the toolchain container on
                 a clean HEAD snapshot, for a host that is not natively a
                 CI-equivalent one (macOS; a runner host with no host toolchain).
  --list-gates   dump the registry as "name<TAB>speed<TAB>description".
  --native       run the whole suite natively on a clean HEAD snapshot
                 (no container). The supported path on Linux.
  --fast         skip gates whose speed class is slow.
  --rebuild      force a devcontainer image rebuild first (container path).

  --selftest-abort <mode>
                 INTERNAL. Runs the real suite runner over fixture gates so
                 suite_abort_selftest can prove a killed run reports no gate
                 verdict (#542). Modes: hang | destroy | fail. Its output is a
                 probe, never a suite verdict.

With no flags: containerised on macOS; native on Linux when no container
runtime is installed. See the header of this file for the design.
EOF
}

fast=0
rebuild=0
native=0
container=0
gate=""
selftest_abort=""

while [[ $# -gt 0 ]]; do
  case "$1" in
    --fast) fast=1 ;;
    --native) native=1 ;;
    --container) container=1 ;;
    --rebuild) rebuild=1 ;;
    --selftest-abort)
      shift
      if [[ $# -eq 0 ]]; then
        echo "ci.sh: --selftest-abort requires a mode (hang | destroy | fail)" >&2
        usage >&2
        exit 2
      fi
      selftest_abort="$1"
      ;;
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

# --- abort self-test probe: INTERNAL --------------------------------------
# Driven only by suite_abort_selftest (scripts/ci/gates/hygiene.sh), which the
# ci-parity gate runs. Placed before every other mode so a probe run can never
# be confused with a real one.
if [[ -n "$selftest_abort" ]]; then
  cd "$REPO_ROOT"
  ci_abort_probe "$selftest_abort"
  exit $?
fi

# --- single-gate mode: the CI path ----------------------------------------
# Runs in place (CI already provided a clean checkout) and natively (the
# runner IS the target environment). No container, so this path has no
# container-runtime dependency at all.
#
# `--container` opts out of this short-circuit and falls through to host mode,
# which re-enters with RA8_CI_GATE set. Some hosts are deliberately NOT
# CI-equivalent natively: macOS cannot be, and a runner host whose only
# toolchain is the image its runners boot is not either.
if [[ -n "$gate" && "$container" != "1" ]]; then
  cd "$REPO_ROOT"
  export_tools_cache
  # No snapshot on this path -- the checkout IS the tree under test, so there
  # is nothing to delete under the run. The abort traps still go on, so a
  # killed single-gate run says it was killed and exits UNKNOWN rather than
  # handing its caller a gate's 143 to read as a content failure (#542).
  ci_install_abort_traps
  run_one_gate "$gate"
  exit $?
fi

if [[ "$container" == "1" && -z "$gate" ]]; then
  echo "ci.sh: --container selects how --gate runs; it needs a gate name." >&2
  echo "       The whole suite is already containerised by default." >&2
  exit 2
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
  run_suite_on_snapshot "${RA8_CI_FAST:-$fast}" "${RA8_CI_GATE:-}"
  exit $?
fi

# --- native full-suite mode -----------------------------------------------
if [[ "$native" == "1" ]]; then
  export_tools_cache
  run_suite_on_snapshot "$fast"
  exit $?
fi

# ===========================================================================
# HOST MODE. Build the devcontainer image, then re-enter inside the container.
# ===========================================================================
# The body lives in scripts/ci/lib/container.sh -- runtime selection and the
# `run` command line are transport, not a gate definition, and this file is
# the gate registry. It ends in `exec`, so control does not come back.
# shellcheck source=scripts/ci/lib/container.sh
. "${SCRIPT_DIR}/ci/lib/container.sh"
ci_host_mode_exec "$fast" "$gate" "$rebuild" "$IMAGE_TAG" "$REPO_ROOT"
