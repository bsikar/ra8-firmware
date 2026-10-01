#!/bin/bash -p
# SPDX-License-Identifier: MIT
# Copyright (c) 2026 Brighton Sikarskie
# SHEBANG-SECURITY: -p blocks BASH_ENV and exported-function startup injection.
#
# scripts/ci.sh -- THE single definition of every CI gate in this repository.
#
# ONE SOURCE OF TRUTH
# Every check CI runs is a shell function in this file, listed in the
# RA8_GATE_REGISTRY table below. The GitHub Actions workflows contain no check
# bodies at all: each gate-bearing step is a thin
#
#     run: just quality::local::gate <name>
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
# GitHub Actions workflows are gone (CI moves to tools/ra8ci), so this registry
# is the only list of gates; ra8ci and `just quality::local::gate` both read it.
#
# ADDING A NEW GATE (the whole procedure)
# ---------------------------------------------------------------------------
#   1. Add one row to RA8_GATE_REGISTRY.
#   2. Write the matching `gate_<name>` function (dashes become underscores).
#
# A row with no function fails `--list-gates`, so half the work cannot pass.
#
# ---------------------------------------------------------------------------
# RUNNING IT
# ---------------------------------------------------------------------------
#   /bin/bash -p scripts/ci.sh --gate <name>   # one gate, natively (CI path)
#   /bin/bash -p scripts/ci.sh --gate <name> --container   # toolchain image
#   /bin/bash -p scripts/ci.sh --native        # all gates on a HEAD snapshot
#   /bin/bash -p scripts/ci.sh --fast          # skip the slow gates
#   /bin/bash -p scripts/ci.sh --list-gates    # machine-readable registry dump
#   /bin/bash -p scripts/ci.sh                 # containerised (macOS path)
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
#                re-run. scripts/ci/lib/abort.sh has the whole story.
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

  SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
  REPO_ROOT="$(cd "$SCRIPT_DIR/.." && pwd)"
  # The tag the containerised path boots. Where it comes from, and what stops it
  # going stale, is scripts/ci/devcontainer_image.sh -- the one place that knows
  # how to build it and how to tell a current image from an old one.
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
    "ci-parity|fast|gate registry, runner and gate-body self-tests"
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
    "markdown-references|fast|first-party Markdown links, anchors, and repository paths"
    "cmake-source-paths|fast|every repository-rooted path the CMake files name resolves, and the viewer KEEP/DROP partition"
    "zig-override-members|fast|every overridable Zig default is reachable from the member that overrides it"
    "shebangs|fast|first-party shell scripts carry an env-based shebang"
    "entry-points|fast|hosted vs freestanding main() contract per build domain"
    "tier-imports|fast|the platform never imports apps/; apps/shared_libs never imports a form"
    "bench-lock|fast|every bench-touching script takes the bench lock"
    "annotations|fast|RA8_* annotation attributes (libclang)"
    "enum-underlying-casts|fast|fixed-enum initializer cast safety (libclang)"
    "doc-attachment|fast|a Doxygen block describes the symbol it is attached to"
    "tests-readme|fast|tests/README.md documents every tests/ subdir, none stale"
    "disambig-readmes|fast|disambiguation READMEs: every machine-checked claim still holds"
    "pinout-freshness|fast|committed docs/pinouts/ matches a fresh parse of the datasheets"
    "font-coverage|fast|the committed font cmaps cover every declared codepoint"
    "zig-parallel-trees|fast|no migrated Zig library quietly regrew a C implementation"
    "arch-caps|fast|every gated capability flag is answered by a core and its backends"
    "arch-compiles|fast|arch/arch.h compiles for every core and both capability extremes"
    "measured-counts|fast|every count a MEASURED BLOCK page argues from still matches the tree"
    "lint-py-shell|fast|ruff check + shellcheck"
    "lint-go|fast|go vet + staticcheck over first-party Go"
    "lint-zig|fast|zig fmt over first-party Zig"
    "zig-abi-policy|fast|Zig C ABI inventory, exports, contracts and compatibility"
    "lint-rust|fast|Clippy + rustfmt over first-party Rust"
    "lint-cmake|fast|cmake-lint over every listfile"
    "lint-yaml|fast|yamllint over tracked YAML"
    "lint-just|fast|justfile structure, headers and portable ROOT"
    "lint-ld|fast|linker-script structure, headers and symbol closure"
    "lint-asm|fast|assembly headers, sections and exported-symbol shape"
    "lint-devcontainer|fast|hadolint over the Dockerfile, zsh -n over the zshrc"
    "lint-coverage|fast|every code file is claimed by a linter and a formatter"
    "reserved-addrs|fast|address enums never point into a HUM Reserved window"
    "agnostic-registers|fast|concrete RA8 driver reach-ins may only shrink"
    "cite-check|fast|HUM citation validator (strict)"
    "hum-register-map|fast|register symbols cross-checked against the HUM register tables"
    "roadmap-stats|fast|historical HAL completion record stats"
    "sbom|fast|CycloneDX SBOM freshness"
    "soup-upstream|fast|vendored SOUP matches the upstream blobs recorded for its pin"
    "nsc-cmse|fast|ra8_nsc veneers compile under -mcmse"
    "unit-tests|slow|host unit tests (ctest)"
    "test-go|fast|go test with race detector and coverage floor"
    "test-zig|fast|zig test over first-party Zig"
    "test-rust|fast|cargo test over first-party Rust"
    "ubsan|slow|host unit tests under UBSan"
    "artefact-freshness|slow|committed generated docs match a fresh regenerate"
    "cache-bench|slow|cache/glyph benchmark toolchain"
    "tools-build|slow|first-party host tools compile, link and test on Linux"
    "build-cross|slow|cross-build every example app"
    "build-cross-union|slow|the cross-build shards covered every app exactly once"
    "sg-offsets|slow|NSC SG-veneer slot offsets in the linked secure ELF"
    "stack-usage|slow|aggregate -fstack-usage frames"
    "emulator-smoke|slow|ra8_emulator boot smoke over the example apps"
    "emulator-matrix|slow|every example booted in ra8_emulator, ratcheted downward"
    "emulator-io-fabric|slow|ra8_io fabric demos in ra8_emulator"
    "eil-integration|slow|every HIL app booted in ra8_emulator against its hil.conf"
    "osv-scan|manual|OSV CVE sweep of the vendored SOUP (network, scheduled)"
    "soup-upstream-refresh|manual|re-fetch every SOUP upstream and re-prove the manifests (network)"
    "fuzz-sweep|manual|libFuzzer sweep of every harness (nightly budget)"
    "runner-clock|manual|no CI runner moved its wall clock under a running job"
    "hil-all|manual|hardware-in-the-loop suite on the bench EK-RA8D2"
    "bench-lock-selftest|manual|the bench lock proved against the real bench host"
    "macos-host-build|manual|Zig host build roots build and test natively on arm64 macOS"
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

  # Nested fixture repositories must not inherit hook-exported GIT_* routing.
  # shellcheck source=scripts/dev/git_environment.sh
  . "${SCRIPT_DIR}/dev/git_environment.sh"

  # clang-format is pinned to major 22 project-wide: other majors disagree on
  # edge cases and produce diffs CI rejects. Absence is a hard failure, not a
  # fallback -- a run under clang-format-18 proves nothing about the gate.
  # cpu_count() and ra8_max_jobs() -- the ONE canonical bounded-parallelism
  # source. Gate bodies derive every `-j` / `-P` width from ra8_max_jobs,
  # never a raw nproc, so N gate jobs on one shared box do not each grab all
  # cores. The standalone builders / checks / emulator drivers a gate shells out to
  # source the same file, so there is a single home for the policy.
  # shellcheck source=scripts/ci/lib/parallelism.sh
  . "${SCRIPT_DIR}/ci/lib/parallelism.sh"

  # use_pinned_tool_path() + require_tool_versions() -- deterministic tool
  # resolution. run_one_gate calls use_pinned_tool_path so every gate,
  # however the shell was entered (a login shell, a non-interactive `ssh dev`, a
  # GitHub Actions step), resolves the SAME pinned binaries; require_tool_versions
  # then makes the wrong version fail loudly. One home for the policy, sourced the
  # same way as parallelism.sh.
  # export_tools_cache() lives there too: the persistent pinned-tool cache
  # is part of the same "how a gate reaches its pinned tools"
  # contract, and keeping it beside use_pinned_tool_path holds this file
  # under the 1000-line maintainability cap check_file_size.py enforces.
  # shellcheck source=scripts/ci/lib/tool_env.sh
  . "${SCRIPT_DIR}/ci/lib/tool_env.sh"

  # The abort machinery: a run that was KILLED, or whose snapshot vanished
  # under it, reports UNKNOWN and stops -- it never invents gate failures against
  # a tree that is no longer there. Exit 3, the same "no verdict" code
  # scripts/ci/monitor.sh uses. Read that file's header before changing any of
  # it; the trap shape in particular is load-bearing.
  # shellcheck source=scripts/ci/lib/abort.sh
  . "${SCRIPT_DIR}/ci/lib/abort.sh"

  # use_pinned_lang_toolchains() -- resolve the pinned Zig and Rust toolchains
  # the migrated libraries and the ABI fixtures build against. The deployed ARC
  # runner image predates those .devcontainer/Dockerfile layers, so without this
  # every gate that configures the tree dies on
  # cmake/zig_abi_contract.cmake's "requires zig on PATH" instead of returning a
  # verdict. Sourced like parallelism.sh; the pins come from the Dockerfile so
  # there is no second copy to drift.
  # shellcheck source=scripts/ci/lib/lang_toolchains.sh
  . "${SCRIPT_DIR}/ci/lib/lang_toolchains.sh"

  # use_pinned_arm_toolchain() -- put the pinned Arm GNU Toolchain (cortex-m85
  # aware) on PATH. One home for the policy so ci.sh's cross-build gates and
  # scripts/checks/clang_tidy.sh (its firmware pass, and the pre-commit hook that
  # runs it) resolve the SAME arm-none-eabi-gcc. Sourced like parallelism.sh.
  # shellcheck source=scripts/ci/lib/arm_toolchain.sh
  . "${SCRIPT_DIR}/ci/lib/arm_toolchain.sh"

  # Refuse to run a ra8_emulator gate on an unpinned Unicorn.
  #
  # ra8_emulator boots the real firmware .elf on Unicorn, and different Unicorn
  # versions decode Armv8.1-M (Helium/MVE) differently, so an unpinned emulator
  # makes "same commit, different verdict" structural. This is the
  # fail-loud counterpart to require_cmd: the check binds the ACTUAL libunicorn
  # ra8_emulator will link and exits non-zero -- with remediation -- when it is not
  # the pin, rather than letting a fossil produce an unreproducible green run.
  require_pinned_unicorn() {
    # The pin check is a detector, so prove it still detects before believing it:
    # a version comparison that silently stopped comparing would wave every
    # Unicorn through and hand back exactly the unreproducible green run above.
    bash scripts/checks/check_unicorn_version.sh --selftest
    bash scripts/checks/check_unicorn_version.sh
  }

  # The commit-message gates need TWO different git repositories (the checker
  # scripts from the snapshot, the history from the host repo) and a commit
  # range that survives every GitHub event shape. That family lives in
  # scripts/ci/lib/history.sh -- it is history resolution, not a gate
  # definition, and this file is the gate registry (RA8FW-362).
  # shellcheck source=scripts/ci/lib/history.sh
  . "${SCRIPT_DIR}/ci/lib/history.sh"

  # Run independent steps to completion and print one final verdict table.
  # Gate bodies use this when a later check is still meaningful after an
  # earlier one fails; prerequisites must remain in the same fail-fast group.
  ci_run_all() {
    local -a failures=() labels=() results=()
    local label helper output_file step_status restore_errexit=0
    [[ $- == *e* ]] && restore_errexit=1
    while (($# >= 2)); do
      label="$1"
      shift
      helper="$1"
      shift
      labels+=("$label")
      output_file="$(mktemp)"
      printf '==> CI step: %s\n' "$label"
      set +e
      "$helper" >"$output_file" 2>&1
      step_status=$?
      if ((restore_errexit)); then
        set -e
      fi
      cat "$output_file"
      rm -f "$output_file"
      if ((step_status == 0)); then
        results+=("PASS")
      else
        results+=("FAIL (exit ${step_status})")
        failures+=("${label} (exit ${step_status})")
      fi
    done
    printf '\n== CI step summary ==\n'
    local index=0
    while ((index < ${#labels[@]})); do
      printf '  %-36s %s\n' "${labels[$index]}" "${results[$index]}"
      index=$((index + 1))
    done
    if ((${#failures[@]})); then
      printf 'CI step summary: %d failed, %d passed\n' \
        "${#failures[@]}" "$((${#results[@]} - ${#failures[@]}))" >&2
      printf '  - %s\n' "${failures[@]}" >&2
      return 1
    fi
    printf 'CI step summary: %d passed, 0 failed\n' "${#results[@]}"
    return 0
  }

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

  # Machine-readable gate dump (ra8ci and just/ci_gate.just read it). Also
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
    # The tree under test must still be there. This is the ONE choke point
    # every dispatch passes through -- the --gate CLI path and run_suite via
    # run_gate_capture both land here -- so no gate has to remember to check, and
    # a vanished snapshot is refused with a named reason instead of being
    # discovered by each remaining gate as a content failure.
    ci_require_tree_intact "$name" || return "$RA8_CI_EXIT_ABORTED"
    # Deterministic tool resolution BEFORE any gate body runs: normalise
    # PATH so a non-login shell resolves the same pinned binaries a login shell
    # does. This is the single choke point every gate passes through -- the
    # --gate CLI path and run_suite (via run_gate_capture) both land here -- so no
    # gate has to remember to do it.
    if ! use_pinned_tool_path; then
      echo "ci.sh: refusing to run '$name' without the declared tool environment." >&2
      return 1
    fi
    # Same choke point for the language toolchains the Zig migration made
    # load-bearing. Non-fatal by design: a gate that needs zig or cargo still
    # fails through its own require_cmd diagnostic when provisioning could not
    # reach the official download, rather than dying here with a curl error.
    use_pinned_lang_toolchains
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
  # to be honest on its own. suite_registry_selftest asserts it every run.
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
  # executed nothing -- the shape every gate-honesty defect takes.
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
    # An abort outranks everything below it. The rows above it were real
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
      # The full suite shares one disposable snapshot across every gate. Retire
      # completed build trees at their last-use boundary so later instrumented
      # builds cannot exhaust the runner merely because earlier outputs remain
      # resident. The lifecycle helper is snapshot-only; single-gate CI jobs and
      # developers' in-place incremental builds are untouched.
      if ! suite_reclaim_completed_builds "$name"; then
        echo "ci.sh: failed to reclaim completed build trees before '$name'." >&2
        return 1
      fi
      echo ""
      echo "==================================================================="
      echo "== GATE: $name"
      echo "==================================================================="
      gate_names+=("$name")
      # Dispatch via run_gate_capture -- see the ERREXIT warning on it. Never
      # inline this as `if run_one_gate "$name"; then`.
      run_gate_capture "$name"
      # An abort is not a verdict. A signalled run never reaches here at
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
usage: /bin/bash -p scripts/ci.sh [--fast] [--native] [--rebuild]
       /bin/bash -p scripts/ci.sh --gate <name> [--container]
       /bin/bash -p scripts/ci.sh --list-gates

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
                 verdict. Modes: hang | destroy | fail. Its output is a
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
    gate_rc=0
    cd "$REPO_ROOT"
    export_tools_cache
    # No snapshot on this path -- the checkout IS the tree under test, so there
    # is nothing to delete under the run. The abort traps still go on, so a
    # killed single-gate run says it was killed and exits UNKNOWN rather than
    # handing its caller a gate's 143 to read as a content failure.
    ci_install_abort_traps
    run_gate_capture "$gate"
    gate_rc="$RA8_GATE_RC"
    exit "$gate_rc"
  fi

  if [[ "$container" == "1" && -z "$gate" ]]; then
    echo "ci.sh: --container selects how --gate runs; it needs a gate name." >&2
    echo "       The whole suite is already containerised by default." >&2
    exit 2
  fi

  # --- in-container re-entry ------------------------------------------------
  if [[ "${RA8_CI_INNER:-0}" == "1" ]]; then
    export HOME=/home/ra8-ci
    git config --global --add safe.directory "$REPO_ROOT"
    # When the host tree is a linked worktree its git objects live in the main
    # repo's git dir, bind-mounted separately at its host path (see the
    # RA8_CI_GIT_COMMON_DIR block below). Git checks ownership of THAT directory
    # too, and the container runs as root against files owned by the host user,
    # so it needs its own exemption or discovery fails before `git archive`.
    if [[ -n "${RA8_CI_GIT_COMMON_DIR:-}" ]]; then
      git config --global --add safe.directory "$RA8_CI_GIT_COMMON_DIR"
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
else
  [[ "$-" == *p* ]]
fi
