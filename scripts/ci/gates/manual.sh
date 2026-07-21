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
# Gates in this file: mcdc-delta-base, osv-scan, fuzz-sweep, hil-all, docs-publish

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
  echo "Registered harnesses:"
  bash scripts/checks/run_fuzz.sh --list
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
  make -j"$(cpu_count)" "${apps[@]}"
  bash scripts/hil/all.sh --skip-build
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
