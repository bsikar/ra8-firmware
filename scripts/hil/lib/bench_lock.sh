#!/usr/bin/env bash
# SPDX-License-Identifier: MIT
# Copyright (c) 2026 Brighton Sikarskie
#
# bench_lock.sh -- the mandatory bench guard, in the shape of preflash_guard.sh.
#
# `source` this (do not execute it) and call `ra8_bench_require` immediately
# before the first hardware operation. If it returns non-zero the caller MUST
# exit without touching the bench:
#
#     # shellcheck source=scripts/hil/lib/bench_lock.sh
#     source "$_hil_dir/lib/bench_lock.sh"
#     ra8_bench_require "flash ${APP}" || exit $?
#
#   0 -> the bench is ours (freshly taken, or already held by this session)
#   1 -> DENIED: somebody else holds it. Who, and why, is printed.
#   3 -> could not determine -- bench host unreachable, state directory
#        missing. FAIL CLOSED: a lock you cannot read is not a free bench.
#
# The precedent is deliberate. preflash_guard.sh already proves this tree will
# thread one mandatory call through every board-programming script, and already
# has the loud-override pattern for the deliberate case. This is its sibling
# for the other question a flash has to answer: not "is this image safe?" but
# "is this board mine right now?".
#
# HOW THE HOLD IS BOUND TO THE CALLER
# -----------------------------------
# The hold is a bench-host process blocked on its stdin, and its stdin is a
# fifo whose write end this shell holds on fd 7. So the bench is released when
# fd 7 closes -- which happens when the calling script exits, is killed, or
# dies in any way at all, without any trap having to run. Kernel liveness on
# both ends, same doctrine as the flock itself.
#
# One edge is conservative: a CHILD of the calling script inherits fd 7, so a
# child still running after the script is gone keeps the hold open. The bench
# stays locked while a descendant of a bench script is alive -- arguably
# correct -- and it is bounded by the declared budget and the reaper. The EXIT
# trap below closes fd 7 promptly on the normal path, so this only matters when
# a script dies hard.
#
# THE OTHER EDGE IS NOT CONSERVATIVE, AND NEEDS THE FENCE BELOW
# -------------------------------------------------------------
# This header used to claim the dangerous direction could not happen -- "the
# lock is not released while somebody is still working". It can, and it was
# measured doing so (scripts/hil/bench_contention.sh, phase `death`).
#
# The hold travels over its OWN ssh connection, separate from the connections a
# guarded script uses to drive the hardware. Kill the hold's ssh client and the
# bench host sees EOF and releases at once -- while the calling script, which
# knows nothing about it, carries on flashing over a connection that is still
# perfectly alive. On the bench this let a SIGKILLed holder's J-Link session run
# 75 s past its own release, during which three other machines took the lock in
# turn and programmed MRAM underneath it. The hold does not have to be killed
# for this: a network blip, an sshd restart or ServerAlive giving up does the
# same thing.
#
# `bench.sh run` never had this hole, because bench_supervise() runs the payload
# as a CHILD and fences it -- if the holder dies, the payload is stopped. The
# guard could not copy that shape, because a sourced guard returns to its caller
# rather than wrapping it. So it fences in the other direction: a background
# watcher signals the CALLER when the hold goes away. Same promise, same
# doctrine ("one implementation of how a hold is taken, two entry points"),
# reversed plumbing.
#
# What the fence does NOT do: reach across ssh and stop a JLinkExe already
# running on the bench host. Neither does bench_supervise. It stops the caller
# from issuing anything further, within one poll interval, and says so loudly --
# which is the difference between a bounded, announced loss of protection and an
# unbounded silent one.
#
# Portability: bash 3.2 (the macOS system bash) -- no name-refs, no mapfile.

_ra8_bench_lib_dir="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
_ra8_bench_cli="$_ra8_bench_lib_dir/../bench.sh"

# The whole client half, so the guard takes a hold the exact same way
# `bench.sh run` does. One implementation, two entry points.
# shellcheck source=scripts/hil/lib/bench_client.sh
source "$_ra8_bench_lib_dir/bench_client.sh"

# fd 7 is the guard's, and only the guard's. bench.sh uses 8, its selftest
# uses 9, and the HIL scripts use 3 (recover.sh's `3<>$UART`) -- so nothing in
# this tree can close this one by accident.
_RA8_BENCH_GUARD_FD=7
_RA8_BENCH_GUARD_TMP=""

# The fence: pid of the watcher, and how often it looks. One second matches
# bench_supervise(), and bounds the unprotected window to about a second --
# against the 75 s measured without it.
_RA8_BENCH_FENCE_PID=""
_RA8_BENCH_FENCE_POLL_S="${RA8_BENCH_FENCE_POLL_S:-1}"

# How long the caller gets to wind up after SIGTERM before SIGKILL, matching
# bench_supervise()'s grace.
_RA8_BENCH_FENCE_GRACE_S=5

# Watch the hold, and stop the CALLER if it goes away.
#
# fd 7 is closed in the watcher first, and that is load-bearing rather than
# tidy: the write end of the fifo IS the hold, so a fence that inherited it
# would keep the bench locked after the caller died -- it would cause the exact
# failure the file's other edge describes.
_ra8_bench_start_fence() {
  local holder="$1" caller="$2"
  (
    eval "exec ${_RA8_BENCH_GUARD_FD}>&-"
    while kill -0 "$caller" 2>/dev/null; do
      if ! kill -0 "$holder" 2>/dev/null; then
        printf '\n[bench-guard] ***************************************************\n' >&2
        printf '[bench-guard] *** THE BENCH HOLD DIED WHILE THIS SCRIPT RAN.  ***\n' >&2
        printf '[bench-guard] *** The board is no longer ours. Stopping now.  ***\n' >&2
        printf '[bench-guard] ***************************************************\n' >&2
        printf '[bench-guard] lock was %s -- another actor may already have taken it.\n' \
          "${RA8_BENCH_LOCK_ID:-unknown}" >&2
        kill -TERM "$caller" 2>/dev/null
        sleep "$_RA8_BENCH_FENCE_GRACE_S"
        kill -KILL "$caller" 2>/dev/null
        exit 0
      fi
      sleep "$_RA8_BENCH_FENCE_POLL_S"
    done
  ) &
  _RA8_BENCH_FENCE_PID="$!"
}

# Take the fence down. MUST happen before fd 7 closes on the normal path:
# closing fd 7 is what makes the holder exit, and a fence still watching would
# see that as the hold dying and shoot a caller that is already leaving.
# Both statuses are swallowed deliberately, and it is not defensive noise: we
# have just SIGTERMed this job, so `wait` returns 143 BY CONSTRUCTION. Every
# guarded script runs `set -e`, and this runs from their EXIT trap, so an
# unguarded `wait` here makes 143 the exit status of the whole script -- which
# it did, turning every clean `make hil-flash` into a failure, until this line
# was written the way it is. `return 0` closes the same hole for the caller of
# this function.
_ra8_bench_stop_fence() {
  [ -n "$_RA8_BENCH_FENCE_PID" ] || return 0
  kill -TERM "$_RA8_BENCH_FENCE_PID" 2>/dev/null || true
  wait "$_RA8_BENCH_FENCE_PID" 2>/dev/null || true
  _RA8_BENCH_FENCE_PID=""
  return 0
}

# Prepend a command to whatever EXIT trap the caller already installed, rather
# than replacing it. Several HIL scripts clean up a mktemp in theirs, and a
# guard that silently ate that would leak a file per flash. `trap -p` prints a
# re-runnable `trap -- '...' EXIT`, so splicing our command into the front of
# its quoted body and re-issuing it composes the two.
_ra8_bench_add_exit_trap() {
  local new="$1" old
  old="$(trap -p EXIT)"
  if [ -z "$old" ]; then
    # shellcheck disable=SC2064  # expanding NOW is the point: $new is the literal function name to install, and late expansion would install nothing.
    trap "$new" EXIT
    return 0
  fi
  eval "${old/trap -- \'/trap -- \'$new; }"
}

# Release the hold this shell owns. Idempotent; safe to call when there is
# nothing to release.
ra8_bench_release_local() {
  [ -n "${_RA8_BENCH_GUARD_TMP:-}" ] || return 0
  # Before anything else: the fence exists to notice the holder dying, and the
  # very next line makes the holder die on purpose.
  _ra8_bench_stop_fence
  # THE release. Everything after this is tidying.
  eval "exec ${_RA8_BENCH_GUARD_FD}>&-"
  if [ -n "${RA8_BENCH_HOLDER_PID:-}" ]; then
    # Same reasoning as _ra8_bench_stop_fence: this runs from a `set -e`
    # script's EXIT trap, so a holder that exited non-zero must not become the
    # script's own exit status.
    wait "$RA8_BENCH_HOLDER_PID" 2>/dev/null || true
  fi
  rm -rf "$_RA8_BENCH_GUARD_TMP" 2>/dev/null
  _RA8_BENCH_GUARD_TMP=""
  unset RA8_BENCH_LOCK_ID
  return 0
}

# How long this invocation will queue for a bench somebody else holds, in
# seconds, or empty when what was asked for cannot be parsed.
#
# Two spellings, one answer. RA8_BENCH_WAIT_S is seconds and is what the guard
# has always read; RA8_BENCH_WAIT is the human form ("10m", "2h", "900s") that
# `make hil-... WAIT=` sets, parsed HERE by bench_duration() rather than in the
# Makefile -- there is already one duration parser and a second one in `make`
# syntax is how the two drift apart.
_ra8_bench_wait_s() {
  local parsed
  if [ -n "${RA8_BENCH_WAIT:-}" ]; then
    parsed="$(bench_duration "$RA8_BENCH_WAIT")"
    [ -n "$parsed" ] || return 1
    printf '%s' "$parsed"
    return 0
  fi
  parsed="${RA8_BENCH_WAIT_S:-0}"
  case "$parsed" in
    '' | *[!0-9]*) return 1 ;;
    *) ;;
  esac
  printf '%s' "$parsed"
}

# Is the id in RA8_BENCH_LOCK_ID still the live holder? Asked of the bench
# host, never assumed from the variable: an exported id whose holder has since
# died would otherwise let a nested script skip the guard on a free bench.
#
# bench_lock_id_now() comes from lib/bench_client.sh. It used to be defined in
# bench.sh, which this file does not source, so this function called something
# that did not exist: it printed "command not found" to stderr, returned empty,
# and every nested guard call therefore concluded the lock was NOT ours. The
# caller then opened a second channel on fd 7 -- closing the first, which IS
# the hold -- so a script calling two guarded scripts in turn released and
# re-took the bench between them, with a window in which anyone could take it.
_ra8_bench_still_ours() {
  local seen
  seen="$(bench_lock_id_now)"
  [ -n "$seen" ] && [ "$seen" = "${RA8_BENCH_LOCK_ID:-}" ]
}

# Start the holder and open the liveness channel on fd 7 IN THIS SHELL. That
# open fd is the whole binding: it is inherited from here, and the kernel closes
# it when this shell dies, whatever kills it. It has to happen in the caller's
# shell rather than a subshell, which is why the guard is sourced and not
# executed.
_ra8_bench_open_channel() {
  local lock_id="$1" cls="$2" name="$3" intent="$4" budget_s="$5"
  _RA8_BENCH_GUARD_TMP="$(mktemp -d "${TMPDIR:-/tmp}/ra8-bench-guard.XXXXXX")" || return 1
  mkfifo "$_RA8_BENCH_GUARD_TMP/hold.in" || {
    rm -rf "$_RA8_BENCH_GUARD_TMP"
    _RA8_BENCH_GUARD_TMP=""
    return 1
  }
  : >"$_RA8_BENCH_GUARD_TMP/hold.out"
  bench_start_holder "$lock_id" "$cls" "$name" "$intent" "$budget_s" \
    "$_RA8_BENCH_WAIT_EFFECTIVE" "${_RA8_BENCH_GLASS:-false}" \
    "$_RA8_BENCH_GUARD_TMP/hold.in" "$_RA8_BENCH_GUARD_TMP/hold.out" "${CONFIRM:-}"
  if [ -z "$RA8_BENCH_HOLDER_PID" ]; then
    printf '[bench-guard] FATAL -- could not start a holder (PI_HOST unset?).\n' >&2
    rm -rf "$_RA8_BENCH_GUARD_TMP"
    _RA8_BENCH_GUARD_TMP=""
    return 1
  fi
  eval "exec ${_RA8_BENCH_GUARD_FD}>\"$_RA8_BENCH_GUARD_TMP/hold.in\""
}

# Denied and could-not-tell get different words, because they are different
# answers and only one of them is somebody else's fault.
_ra8_bench_report_failure() {
  if [ "$1" -eq 1 ]; then
    printf '[bench-guard] REFUSED -- the bench is not yours.\n' >&2
    bench_report_denial "$_RA8_BENCH_GUARD_TMP/hold.out"
    # "wait for it" used to be the whole advice, with no way to do it. The knob
    # existed -- RA8_BENCH_WAIT_S has always been read a few lines below -- but
    # it was set by nothing, named in no document and reachable from no make
    # target, so in practice every guarded entry point took the lock with
    # flock -n and failed the instant anybody else held it. On a bench shared by
    # ~20 agents and a nightly CI job, telling people to wait while offering
    # only "fail" or "preempt" pushes them towards preempting.
    printf '[bench-guard] QUEUE for it (block in the kernel until it is free):\n' >&2
    printf '[bench-guard]   make hil-<target> ... WAIT=10m\n' >&2
    printf '[bench-guard]   RA8_BENCH_WAIT=10m %s\n' "${0##*/}" >&2
    printf '[bench-guard] or preempt (journaled, announced):\n' >&2
    printf '[bench-guard]   make bench-take WHY="..."\n' >&2
    return 0
  fi
  printf '[bench-guard] UNKNOWN -- could not establish a hold. NOT touching the bench.\n' >&2
  sed 's/^/[bench-guard]   /' "$_RA8_BENCH_GUARD_TMP/hold.out" >&2 2>/dev/null
}

# ra8_bench_require <intent> [budget]
#
# budget accepts 15m / 2h / 900s / 900 and defaults to 15 minutes -- the worst
# single app is a 240 s verify plus a flash plus the rfp-cli retry path. Suites
# pass their own (all.sh asks for 2h).
#
# WAITING, OR NOT
# ---------------
# How long to queue for a bench somebody else holds. RA8_BENCH_WAIT takes the
# human form ("10m", "2h", "900s") and RA8_BENCH_WAIT_S plain seconds; both
# arrive at _ra8_bench_wait_s() above, and RA8_BENCH_WAIT wins.
#
# It defaults to 0 -- fail fast -- because a person at a terminal would rather
# be told than blocked, and because a script that blocks by default turns a busy
# bench into a pile of silently stalled jobs. Set it (or `make hil-... WAIT=10m`,
# or either name in .env for a standing policy) and the guard blocks in the
# kernel instead, which is what you want for CI and for an unattended agent. It
# becomes `flock -w` on the bench host, so the wait is a real kernel wait and
# not a poll loop.
#
# Measured on this bench with four machines competing for sixteen turns: every
# actor got every turn, and the grant order was exact round-robin. Linux flock
# promises no such ordering, so treat that as an observation about this kernel
# rather than a contract -- but note that an unfair queue would show up as a
# denial when the budget ran out, never as a silent hang.
_RA8_BENCH_WAIT_EFFECTIVE=0

ra8_bench_require() {
  local intent="${1:-}" budget="${2:-15m}" budget_s cls name lock_id rc
  if [ -z "$intent" ]; then
    printf '[bench-guard] FATAL -- ra8_bench_require needs an intent string.\n' >&2
    return 3
  fi

  # Already ours? Nested scripts (reflash.sh -> flash.sh, all.sh -> its
  # runners) and anything under `bench.sh run` land here, and must not
  # deadlock against a lock they are already inside.
  if [ -n "${RA8_BENCH_LOCK_ID:-}" ] && _ra8_bench_still_ours; then
    bench_host touch "$RA8_BENCH_LOCK_ID" >/dev/null 2>&1
    printf '[bench-guard] holding (%s) -- %s\n' "$RA8_BENCH_LOCK_ID" "$intent" >&2
    return 0
  fi

  _RA8_BENCH_WAIT_EFFECTIVE="$(_ra8_bench_wait_s)"
  if [ -z "$_RA8_BENCH_WAIT_EFFECTIVE" ]; then
    printf '[bench-guard] FATAL -- unparseable wait "%s" (want 10m / 2h / 900s).\n' \
      "${RA8_BENCH_WAIT:-${RA8_BENCH_WAIT_S:-}}" >&2
    return 3
  fi

  budget_s="$(bench_duration "$budget")"
  if [ -z "$budget_s" ]; then
    printf '[bench-guard] FATAL -- unparseable budget "%s" (want 15m / 2h / 900s).\n' \
      "$budget" >&2
    return 3
  fi
  cls="$(bench_default_class)"
  name="$(bench_default_name "$cls")"
  lock_id="$(bench_new_lock_id)"

  _ra8_bench_open_channel "$lock_id" "$cls" "$name" "$intent" "$budget_s" || return 3

  bench_await_ack "$lock_id" "$_RA8_BENCH_GUARD_TMP/hold.out" \
    "$RA8_BENCH_HOLDER_PID" \
    $((_RA8_BENCH_WAIT_EFFECTIVE + RA8_BENCH_QUIESCE_S + 30))
  rc=$?
  if [ "$rc" -ne 0 ]; then
    _ra8_bench_report_failure "$rc"
    ra8_bench_release_local
    return "$rc"
  fi

  RA8_BENCH_LOCK_ID="$lock_id"
  RA8_BENCH_INTENT="$intent"
  export RA8_BENCH_LOCK_ID RA8_BENCH_INTENT
  _ra8_bench_add_exit_trap ra8_bench_release_local
  # $$ is the CALLER's pid: this file is sourced, so no fork has happened.
  _ra8_bench_start_fence "$RA8_BENCH_HOLDER_PID" "$$"
  printf '[bench-guard] acquired (%s, %s [%s], budget %s) -- %s\n' \
    "$lock_id" "$name" "$cls" "$budget" "$intent" >&2
  return 0
}

# ra8_bench_require_recovery <intent> [budget]
#
# For the destructive-recovery scripts -- recover.sh, dlm_reset.sh, erase.sh,
# reflash.sh, flash_retry.sh, tapo.sh board. Recovery is MORE destructive than
# an ordinary flash, not less, so it is not exempt from the lock: an
# `rfp-cli -erase-chip` landing in the middle of somebody's suite is the worst
# collision available. What it gets instead is a break-glass path.
#
# Set RA8_BENCH_BREAK_GLASS to a reason to force-take. That preempts an agent
# or CI holder; preempting a HUMAN additionally needs the CONFIRM handshake,
# which `bench.sh take` enforces -- because a human holder may be physically at
# the bench with a probe on a test point, and nothing here can see that.
ra8_bench_require_recovery() {
  local intent="${1:-}" budget="${2:-15m}" rc
  if [ -z "${RA8_BENCH_BREAK_GLASS:-}" ]; then
    ra8_bench_require "$intent" "$budget"
    rc=$?
    if [ "$rc" -eq 1 ]; then
      printf '[bench-guard] this is a RECOVERY operation. If the board is wedged the\n' >&2
      printf '[bench-guard] holder is probably dead or is the one that wedged it. To\n' >&2
      printf '[bench-guard] force-take (always journaled, always announced):\n' >&2
      printf '[bench-guard]   RA8_BENCH_BREAK_GLASS="board wedged mid-flash" %s\n' "$0" >&2
    fi
    return "$rc"
  fi
  printf '[bench-guard] BREAK-GLASS: %s\n' "$RA8_BENCH_BREAK_GLASS" >&2
  # ONE operation: the bench host preempts the incumbent and takes the lock
  # without ever letting it go free in between. A separate release-then-acquire
  # would leave a window for a third actor -- and the reason anyone is breaking
  # glass is that the board is already in a bad state.
  #
  # Precedence is decided THERE, next to the incumbent's record: an agent or CI
  # holder is preempted, a human holder needs CONFIRM. Nothing on this side can
  # talk its way past it.
  _RA8_BENCH_GLASS=true ra8_bench_require \
    "$intent (break-glass: $RA8_BENCH_BREAK_GLASS)" "$budget"
}
