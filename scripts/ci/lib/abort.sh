# shellcheck shell=bash
# SPDX-License-Identifier: MIT
# Copyright (c) 2026 Brighton Sikarskie
#
# scripts/ci/lib/abort.sh -- a KILLED suite has no verdict.
#
# SOURCED, NEVER EXECUTED. scripts/ci.sh is the only entry point; this file
# holds the machinery that decides when a run has stopped being a measurement.
#
# ===========================================================================
# WHAT THIS EXISTS FOR (#542)
# ===========================================================================
# run_suite_on_snapshot used to install ONE trap for four events:
#
#     trap "rm -rf '$work' '$work.index'" EXIT INT TERM
#
# A bash trap handler that does not itself exit RETURNS TO WHERE THE SHELL WAS
# INTERRUPTED. So on SIGTERM the snapshot every gate is standing in was deleted
# and the suite CARRIED ON, inside a directory that no longer existed. Every
# gate after that point failed on missing files:
#
#     shell-init: error retrieving current directory: getcwd: cannot access ...
#     bash: scripts/checks/check_unicorn_version.sh: No such file or directory
#       emulator-matrix     FAIL
#       emulator-io-fabric  FAIL
#       eil-integration     FAIL
#
# and the run printed `RESULT: FAIL` with a per-gate table indistinguishable
# from a real content failure. Those failures were INVENTED: they describe
# nothing about the tree under test, because there was no tree under test.
#
# That is worse than an ordinary bug in a repository whose operating rule is
# "own every red". A fabricated red either burns a lane chasing a ghost -- it
# cost three full suite runs before anyone spotted `Terminated` in the log --
# or, much worse, teaches people that reds on the shared box can be waved
# through, which is how a real regression gets through.
#
# It fires in practice, not in theory. A detached `nohup make ci-native &`
# started over ssh is reaped by systemd-logind when the last session for that
# user closes (KillUserProcesses), and the shared boxes are contended.
#
# ===========================================================================
# THE CONTRACT: "I WAS KILLED" IS UNKNOWN, NOT FAIL
# ===========================================================================
# This tree already has an exit-code contract for a verdict that could not be
# established -- scripts/ci/monitor.sh, whose `3` means UNKNOWN and is neither
# a pass nor a fail. An aborted suite is exactly that answer, so it reports
# exactly that code. A killed run is not a failing run, and printing FAIL for
# one is the same class of lie as printing PASS for a check that never ran.
#
# Three mechanisms, so that a killed suite CANNOT emit a gate verdict:
#
#   1. the signal traps TERMINATE. ci_abort_on_signal cleans up, says it was
#      signalled, and exits 3 -- it never falls through to the next gate.
#   2. every gate dispatch is preconditioned on the tree still being there
#      (ci_require_tree_intact, called from run_one_gate -- the one choke point
#      both the suite and the single-gate path pass through). A gate is refused
#      with a named reason rather than left to discover the missing tree as a
#      content failure.
#   3. cleanup belongs to the shell that CREATED the snapshot and to nothing
#      else, so a gate subshell can never delete the tree its siblings are
#      about to run in.
#
# suite_abort_selftest (scripts/ci/gates/hygiene.sh, run by the ci-parity gate)
# proves all three on every run, in both directions: a signalled suite prints
# no gate FAILURE and exits 3, and an uninterrupted one still reports a real
# mid-body failure as FAIL.
#
# The whole guarded block is idempotent so any number of scripts can source it.
if [ -z "${_RA8_ABORT_SH:-}" ]; then
  _RA8_ABORT_SH=1

  # The exit-code contract, deliberately identical to scripts/ci/monitor.sh's.
  # One meaning of 3 across the tooling: no verdict could be established.
  RA8_CI_EXIT_ABORTED=3

  # Non-empty once this run has stopped being a measurement; the text says why.
  # Every reader goes through ci_aborted / ci_abort_reason.
  RA8_CI_ABORT_REASON=""

  # The snapshot this run is gating, the PID of the shell that created it, and
  # whether it has been proven to exist (see ci_snapshot_seal). Empty dir means
  # "no snapshot" -- the in-place single-gate path, where there is nothing to
  # delete and nothing to lose.
  RA8_CI_SNAPSHOT_DIR=""
  RA8_CI_SNAPSHOT_OWNER=""
  RA8_CI_SNAPSHOT_SEALED=0

  # Paths the snapshot must still contain for a gate run in it to mean
  # anything: the runner itself, the gate bodies it dispatches, and the build
  # entry points the heavy gates drive.
  #
  # These are checked TWICE against two different observations, which is what
  # keeps the guard from comparing a constant with itself: ci_snapshot_seal
  # proves every one of them present the moment the snapshot is materialised
  # (a missing one there means the extraction failed, and fails the run), and
  # ci_require_tree_intact re-checks the same set before each gate. An absence
  # at the second observation is therefore a measured DISAPPEARANCE.
  RA8_CI_SNAPSHOT_SENTINELS=(
    "scripts/ci.sh"
    "scripts/ci/gates"
    "Makefile"
    "CMakeLists.txt"
  )

  # Non-vacuity floor for the sentinel set. A guard whose expected set has
  # collapsed to nothing can never fire and would report an intact tree
  # forever; that is the failure mode this repo keeps finding, so the floor is
  # asserted rather than assumed.
  RA8_CI_SNAPSHOT_SENTINEL_FLOOR=3

  # Has this run stopped being a measurement?
  ci_aborted() {
    [[ -n "$RA8_CI_ABORT_REASON" ]]
  }

  ci_abort_reason() {
    printf '%s\n' "$RA8_CI_ABORT_REASON"
  }

  # Remove the snapshot -- and ONLY from the shell that created it.
  #
  # bash resets caught signals to their inherited disposition inside a ( )
  # subshell, so a gate body should never reach this. The owner check makes
  # that a checked fact rather than an argument about bash semantics: whatever
  # runs this in a subshell, the tree the siblings are about to use survives.
  ci_snapshot_cleanup() {
    [[ -n "$RA8_CI_SNAPSHOT_DIR" ]] || return 0
    [[ "$BASHPID" == "$RA8_CI_SNAPSHOT_OWNER" ]] || return 0
    rm -rf "$RA8_CI_SNAPSHOT_DIR" "$RA8_CI_SNAPSHOT_DIR.index"
  }

  # THE signal handler. It exits; it does not fall through.
  #
  # This is the whole of #542 in one function. The previous handler cleaned up
  # and returned, which is why a signalled run went on to "measure" a tree it
  # had just deleted.
  ci_abort_on_signal() {
    local sig="$1"
    RA8_CI_ABORT_REASON="the run was killed by SIG${sig}"
    echo "" >&2
    echo "===================================================================" >&2
    echo "== ci.sh: ABORTED -- $RA8_CI_ABORT_REASON" >&2
    echo "===================================================================" >&2
    echo "This run did not fail: it was killed. No gate after this point was" >&2
    echo "measured, and the snapshot the gates run in is being removed now," >&2
    echo "so no verdict is being reported for anything -- a FAIL table here" >&2
    echo "would describe nothing about the tree under test (#542)." >&2
    echo "" >&2
    echo "Exit $RA8_CI_EXIT_ABORTED is UNKNOWN: neither a pass nor a fail," >&2
    echo "the same contract 'make ci-status' uses. Re-run the suite to get a" >&2
    echo "verdict; do not read this as one." >&2
    echo "" >&2
    echo "A run started as a detached background job over ssh is killed this" >&2
    echo "way when the session closes (systemd-logind KillUserProcesses)." >&2
    echo "Hold the session open, or run it under systemd-run / tmux." >&2
    ci_snapshot_cleanup
    exit "$RA8_CI_EXIT_ABORTED"
  }

  # Install the traps. Cleanup and abort are DELIBERATELY separate handlers:
  # one trap serving both is what let a signal double as ordinary teardown.
  ci_install_abort_traps() {
    trap 'ci_snapshot_cleanup' EXIT
    trap 'ci_abort_on_signal INT' INT
    trap 'ci_abort_on_signal TERM' TERM
    trap 'ci_abort_on_signal HUP' HUP
    trap 'ci_abort_on_signal QUIT' QUIT
  }

  # Take ownership of a freshly-created snapshot directory and arm the traps.
  #
  # Called BEFORE the tree is materialised into it, so an abort during the
  # materialise still cleans up -- and still exits instead of continuing into a
  # suite over a half-written tree.
  ci_snapshot_own() {
    RA8_CI_SNAPSHOT_DIR="$1"
    RA8_CI_SNAPSHOT_OWNER="$BASHPID"
    RA8_CI_SNAPSHOT_SEALED=0
    ci_install_abort_traps
  }

  # Prove the snapshot exists, then arm the per-gate re-check against it.
  #
  # The sentinels are verified HERE first. If one is missing now, the snapshot
  # never materialised and the run must fail on that rather than dispatch gates
  # into a tree that was never there.
  ci_snapshot_seal() {
    local sentinel missing=()
    if [[ "${#RA8_CI_SNAPSHOT_SENTINELS[@]}" -lt "$RA8_CI_SNAPSHOT_SENTINEL_FLOOR" ]]; then
      echo "ERROR: the snapshot-integrity guard has ${#RA8_CI_SNAPSHOT_SENTINELS[@]}" >&2
      echo "       sentinel path(s), below the floor of" >&2
      echo "       $RA8_CI_SNAPSHOT_SENTINEL_FLOOR. A guard with nothing to look" >&2
      echo "       for can never fire and would report an intact tree forever." >&2
      return 1
    fi
    for sentinel in "${RA8_CI_SNAPSHOT_SENTINELS[@]}"; do
      [[ -e "$RA8_CI_SNAPSHOT_DIR/$sentinel" ]] || missing+=("$sentinel")
    done
    if [[ "${#missing[@]}" -gt 0 ]]; then
      echo "ERROR: the snapshot at $RA8_CI_SNAPSHOT_DIR is missing ${missing[*]}" >&2
      echo "       immediately after being materialised. It was never a copy of" >&2
      echo "       HEAD, so no gate run in it would mean anything." >&2
      return 1
    fi
    RA8_CI_SNAPSHOT_SEALED=1
    return 0
  }

  # Record that the tree under test has gone, with the reason, and say what it
  # means for the verdict.
  ci_tree_lost() {
    local what="$1" why="$2"
    RA8_CI_ABORT_REASON="the tree under test disappeared ($why)"
    echo "" >&2
    echo "===================================================================" >&2
    echo "== ci.sh: ABORTED before gate '$what' -- $why" >&2
    echo "===================================================================" >&2
    echo "Refusing to run this gate. The directory the suite was gating is" >&2
    echo "gone, so whatever this gate reported would be about nothing: every" >&2
    echo "check would 'fail' on missing files and the table would blame" >&2
    echo "whichever gates happened to come last (#542)." >&2
    echo "" >&2
    echo "Exit $RA8_CI_EXIT_ABORTED is UNKNOWN -- neither a pass nor a fail." >&2
  }

  # THE precondition on every gate dispatch: the tree under test is still there.
  #
  # Two observations, in the order they can fail. The working directory is
  # checked with `pwd -P`, which calls getcwd(): a deleted cwd fails there,
  # while bash's cached $PWD would still print a plausible path. The sentinels
  # are then re-checked against the set ci_snapshot_seal proved present.
  #
  # Returns 0 when there is no snapshot to lose (the in-place single-gate path)
  # -- there is nothing to guard, and inventing a guard for it would be the
  # constant-compared-with-itself shape this file is trying to end.
  ci_require_tree_intact() {
    local what="$1" here sentinel missing=()
    here="$(pwd -P 2>/dev/null || true)"
    if [[ -z "$here" ]]; then
      ci_tree_lost "$what" "the working directory no longer exists"
      return 1
    fi
    [[ "$RA8_CI_SNAPSHOT_SEALED" == "1" ]] || return 0
    if [[ ! -d "$RA8_CI_SNAPSHOT_DIR" ]]; then
      ci_tree_lost "$what" "the snapshot $RA8_CI_SNAPSHOT_DIR is gone"
      return 1
    fi
    for sentinel in "${RA8_CI_SNAPSHOT_SENTINELS[@]}"; do
      [[ -e "$RA8_CI_SNAPSHOT_DIR/$sentinel" ]] || missing+=("$sentinel")
    done
    if [[ "${#missing[@]}" -gt 0 ]]; then
      ci_tree_lost "$what" "the snapshot lost ${missing[*]}"
      return 1
    fi
    return 0
  }

  # ===========================================================================
  # THE PROBE the abort self-test drives.
  # ===========================================================================
  # Reached ONLY through `bash scripts/ci.sh --selftest-abort <mode>`, and only
  # from suite_abort_selftest in scripts/ci/gates/hygiene.sh.
  #
  # It swaps the registry for fixture rows and then runs THE REAL
  # run_suite_on_snapshot -- the same materialise, the same traps, the same
  # dispatch, the same summary. Reimplementing a miniature runner here would
  # test a copy of the runner rather than the runner, which is the mistake
  # scripts/ci.sh exists to prevent.
  #
  # The fixtures are defined INSIDE this function, the way
  # suite_errexit_selftest defines its probe gate: a fixture defined at file
  # scope would be dispatchable by name through `--gate`, and a gate that
  # nothing schedules but anything can run is not a gate.
  #
  # `ra8-probe-after` FAILS when it runs. That is the point: under the #542
  # behaviour the runner carried on past the abort, so a suite that reaches
  # this fixture produces exactly the invented FAIL row the self-test asserts
  # can no longer appear.
  # Define the fixture gates. Defined INSIDE a function, the way
  # suite_errexit_selftest defines its probe gate: a fixture defined at file
  # scope would be dispatchable by name through `--gate`, and a gate that
  # nothing schedules but anything can run is not a gate.
  #
  # `ra8-probe-after` FAILS when it runs. That is the point: under the #542
  # behaviour the runner carried on past the abort, so a suite that reaches
  # this fixture produces exactly the invented FAIL row the self-test asserts
  # can no longer appear.
  _ci_abort_probe_fixtures() {
    # shellcheck disable=SC2329  # dispatched indirectly, by registry name.
    gate_ra8_probe_hang() (
      set -e
      # Publish this shell's PID and block. The self-test waits for the file
      # before signalling: killing a run that had not yet entered a gate would
      # prove nothing about what happens mid-gate.
      printf '%s\n' "$$" >"${RA8_CI_PROBE_MARKER:?the abort probe needs RA8_CI_PROBE_MARKER}"
      sleep 60
    )
    # shellcheck disable=SC2329  # dispatched indirectly, by registry name.
    gate_ra8_probe_destroy() (
      set -e
      # Exactly what the #542 trap did: delete the tree the suite is standing
      # in, from under the still-running runner.
      rm -rf "${RA8_CI_SNAPSHOT_DIR:?the destroy probe needs an armed snapshot}"
    )
    # shellcheck disable=SC2329  # dispatched indirectly, by registry name.
    gate_ra8_probe_after() (
      set -e
      echo "RA8-PROBE-AFTER-RAN"
      false
    )
    # shellcheck disable=SC2329  # dispatched indirectly, by registry name.
    gate_ra8_probe_ok() (
      set -e
      echo "RA8-PROBE-OK-RAN"
    )
    # shellcheck disable=SC2329  # dispatched indirectly, by registry name.
    gate_ra8_probe_fail() (
      set -e
      false
      echo "RA8-PROBE-FAIL-CONTINUED"
    )
  }

  # Swap the registry for the two fixture rows this mode needs.
  _ci_abort_probe_registry() {
    # shellcheck disable=SC2034  # read by list_gates in scripts/ci.sh, the only thing that sources this file.
    case "$1" in
      hang)
        RA8_GATE_REGISTRY=(
          "ra8-probe-hang|fast|abort probe: blocks until the run is killed"
          "ra8-probe-after|fast|abort probe: must never run"
        )
        ;;
      destroy)
        RA8_GATE_REGISTRY=(
          "ra8-probe-destroy|fast|abort probe: deletes the tree under test"
          "ra8-probe-after|fast|abort probe: must never run"
        )
        ;;
      fail)
        RA8_GATE_REGISTRY=(
          "ra8-probe-ok|fast|abort probe: passes"
          "ra8-probe-fail|fast|abort probe: fails part-way through its body"
        )
        ;;
      *)
        echo "ci.sh: unknown --selftest-abort mode '$1'" >&2
        echo "       modes: hang | destroy | fail" >&2
        return 2
        ;;
    esac
  }

  ci_abort_probe() {
    local mode="$1"
    _ci_abort_probe_fixtures
    _ci_abort_probe_registry "$mode" || return 2
    echo "ci.sh: ABORT-PROBE (mode=$mode) -- fixture gates driven by" >&2
    echo "       suite_abort_selftest. This is NOT a suite verdict." >&2
    run_suite_on_snapshot 0
  }
fi
