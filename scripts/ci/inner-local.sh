#!/usr/bin/env bash
# SPDX-License-Identifier: MIT
# Copyright (c) 2026 Brighton Sikarskie
#
# scripts/ci.sh -- reproduce the GitHub Actions CI gates locally, inside the
# project devcontainer, BEFORE pushing.
#
# Why this exists:
#   CI (.github/workflows/firmware.yml) runs on self-hosted Linux runners and
#   repeatedly diverges from a macOS developer run:
#     * The format gate pins clang-format-22; Homebrew (macOS) and Ubuntu ship
#       different majors that disagree on edge cases.
#     * The host unit tests install RAM with mmap(MAP_FIXED, 0x40000000, ...)
#       (tests/mocks/ra8_sim_mmap.c). macOS arm64 refuses MAP_FIXED below 4 GiB,
#       so every test SIGKILLs before main() on the Mac.
#   Running the gates inside the Ubuntu 24.04 devcontainer reproduces the runner
#   environment, so a red gate is caught here instead of in CI.
#
# Usage (host):
#   bash scripts/ci.sh            # full gate suite (mirrors firmware.yml)
#   bash scripts/ci.sh --fast     # skip the slow clang-tidy + coverage gates
#   bash scripts/ci.sh --rebuild  # force a devcontainer image rebuild first
#
# The script re-enters itself inside the container with RA8_CI_INNER=1, where it
# extracts a clean `git archive HEAD` (exactly what CI checks out) into a
# throwaway dir and runs the gates there. The host repo is bind-mounted
# read-only, so the host source tree and its macOS CMake caches are never
# touched and stray in-source build artifacts never pollute the gates.

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "$SCRIPT_DIR/../.." && pwd)"
IMAGE_TAG="ra8-ci:latest"
DOCKERFILE="$REPO_ROOT/.devcontainer/Dockerfile"

# ===========================================================================
# IN-CONTAINER MODE. Entered via `docker run ... -e RA8_CI_INNER=1`. Runs every
# gate, records PASS/FAIL, prints a summary, and exits non-zero if any failed.
# ===========================================================================
if [[ "${RA8_CI_INNER:-0}" == "1" ]]; then
  fast="${RA8_CI_FAST:-0}"

  # Run the gates against a CLEAN snapshot of committed HEAD -- exactly what CI
  # checks out -- NOT the bind-mounted working tree. The host tree carries
  # gitignored in-source build dirs (src/app/build-sim/, examples/*/*/build/,
  # tools/*/build/, ...) whose CMake-generated junk (CMakeCCompilerId.c, ...)
  # would otherwise make clang-format / cppcheck / check_magic_numbers report
  # failures CI never sees. Extracting `git archive HEAD` into a throwaway dir
  # gives the gates the same clean tree the runner gets, and the host repo stays
  # read-only. Uncommitted changes are intentionally excluded -- they are not
  # what `git push` ships -- so a dirty tree gets a heads-up below.
  export HOME=/tmp
  git config --global --add safe.directory "$REPO_ROOT" >/dev/null 2>&1 || true
  if [[ -n "$(git -C "$REPO_ROOT" status --porcelain 2>/dev/null)" ]]; then
    echo "NOTE: working tree is dirty -- make ci tests committed HEAD only" >&2
    echo "      (like CI). Commit your changes to have them gated." >&2
  fi
  work="/tmp/citree"
  rm -rf "$work"
  mkdir -p "$work"
  # git-lfs is installed in the image so `git archive` does not choke trying to
  # spawn the filter for the content/library/*.epub LFS pointers;
  # GIT_LFS_SKIP_SMUDGE=1 makes it emit those pointer files as-is (no gate reads
  # them, and no LFS object or network fetch is needed). Streaming from the
  # packed .git objects into the container-local /citree is far faster than
  # copying the working tree file-by-file over the (virtiofs) bind mount.
  GIT_LFS_SKIP_SMUDGE=1 git -C "$REPO_ROOT" archive HEAD | tar -x -C "$work"
  cd "$work"

  # Pin clang-format to the CI version; fall back with a loud warning so the
  # gate still RUNS (just possibly disagreeing with CI on edge cases).
  pick_clang_format() {
    local candidate
    for candidate in clang-format-22 clang-format-21 clang-format-20 \
      clang-format-19 clang-format-18 clang-format; do
      if command -v "$candidate" >/dev/null 2>&1; then
        printf '%s\n' "$candidate"
        return 0
      fi
    done
    return 1
  }
  # errexit off around the CALL, re-armed INSIDE the substitution -- see the
  # note in agent_workspace.sh. An empty $cf is already handled below, so a
  # mid-body failure lands on the explicit error path instead of silently
  # selecting whatever the last candidate happened to echo.
  set +e
  cf="$(
    set -e
    pick_clang_format
  )"
  set -e
  if [[ -z "$cf" ]]; then
    echo "ERROR: no clang-format binary found in the container." >&2
    exit 1
  fi
  if [[ "$cf" != "clang-format-22" ]]; then
    echo "WARNING: clang-format-22 (the CI pin) is absent; using '$cf'." >&2
    echo "         Format results may differ from CI. Add clang-format-22 to" >&2
    echo "         .devcontainer/Dockerfile to make this gate faithful." >&2
  fi

  gate_names=()
  gate_results=()

  run_gate() {
    local name="$1"
    shift
    echo ""
    echo "==================================================================="
    echo "== GATE: $name"
    echo "==================================================================="
    if "$@"; then
      gate_names+=("$name")
      gate_results+=("PASS")
    else
      gate_names+=("$name")
      gate_results+=("FAIL")
    fi
  }

  # --- gate: clang-format (firmware.yml job: format) -----------------------
  # shellcheck disable=SC2329  # invoked indirectly as run_gate's "$@" below.
  gate_clang_format() {
    CLANG_FORMAT="$cf" bash scripts/checks/format_code.sh --check --verbose
  }

  # --- gate: cppcheck (firmware.yml job: cppcheck) -------------------------
  # Mirrors the workflow step exactly: convert each .cppcheck-suppressions line
  # into an explicit --suppress= flag (cppcheck 2.13 on Ubuntu 24.04 is finicky
  # about --suppressions-list), skip examples/host/* (macOS-only dev tools), and
  # run cppcheck over src libs <example app dirs> with --error-exitcode=1.
  # shellcheck disable=SC2329  # invoked indirectly as run_gate's "$@" below.
  gate_cppcheck() (
    set -e
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

  # --- gate: pre-commit check_*.py suite (job: pre-commit-checks) -----------
  # The exact "Run all check_*.py scripts" block from firmware.yml.
  # shellcheck disable=SC2329  # invoked indirectly as run_gate's "$@" below.
  gate_precommit_checks() (
    set -e
    python3 scripts/checks/check_obsolete_standards.py
    python3 scripts/checks/check_world_tags.py --strict
    python3 scripts/checks/check_mcdc_block.py
    python3 scripts/checks/check_no_dynamic_alloc.py --all
    python3 scripts/checks/check_no_ai_attribution.py
    python3 scripts/checks/check_no_null.py --all
    python3 scripts/checks/check_function_size.py
    python3 scripts/checks/check_file_size.py
    python3 scripts/checks/check_header_file_placement.py
    python3 scripts/checks/check_example_board_pins.py
    python3 scripts/checks/check_core_layering.py
    python3 scripts/checks/check_final_newline.py
    python3 scripts/checks/check_magic_numbers.py
    python3 scripts/checks/check_no_gnu_attribute.py
  )

  # --- gate: clang-tidy (firmware.yml job: tidy) ---------------------------
  # shellcheck disable=SC2329  # invoked indirectly as run_gate's "$@" below.
  gate_clang_tidy() {
    bash scripts/checks/clang_tidy.sh --check
  }

  # --- gate: host unit tests (firmware.yml job: unit-tests) ----------------
  # shellcheck disable=SC2329  # invoked indirectly as run_gate's "$@" below.
  gate_host_tests() (
    set -e
    bash tests/build_tests.sh
    bash tests/run_tests.sh
  )

  # --- gate: coverage gate (firmware.yml job: coverage) --------------------
  # shellcheck disable=SC2329  # invoked indirectly as run_gate's "$@" below.
  gate_coverage() {
    bash scripts/checks/coverage.sh --gate
  }

  # --- gate: MISRA-C 2012 ratchet (firmware.yml job: misra) ----------------
  # The devcontainer's cppcheck matches the runner (Ubuntu 24.04 / 2.13), so
  # the committed .github/misra-baseline.txt is directly comparable here.
  # shellcheck disable=SC2329  # invoked indirectly as run_gate's "$@" below.
  gate_misra() (
    set -e
    bash scripts/checks/misra_check_inner.sh
    python3 scripts/checks/misra_ratchet.py --check
  )

  run_gate "clang-format" gate_clang_format
  run_gate "cppcheck" gate_cppcheck
  run_gate "pre-commit-checks" gate_precommit_checks
  if [[ "$fast" != "1" ]]; then
    run_gate "clang-tidy" gate_clang_tidy
  fi
  run_gate "host-tests" gate_host_tests
  if [[ "$fast" != "1" ]]; then
    run_gate "coverage" gate_coverage
    run_gate "misra-ratchet" gate_misra
  fi

  echo ""
  echo "==================================================================="
  echo "== make ci summary$([[ "$fast" == "1" ]] && echo "  (--fast: clang-tidy + coverage skipped)")"
  echo "==================================================================="
  failed=0
  idx=0
  while [[ "$idx" -lt "${#gate_names[@]}" ]]; do
    printf '  %-20s %s\n' "${gate_names[$idx]}" "${gate_results[$idx]}"
    [[ "${gate_results[$idx]}" == "FAIL" ]] && failed=1
    idx=$((idx + 1))
  done
  echo "-------------------------------------------------------------------"
  if [[ "$failed" -ne 0 ]]; then
    echo "  RESULT: FAIL"
    exit 1
  fi
  echo "  RESULT: PASS"
  exit 0
fi

# ===========================================================================
# HOST MODE. Build the devcontainer image, then re-enter inside the container.
# ===========================================================================
fast=0
rebuild=0
usage() {
  echo "usage: bash scripts/ci.sh [--fast] [--rebuild]"
  echo "  --fast     skip the slow clang-tidy + coverage gates"
  echo "  --rebuild  force a devcontainer image rebuild first"
}
for arg in "$@"; do
  case "$arg" in
    --fast) fast=1 ;;
    --rebuild) rebuild=1 ;;
    -h | --help)
      usage
      exit 0
      ;;
    *)
      echo "ci.sh: unknown flag '$arg'" >&2
      usage >&2
      exit 2
      ;;
  esac
done

if ! command -v docker >/dev/null 2>&1; then
  echo "error: docker not on PATH. Install: brew install colima docker" >&2
  exit 1
fi

# On macOS the project uses colima (no Docker Desktop license). Auto-start it,
# matching scripts/ci/test-docker.sh.
if [[ "$(uname -s)" == "Darwin" ]]; then
  if ! command -v colima >/dev/null 2>&1; then
    echo "error: colima not on PATH. Install: brew install colima" >&2
    exit 1
  fi
  if ! colima status >/dev/null 2>&1; then
    echo "==> starting colima VM (4 CPU, 6 GiB)"
    colima start --cpu 4 --memory 6
  fi
fi

if ! docker info >/dev/null 2>&1; then
  echo "error: docker daemon not reachable (try: colima start)" >&2
  exit 1
fi

# Build the devcontainer image. The Dockerfile has no COPY/ADD, so the tiny
# .devcontainer/ directory is a sufficient build context -- do NOT ship the
# multi-GB repo (datasheets, build trees) to the daemon as context.
if [[ "$rebuild" == "1" ]] || ! docker image inspect "$IMAGE_TAG" >/dev/null 2>&1; then
  echo "==> building $IMAGE_TAG from .devcontainer/Dockerfile"
  docker build -t "$IMAGE_TAG" -f "$DOCKERFILE" "$REPO_ROOT/.devcontainer"
else
  echo "==> reusing cached image $IMAGE_TAG (--rebuild / REBUILD=1 to refresh)"
fi

echo "==> running CI gates in container (fast=$fast)"
# The host repo is bind-mounted READ-ONLY: the in-container step extracts a
# clean `git archive HEAD` into a throwaway dir and builds there, so the host
# source tree and its macOS CMake caches are never touched. Run as root so that
# throwaway tree (and its fresh build dirs) is writable.
exec docker run --rm \
  -u 0:0 \
  -e RA8_CI_INNER=1 \
  -e RA8_CI_FAST="$fast" \
  -e HOME=/tmp \
  -e CMAKE_BUILD_PARALLEL_LEVEL=4 \
  -v "$REPO_ROOT":/workspace:ro \
  -w /workspace \
  "$IMAGE_TAG" \
  bash scripts/ci.sh
