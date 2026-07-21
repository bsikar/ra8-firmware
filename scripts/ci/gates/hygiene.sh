# shellcheck shell=bash
# SPDX-License-Identifier: MIT
# Copyright (c) 2026 Brighton Sikarskie
#
# scripts/ci/gates/hygiene.sh -- Textual hygiene gates: encoding, headers, attribution bans, formatting.
#
# SOURCED, NEVER EXECUTED. scripts/ci.sh sources every file in this directory
# and is the only entry point; RA8_GATE_REGISTRY -- the single list of what
# gates exist -- stays there too. These files hold gate BODIES only, so there
# is still exactly one home for a gate's definition and exactly one command
# for a workflow to call (bash scripts/ci.sh --gate <name>). Adding a second
# registry here would recreate the drift the single-definition rule exists to
# prevent.
#
# Gates in this file: ci-parity, ascii, copyright, since, no-ai-attribution, no-ai-attribution-commits, inclusive-terminology, inclusive-terminology-commits, format

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
  python3 scripts/ci/check_ci_parity.py --selftest
  python3 scripts/ci/check_ci_parity.py
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
  # shellcheck disable=SC2329  # driven indirectly through run_gate_capture below.
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
  # shellcheck disable=SC2154  # RA8_GATE_RC comes from scripts/ci.sh, the only thing that sources this file.
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
# Build the BROKEN fixture: a one-commit repository whose single message is the
# `ci.sh snapshot of HEAD` that the #348 false-green guard exists to reject.
# Fixture construction only -- every assertion and the cleanup/exit path stays
# in commit_range_selftest, so there is one place that decides the verdict.
_crs_make_snapshot_repo() {
  local dir="$1"
  mkdir -p "$dir"
  printf 'snapshot\n' >"$dir/file.txt"
  git -C "$dir" init --quiet
  git -C "$dir" add -A
  git -C "$dir" -c user.email=ci@localhost -c user.name=ci \
    commit --quiet --no-verify -m "ci.sh snapshot of HEAD"
}

# Build the LEGAL fixture: three real commits, then a detached HEAD so there is
# no upstream. ci_commit_range must fall through to HEAD~1..HEAD, a range
# spanning exactly one commit -- an ordinary single-commit push, which it would
# be a false failure to reject.
_crs_make_real_repo() {
  local dir="$1" n
  mkdir -p "$dir"
  git -C "$dir" init --quiet
  for n in 1 2 3; do
    printf 'rev %s\n' "$n" >"$dir/file.txt"
    git -C "$dir" add -A
    git -C "$dir" -c user.email=ci@localhost -c user.name=ci \
      commit --quiet --no-verify -m "test: commit $n"
  done
  git -C "$dir" checkout --quiet --detach HEAD
}

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
  _crs_make_snapshot_repo "$fake"

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
  _crs_make_real_repo "$real"

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
    python3 scripts/fix/fix-encoding.py --check "$dir"
  done
)

# --- copyright ------------------------------------------------------------
gate_copyright() (
  set -e
  local files=() line
  while IFS= read -r line; do files+=("$line"); done < <(
    git ls-files '*.c' '*.h' '*.cpp' '*.hpp' '*.cmake' '*.sh' '*.py' 'CMakeLists.txt'
  )
  python3 scripts/checks/check-copyright.py "${files[@]}"
)

# --- since ----------------------------------------------------------------
gate_since() (
  set -e
  local files=() line
  while IFS= read -r line; do files+=("$line"); done < <(
    git ls-files 'libs/ra8_*/inc/*.h'
  )
  python3 scripts/checks/check-since-version.py "${files[@]}"
)

# --- no-ai-attribution ----------------------------------------------------
gate_no_ai_attribution() {
  python3 scripts/checks/check_no_ai_attribution.py
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
  return "$rc"
)

# --- inclusive-terminology ------------------------------------------------
gate_inclusive_terminology() {
  python3 scripts/checks/check_inclusive_terminology.py
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
    python3 scripts/checks/check_inclusive_terminology_commits.py
)

# --- format ---------------------------------------------------------------
gate_format() (
  set -e
  local cf
  cf="$(pick_clang_format)"
  CLANG_FORMAT="$cf" bash scripts/checks/format_code.sh --check --verbose
)
