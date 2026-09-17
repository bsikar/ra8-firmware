#!/usr/bin/env bash
# SPDX-License-Identifier: MIT
# Copyright (c) 2026 Brighton Sikarskie
# shellcheck shell=bash
#
# scripts/ci/gates/manual.sh -- Manual / scheduled gates -- network, nightly budget, or bench hardware.
#
# SOURCED, NEVER EXECUTED. scripts/ci.sh sources every file in this directory
# and is the only entry point; RA8_GATE_REGISTRY -- the single list of what
# gates exist -- stays there too. These files hold gate BODIES only, so there
# is still exactly one home for a gate's definition and exactly one command
# for a workflow to call (`just quality::local::gate <name>`). Adding a second
# registry here would recreate the drift the single-definition rule exists to
# prevent.
#
# Gates in this file: mcdc-delta-base, osv-scan, fuzz-sweep, runner-clock,
#                     hil-all, docs-publish, macos-host-build

# --- mcdc-delta-base (manual) ---------------------------------------------
# Builds the BASE branch's MC/DC summary in a throwaway worktree so the PR
# comment can show a per-file delta. Informational, never blocking: the
# workflow marks the step continue-on-error and an empty summary just renders
# the PR column alone.
gate_mcdc_delta_base() (
  set -e
  local base_ref="${RA8_MCDC_BASE_REF:-main}"
  local history_repo tree
  history_repo="$(ci_history_repo)"
  tree="$(mktemp -d "${TMPDIR:-/tmp}/ra8-mcdc-base.XXXXXXXX")"
  rm -rf "$tree"
  python3 scripts/dev/git_environment.py \
    --check-attributes "$history_repo" --commit "origin/${base_ref}"
  install_sanitized_git_environment
  GIT_LFS_SKIP_SMUDGE=1 run_sanitized_git -C "$history_repo" \
    -c core.attributesFile=/dev/null \
    -c core.fsmonitor=false \
    -c core.hooksPath=/dev/null \
    -c filter.lfs.process= \
    -c filter.lfs.required=false \
    -c filter.lfs.smudge=/bin/cat \
    worktree add "$tree" "origin/${base_ref}"
  (
    cd "$tree"
    CC=clang-18 CXX=clang++-18 RA8_MCDC_THRESHOLD=0 \
      bash scripts/report/mcdc_report.sh --in-container
  ) || echo "base-branch MC/DC build failed -- the delta will be PR-only"
  # The STATUS file is what makes the failure legible downstream. This used to
  # write an empty base-summary.txt, which the renderer could not tell from a
  # base branch that genuinely measured nothing -- so every row's delta came
  # out `n/a` and the posted comment read as "no MC/DC regressions" when in
  # fact no comparison had been performed at all (#536). A failed build must
  # say it failed, not hand downstream an empty file to misread.
  if [[ -f "$tree/build/mcdc-report/summary.txt" ]]; then
    cp "$tree/build/mcdc-report/summary.txt" base-summary.txt
    echo "ok" >base-summary.status
  else
    : >base-summary.txt
    echo "unavailable" >base-summary.status
  fi
  run_sanitized_git -c core.attributesFile=/dev/null -c core.fsmonitor=false \
    -c core.hooksPath=/dev/null -C "$history_repo" \
    worktree remove --force "$tree" || rm -rf "$tree"
)

# --- mcdc-delta-render (manual) -------------------------------------------
# Renders the PR comment BODY. It is a gate, not inline YAML, on purpose: the
# ~70 lines of JavaScript this replaces parsed llvm-cov output and computed
# deltas inside an `actions/github-script` step, and check_ci_parity.py only
# inspects steps with a `run:` key -- so a `uses:` step was entirely outside
# the guard that exists to stop check logic growing a second home in the
# workflows (#536). Moving it under scripts/ puts it back in scope and gives
# it a --selftest.
gate_mcdc_delta_render() (
  set -e
  python3 scripts/report/mcdc_delta_comment.py --selftest
  python3 scripts/report/mcdc_delta_comment.py \
    --pr pr-mcdc/summary.txt \
    --base base-summary.txt \
    --base-status base-summary.status \
    --out mcdc-delta-comment.md
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
  bash scripts/checks/osv_scan.sh --selftest
  bash scripts/checks/osv_scan.sh --scanner ./osv-scanner --output-dir osv-report
)

# --- soup-upstream-refresh (manual) ---------------------------------------
# The networked half of the SOUP provenance guarantee (#548). The per-push
# soup-upstream gate verifies the tree against committed manifests; those
# manifests are only as good as the moment they were fetched. This gate
# re-fetches every pinned upstream revision and fails if what upstream now
# publishes differs from what we recorded -- a moved tag, a rewritten history,
# a replaced release artifact, a project that disappeared. None of that is
# visible offline, and all of it silently invalidates the claim.
#
# It writes nothing (--verify-upstream): a scheduled job that quietly adopted
# upstream's new bytes would launder exactly the event it exists to report.
# Scheduled weekly alongside osv-scan for the same reason -- both are questions
# about the outside world, which changes on its own clock and not on ours.
gate_soup_upstream_refresh() (
  set -e
  require_cmd git
  python3 scripts/checks/check_soup_upstream.py --selftest
  python3 scripts/checks/check_soup_upstream.py --verify-upstream
  # Fetched C6 SOUP cannot use the vendored offline replay proof. Fetch only
  # its exact 40-hex pin into a disposable checkout, apply-check the numbered
  # series, then reverse it to the clean pin. This never builds, flashes, or
  # contacts a bench.
  python3 scripts/checks/check_c6_patch_upstream.py --selftest
  python3 scripts/checks/check_c6_patch_upstream.py --verify-upstream
)

# --- fuzz-sweep (manual) --------------------------------------------------
# Deep-runs every registered harness (the RA8_FUZZ_TARGETS registry in
# tests/fuzz/CMakeLists.txt, consumed through run_fuzz.sh --all) with a real
# per-target budget. --list parses the registry and cross-checks it against
# the tests/fuzz/src/fuzz_ra8_*.c sources, so registry drift fails before the
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
# the shared user quota `just quality::local::gate ci-status-contract` exists to protect.
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
# `just quality::native` red with a question the box cannot be asked. Its workflow
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
# manifest. The dev-box listener builds natively and reaches the EK-RA8D2
# through the dedicated Pi 5 instrument-host login.
RA8_HIL_APP_FLOOR=100

_hil_require_census() {
  local count="$1"
  if [[ ! "$count" =~ ^[0-9]+$ || "$count" -lt "$RA8_HIL_APP_FLOOR" ]]; then
    echo "hil-all: app census collapsed to $count (floor $RA8_HIL_APP_FLOOR)" >&2
    return 1
  fi
}

gate_hil_all() (
  set -e
  # Prove both resolver directions before trusting the live version probe.
  # The probe binds GCC and every HIL-relevant binutil to one 13.3 directory.
  arm_toolchain_selftest
  use_pinned_arm_toolchain
  require_pinned_arm_toolchain
  require_cmd just
  require_cmd xargs
  /bin/bash -p scripts/builders/all_examples.sh --selftest
  /bin/bash -p scripts/hil/all.sh --list
  # hil_all.sh builds them itself, but doing it explicitly first gives clearer
  # logs when a build (not a flash) fails.
  local apps=() line
  while IFS= read -r line; do apps+=("$line"); done < <(
    find examples/ek_ra8d2/hw_validated/hil -mindepth 1 -maxdepth 1 -type d \
      -exec basename {} \;
  )
  _hil_require_census "${#apps[@]}"
  local targets=() app
  for app in "${apps[@]}"; do
    targets+=("ek_ra8d2/hw_validated/hil/$app")
  done
  echo "hil-all: building ${#apps[@]} apps with at most $(ra8_max_jobs) concurrent jobs"
  printf '%s\0' "${targets[@]}" |
    MAX_JOBS="$(ra8_max_jobs)" RA8_SELECTED_APP_FLOOR="$RA8_HIL_APP_FLOOR" \
    BUILD_TYPE=RelWithDebInfo /bin/bash -p scripts/builders/all_examples.sh --selected0
  /bin/bash -p scripts/hil/all.sh --skip-build
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
  /bin/bash -p scripts/hil/bench.sh selftest --ssh-death
)

# --- docs-publish (manual) ------------------------------------------------
# Builds the Doxygen HTML and force-pushes it to the orphan gh-pages branch.
# Not a pass/fail quality gate -- registered so ci-parity can bind the publish
# workflow's step and no unreviewed `run:` body hides inside it.
gate_docs_publish() (
  set -e
  require_cmd just "the publish gate builds through the authoritative docs recipe"
  # Same hard dependency as the docs gate, and it matters more here: without
  # `dot`, build_docs.sh degrades to text-only output and this gate would
  # force-push a diagram-free site over the live one, succeeding the whole way.
  # The publish path is exactly where a silent degradation does the damage.
  require_cmd dot
  /bin/bash -p scripts/dev/run_just.sh docs::build
  # Verify the site about to be published actually contains its diagrams,
  # against the real output tree `just docs::build` just wrote. --selftest first, so
  # the publish path never trusts an unproven detector (#531).
  python3 scripts/checks/check_doc_diagrams.py --selftest
  python3 scripts/checks/check_doc_diagrams.py --html build/docs/html
  /bin/bash -p scripts/builders/publish_docs.sh
)

# --- macos-host-build (manual) --------------------------------------------
# The native arm64 macOS half of #899, and the only job in this tree that runs
# on a Mac.
#
# Every other gate runs on Linux, so the one property none of them can observe
# is the one the issue is about: whether the host Zig roots link on a real
# Apple Silicon machine, where the Command Line Tools libSystem.tbd lists
# arm64e-macos but not arm64-macos. Cross-compiling for aarch64-macos from
# Linux proves the target SELECTION and nothing more -- an explicit os_tag
# makes the query non-native, so zig links its OWN bundled libSystem.tbd,
# which does declare arm64-macos, and the SDK stub that omits it is never
# touched. A cross-build therefore cannot fail the way an affected Mac fails.
# This gate refuses on any other host rather than report a pass for a
# measurement it did not take.
#
# It is `manual` on purpose: a fast/slow gate joins the Linux `--native`
# sweep, where a macOS-only gate could only ever fail.
#
# Scope is the three build roots that need nothing but Zig. reg_gen's test
# suite wants the pinned C23 host compiler, and firmware_pipeline/zig,
# abi_chain_fixture and rust_abi_fixture/zig link Cargo-built archives; giving
# a hosted Mac those toolchains is its own slice, so this gate covers what it
# can actually prove today rather than claiming the whole Zig surface.
gate_macos_host_build() (
  set -e
  # `uname -m` describes this PROCESS, not this machine: under Rosetta 2 an
  # arm64 Mac reports x86_64, and the refusal below used to send the owner off
  # to find hardware they were already sitting at. host_arch.sh separates the
  # two so the diagnosis names Rosetta and the native re-run instead.
  # shellcheck source=scripts/ci/lib/host_arch.sh
  . scripts/ci/lib/host_arch.sh
  # shellcheck source=scripts/ci/lib/macos_host_roots.sh
  . scripts/ci/lib/macos_host_roots.sh
  local host_os host_proc_arch host_arch
  host_os="$(uname -s)"
  host_proc_arch="$(uname -m)"
  host_arch="$(ra8_host_hardware_arch "${host_os}" "${host_proc_arch}")"
  if ra8_host_translated "${host_os}" "${host_proc_arch}"; then
    printf 'error: macos-host-build measures the native arm64 macOS link path, and this shell is\n' >&2
    printf 'error: %s.\n' "$(ra8_host_arch_summary "${host_os}" "${host_proc_arch}")" >&2
    printf 'error: a translated zig links the x86_64 path, so it cannot observe the missing\n' >&2
    printf 'error: arm64-macos slice in the SDK libSystem stub at all (#899).\n' >&2
    ra8_host_translation_advice >&2
    return 1
  fi
  if [[ "${host_os}" != "Darwin" || "${host_arch}" != "arm64" ]]; then
    printf 'error: macos-host-build measures the native arm64 macOS link path; this host is %s.\n' \
      "$(ra8_host_arch_summary "${host_os}" "${host_proc_arch}")" >&2
    printf 'error: run it on an arm64 macOS runner -- a cross-build from here links the bundled\n' >&2
    printf 'error: libSystem stub and would pass without ever touching the SDK one (#899).\n' >&2
    return 1
  fi
  require_cmd zig "the macos-host-build gate builds every host root with the pinned Zig"
  require_tool_versions zig

  # An SDK precondition has to RUN the probe, not check that xcrun exists.
  # macOS ships /usr/bin/xcrun as a stub on every install, so `require_cmd
  # xcrun` passed on a Mac with no Command Line Tools at all; the graph then
  # found no SDK, pinned the bundled libSystem stub, every root built, and this
  # gate reported green for the native SDK link path it never took (#899).
  # macos_sdk.sh runs `xcrun --show-sdk-path` and keeps the failures apart --
  # no developer directory, an unaccepted licence, a moved SDK, an SDK with no
  # libSystem stub -- because each needs a different fix.
  printf '=== active macOS SDK ===\n'
  # shellcheck source=scripts/ci/lib/macos_sdk.sh
  . scripts/ci/lib/macos_sdk.sh
  ra8_macos_sdk_require

  # Diagnostics first and unconditionally: which stub the graph chose, and why,
  # is the single input that decides this whole gate, and a failure is
  # unreadable without it. This prints the build graph's OWN decision rather
  # than re-deriving it here with grep, so the gate cannot disagree with the
  # thing it is gating -- and it distinguishes "the stub omits arm64-macos"
  # from "there was no stub to read", which need different fixes.
  printf '=== host target decision ===\n'
  (cd tools/zig_build && zig build explain-host-target)

  # Which roots this gate builds is a declared list with a reason per root,
  # not three names inlined here: a host root added later takes its default
  # target from ra8_build.hostDefaultTargetQuery and so looks correct from
  # Linux, while nothing ever builds it on a Mac. macos_host_roots.sh --selftest
  # fails when a build.zig exists that the manifest does not mention, and the
  # coverage is printed so a GREEN run states what it did not measure.
  printf '\n=== gate coverage ===\n'
  ra8_macos_host_announce_coverage

  local root
  while IFS= read -r root; do
    printf '\n=== %s: zig build (default host target) ===\n' "${root}"
    (cd "${root}" && zig build --summary all)
    printf '\n=== %s: zig build test ===\n' "${root}"
    (cd "${root}" && zig build test --summary all)
  done < <(ra8_macos_host_covered_roots)

  # Linking is not the claim; what came out of the link is. Read the emitted
  # Mach-O back and check it is a native arm64 image, stamped with the
  # deployment target this build was configured for, linked against the system
  # libSystem. A build that quietly took Zig's default macOS floor instead of
  # the host's version also exits zero, and would otherwise pass here.
  printf '\n=== apps/host/image_pyramid: verify-host-artifact ===\n'
  (cd apps/host/image_pyramid && zig build verify-host-artifact --summary all)

  # The forced-bundled escape hatch is load-bearing on an affected Mac, so it
  # is part of the verdict.
  printf '\n=== apps/host/image_pyramid: -Dmacos-libsystem=bundled ===\n'
  (cd apps/host/image_pyramid && zig build -Dmacos-libsystem=bundled)

  # The forced-SDK leg is INFORMATIONAL and may legitimately fail: failing is
  # precisely the bug #899 reports, and the default auto path above is what
  # carries the verdict. It stays in the log so the day Apple ships an
  # arm64-macos target in the stub is visible here instead of going unnoticed.
  printf '\n=== apps/host/image_pyramid: -Dmacos-libsystem=sdk (informational) ===\n'
  if (cd apps/host/image_pyramid && zig build -Dmacos-libsystem=sdk); then
    printf 'informational: the SDK stub linked cleanly on this runner image\n'
  else
    printf 'informational: the SDK stub did NOT link here -- expected on an affected SDK (#899)\n'
  fi
)

# ===========================================================================
# REGISTRY PLUMBING
# ===========================================================================
