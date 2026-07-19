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
# Keep this suite a SUPERSET-or-equal of firmware.yml, never a subset. Two jobs
# were missing here for exactly that reason and let a red push through: the
# annotation gate (a separate step of the pre-commit-gate job, not part of its
# check_*.py block) and the MISRA ratchet (its own job -- `make cppcheck` runs
# a different rule set with no baseline and does not cover it). When adding a
# job to firmware.yml, add the matching gate below in the same change.
#
# Usage (host):
#   bash scripts/ci.sh            # full gate suite (mirrors firmware.yml)
#   bash scripts/ci.sh --fast     # skip the slow misra/clang-tidy/coverage/ubsan gates
#   bash scripts/ci.sh --rebuild  # force a devcontainer image rebuild first
#
# The script re-enters itself inside the container with RA8_CI_INNER=1, where it
# extracts a clean `git archive HEAD` (exactly what CI checks out) into a
# throwaway dir and runs the gates there. The host repo is bind-mounted
# read-only, so the host source tree and its macOS CMake caches are never
# touched and stray in-source build artifacts never pollute the gates.

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "$SCRIPT_DIR/.." && pwd)"
IMAGE_TAG="ra8-ci:latest"
DOCKERFILE="$REPO_ROOT/.devcontainer/Dockerfile"

# ===========================================================================
# IN-CONTAINER MODE. Entered via `<runtime> run ... -e RA8_CI_INNER=1`. Runs
# every gate, records PASS/FAIL, prints a summary, and exits non-zero if any
# failed.
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
  # A worktree's objects live in the main repo's git dir, which is a separate
  # mount and therefore needs its own ownership exemption; without it git
  # refuses with "detected dubious ownership" before archive ever runs.
  git config --global --add safe.directory '*' >/dev/null 2>&1 || true
  if [[ -n "$(git -C "$REPO_ROOT" status --porcelain 2>/dev/null)" ]]; then
    echo "NOTE: working tree is dirty -- make ci tests committed HEAD only" >&2
    echo "      (like CI). Commit your changes to have them gated." >&2
  fi
  # The snapshot dir is created fresh per run and destroyed on exit, so no
  # build output can outlive the run that produced it. That is what makes the
  # stale-.gcda class impossible rather than merely remembered: a coverage run
  # can never find a *different branch's* .gcda here, because "here" did not
  # exist a moment ago. Do NOT replace this with a fixed path -- a fixed path
  # is a cache, and a cache is exactly the bug.
  #
  # It must also be a path the invoking user can write. The former hardcoded
  # /citree assumed a root-owned container filesystem and failed with
  # "Permission denied" under rootless podman and under any direct
  # RA8_CI_INNER=1 run, which is what drove people to hand-edit this script.
  work="$(mktemp -d "${TMPDIR:-/tmp}/ra8-ci-snapshot.XXXXXXXX")"
  trap 'rm -rf "$work"' EXIT INT TERM
  # git-lfs is installed in the image so `git archive` does not choke trying to
  # spawn the filter for the content/library/*.epub LFS pointers;
  # GIT_LFS_SKIP_SMUDGE=1 makes it emit those pointer files as-is (no gate reads
  # them, and no LFS object or network fetch is needed). Streaming from the
  # packed .git objects into the container-local /citree is far faster than
  # copying the working tree file-by-file over the (virtiofs) bind mount.
  GIT_LFS_SKIP_SMUDGE=1 git -C "$REPO_ROOT" archive HEAD | tar -x -C "$work"
  cd "$work"

  # Make the snapshot a real git repo with everything staged.
  #
  # Several checks in the pre-commit suite (check_obsolete_standards.py,
  # check_mcdc_block.py) are written for hook use and enumerate their inputs
  # with `git diff --cached --name-only`. An extracted archive is not a git
  # repo, so that call failed with exit 129 and the scripts died on an uncaught
  # CalledProcessError -- while the gate still reported PASS, because the
  # `set -e` subshell masks it. The result was two checks silently NOT running
  # under `make ci` that DO run in CI, where actions/checkout provides a real
  # index. That is precisely the local-vs-CI divergence this script exists to
  # prevent, so fix the snapshot rather than the checkers.
  #
  # Staging everything is the semantically correct answer here: in a full-tree
  # gate run, the set of files "being committed" is the whole tree.
  git init -q . 2>/dev/null || true
  git add -A 2>/dev/null || true

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
  cf="$(pick_clang_format || true)"
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
  gate_clang_format() {
    CLANG_FORMAT="$cf" bash scripts/format_code.sh --check --verbose
  }

  # --- gate: cppcheck (firmware.yml job: cppcheck) -------------------------
  # Mirrors the workflow step exactly: convert each .cppcheck-suppressions line
  # into an explicit --suppress= flag (cppcheck 2.13 on Ubuntu 24.04 is finicky
  # about --suppressions-list), skip examples/host/* (macOS-only dev tools), and
  # run cppcheck over src libs <example app dirs> with --error-exitcode=1.
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
  gate_precommit_checks() (
    set -e
    python3 scripts/utils/check_obsolete_standards.py
    python3 scripts/utils/check_world_tags.py --strict
    python3 scripts/utils/check_mcdc_block.py
    python3 scripts/utils/check_no_dynamic_alloc.py --all
    python3 scripts/utils/check_no_ai_attribution.py
    python3 scripts/utils/check_no_null.py --all
    python3 scripts/utils/check_function_size.py
    python3 scripts/utils/check_file_size.py
    python3 scripts/utils/check_final_newline.py
    python3 scripts/utils/check_magic_numbers.py
    python3 scripts/utils/check_no_gnu_attribute.py
    python3 scripts/utils/check_tz_boundary_discard.py
    # The block below was missing here while firmware.yml ran every one of
    # them, so this suite was a SUBSET of CI -- the exact condition the file
    # header forbids. Fifteen gates could go red on the runner after a local
    # `make ci` reported PASS. check_ruff.py in particular lints these very
    # scripts, so a change to the gates themselves was the least-covered
    # thing in the tree.
    python3 scripts/utils/check_assert_casts.py tests/*.c
    python3 scripts/utils/check_core_layering.py
    python3 scripts/utils/check_example_board_pins.py
    python3 scripts/utils/check_header_file_placement.py
    python3 scripts/utils/check_hil_alive_policy.py
    python3 scripts/utils/check_inclusive_terminology.py
    python3 scripts/utils/check_line_citations.py
    python3 scripts/utils/check_new_compound_has_mcdc.py
    python3 scripts/utils/check_no_driver_asm_guard.py
    python3 scripts/utils/check_no_wave_references.py
    python3 scripts/utils/check_nsc_veneer_defs.py
    python3 scripts/utils/check_stub_crypto_guarded.py
    # --require makes a missing linter fatal rather than a silent skip; the
    # container ships both, and a skipped linter is indistinguishable from a
    # clean one in the log.
    python3 scripts/utils/check_ruff.py --require
    python3 scripts/utils/check_shell.py --require
    # check_hil_sil_parity.py: SIM==HIL -- every hw_validated/hil app must also
    # be exercised in board_sim (sil_all.sh). Fails if a hil/ app has no
    # hil.conf, is not in sil_all.sh's run set, or declares a HIL_MODE board_sim
    # cannot check. Re-derives both harnesses' discovery dynamically so an added
    # HIL app cannot silently escape SIM coverage.
    python3 scripts/utils/check_hil_sil_parity.py
  )

  # --- gate: TrustZone NSC veneers under -mcmse (job: nsc-cmse) ------------
  # firmware.yml compiles every libs/ra8_nsc TU with -mcmse -Wall -Wextra
  # -Werror as its own step. The warning flags are load-bearing: a bare
  # -fsyntax-only run is what let a redefined RA8_NSC_VENEER silently drop
  # the cmse_nonsecure_entry attribute, producing a broken secure gateway
  # that compiled clean.
  gate_nsc_cmse() {
    bash scripts/utils/check_nsc_cmse.sh
  }

  # --- gate: annotation attributes (job: pre-commit-checks) ----------------
  # firmware.yml runs check_annotations.py as its own step inside the
  # pre-commit-gate job, NOT as part of the check_*.py block above. It was
  # therefore absent from this suite entirely: a full local `make ci` passed
  # while CI failed the "Annotation-attribute gate (libclang)" step. The
  # import probe is load-bearing -- without it a container missing the
  # binding makes the strict gate exit 0 and report nothing, which is worse
  # than not running it at all.
  gate_annotations() (
    set -e
    python3 -c "import clang.cindex" || {
      echo "ERROR: the libclang Python binding is missing, so the annotation" >&2
      echo "       gate cannot run. CI installs libclang==18.1.1; add it to" >&2
      echo "       .devcontainer/Dockerfile so this gate is faithful." >&2
      exit 1
    }
    # Regression test for the checker itself before trusting its verdict.
    python3 scripts/utils/check_annotations.py --selftest
    python3 scripts/utils/check_annotations.py --check
  )

  # --- gate: MISRA-C 2012 ratchet (firmware.yml job: misra) ----------------
  # Audit + ratchet against .github/misra-baseline.txt. `make cppcheck` is
  # NOT a substitute: it runs a different rule set (style/performance, no
  # misra.py addon) and has no baseline comparison, so a new MISRA finding
  # sails through it. Kept out of --fast: the audit dumps every TU under
  # libs/ src/ port/ and takes minutes.
  gate_misra() (
    set -e
    bash scripts/utils/misra_check.sh
    python3 scripts/utils/misra_ratchet.py --check
  )

  # --- gate: clang-tidy (firmware.yml job: tidy) ---------------------------
  gate_clang_tidy() {
    bash scripts/clang_tidy.sh --check
  }

  # --- gate: host unit tests (firmware.yml job: unit-tests) ----------------
  gate_host_tests() (
    set -e
    bash tests/build_tests.sh
    bash tests/run_tests.sh
  )

  # --- gate: coverage gate (firmware.yml job: coverage) --------------------
  gate_coverage() {
    bash scripts/coverage.sh --gate
  }

  # --- gate: UBSan host tests (firmware.yml job: ubsan) --------------------
  gate_ubsan() {
    make ubsan
  }

  run_gate "clang-format" gate_clang_format
  run_gate "cppcheck" gate_cppcheck
  run_gate "pre-commit-checks" gate_precommit_checks
  run_gate "nsc-cmse" gate_nsc_cmse
  run_gate "annotations" gate_annotations
  if [[ "$fast" != "1" ]]; then
    run_gate "misra" gate_misra
    run_gate "clang-tidy" gate_clang_tidy
  fi
  run_gate "host-tests" gate_host_tests
  if [[ "$fast" != "1" ]]; then
    run_gate "coverage" gate_coverage
    run_gate "ubsan" gate_ubsan
  fi

  echo ""
  echo "==================================================================="
  echo "== make ci summary$([[ "$fast" == "1" ]] && echo "  (--fast: clang-tidy + coverage + ubsan skipped)")"
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
  echo "  --fast     skip the slow clang-tidy + coverage + ubsan gates"
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

# Container runtime. podman is preferred (daemonless, and rootless where the
# kernel allows it, so a gate run cannot outlive the user's session); docker via
# colima remains the macOS path.
#
# RA8_CONTAINER_RUNTIME may carry arguments, e.g. "sudo podman". That is not
# hypothetical: on a verification box that is itself an unprivileged LXC
# container, rootless podman runs containers fine but cannot BUILD the
# devcontainer -- apt drops privileges to the _apt user and its setgroups(2)
# call is denied inside the nested user namespace ("Failed to setgroups -
# setgroups (22: Invalid argument)"). Running podman as root inside that LXC
# sidesteps the nested namespace, and root there is still unprivileged on the
# Proxmox host, so the security boundary is unchanged.
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
  echo "error: no container runtime on PATH (looked for podman, docker, nerdctl)." >&2
  echo "  Debian/Ubuntu: sudo apt-get install -y podman uidmap" >&2
  echo "  macOS:         brew install colima docker && colima start" >&2
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

echo "==> running CI gates in container (runtime=${RUNTIME_CMD[*]} fast=$fast)"
# The host repo is bind-mounted READ-ONLY: the in-container step extracts a
# clean `git archive HEAD` into a throwaway dir and builds there, so the host
# source tree and its macOS CMake caches are never touched. Run as root so that
# throwaway tree (and its fresh build dirs) is writable.
# The compiler cache is the ONE thing deliberately allowed to outlive the
# container. It is content-addressed, so unlike a build directory it cannot
# carry stale state into a later run: a changed input is simply a different
# key. Mounting it read-write is what lets a second `make ci` -- by any agent,
# from any workspace -- hit objects the first one compiled.
#
# CCACHE_BASEDIR + CCACHE_NOHASHDIR are required for that to work at all here:
# each run builds in a fresh mktemp snapshot, and without path normalisation
# every compilation would hash differently and always miss.
# Git worktree support. In a worktree, $REPO_ROOT/.git is a FILE holding
# "gitdir: /path/to/main/.git/worktrees/<name>" -- an absolute path OUTSIDE the
# workspace. Bind-mounting only the workspace therefore gives the container a
# dangling gitdir pointer and `git archive HEAD` dies with
# "fatal: not a git repository". Mounting the main repo's common git dir at the
# SAME absolute path makes the pointer resolve.
#
# This matters because per-agent worktrees are the isolation model on the shared
# verification box (scripts/agent_workspace.sh): without this, `make ci` would
# work only from a full clone, which is precisely the duplication that filled
# the box's disk.
worktree_args=()
if [[ -f "$REPO_ROOT/.git" ]]; then
  git_common="$(git -C "$REPO_ROOT" rev-parse --path-format=absolute --git-common-dir 2>/dev/null || true)"
  if [[ -n "$git_common" && -d "$git_common" ]]; then
    worktree_args=(-v "$git_common":"$git_common":ro)
    echo "==> git worktree detected; also mounting $git_common"
  else
    echo "warning: $REPO_ROOT/.git is a file but its common git dir was not found;" >&2
    echo "         the in-container git archive will probably fail." >&2
  fi
fi

ccache_args=()
CCACHE_HOST_DIR="${RA8_CCACHE_DIR:-/var/cache/ccache-ra8}"
if [[ -d "$CCACHE_HOST_DIR" && -w "$CCACHE_HOST_DIR" ]]; then
  ccache_args=(
    -v "$CCACHE_HOST_DIR":/ccache
    -e CCACHE_DIR=/ccache
    -e CCACHE_BASEDIR=/
    -e CCACHE_NOHASHDIR=1
    -e "CCACHE_SLOPPINESS=include_file_mtime,include_file_ctime,locale,time_macros"
    -e CCACHE_MAXSIZE="${RA8_CCACHE_MAXSIZE:-20G}"
  )
  echo "==> compiler cache: $CCACHE_HOST_DIR -> /ccache"
else
  echo "==> compiler cache: none ($CCACHE_HOST_DIR absent or not writable)"
fi

# --rm is load-bearing, not hygiene: the snapshot tree and every build dir the
# gates create live INSIDE the container, so the container exiting is what
# reclaims them. Nothing a gate writes can survive to poison the next run, which
# is why no "rm -rf tests/build build/tidy first" step is needed here.
#
# CMAKE_BUILD_PARALLEL_LEVEL defaults to 4 (the CI runner's value, so `make ci`
# reproduces its timing) but is overridable for a beefier verification box.
exec "${RUNTIME_CMD[@]}" run --rm \
  -u 0:0 \
  -e RA8_CI_INNER=1 \
  -e RA8_CI_FAST="$fast" \
  -e HOME=/tmp \
  -e CMAKE_BUILD_PARALLEL_LEVEL="${CMAKE_BUILD_PARALLEL_LEVEL:-4}" \
  "${ccache_args[@]}" \
  "${worktree_args[@]}" \
  -v "$REPO_ROOT":"$REPO_ROOT":ro \
  -w "$REPO_ROOT" \
  "$IMAGE_TAG" \
  bash scripts/ci.sh
