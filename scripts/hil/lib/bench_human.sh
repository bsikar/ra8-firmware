#!/usr/bin/env bash
# SPDX-License-Identifier: MIT
# Copyright (c) 2026 Brighton Sikarskie
#
# bench_human.sh -- the verbs a PERSON standing at the bench uses: hold, free,
# extend, take.
#
# `source` this (do not execute it); scripts/hil/bench.sh pulls it in for those
# four verbs only.
#
# WHY A HUMAN HOLD IS PROCESS-BOUND BY DEFAULT
# --------------------------------------------
# Everything else in this design refuses to trust a promise: a holder is a live
# process, and the kernel is the liveness oracle. A detached human lease is the
# one place that breaks -- somebody holds the bench for eight hours, walks away,
# and nothing auto-kills a human holder by design. So `hold` is a SHELL. You
# keep it open while you work and the bench is yours; you close it, or your
# laptop dies, and the bench comes back. The cost is that a sleeping laptop
# drops the bench, and that is the right trade: the alternative loses the bench
# to somebody who has gone home.
#
# `hold --detached` is the escape hatch for the genuine case -- you are
# physically at the bench with the lid closed -- and it therefore REQUIRES
# --for and is capped. `status` says plainly which kind is in force, so nobody
# has to infer it from how long the hold has been open.

_ra8_bench_human_dir="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
# shellcheck source=scripts/hil/lib/bench_client.sh
source "$_ra8_bench_human_dir/bench_client.sh"

# The phrase that acknowledges a person may physically be at the bench. It is
# long and unguessable on purpose: a preempt that can be typed by accident is
# not an acknowledgement of anything. It must match bench_host.sh's
# BH_CONFIRM_PHRASE, and bench.sh selftest asserts they still agree.
RA8_BENCH_CONFIRM_PHRASE="someone-may-be-at-the-bench"

# ---------------------------------------------------------------------------
# hold
# ---------------------------------------------------------------------------

cmd_hold() {
  local intent="" budget_s="" wait_s=0 detached=0 name=""
  while [ $# -gt 0 ]; do
    case "$1" in
      --intent | --why)
        intent="${2:-}"
        shift 2
        ;;
      --for)
        budget_s="$(bench_duration "${2:-}")"
        shift 2
        ;;
      --wait)
        wait_s="$(bench_duration "${2:-}")"
        shift 2
        ;;
      --name)
        name="${2:-}"
        shift 2
        ;;
      --detached)
        detached=1
        shift
        ;;
      *)
        bench_say "unknown option '$1'"
        return "$RA8_BENCH_EXIT_UNKNOWN"
        ;;
    esac
  done
  [ -n "$intent" ] || {
    bench_say "usage: bench.sh hold --why \"<what you are doing>\" [--for 2h] [--detached]"
    bench_say "An unreadable lock is a lock people force-take blindly, so --why is required."
    return "$RA8_BENCH_EXIT_UNKNOWN"
  }
  [ -n "$name" ] || name="$(bench_default_name human)"
  [ -n "$wait_s" ] || wait_s=0

  if [ "$detached" -eq 1 ]; then
    [ -n "$budget_s" ] || {
      bench_say "--detached REQUIRES --for. A detached hold survives this command,"
      bench_say "so nothing but its budget bounds it -- and a default is what gets"
      bench_say "forgotten. Cap is 8h."
      return "$RA8_BENCH_EXIT_UNKNOWN"
    }
    cmd_acquire --intent "$intent" --for "${budget_s}s" --wait "${wait_s}s" \
      --as human --name "$name"
    return $?
  fi

  # Process-bound: the shell IS the hold.
  [ -n "$budget_s" ] || budget_s=7200
  bench_say "starting a shell that holds the bench. Exit it (Ctrl-D) to release."
  cmd_run --intent "$intent" --for "${budget_s}s" --wait "${wait_s}s" \
    --as human --name "$name" -- "${SHELL:-/bin/bash}"
  local rc=$?
  bench_say "shell exited -- bench released."
  return "$rc"
}

# ---------------------------------------------------------------------------
# free
# ---------------------------------------------------------------------------

# Distinct from `release --lock-id`: `free` identifies the hold by WHO owns it
# rather than by an id nobody wrote down, which is what a person at a terminal
# actually has. It refuses to release anybody else's -- that is a preempt, and
# preempts go through `take` so they are journaled and acknowledged.
cmd_free() {
  local probe me holder cls lock_id
  probe="$(bench_host probe 2>/dev/null)"
  lock_id="$(bench_field "$probe" f_lock_id)"
  if [ "$(bench_field "$probe" state FREE)" != "HELD" ] || [ -z "$lock_id" ]; then
    bench_say "the bench is not held -- nothing to free."
    return "$RA8_BENCH_EXIT_FREE"
  fi
  holder="$(bench_field "$probe" f_holder_name)"
  cls="$(bench_field "$probe" f_holder_class)"
  me="$(bench_default_name human)"
  if [ "$holder" != "$me" ] && [ "$holder" != "${RA8_BENCH_ACTOR:-}" ]; then
    bench_say "refusing: the bench is held by $holder ($cls), not by you ($me)."
    bench_say "To preempt it:  make bench-take WHY=\"...\""
    return "$RA8_BENCH_EXIT_HELD"
  fi
  bench_host release "$lock_id"
}

# ---------------------------------------------------------------------------
# extend
# ---------------------------------------------------------------------------

cmd_extend() {
  local extra_s="" probe lock_id holder me
  while [ $# -gt 0 ]; do
    case "$1" in
      --for)
        extra_s="$(bench_duration "${2:-}")"
        shift 2
        ;;
      *)
        bench_say "unknown option '$1'"
        return "$RA8_BENCH_EXIT_UNKNOWN"
        ;;
    esac
  done
  [ -n "$extra_s" ] || {
    bench_say "usage: bench.sh extend --for 30m"
    return "$RA8_BENCH_EXIT_UNKNOWN"
  }
  probe="$(bench_host probe 2>/dev/null)"
  lock_id="$(bench_field "$probe" f_lock_id)"
  [ -n "$lock_id" ] || {
    bench_say "the bench is not held -- nothing to extend."
    return "$RA8_BENCH_EXIT_FREE"
  }
  holder="$(bench_field "$probe" f_holder_name)"
  me="${RA8_BENCH_LOCK_ID:-}"
  if [ -n "$me" ] && [ "$me" != "$lock_id" ]; then
    bench_say "refusing: your hold ($me) is not the current one ($lock_id)."
    return "$RA8_BENCH_EXIT_HELD"
  fi
  if [ -z "$me" ] && [ "$holder" != "$(bench_default_name human)" ] &&
    [ "$holder" != "${RA8_BENCH_ACTOR:-}" ]; then
    bench_say "refusing: the bench is held by $holder, not by you."
    return "$RA8_BENCH_EXIT_HELD"
  fi
  bench_host extend "$lock_id" "$extra_s"
}

# ---------------------------------------------------------------------------
# take -- the preempt
# ---------------------------------------------------------------------------

# Precedence itself lives on the bench host (bh_preempt), where the incumbent
# record is, so a client cannot talk its way past it. This verb's job is to
# collect a reason, refuse to proceed without one, and pass the acknowledgement
# through.
cmd_take() {
  local reason="" intent="" confirm="${CONFIRM:-}" budget_s=1800 wait_s=0
  while [ $# -gt 0 ]; do
    case "$1" in
      --reason | --why)
        reason="${2:-}"
        shift 2
        ;;
      --intent)
        intent="${2:-}"
        shift 2
        ;;
      --confirm)
        confirm="${2:-}"
        shift 2
        ;;
      --for)
        budget_s="$(bench_duration "${2:-}")"
        shift 2
        ;;
      *)
        bench_say "unknown option '$1'"
        return "$RA8_BENCH_EXIT_UNKNOWN"
        ;;
    esac
  done
  [ -n "$reason" ] || {
    bench_say "usage: make bench-take WHY=\"why you are taking it from them\""
    bench_say "The reason is journaled. Preempting somebody silently is how a"
    bench_say "half-flashed board becomes a mystery."
    return "$RA8_BENCH_EXIT_UNKNOWN"
  }
  [ -n "$intent" ] || intent="$reason"
  [ -n "$budget_s" ] || budget_s=1800

  local probe cls holder
  probe="$(bench_host probe 2>/dev/null)"
  if [ "$(bench_field "$probe" state FREE)" != "HELD" ]; then
    bench_say "the bench is already free -- taking it normally."
  else
    cls="$(bench_field "$probe" f_holder_class)"
    holder="$(bench_field "$probe" f_holder_name)"
    if [ "$cls" = "human" ] && [ "$confirm" != "$RA8_BENCH_CONFIRM_PHRASE" ]; then
      bench_say "refusing: the bench is held by $holder (human), active since"
      bench_say "  $(bench_field "$probe" f_acquired_at '?') -- \"$(bench_field "$probe" f_intent)\""
      bench_say ""
      bench_say "  A human holder can only be preempted by another human with an"
      bench_say "  explicit acknowledgement that someone may physically be at the bench:"
      bench_say "      make bench-take WHY=\"...\" CONFIRM=$RA8_BENCH_CONFIRM_PHRASE"
      return "$RA8_BENCH_EXIT_HELD"
    fi
    bench_say "preempting $holder ($cls) -- $reason"
  fi

  cmd_acquire --intent "$intent" --for "${budget_s}s" --wait "${wait_s}s" \
    --as human --break-glass --confirm "$confirm"
}
