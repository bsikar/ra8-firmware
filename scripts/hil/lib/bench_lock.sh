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
# The honest edge: a CHILD of the calling script inherits fd 7, so a child
# still running after the script is gone keeps the hold open. That is a
# conservative failure (the bench stays locked while a descendant of a bench
# script is alive -- arguably correct) and it is bounded by the declared budget
# and the reaper. It is never the dangerous direction: the lock is not released
# while somebody is still working. The EXIT trap below closes fd 7 promptly on
# the normal path so this only matters when a script dies hard.
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
  # THE release. Everything after this is tidying.
  eval "exec ${_RA8_BENCH_GUARD_FD}>&-"
  if [ -n "${RA8_BENCH_HOLDER_PID:-}" ]; then
    wait "$RA8_BENCH_HOLDER_PID" 2>/dev/null
  fi
  rm -rf "$_RA8_BENCH_GUARD_TMP" 2>/dev/null
  _RA8_BENCH_GUARD_TMP=""
  unset RA8_BENCH_LOCK_ID
  return 0
}

# Is the id in RA8_BENCH_LOCK_ID still the live holder? Asked of the bench
# host, never assumed from the variable: an exported id whose holder has since
# died would otherwise let a nested script skip the guard on a free bench.
_ra8_bench_still_ours() {
  local seen
  seen="$(bench_lock_id_now)"
  [ -n "$seen" ] && [ "$seen" = "${RA8_BENCH_LOCK_ID:-}" ]
}

# ra8_bench_require <intent> [budget]
#
# budget accepts 15m / 2h / 900s / 900 and defaults to 15 minutes -- the worst
# single app is a 240 s verify plus a flash plus the rfp-cli retry path. Suites
# pass their own (all.sh asks for 2h).
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

  budget_s="$(bench_duration "$budget")"
  if [ -z "$budget_s" ]; then
    printf '[bench-guard] FATAL -- unparseable budget "%s" (want 15m / 2h / 900s).\n' \
      "$budget" >&2
    return 3
  fi
  cls="$(bench_default_class)"
  name="$(bench_default_name "$cls")"
  lock_id="$(bench_new_lock_id)"

  _RA8_BENCH_GUARD_TMP="$(mktemp -d "${TMPDIR:-/tmp}/ra8-bench-guard.XXXXXX")" || return 3
  mkfifo "$_RA8_BENCH_GUARD_TMP/hold.in" || {
    rm -rf "$_RA8_BENCH_GUARD_TMP"
    _RA8_BENCH_GUARD_TMP=""
    return 3
  }
  : >"$_RA8_BENCH_GUARD_TMP/hold.out"

  bench_start_holder "$lock_id" "$cls" "$name" "$intent" "$budget_s" \
    "${RA8_BENCH_WAIT_S:-0}" "${_RA8_BENCH_GLASS:-false}" \
    "$_RA8_BENCH_GUARD_TMP/hold.in" "$_RA8_BENCH_GUARD_TMP/hold.out" "${CONFIRM:-}"
  if [ -z "$RA8_BENCH_HOLDER_PID" ]; then
    printf '[bench-guard] FATAL -- could not start a holder (PI_HOST unset?).\n' >&2
    rm -rf "$_RA8_BENCH_GUARD_TMP"
    _RA8_BENCH_GUARD_TMP=""
    return 3
  fi
  # Opening the write end in THIS shell is what binds the hold to this script's
  # lifetime. It has to happen here and not in a subshell, which is why the
  # guard is sourced rather than executed.
  eval "exec ${_RA8_BENCH_GUARD_FD}>\"$_RA8_BENCH_GUARD_TMP/hold.in\""

  bench_await_ack "$lock_id" "$_RA8_BENCH_GUARD_TMP/hold.out" \
    "$RA8_BENCH_HOLDER_PID" $((${RA8_BENCH_WAIT_S:-0} + 30))
  rc=$?
  if [ "$rc" -eq 1 ]; then
    printf '[bench-guard] REFUSED -- the bench is not yours.\n' >&2
    bench_report_denial "$_RA8_BENCH_GUARD_TMP/hold.out"
    printf '[bench-guard] wait for it, or preempt with:\n' >&2
    printf '[bench-guard]   make bench-take WHY="..."\n' >&2
    ra8_bench_release_local
    return 1
  fi
  if [ "$rc" -ne 0 ]; then
    printf '[bench-guard] UNKNOWN -- could not establish a hold. NOT touching the bench.\n' >&2
    sed 's/^/[bench-guard]   /' "$_RA8_BENCH_GUARD_TMP/hold.out" >&2 2>/dev/null
    ra8_bench_release_local
    return 3
  fi

  RA8_BENCH_LOCK_ID="$lock_id"
  RA8_BENCH_INTENT="$intent"
  export RA8_BENCH_LOCK_ID RA8_BENCH_INTENT
  _ra8_bench_add_exit_trap ra8_bench_release_local
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
