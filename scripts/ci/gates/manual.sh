# shellcheck shell=bash
# SPDX-License-Identifier: MIT
# Copyright (c) 2026 Brighton Sikarskie
#
# scripts/ci/gates/manual.sh -- Manual / scheduled gates -- network, nightly budget, or bench hardware.
#
# SOURCED, NEVER EXECUTED. scripts/ci.sh sources every file in this directory
# and is the only entry point; RA8_GATE_REGISTRY -- the single list of what
# gates exist -- stays there too. These files hold gate BODIES only, so there
# is still exactly one home for a gate's definition and exactly one command
# for a workflow to call (bash scripts/ci.sh --gate <name>). Adding a second
# registry here would recreate the drift the single-definition rule exists to
# prevent.
#
# Gates in this file: mcdc-delta-base, osv-scan, fuzz-sweep, runner-clock,
#                     hil-all, docs-publish

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
      bash scripts/report/mcdc_report.sh --in-container
  ) || echo "base-branch MC/DC build failed -- the delta will be PR-only"
  if [[ -f "$tree/build/mcdc-report/summary.txt" ]]; then
    cp "$tree/build/mcdc-report/summary.txt" base-summary.txt
  else
    : >base-summary.txt
  fi
  git worktree remove --force "$tree" || rm -rf "$tree"
)

# --- osv-scan (manual) ----------------------------------------------------
# Two legs (scripts/checks/osv_scan.sh): the SBOM purl leg, and the commit leg
# that resolves GIT-range advisories for the git-vendored C/C++ SOUP. Exits 1
# on any finding. Downloads a version-pinned, sha256-verified scanner, so it
# needs the network and is scheduled weekly rather than run per push.
gate_osv_scan() (
  set -e
  require_cmd curl
  local version="${OSV_SCANNER_VERSION:?set OSV_SCANNER_VERSION}"
  local sha256="${OSV_SCANNER_SHA256:?set OSV_SCANNER_SHA256}"
  # The scan is only as good as the SBOM it reads; refuse to scan a stale one.
  python3 scripts/gen/gen_sbom.py --check
  curl -fsSL -o osv-scanner \
    "https://github.com/google/osv-scanner/releases/download/v${version}/osv-scanner_linux_amd64"
  echo "${sha256}  osv-scanner" | sha256sum -c -
  chmod +x osv-scanner
  bash scripts/checks/osv_scan.sh --scanner ./osv-scanner --output-dir osv-report
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
  # Prove the budget check before spending the budget it guards. libFuzzer
  # enforces -max_total_time off the steppable wall clock, so on a host that
  # steps its clock a sweep stops after seconds and still exits 0 (#509);
  # run_fuzz.sh times each harness on CLOCK_MONOTONIC and refuses to call that
  # a pass. The selftest drives every branch of that rule with synthetic
  # inputs, so a rule that quietly stopped matching cannot pass as clean.
  bash scripts/checks/run_fuzz.sh --selftest
  echo "Registered harnesses:"
  bash scripts/checks/run_fuzz.sh --list
  # Project the wall time BEFORE spending it. The sweep is serial -- every
  # harness gets the full budget in turn -- so its duration is (registry size)
  # x budget, and adding a harness silently lengthens the nightly job. That is
  # exactly how the workflow's timeout eroded from 110 min of slack to 39
  # without anyone noticing: the comment still said "13 harnesses" while the
  # registry had grown to 20. Printing the projection at startup puts the
  # number in the log every run, so the next growth is visible immediately
  # instead of arriving as a truncated sweep at 3am.
  local n_targets
  n_targets="$(bash scripts/checks/run_fuzz.sh --list | grep -c .)"
  echo "projected sweep: ${n_targets} harnesses x ${budget}s = ~$((n_targets * budget / 60)) min of fuzzing, plus the clang build"
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
  bash scripts/checks/run_fuzz.sh --all "$budget" 2>&1 | tee fuzz-nightly.log
)

# --- runner-clock (manual) ------------------------------------------------
# Reads step timestamps back out of the Actions API and fails if any runner
# recorded a step that finished before it started, or started before the
# previous one finished. Neither is possible on a clock that does not step,
# and a runner that steps its clock corrupts every gate whose contract is a
# duration -- the fuzz budget, timeout-minutes, any benchmark (#509).
#
# Scheduled rather than per-push: it is a statement about the FLEET, not about
# the commit, and it costs one API call per run scanned. In CI that spends the
# workflow's own GITHUB_TOKEN budget, which is per-repository and separate from
# the shared user quota `make ci-status` exists to protect.
gate_runner_clock() (
  set -e
  # No require_cmd gh: the checker speaks the API over urllib, because the
  # ra8-ci runner image does not ship the GitHub CLI and a gate that needed it
  # would fail nightly with a provisioning error rather than a verdict. It
  # takes GH_TOKEN / GITHUB_TOKEN, falling back to an authenticated gh on a
  # developer box, and exits 2 -- not 0 -- when it has neither.
  #
  # Prove the detector before trusting its verdict: a clean scan from a
  # detector that stopped detecting is indistinguishable from a healthy fleet.
  python3 scripts/checks/check_runner_clock.py --selftest
  python3 scripts/checks/check_runner_clock.py --runs "${RA8_CLOCK_SCAN_RUNS:-60}"
)

# --- runner-image-deps (manual) -------------------------------------------
# Every tool a gate DECLARES with require_cmd / require_python_mod has to exist
# in the image the gates run in (#513).
#
# manual, not fast, and that is the honest classification rather than a way of
# opting out of the local suite: the subject is the DEPLOYED runner image, and
# neither the dev box nor the macOS devcontainer is one. The checker refuses to
# answer about anything else -- so scheduling it locally would make every
# `make ci-native` red with a question the box cannot be asked. Its workflow
# step runs on `ra8-ci`, where the step is already executing inside the image.
#
# The selftest runs first, as everywhere else here: an extractor that has
# stopped matching require_cmd reports an empty dependency set, and an empty
# set is indistinguishable from a complete image.
gate_runner_image_deps() (
  set -e
  require_cmd python3 "python3 is the interpreter every gate driver already needs"
  python3 scripts/checks/check_runner_image_deps.py --selftest
  python3 scripts/checks/check_runner_image_deps.py
)

# --- hil-all (manual) -----------------------------------------------------
# Drives scripts/hil/all.sh, which auto-discovers every app under
# examples/ek_ra8d2/hw_validated/hil/ and verifies each via its hil.conf
# manifest. Needs the bench EK-RA8D2 attached to the Pi 5 runner.
gate_hil_all() (
  set -e
  require_cmd arm-none-eabi-gcc
  bash scripts/hil/all.sh --list
  # hil_all.sh builds them itself, but doing it explicitly first gives clearer
  # logs when a build (not a flash) fails.
  local apps=() line
  while IFS= read -r line; do apps+=("$line"); done < <(
    find examples/ek_ra8d2/hw_validated/hil -mindepth 1 -maxdepth 1 -type d \
      -exec basename {} \;
  )
  make -j"$(ra8_max_jobs)" "${apps[@]}"
  bash scripts/hil/all.sh --skip-build
)

# --- bench-lock-selftest (manual) -----------------------------------------
# The bench lock, proved against the REAL bench host rather than asserted.
#
# It has to be `manual` because it needs the bench host: a hosted runner has no
# ssh path to it and no /var/lib/ra8-bench. It runs against a THROWAWAY state
# directory, so it takes no instrument, touches no board, and cannot interfere
# with a hold anybody else is keeping -- which is why it can sit in the HIL
# workflow ahead of the suite instead of competing with it.
#
# --ssh-death is the case that matters. The whole no-stale-lock property rests
# on ssh reaping its remote payload when the client dies, and ssh does not do
# that in general -- it does it here only because the payload blocks on the ssh
# channel as its stdin. So this SIGKILLs a real ssh client and asserts the flock
# drops. If it ever fails, the design has silently degraded to a TTL lease and
# the right answer is to say so, not to bolt a TTL on.
gate_bench_lock_selftest() (
  set -e
  require_cmd ssh
  bash scripts/hil/bench.sh selftest --ssh-death
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
  python3 scripts/checks/check_doc_diagrams.py --html build/docs/html
  bash scripts/builders/publish_docs.sh
)

# ===========================================================================
# REGISTRY PLUMBING
# ===========================================================================
