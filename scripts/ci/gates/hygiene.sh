#!/usr/bin/env bash
# SPDX-License-Identifier: MIT
# Copyright (c) 2026 Brighton Sikarskie
# shellcheck shell=bash
#
# scripts/ci/gates/hygiene.sh -- Textual hygiene gates: encoding, headers, attribution bans, formatting.
#
# SOURCED, NEVER EXECUTED. scripts/ci.sh sources every file in this directory
# and is the only entry point; RA8_GATE_REGISTRY -- the single list of what
# gates exist -- stays there too. These files hold gate BODIES only, so there
# is still exactly one home for a gate's definition and exactly one command
# for a workflow to call (`just quality::local::gate <name>`). Adding a second
# registry here would recreate the drift the single-definition rule exists to
# prevent.
#
# Gates in this file: ci-parity, ascii, copyright, since, markdown-references,
# no-ai-attribution, no-ai-attribution-commits, inclusive-terminology,
# inclusive-terminology-commits, format

# --- ci-parity ------------------------------------------------------------
# The backstop for the whole scheme: workflow-as-driver still drifts if
# someone pastes a raw `run:` check into the YAML. This gate refuses that.
# A `( set -e )` subshell, not a `{ }` body: run_gate_capture disables errexit
# around the CALL, so a `{ }` gate with several commands reports only its LAST
# command's status and swallows everything before it. Every other multi-command
# gate here is already a subshell; this one was the exception, which is exactly
# why suite_errexit_selftest's `return 1` below could not fail the gate.
# `just quality::local::gate ci-status-contract` is how every agent decides whether a commit is healthy, and
# it has twice returned the WRONG verdict -- once read as FAIL, once as PASS --
# because the branch-level "overall:" header was mistaken for a per-sha answer,
# and because the per-sha path exited on the branch verdict rather than on the
# runs for that sha. Neither failure was visible: the tool printed a plausible
# line and exited a plausible code. This asserts the three-value contract in
# both directions before anyone can trust it again.
gate_ci_status_contract() (
  set -e
  require_cmd python3 "python3 is the interpreter every gate driver already needs"
  /bin/bash -p scripts/ci/monitor.sh selftest
)

gate_ci_parity() (
  set -e
  require_python_mod yaml "run 'just setup-python'"
  suite_errexit_selftest
  suite_registry_selftest
  suite_build_lifecycle_selftest
  bash scripts/ci/lib/nofile.sh --selftest
  # The runner must also be honest about runs that STOPPED. A SIGTERMed suite
  # once deleted its own snapshot and kept going, inventing a FAIL for every
  # gate that came after; a fabricated red costs a lane its time and teaches
  # people to discount reds (#542).
  suite_abort_selftest
  # suite_errexit_selftest proves the RUNNER propagates a mid-body failure --
  # but only for the shape it probes with, a `( set -e )` subshell. It is
  # therefore blind to a gate written as a `{ }` block, which runs in the
  # calling shell and so inherits run_gate_capture's `set +e`. Two gates were
  # in exactly that state (#190): each ran a checker `--selftest` first "so a
  # detector that stopped matching cannot pass as clean", then discarded that
  # selftest's status. check_gate_bodies.py covers the other half -- every
  # dispatched body must itself be capable of failing.
  python3 scripts/ci/check_gate_bodies.py --selftest
  python3 scripts/ci/check_gate_bodies.py
  python3 scripts/ci/check_ci_parity.py --selftest
  python3 scripts/ci/check_ci_parity.py
  # The third leg. check_gate_bodies proves a gate CAN fail; check_ci_parity
  # proves it is SCHEDULED. Neither proves the checker inside it still
  # detects anything -- the requirement CLAUDE.md and ci.sh both state and
  # which nothing enforced, so 28 gate-wired detectors had no --selftest and
  # one had a selftest no gate ran (#531).
  python3 scripts/ci/check_selftest_coverage.py --selftest
  python3 scripts/ci/check_selftest_coverage.py --check
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

# Assert that run_suite() refuses to report PASS for a suite that ran nothing.
#
# Two ways the suite could once do exactly that, both fixed in #190 and both
# asserted here so neither can come back:
#
#   1. an EMPTY gate selection. The summary loop is bounded by `gate_names`,
#      so with zero entries it never set `failed` and printed RESULT: PASS.
#   2. an INVALID registry. run_suite consumed `list_gates` through a process
#      substitution -- `done < <(list_gates)` -- whose exit status bash
#      discards and `set -e` cannot see. list_gates `continue`s past a row
#      whose gate_*() function does not exist, so that gate silently dropped
#      out of the suite and the run still reported PASS.
#
# Both probes mutate RA8_GATE_REGISTRY, so each runs inside its own subshell
# and the real registry is untouched.
suite_registry_selftest() {
  local probe_log rc=0 failures=0
  probe_log="$(mktemp "${TMPDIR:-/tmp}/ra8-registry-probe.XXXXXXXX")"

  # 1. empty registry -> the suite must FAIL, not pass on an empty summary.
  rc=0
  (
    RA8_GATE_REGISTRY=()
    run_suite 0
  ) >"$probe_log" 2>&1 || rc=$?
  if [[ "$rc" -eq 0 ]]; then
    echo "ERROR: run_suite reported success with an EMPTY gate registry." >&2
    echo "       A suite that executed nothing has not passed." >&2
    failures=1
  fi

  # 2. a registry row with no gate_*() behind it -> the suite must FAIL rather
  #    than quietly drop the row and grade the remainder.
  rc=0
  (
    # shellcheck disable=SC2034  # read by list_gates in scripts/ci.sh, which sources this file.
    RA8_GATE_REGISTRY=("ra8-registry-probe-missing-fn|fast|probe row with no function")
    run_suite 0
  ) >"$probe_log" 2>&1 || rc=$?
  if [[ "$rc" -eq 0 ]]; then
    echo "ERROR: run_suite reported success for a registry row whose gate" >&2
    echo "       function does not exist. list_gates' failure status is being" >&2
    echo "       discarded -- almost certainly by consuming it through a" >&2
    echo "       process substitution (\`done < <(list_gates)\`)." >&2
    failures=1
  fi

  rm -f "$probe_log"
  if [[ "$failures" -ne 0 ]]; then
    return 1
  fi
  echo "ci.sh: suite-runner registry self-test OK (empty and invalid registries fail)."
}

# --- the abort self-test (#542) -------------------------------------------
# Assert that a run which STOPPED BEING A MEASUREMENT cannot emit a gate
# verdict -- and that a run which merely failed still reports FAIL.
#
# The defect: run_suite_on_snapshot installed one trap for four events,
# `trap "rm -rf '$work'" EXIT INT TERM`. A bash handler that does not itself
# exit returns to where the shell was interrupted, so a SIGTERMed suite deleted
# the snapshot every gate stands in and CARRIED ON inside it. The remaining
# gates "failed" on missing files and the run printed a FAIL table describing
# nothing about the tree under test. It cost three suite runs to attribute, and
# a fabricated red is worse than a missing check in a tree whose rule is "own
# every red".
#
# Both directions, as everywhere else here. A guard that only ever fires would
# be just as useless: an abort machine that swallowed real failures would turn
# every red into UNKNOWN, which is the same lie in the other direction.
#
# Assert the probe's exit status, naming what the run was supposed to be.
_abort_expect_rc() {
  local got="$1" want="$2" what="$3"
  [[ "$got" == "$want" ]] && return 0
  echo "ERROR: ci.sh abort self-test FAILED -- $what" >&2
  echo "       the probe exited $got; expected $want." >&2
  return 1
}

# Assert a pattern IS in the probe's output.
_abort_expect() {
  local log="$1" pattern="$2" what="$3"
  grep -qE "$pattern" "$log" && return 0
  echo "ERROR: ci.sh abort self-test FAILED -- $what" >&2
  echo "       expected /$pattern/ in the probe output; it is not there." >&2
  return 1
}

# Assert a pattern is NOT in the probe's output, and show it when it is.
_abort_reject() {
  local log="$1" pattern="$2" what="$3"
  grep -qE "$pattern" "$log" || return 0
  echo "ERROR: ci.sh abort self-test FAILED -- $what" >&2
  echo "       found /$pattern/ in the probe output:" >&2
  grep -nE "$pattern" "$log" | sed 's/^/         /' >&2
  return 1
}

# Run the probe to completion, uninterrupted. Status in RA8_ABORT_PROBE_RC.
_abort_run_probe() {
  local mode="$1" log="$2"
  RA8_ABORT_PROBE_RC=0
  /bin/bash -p scripts/ci.sh --selftest-abort "$mode" >"$log" 2>&1
  RA8_ABORT_PROBE_RC=$?
  return 0
}

# Run the probe and KILL IT MID-GATE, the way systemd-logind kills a detached
# run when the last ssh session for the user closes.
#
# The kill targets the process GROUP, which is what makes this a reproduction
# rather than a lookalike. Signalling only the top-level PID would not do it:
# bash defers a trap until the foreground command completes, so the run would
# sit in the fixture's `sleep` for its full duration and the handler would fire
# on an already-finished gate -- the opposite of the case under test. `set -m`
# turns job control on for the launch, which puts the background job in its own
# process group whose PGID is its PID, so `kill -- -$!` reaches the run AND the
# `sleep` it is blocked in. (It is used instead of `setsid` because it is a
# bash builtin: identical behaviour, one fewer external dependency, and it
# works on macOS where util-linux is absent.)
#
# The marker is the non-vacuity floor: no marker means the probe never reached
# a gate, and killing a run that had not started measuring anything would prove
# nothing. That is a self-test failure, not a skip.
_abort_run_killed_probe() {
  local marker="$1" log="$2" job waited=0
  RA8_ABORT_PROBE_RC=0
  rm -f "$marker"
  set -m
  RA8_CI_PROBE_MARKER="$marker" /bin/bash -p scripts/ci.sh \
    --selftest-abort hang >"$log" 2>&1 &
  job=$!
  set +m
  # Bounded wait for the probe to be INSIDE a gate: it materialises a full HEAD
  # snapshot first, measured at ~6 s on the dev box. The bound is 300 s -- 50x
  # that -- because these boxes are shared and a tight bound would turn
  # contention into a red, which is the failure mode this whole change exists
  # to end. It stays BOUNDED, and timing out is a loud failure rather than a
  # skip: see the caller.
  while [[ ! -s "$marker" && "$waited" -lt 3000 ]]; do
    sleep 0.1
    waited=$((waited + 1))
  done
  if [[ ! -s "$marker" ]]; then
    kill -TERM -- "-$job" 2>/dev/null
    wait "$job" 2>/dev/null
    RA8_ABORT_PROBE_RC=-1
    return 0
  fi
  kill -TERM -- "-$job" 2>/dev/null
  wait "$job"
  RA8_ABORT_PROBE_RC=$?
  return 0
}

# Direction 1: a SIGNALLED suite emits no gate verdict at all.
_abort_check_killed() {
  local tmp="$1" failures=0
  local log="$tmp/hang.log"
  _abort_run_killed_probe "$tmp/marker" "$log"
  if [[ "$RA8_ABORT_PROBE_RC" == "-1" ]]; then
    echo "ERROR: ci.sh abort self-test FAILED -- the probe never entered a" >&2
    echo "       gate, so nothing was killed mid-measurement and this" >&2
    echo "       self-test proved nothing. Probe output:" >&2
    sed 's/^/         /' "$log" >&2
    return 1
  fi
  # The second half of the floor: the marker proves the fixture ran, this
  # proves the runner had entered a GATE when the signal landed. Killing a run
  # between gates would exercise a different path and quietly prove less.
  _abort_expect "$log" '== GATE: ra8-probe-hang' \
    "the kill must land while a gate is running" || failures=$((failures + 1))
  _abort_expect_rc "$RA8_ABORT_PROBE_RC" 3 \
    "a killed suite must exit 3 (UNKNOWN), not a pass or a fail" || failures=$((failures + 1))
  _abort_expect "$log" 'ABORTED' \
    "a killed suite must SAY it was killed" || failures=$((failures + 1))
  _abort_reject "$log" 'RESULT: FAIL' \
    "a killed suite must not report a suite verdict" || failures=$((failures + 1))
  _abort_reject "$log" '^[[:space:]]+ra8-probe-[a-z-]+[[:space:]]+FAIL' \
    "a killed suite must not invent a per-gate FAIL row" || failures=$((failures + 1))
  _abort_reject "$log" 'RA8-PROBE-AFTER-RAN' \
    "a killed suite must not run the gate after the abort" || failures=$((failures + 1))
  return "$failures"
}

# Direction 2: the tree vanishing under the run is UNKNOWN too.
#
# The fixture does exactly what the old trap did -- deletes the snapshot from
# under the still-running runner -- so this direction pins the ORIGINAL symptom
# without needing a signal to produce it.
_abort_check_destroyed() {
  local tmp="$1" failures=0
  local log="$tmp/destroy.log"
  _abort_run_probe destroy "$log"
  _abort_expect_rc "$RA8_ABORT_PROBE_RC" 3 \
    "a suite whose tree disappeared must exit 3 (UNKNOWN)" || failures=$((failures + 1))
  _abort_expect "$log" 'RESULT: ABORTED' \
    "a suite whose tree disappeared must say so" || failures=$((failures + 1))
  # The gate after the destroyer must be REACHED and REFUSED -- its header
  # printed, its body never entered. Without this the direction would also pass
  # if the runner had simply stopped early for some unrelated reason.
  _abort_expect "$log" '== GATE: ra8-probe-after' \
    "the gate after a vanished tree must be reached and refused" || failures=$((failures + 1))
  _abort_reject "$log" 'RESULT: FAIL' \
    "a vanished tree is not a content failure" || failures=$((failures + 1))
  _abort_reject "$log" 'RA8-PROBE-AFTER-RAN' \
    "no gate may be dispatched into a tree that is gone" || failures=$((failures + 1))
  return "$failures"
}

# Direction 3: an UNINTERRUPTED run still reports a real failure.
#
# The must-stay-quiet half. An abort machine that turned every red into UNKNOWN
# would pass both directions above and enforce nothing.
_abort_check_failing() {
  local tmp="$1" failures=0
  local log="$tmp/fail.log"
  _abort_run_probe fail "$log"
  _abort_expect_rc "$RA8_ABORT_PROBE_RC" 1 \
    "an uninterrupted suite with a failing gate must still exit 1 (FAIL)" || failures=$((failures + 1))
  _abort_expect "$log" 'RESULT: FAIL' \
    "a real gate failure must still be reported as FAIL" || failures=$((failures + 1))
  _abort_expect "$log" '^[[:space:]]+ra8-probe-fail[[:space:]]+FAIL' \
    "the failing gate must still be named in the table" || failures=$((failures + 1))
  _abort_reject "$log" 'ABORTED' \
    "a failing gate is not an abort" || failures=$((failures + 1))
  _abort_reject "$log" 'RA8-PROBE-FAIL-CONTINUED' \
    "a gate that fails mid-body must not continue" || failures=$((failures + 1))
  return "$failures"
}

# ERREXIT is turned OFF here explicitly, not assumed off. gate_ci_parity calls
# this under `set -e`, and a ( ) subshell INHERITS that, while every probe below
# legitimately returns non-zero: a killed run exits 3, a failing suite exits 1,
# and each `grep` assertion may not match. With errexit live the very first of
# those aborts the self-test mid-way -- silently, printing no assertion at all
# and handing the gate the probe's exit code as if it were the gate's own. That
# is what it did before this line existed.
suite_abort_selftest() (
  set +e
  set -uo pipefail
  local tmp failures=0
  tmp="$(mktemp -d "${TMPDIR:-/tmp}/ra8-abort-selftest.XXXXXXXX")"

  _abort_check_killed "$tmp"
  failures=$((failures + $?))
  _abort_check_destroyed "$tmp"
  failures=$((failures + $?))
  _abort_check_failing "$tmp"
  failures=$((failures + $?))

  rm -rf "$tmp"
  if [[ "$failures" -ne 0 ]]; then
    echo "ERROR: ci.sh abort self-test FAILED ($failures assertion(s))." >&2
    echo "       A killed suite is inventing gate results again (#542), or" >&2
    echo "       the abort machinery has started swallowing real failures." >&2
    return 1
  fi
  echo "ci.sh: abort self-test OK (SIGTERM -> UNKNOWN with no gate verdict;" \
    "a vanished tree -> UNKNOWN; an uninterrupted failure -> FAIL)."
)

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
  run_sanitized_git -C "$dir" init --quiet
  run_sanitized_git -C "$dir" add -A
  run_sanitized_git -C "$dir" -c user.email=ci@localhost -c user.name=ci \
    commit --quiet --no-verify -m "ci.sh snapshot of HEAD"
}

# Build the LEGAL fixture: three real commits, then a detached HEAD so there is
# no upstream. ci_commit_range must fall through to HEAD~1..HEAD, a range
# spanning exactly one commit -- an ordinary single-commit push, which it would
# be a false failure to reject.
_crs_make_real_repo() {
  local dir="$1" n
  mkdir -p "$dir"
  run_sanitized_git -C "$dir" init --quiet
  for n in 1 2 3; do
    printf 'rev %s\n' "$n" >"$dir/file.txt"
    run_sanitized_git -C "$dir" add -A
    run_sanitized_git -C "$dir" -c user.email=ci@localhost -c user.name=ci \
      commit --quiet --no-verify -m "test: commit $n"
  done
  run_sanitized_git -C "$dir" checkout --quiet --detach HEAD
}

commit_range_selftest() (
  # A SUBSHELL, not a { } body: this probe flips errexit while driving the
  # guard, and the gates that call it run `set -uo pipefail` WITHOUT -e.
  # A { } body would leave errexit enabled in the caller after returning,
  # silently changing how the rest of the gate behaves.
  set -uo pipefail
  require_cmd git || exit 1
  install_sanitized_git_environment || exit 1
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
    GITHUB_EVENT_PATH="" GITHUB_SHA="" GITHUB_EVENT_NAME="" ci_commit_range)"
  span="$(run_sanitized_git -C "$real" rev-list --count "$range" 2>/dev/null)" || span=""
  if [[ -z "$span" || "$span" -lt 1 ]]; then
    rm -rf "$tmp"
    echo "ERROR: ci.sh commit-range self-test FAILED (direction 2, range)." >&2
    echo "       Resolved range '$range' does not resolve in the history" >&2
    echo "       repo, so the gates would scan nothing. This is the" >&2
    echo "       failure mode of exporting a host range into a snapshot" >&2
    echo "       whose object store lacks those commits." >&2
    exit 1
  fi

  # --- Direction 3: the force-push degeneracy (#357 meets a rewrite). -----
  # After a force push the event's before-sha is gone and @{upstream} equals
  # the head, so the naive range is head..head -- zero commits. On a PUSH
  # event ci_commit_range must drop that base and cover the tip commit; on a
  # dispatch it must keep the vacuous range for ci_report_commit_range to
  # reject. Model upstream == head with a local branch tracking a twin ref.
  run_sanitized_git -C "$real" checkout --quiet -b crs_main
  run_sanitized_git -C "$real" branch --quiet crs_twin crs_main
  run_sanitized_git -C "$real" branch --quiet --set-upstream-to=crs_twin crs_main
  local push_range dispatch_range push_span
  push_range="$(RA8_CI_HISTORY_REPO="$real" RA8_CI_COMMIT_RANGE="" \
    GITHUB_EVENT_PATH="" GITHUB_SHA="" GITHUB_EVENT_NAME="push" ci_commit_range)"
  push_span="$(run_sanitized_git -C "$real" rev-list --count "$push_range" 2>/dev/null)" || push_span=""
  if [[ -z "$push_span" || "$push_span" -lt 1 ]]; then
    rm -rf "$tmp"
    echo "ERROR: ci.sh commit-range self-test FAILED (direction 3, push)." >&2
    echo "       With upstream == head (the post-force-push state) a PUSH" >&2
    echo "       event resolved '$push_range' spanning nothing. The gates" >&2
    echo "       would fail loudly on every force-pushed tip instead of" >&2
    echo "       scanning the tip commit." >&2
    exit 1
  fi
  dispatch_range="$(RA8_CI_HISTORY_REPO="$real" RA8_CI_COMMIT_RANGE="" \
    GITHUB_EVENT_PATH="" GITHUB_SHA="" GITHUB_EVENT_NAME="" ci_commit_range)"
  if [[ "$(run_sanitized_git -C "$real" rev-list --count "$dispatch_range" 2>/dev/null)" != "0" ]]; then
    rm -rf "$tmp"
    echo "ERROR: ci.sh commit-range self-test FAILED (direction 3, dispatch)." >&2
    echo "       With upstream == head a non-push event resolved" >&2
    echo "       '$dispatch_range' spanning commits. The #357 vacuity" >&2
    echo "       rejection for manual re-runs has been bypassed." >&2
    exit 1
  fi

  rm -rf "$tmp"
  echo "ci.sh: commit-range self-test OK (rejects a synthetic snapshot," \
    "accepts real history; tricky one-commit span resolved to '$range';" \
    "force-push fallback covers the tip, dispatch stays vacuous)."
)

# --- ascii ----------------------------------------------------------------
# Every first-party root. fix-encoding.py skips third_party and any non-text
# extension, so vendored assets (the doxygen-awesome theme under docs/,
# datasheets, fonts, epubs) are exempt automatically.
# Scope is DERIVED from git ls-files, never a directory list. This gate used to
# loop over `src libs tests examples port scripts tools docs`, so the encoding
# policy never saw the repo root, .github/, cmake/, coprocessor/, infra/ or
# just/ -- 106 files, including CLAUDE.md, the file that STATES the policy
# (#533). A hardcoded root list does not fail when it goes stale; it reports
# success over a shrinking slice. --selftest proves the detector fires on a
# non-ASCII byte before a clean run is believed.
gate_ascii() (
  set -e
  python3 scripts/fix/fix-encoding.py --selftest
  python3 scripts/fix/fix-encoding.py --check --all
)

# --- markdown-references --------------------------------------------------
# Markdown links and repository paths survive moves as plausible prose.
# Inventory every tracked document, leave upstream Markdown untouched, prove
# the parser in both directions, and regenerate the HUM map from its tracked
# full PDF before trusting a clean scan.
gate_markdown_references() (
  set -e
  require_cmd python3 "the Markdown reference checker is a Python gate"
  python3 scripts/checks/check_markdown_references.py --selftest
  python3 scripts/checks/check_markdown_references.py
  python3 scripts/checks/check_chapter_map_freshness.py --selftest
  python3 scripts/checks/check_chapter_map_freshness.py
)

# --- copyright ------------------------------------------------------------
# ONE canonical attribution per file, in the place each comment convention
# already keeps its metadata -- two forms, because this tree has two
# conventions and forcing one on both made the C headers worse:
#
#   C family -- INSIDE the @file Doxygen block, as its closing tag group:
#       * @copyright Copyright (c) 2026 Brighton Sikarskie
#       * SPDX-License-Identifier: MIT
#     never in a separate comment above the block. @author / @date / @since
#     are PRESERVED where a file has them and are never invented: 2190 of the
#     2297 C-family files have never carried @author or @date, and
#     manufacturing those would be fabricated provenance, not a standard.
#
#   Hash-comment files (shell, python, cmake, make, yaml) -- no doc-comment
#   convention to live inside, so the pair leads the file after any shebang:
#       # SPDX-License-Identifier: MIT
#       # Copyright (c) 2026 Brighton Sikarskie
#
# The check used to ask only whether the two strings appeared SOMEWHERE, which
# let several conventions coexist; it now fixes the ORDER, POSITION and exact
# TEXT for both forms. --selftest FIRST proves the rules fire and stay quiet
# (and that the fixer merges, preserves provenance and is idempotent) before
# the scan; --all judges the DERIVED first-party scope (lint_targets) so a new
# top-level directory is covered the day it lands, not via a hand-kept glob.
gate_copyright() (
  set -e
  python3 scripts/checks/check-copyright.py --selftest
  python3 scripts/checks/check-copyright.py --all
)

# --- since ----------------------------------------------------------------
# --all runs BOTH halves over the DERIVED first-party set (#358): the @since
# PRESENCE check on every libs/ra8_*/inc/ public header, AND the @since VALUE
# check on every first-party source. The old form passed only the public
# headers, so the value check ("every @since equals the one VERSION string")
# never ran on tools/, port/ or the deeply-nested example apps -- and drifted
# @since values sat there unseen. --selftest proves both halves first.
gate_since() (
  set -e
  python3 scripts/checks/check-since-version.py --selftest
  python3 scripts/checks/check-since-version.py --all
)

# --- toolchain-parity -----------------------------------------------------
# CI jobs execute inside Ansible-owned runner images, while native development
# gates execute on the Ansible-owned Debian dev box. Both environments consume
# the pins in .devcontainer/Dockerfile, but either deployed copy can still lag
# its declaration. This gate makes that drift loud in one place: --all verifies
# every pinned tool and names any mismatch instead of failing cryptically in a
# downstream build. --selftest proves the comparator both ways first. Repair
# drift through its owner: `just infra::apply dev` for the dev box or
# `just infra::apply <runner-host>` for a CI runner image.
gate_toolchain_parity() (
  set -e
  /usr/bin/python3 -I scripts/dev/managed_python_env.py --selftest
  /usr/bin/python3 -I scripts/dev/managed_python_env.py check-consumers --root "$PWD"
  bash scripts/ci/lib/tool_env.sh --selftest
  /bin/bash -p scripts/dev/setup_python.sh --selftest
  python3 scripts/checks/check_tool_versions.py --selftest
  python3 scripts/checks/check_tool_versions.py --all
)

# --- no-ai-attribution ----------------------------------------------------
gate_no_ai_attribution() (
  set -e
  # --selftest FIRST (#358): proves the ban fires and that tools/, .github/ and
  # every other tracked-text tree the old SCAN_DIRS omitted are back in scope.
  #
  # A `( set -e )` subshell for the reason spelled out on gate_ci_parity above:
  # as a `{ }` block this ran under run_gate_capture's `set +e`, so the
  # selftest's status was discarded and only the tree scan decided the verdict.
  python3 scripts/checks/check_no_ai_attribution.py --selftest
  python3 scripts/checks/check_no_ai_attribution.py
)

# --- no-ai-attribution-commits --------------------------------------------
# The file scanner cannot see commit messages -- trailers live in git
# metadata, not tracked files. Run the real scripts/git/commit-msg gate (the
# single source of truth for the brand list) over the push/PR range.
# CHECK_ONLY=ai runs just the attribution half, so a stray legacy term is
# attributed to -- and fails -- the inclusive-terminology gate instead.
gate_no_ai_attribution_commits() (
  set -uo pipefail
  local range rc=0 sha f hook_out repo identity
  # Prove the guard still tells real history from a snapshot, THEN prove this
  # run has real history to read. Both before the scan: a detector that has
  # stopped seeing its subject must not get to print a green line first.
  commit_range_selftest || return 1
  repo="$(ci_history_repo)"
  ci_require_real_history "$repo" || return 1
  range="$(ci_commit_range)"
  # Print the commit COUNT and reject the zero-commit dispatch range (#357):
  # a run that examined nothing must not read as a pass.
  ci_report_commit_range "$repo" "$range" || return 1
  for sha in $(ci_history_git "$repo" rev-list "$range"); do
    f="$(mktemp)"
    ci_history_git "$repo" log -1 --format=%B "$sha" >"$f"
    identity="$(ci_history_git "$repo" log -1 --format='%an %ae %cn %ce' "$sha")"
    if ! hook_out="$(COMMIT_IDENTITY="$identity" CHECK_ONLY=ai /bin/bash -p scripts/git/commit-msg "$f" 2>&1)"; then
      echo "$hook_out"
      echo "::error::Commit $sha carries a forbidden trailer in its message"
      rc=1
    fi
    rm -f "$f"
  done
  return "$rc"
)

# --- inclusive-terminology ------------------------------------------------
gate_inclusive_terminology() (
  set -e
  # --selftest FIRST (#549): proves the detector fires on a legacy symbol,
  # spares vendored/hardware names, and that the derived scope reaches the
  # roots (infra/, just/) a hardcoded list had dropped.
  python3 scripts/checks/check_inclusive_terminology.py --selftest
  python3 scripts/checks/check_inclusive_terminology.py
)

# --- inclusive-terminology-commits ----------------------------------------
gate_inclusive_terminology_commits() (
  set -uo pipefail
  local range repo
  # --selftest FIRST: proves the detector fires on an un-annotated legacy
  # term and that a LEGACY-OK opt-out at the end of a wrapped paragraph
  # covers the whole paragraph, not only the physical line it sits on.
  python3 scripts/checks/check_inclusive_terminology_commits.py --selftest || return 1
  # See gate_no_ai_attribution_commits: self-test, then real-history guard,
  # then the scan.
  commit_range_selftest || return 1
  repo="$(ci_history_repo)"
  ci_require_real_history "$repo" || return 1
  range="$(ci_commit_range)"
  # Print the commit COUNT and reject the zero-commit dispatch range (#357):
  # a run that examined nothing must not read as a pass.
  ci_report_commit_range "$repo" "$range" || return 1
  ci_history_git "$repo" log "$range" --format=%B |
    python3 scripts/checks/check_inclusive_terminology_commits.py
)

# --- format ---------------------------------------------------------------
gate_format() (
  set -e
  # format_code.sh drives check_comment_format.py, which is a DETECTOR: it is
  # the only thing that reports a comment block clang-format tore in two.
  # Prove both the formatter-version guard and comment detector still fire
  # before believing their verdict on the tree.
  require_cmd python3
  bash scripts/checks/format_code.sh --selftest
  python3 scripts/checks/check_comment_format.py --selftest
  python3 scripts/checks/check_pointer_boilerplate.py --selftest
  python3 scripts/checks/check_pointer_boilerplate.py
  CLANG_FORMAT=clang-format-22 bash scripts/checks/format_code.sh --check --verbose
)
